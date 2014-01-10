/*
    +--------------------------------------------------------------------+
    | PECL :: ircclient                                                  |
    +--------------------------------------------------------------------+
    | Redistribution and use in source and binary forms, with or without |
    | modification, are permitted provided that the conditions mentioned |
    | in the accompanying LICENSE file are met.                          |
    +--------------------------------------------------------------------+
    | Copyright (c) 2011, Michael Wallner <mike@php.net>                 |
    +--------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <main/php.h>
#include <main/php_ini.h>
#include <main/php_network.h>
#include <ext/standard/php_string.h>
#include <ext/standard/info.h>
#include <ext/standard/basic_functions.h>

#include <Zend/zend.h>
#include <Zend/zend_constants.h>
#include <Zend/zend_interfaces.h>

#ifdef ZTS
#include <TSRM/TSRM.h>
#endif

#include "php_ircclient.h"

#include <errno.h>
#include <ctype.h>
#include <libircclient.h>

PHP_FUNCTION(parse_origin)
{
	char *origin_str;
	int origin_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &origin_str, &origin_len)) {
		zval *znick, *zuser, *zhost;
		size_t siz = 0;
		char *ptr;

		MAKE_STD_ZVAL(znick); ZVAL_NULL(znick);
		MAKE_STD_ZVAL(zuser); ZVAL_NULL(zuser);
		MAKE_STD_ZVAL(zhost); ZVAL_NULL(zhost);

		for (ptr = origin_str; *ptr; ptr += siz + 1) {
			if ((siz = strcspn(ptr, "!@"))) {
				switch (ptr[siz]) {
					case '!':
						ZVAL_STRINGL(znick, ptr, siz, 1);
						break;
					case '@':
						ZVAL_STRINGL(zuser, ptr, siz, 1);
						break;
					case '\0':
						ZVAL_STRINGL(zhost, ptr, siz, 1);
						goto done;
						break;
					default:
						break;
				}
			}
		}
done:
		array_init(return_value);
		add_assoc_zval_ex(return_value, ZEND_STRS("nick"), znick);
		add_assoc_zval_ex(return_value, ZEND_STRS("user"), zuser);
		add_assoc_zval_ex(return_value, ZEND_STRS("host"), zhost);
	}
}


const zend_function_entry php_ircclient_function_entry[] = {
	ZEND_NS_FENTRY("irc\\client", parse_origin, ZEND_FN(parse_origin), NULL, 0)
	{0}
};

PHP_MINIT_FUNCTION(ircclient);
PHP_MINFO_FUNCTION(ircclient);

zend_module_entry ircclient_module_entry = {
	STANDARD_MODULE_HEADER,
	"ircclient",
	php_ircclient_function_entry,
	PHP_MINIT(ircclient),
	NULL,
	NULL,
	NULL,
	PHP_MINFO(ircclient),
	PHP_IRCCLIENT_VERSION,
	STANDARD_MODULE_PROPERTIES
};


#ifdef COMPILE_DL_IRCCLIENT
ZEND_GET_MODULE(ircclient)
#endif

static void php_ircclient_event_callback(irc_session_t *session, const char *event, const char *origin, const char **params, unsigned int count);
static void php_ircclient_event_code_callback(irc_session_t *session, unsigned int event, const char *origin, const char **params, unsigned int count);
static void php_ircclient_event_dcc_chat_callback(irc_session_t *session, const char *nick, const char *addr, irc_dcc_t dccid);
static void php_ircclient_event_dcc_send_callback(irc_session_t *session, const char *nick, const char *addr, const char *filename, unsigned long size, irc_dcc_t dccid);

static irc_callbacks_t php_ircclient_callbacks = {
	.event_connect = php_ircclient_event_callback,
	.event_nick = php_ircclient_event_callback,
	.event_quit = php_ircclient_event_callback,
	.event_join = php_ircclient_event_callback,
	.event_part = php_ircclient_event_callback,
	.event_mode = php_ircclient_event_callback,
	.event_umode = php_ircclient_event_callback,
	.event_topic = php_ircclient_event_callback,
	.event_kick = php_ircclient_event_callback,
	.event_channel = php_ircclient_event_callback,
	.event_privmsg = php_ircclient_event_callback,
	.event_notice = php_ircclient_event_callback,
#if PHP_IRCCLIENT_HAVE_EVENT_CHANNEL_NOTICE
	.event_channel_notice = php_ircclient_event_callback,
#endif
	.event_invite = php_ircclient_event_callback,
	.event_ctcp_req = php_ircclient_event_callback,
	.event_ctcp_rep = php_ircclient_event_callback,
	.event_ctcp_action = php_ircclient_event_callback,
	.event_unknown = php_ircclient_event_callback,
	.event_numeric = php_ircclient_event_code_callback,
	.event_dcc_chat_req = php_ircclient_event_dcc_chat_callback,
	.event_dcc_send_req = php_ircclient_event_dcc_send_callback
};

typedef struct php_ircclient_session_callback {
	zval *zfn;
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
} php_ircclient_session_callback_t;

typedef struct php_ircclient_session_object {
	zend_object zo;
	zend_object_value ov;
	irc_session_t *sess;
	unsigned opts;
	HashTable cbc;
#ifdef ZTS
	void ***ts;
#endif
} php_ircclient_session_object_t;

zend_class_entry *php_ircclient_session_class_entry;

void php_ircclient_session_object_free(void *object TSRMLS_DC)
{
	php_ircclient_session_object_t *o = (php_ircclient_session_object_t *) object;

	if (o->sess) {
		irc_destroy_session(o->sess);
		o->sess = NULL;
	}
	zend_hash_destroy(&o->cbc);
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(o);
}

static void php_ircclient_session_callback_dtor(void *ptr)
{
	php_ircclient_session_callback_t *cb = (php_ircclient_session_callback_t *) ptr;

	zend_fcall_info_args_clear(&cb->fci, 1);
	zval_ptr_dtor(&cb->zfn);
}

zend_object_value php_ircclient_session_object_create(zend_class_entry *ce TSRMLS_DC)
{
	php_ircclient_session_object_t *obj;

	obj = ecalloc(1, sizeof(*obj));
#if PHP_VERSION_ID >= 50399
	zend_object_std_init((zend_object *) obj, ce TSRMLS_CC);
	object_properties_init((zend_object *) obj, ce);
#else
	obj->zo.ce = ce;
	ALLOC_HASHTABLE(obj->zo.properties);
	zend_hash_init(obj->zo.properties, zend_hash_num_elements(&ce->default_properties), NULL, ZVAL_PTR_DTOR, 0);
	zend_hash_copy(obj->zo.properties, &ce->default_properties, (copy_ctor_func_t) zval_add_ref, NULL, sizeof(zval *));
#endif

	obj->sess = irc_create_session(&php_ircclient_callbacks);
	irc_set_ctx(obj->sess, obj);
	zend_hash_init(&obj->cbc, 10, NULL, php_ircclient_session_callback_dtor, 0);
	TSRMLS_SET_CTX(obj->ts);

	obj->ov.handle = zend_objects_store_put(obj, NULL, php_ircclient_session_object_free, NULL TSRMLS_CC);
	obj->ov.handlers = zend_get_std_object_handlers();

	return obj->ov;
}

static php_ircclient_session_callback_t *php_ircclient_session_get_callback(php_ircclient_session_object_t *obj, const char *fn_str, size_t fn_len)
{
	zval *zo, *zm;
	php_ircclient_session_callback_t cb, *cbp = NULL;
	TSRMLS_FETCH_FROM_CTX(obj->ts);

	if (SUCCESS == zend_hash_find(&obj->cbc, fn_str, fn_len + 1, (void *) &cbp)) {
		return cbp;
	}

	MAKE_STD_ZVAL(zo);
	Z_TYPE_P(zo) = IS_OBJECT;
	zo->value.obj = obj->ov;
	zend_objects_store_add_ref(zo TSRMLS_CC);

	MAKE_STD_ZVAL(zm);
	ZVAL_STRINGL(zm, estrndup(fn_str, fn_len), fn_len, 0);

	MAKE_STD_ZVAL(cb.zfn);
	array_init_size(cb.zfn, 2);
	add_next_index_zval(cb.zfn, zo);
	add_next_index_zval(cb.zfn, zm);

	if (SUCCESS != zend_fcall_info_init(cb.zfn, IS_CALLABLE_STRICT, &cb.fci, &cb.fcc, NULL, NULL TSRMLS_CC)) {
		zval_ptr_dtor(&cb.zfn);
		return NULL;
	}

	if (SUCCESS != zend_hash_add(&obj->cbc, fn_str, fn_len + 1, &cb, sizeof(cb), (void *) &cbp)) {
		zval_ptr_dtor(&cb.zfn);
		return NULL;
	}

	return cbp;
}

static void php_ircclient_event_callback(irc_session_t *session, const char *event, const char *origin, const char **params, unsigned int count)
{
	char *fn_str;
	int fn_len;
	php_ircclient_session_callback_t *cb;
	php_ircclient_session_object_t *obj = irc_get_ctx(session);
	TSRMLS_FETCH_FROM_CTX(obj->ts);

	fn_str = emalloc(strlen(event) + 2 + 1);
	fn_str[0] = 'o';
	fn_str[1] = 'n';
	fn_len = 2;
	do {
		if (*event != '_') {
			fn_str[fn_len++] = tolower(*event);
		}
	} while (*event++);

	if ((cb = php_ircclient_session_get_callback(obj, fn_str, fn_len -1))) {
		int i;
		zval *zo, *zp;

		MAKE_STD_ZVAL(zo);
		if (origin) {
			ZVAL_STRING(zo, estrdup(origin), 0);
		} else {
			ZVAL_NULL(zo);
		}

		MAKE_STD_ZVAL(zp);
		array_init(zp);
		for (i = 0; i < count; ++i) {
			add_next_index_string(zp, estrdup(params[i]), 0);
		}

		if (SUCCESS == zend_fcall_info_argn(&cb->fci TSRMLS_CC, 2, &zo, &zp)) {
			zend_fcall_info_call(&cb->fci, &cb->fcc, NULL, NULL TSRMLS_CC);
		}

		zval_ptr_dtor(&zo);
		zval_ptr_dtor(&zp);
	}

	efree(fn_str);
}

static void php_ircclient_event_code_callback(irc_session_t *session, unsigned int event, const char *origin, const char **params, unsigned int count)
{
	php_ircclient_session_callback_t *cb;
	php_ircclient_session_object_t *obj = irc_get_ctx(session);
	TSRMLS_FETCH_FROM_CTX(obj->ts);


	if ((cb = php_ircclient_session_get_callback(obj, ZEND_STRL("onNumeric")))) {
		int i;
		zval *zo, *ze, *zp;

		MAKE_STD_ZVAL(zo);
		if (origin) {
			ZVAL_STRING(zo, estrdup(origin), 0);
		} else {
			ZVAL_NULL(zo);
		}

		MAKE_STD_ZVAL(ze);
		ZVAL_LONG(ze, event);

		MAKE_STD_ZVAL(zp);
		array_init(zp);
		for (i = 0; i < count; ++i) {
			add_next_index_string(zp, estrdup(params[i]), 0);
		}

		if (SUCCESS == zend_fcall_info_argn(&cb->fci TSRMLS_CC, 3, &zo, &ze, &zp)) {
			zend_fcall_info_call(&cb->fci, &cb->fcc, NULL, NULL TSRMLS_CC);
		}

		zval_ptr_dtor(&zp);
		zval_ptr_dtor(&ze);
		zval_ptr_dtor(&zo);
	}
}

static void php_ircclient_event_dcc_chat_callback(irc_session_t *session, const char *nick, const char *addr, irc_dcc_t dccid)
{
	php_ircclient_session_callback_t *cb;
	php_ircclient_session_object_t *obj = irc_get_ctx(session);
	TSRMLS_FETCH_FROM_CTX(obj->ts);

	if ((cb = php_ircclient_session_get_callback(obj, ZEND_STRL("onDccChatReq")))) {
		zval *zn, *za, *zd;

		MAKE_STD_ZVAL(zn);
		ZVAL_STRING(zn, estrdup(nick), 0);
		MAKE_STD_ZVAL(za);
		ZVAL_STRING(za, estrdup(addr), 0);
		MAKE_STD_ZVAL(zd);
		ZVAL_LONG(zd, dccid);

		if (SUCCESS == zend_fcall_info_argn(&cb->fci TSRMLS_CC, 3, &zn, &za, &zd)) {
			zend_fcall_info_call(&cb->fci, &cb->fcc, NULL, NULL TSRMLS_CC);
		}

		zval_ptr_dtor(&zd);
		zval_ptr_dtor(&za);
		zval_ptr_dtor(&zn);
	}
}

static void php_ircclient_event_dcc_send_callback(irc_session_t *session, const char *nick, const char *addr, const char *filename, unsigned long size, irc_dcc_t dccid)
{
	php_ircclient_session_callback_t *cb;
	php_ircclient_session_object_t *obj = irc_get_ctx(session);
	TSRMLS_FETCH_FROM_CTX(obj->ts);

	if ((cb = php_ircclient_session_get_callback(obj, ZEND_STRL("onDccChatReq")))) {
		zval *zn, *za, *zf, *zs, *zd;

		MAKE_STD_ZVAL(zn);
		ZVAL_STRING(zn, estrdup(nick), 0);
		MAKE_STD_ZVAL(za);
		ZVAL_STRING(za, estrdup(addr), 0);
		MAKE_STD_ZVAL(zf);
		ZVAL_STRING(zf, estrdup(filename), 0);
		MAKE_STD_ZVAL(zs);
		ZVAL_LONG(zs, size);
		MAKE_STD_ZVAL(zd);
		ZVAL_LONG(zd, dccid);

		if (SUCCESS == zend_fcall_info_argn(&cb->fci TSRMLS_CC, 5, &zn, &za, &zf, &zs, &zd)) {
			zend_fcall_info_call(&cb->fci, &cb->fcc, NULL, NULL TSRMLS_CC);
		}

		zval_ptr_dtor(&zd);
		zval_ptr_dtor(&zs);
		zval_ptr_dtor(&zf);
		zval_ptr_dtor(&za);
		zval_ptr_dtor(&zn);
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_Session___construct, 0, 0, 0)
	ZEND_ARG_INFO(0, nick)
	ZEND_ARG_INFO(0, user)
	ZEND_ARG_INFO(0, real)
ZEND_END_ARG_INFO()
/* {{{ proto void Session::__construct([string nick[, string user[, string real]]]) */
PHP_METHOD(Session, __construct)
{
	char *nick_str = NULL, *user_str = NULL, *real_str = NULL;
	int nick_len = 0, user_len = 0, real_len = 0;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s!s!s!", &nick_str, &nick_len, &user_str, &user_len, &real_str, &real_len)) {
		if (nick_str && nick_len) {
			zend_update_property_stringl(php_ircclient_session_class_entry, getThis(), ZEND_STRL("nick"), nick_str, nick_len TSRMLS_CC);
		}
		if (nick_str && nick_len) {
			zend_update_property_stringl(php_ircclient_session_class_entry, getThis(), ZEND_STRL("nick"), nick_str, nick_len TSRMLS_CC);
		}
		if (real_str && real_len) {
			zend_update_property_stringl(php_ircclient_session_class_entry, getThis(), ZEND_STRL("real"), real_str, real_len TSRMLS_CC);
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doConnect, 0, 0, 2)
	ZEND_ARG_INFO(0, ip6)
	ZEND_ARG_INFO(0, host)
	ZEND_ARG_INFO(0, port)
	ZEND_ARG_INFO(0, password)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doConnect(bool ip6, string host[, int port[, string password]])
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doConnect)
{
	char *server_str, *passwd_str = NULL;
	int server_len, passwd_len = 0;
	long port = 6667;
	zend_bool ip6;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "bs|ls!", &ip6, &server_str, &server_len, &port, &passwd_str, &passwd_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
		char *nick = NULL, *user = NULL, *real = NULL;
		zval *znick, *zuser, *zreal;

		znick = zend_read_property(php_ircclient_session_class_entry, getThis(), ZEND_STRL("nick"), 0 TSRMLS_CC);
		SEPARATE_ARG_IF_REF(znick);
		convert_to_string_ex(&znick);
		if (Z_STRLEN_P(znick)) {
			nick = Z_STRVAL_P(znick);
		}
		zuser = zend_read_property(php_ircclient_session_class_entry, getThis(), ZEND_STRL("user"), 0 TSRMLS_CC);
		SEPARATE_ARG_IF_REF(zuser);
		convert_to_string_ex(&zuser);
		if (Z_STRLEN_P(zuser)) {
			user = Z_STRVAL_P(zuser);
		}
		zreal = zend_read_property(php_ircclient_session_class_entry, getThis(), ZEND_STRL("real"), 0 TSRMLS_CC);
		SEPARATE_ARG_IF_REF(zreal);
		convert_to_string_ex(&zreal);
		if (Z_STRLEN_P(zreal)) {
			real = Z_STRVAL_P(zreal);
		}

		if (ip6) {
			if (0 != irc_connect6(obj->sess, server_str, port, passwd_str, nick, user, real)) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
				RETVAL_FALSE;
			}
		} else if (0 != irc_connect(obj->sess, server_str, port, passwd_str, nick, user, real)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}

		zval_ptr_dtor(&znick);
		zval_ptr_dtor(&zuser);
		zval_ptr_dtor(&zreal);
	}
}
/* }}} */

/* {{{ proto bool Session::isConnected() */
PHP_METHOD(Session, isConnected)
{
	if (SUCCESS == zend_parse_parameters_none()) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		RETURN_BOOL(irc_is_connected(obj->sess));
	}
}
/* }}} */

/* {{{ proto void Session::disconnect() */
PHP_METHOD(Session, disconnect)
{
	if (SUCCESS == zend_parse_parameters_none()) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		irc_disconnect(obj->sess);
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_run, 0, 0, 0)
	ZEND_ARG_INFO(0, read_fd_array_for_select)
	ZEND_ARG_INFO(0, write_fd_array_for_select)
	ZEND_ARG_INFO(0, timeout_seconds)
ZEND_END_ARG_INFO()
/* {{{ proto array Session::run([array read_fds_for_select[, array write_fds_for_select[, double timeout = null]]])
	Returns array(array of readable fds, array of writeable fds) or false on error. */
PHP_METHOD(Session, run)
{
	HashTable *ifds = NULL, *ofds = NULL;
	double to = php_get_inf();
	int connected;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|H!H!d", &ifds, &ofds, &to)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (ifds || ofds) {
			struct timeval t, *tp = NULL;
			fd_set i, o;
			int m = 0;
			zval **zfd, *zr, *zw;

			FD_ZERO(&i);
			FD_ZERO(&o);

			if ((connected = irc_is_connected(obj->sess))) {
				if (0 != irc_add_select_descriptors(obj->sess, &i, &o, &m)) {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "irc_add_select_descriptors: %s", irc_strerror(irc_errno(obj->sess)));
					RETURN_FALSE;
				}
			}
			if (ifds) {
				for (	zend_hash_internal_pointer_reset(ifds);
						SUCCESS == zend_hash_get_current_data(ifds, (void *) &zfd);
						zend_hash_move_forward(ifds)
				) {
					if (Z_TYPE_PP(zfd) == IS_RESOURCE) {
						php_stream *s = NULL;
						int fd = -1;

						php_stream_from_zval_no_verify(s, zfd);

						if (!s || SUCCESS != php_stream_cast(s, PHP_STREAM_AS_FD_FOR_SELECT, (void *) &fd, 1) || fd == -1) {
							php_error_docref(NULL TSRMLS_CC, E_NOTICE, "invalid resource");
						} else {
							PHP_SAFE_FD_SET(fd, &i);
							if (m < fd) {
								m = fd;
							}
						}
					}
				}
			}
			if (ofds) {
				for (	zend_hash_internal_pointer_reset(ofds);
						SUCCESS == zend_hash_get_current_data(ofds, (void *) &zfd);
						zend_hash_move_forward(ofds)
				) {
					if (Z_TYPE_PP(zfd) == IS_RESOURCE) {
						php_stream *s = NULL;
						int fd = -1;

						php_stream_from_zval_no_verify(s, zfd);

						if (!s || SUCCESS != php_stream_cast(s, PHP_STREAM_AS_FD_FOR_SELECT|PHP_STREAM_CAST_INTERNAL, (void *) &fd, 1) || fd == -1) {
							php_error_docref(NULL TSRMLS_CC, E_NOTICE, "invalid resource");
						} else {
							PHP_SAFE_FD_SET(fd, &o);
							if (m < fd) {
								m = fd;
							}
						}
					}
				}
			}

			PHP_SAFE_MAX_FD(m, m);
			array_init(return_value);

			if (to != php_get_inf()) {
				t.tv_sec = (time_t) to;
				t.tv_usec = (suseconds_t) ((to - t.tv_sec) * 1000000.0);
				tp = &t;
			}

			if (0 > select(m + 1, &i, &o, NULL, tp)) {
				if (errno == EINTR) {
					/* interrupt; let userland be able to handle signals etc. */
					return;
				}

				php_error_docref(NULL TSRMLS_CC, E_WARNING, "select() error: %s", strerror(errno));
				RETURN_FALSE;
			}

			if (connected) {
				if (0 != irc_process_select_descriptors(obj->sess, &i, &o)) {
					int err = irc_errno(obj->sess);

					if (err) {
						php_error_docref(NULL TSRMLS_CC, E_WARNING, "irc_process: %s", irc_strerror(err));
						RETURN_FALSE;
					}
				}
			}


			MAKE_STD_ZVAL(zr);
			array_init(zr);
			MAKE_STD_ZVAL(zw);
			array_init(zw);

			if (ifds) {
				for (	zend_hash_internal_pointer_reset(ifds);
						SUCCESS == zend_hash_get_current_data(ifds, (void *) &zfd);
						zend_hash_move_forward(ifds)
				) {
					if (Z_TYPE_PP(zfd) == IS_RESOURCE) {
						php_stream *s = NULL;
						int fd = -1;

						php_stream_from_zval_no_verify(s, zfd);

						if (s && SUCCESS == php_stream_cast(s, PHP_STREAM_AS_FD_FOR_SELECT|PHP_STREAM_CAST_INTERNAL, (void *) &fd, 1) && fd != -1) {
							if (PHP_SAFE_FD_ISSET(fd, &i)) {
								Z_ADDREF_PP(zfd);
								add_next_index_zval(zr, *zfd);
							}
						}
					}
				}
			}
			if (ofds) {
				for (	zend_hash_internal_pointer_reset(ofds);
						SUCCESS == zend_hash_get_current_data(ofds, (void *) &zfd);
						zend_hash_move_forward(ofds)
				) {
					if (Z_TYPE_PP(zfd) == IS_RESOURCE) {
						php_stream *s = NULL;
						int fd = -1;

						php_stream_from_zval_no_verify(s, zfd);

						if (s && SUCCESS == php_stream_cast(s, PHP_STREAM_AS_FD_FOR_SELECT|PHP_STREAM_CAST_INTERNAL, (void *) &fd, 1) && fd != -1) {
							if (PHP_SAFE_FD_ISSET(fd, &o)) {
								Z_ADDREF_PP(zfd);
								add_next_index_zval(zw, *zfd);
							}
						}
					}
				}
			}

			add_next_index_zval(return_value, zr);
			add_next_index_zval(return_value, zw);

			return;

		} else {
			if (0 != irc_run(obj->sess)) {
				int err = irc_errno(obj->sess);

				if (err) {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "irc_run: %s", irc_strerror(err));
					RETURN_FALSE;
				}
			}
		}

		RETURN_TRUE;
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_setOption, 0, 0, 1)
	ZEND_ARG_INFO(0, option)
	ZEND_ARG_INFO(0, enable)
ZEND_END_ARG_INFO()
/* {{{ proto void Session::setOption(int option[, bool enable = true]) */
PHP_METHOD(Session, setOption)
{
	long opt;
	zend_bool onoff = 1;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l|b", &opt, &onoff)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (onoff) {
			obj->opts |= opt;
			irc_option_set(obj->sess, opt);
		} else {
			obj->opts ^= opt;
			irc_option_reset(obj->sess, opt);
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doJoin, 0, 0, 1)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, password)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doJoin(string channel[, string password])
	Returns TRUE when the command was successfully sent. */
PHP_METHOD(Session, doJoin)
{
	char *chan_str, *key_str = NULL;
	int chan_len, key_len = 0;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|s!", &chan_str, &chan_len, &key_str, &key_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_join(obj->sess, chan_str, key_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doPart, 0, 0, 1)
	ZEND_ARG_INFO(0, channel)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doPart(string channel)
	Returns TRUE when the command was successfully sent. */
PHP_METHOD(Session, doPart)
{
	char *chan_str;
	int chan_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &chan_str, &chan_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_part(obj->sess, chan_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doInvite, 0, 0, 2)
	ZEND_ARG_INFO(0, nick)
	ZEND_ARG_INFO(0, channel)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doInvite(string nick, string channel)
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doInvite)
{
	char *chan_str, *nick_str;
	int chan_len, nick_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &nick_str, &nick_len, &chan_str, &chan_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_invite(obj->sess, nick_str, chan_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doNames, 0, 0, 1)
	ZEND_ARG_INFO(0, channel)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doNames(string channel)
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doNames)
{
	char *chan_str;
	int chan_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &chan_str, &chan_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_names(obj->sess, chan_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doList, 0, 0, 1)
	ZEND_ARG_INFO(0, channel)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doList(string channel)
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doList)
{
	char *chan_str;
	int chan_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &chan_str, &chan_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_list(obj->sess, chan_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doTopic, 0, 0, 1)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, topic)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doTopic(string channel[, string topic])
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doTopic)
{
	char *chan_str, *topic_str = NULL;
	int chan_len, topic_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|s!", &chan_str, &chan_len, &topic_str, &topic_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_topic(obj->sess, chan_str, topic_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doChannelMode, 0, 0, 1)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, mode)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doChannelMode(string channel[, string mode])
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doChannelMode)
{
	char *chan_str, *mode_str = NULL;
	int chan_len, mode_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|s!", &chan_str, &chan_len, &mode_str, &mode_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_channel_mode(obj->sess, chan_str, mode_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doKick, 0, 0, 2)
	ZEND_ARG_INFO(0, nick)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, reason)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doKick(string nick, string channel[, string reason])
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doKick)
{
	char *chan_str, *nick_str, *reason_str = NULL;
	int chan_len, nick_len, reason_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|s!", &nick_str, &nick_len, &chan_str, &chan_len, &reason_str, &reason_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_kick(obj->sess, nick_str, chan_str, reason_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doMsg, 0, 0, 2)
	ZEND_ARG_INFO(0, destination)
	ZEND_ARG_INFO(0, message)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doMsg(string destination, string message)
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doMsg)
{
	char *dest_str, *msg_str;
	int dest_len, msg_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &dest_str, &dest_len, &msg_str, &msg_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_msg(obj->sess, dest_str, msg_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doMe, 0, 0, 2)
	ZEND_ARG_INFO(0, destination)
	ZEND_ARG_INFO(0, message)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doMe(string destination, string message)
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doMe)
{
	char *dest_str, *msg_str;
	int dest_len, msg_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &dest_str, &dest_len, &msg_str, &msg_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_me(obj->sess, dest_str, msg_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doNotice, 0, 0, 2)
	ZEND_ARG_INFO(0, destination)
	ZEND_ARG_INFO(0, message)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doNotice(string destination, string message)
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doNotice)
{
	char *dest_str, *msg_str;
	int dest_len, msg_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &dest_str, &dest_len, &msg_str, &msg_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_notice(obj->sess, dest_str, msg_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doQuit, 0, 0, 0)
	ZEND_ARG_INFO(0, reason)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doQuit([string reason])
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doQuit)
{
	char *reason_str = NULL;
	int reason_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s!", &reason_str, &reason_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_quit(obj->sess, reason_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doUserMode, 0, 0, 0)
	ZEND_ARG_INFO(0, mode)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doUserMode([string mode])
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doUserMode)
{
	char *mode_str = NULL;
	int mode_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s!", &mode_str, &mode_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_user_mode(obj->sess, mode_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doNick, 0, 0, 1)
	ZEND_ARG_INFO(0, nick)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doNick(string nick)
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doNick)
{
	char *nick_str;
	int nick_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &nick_str, &nick_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_nick(obj->sess, nick_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doWhois, 0, 0, 0)
	ZEND_ARG_INFO(0, nick)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doWhois([string nick])
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doWhois)
{
	char *nick_str = NULL;
	int nick_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s!", &nick_str, &nick_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_whois(obj->sess, nick_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doCtcpReply, 0, 0, 2)
	ZEND_ARG_INFO(0, nick)
	ZEND_ARG_INFO(0, reply)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doCtcpReply(string nick, string reply)
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doCtcpReply)
{
	char *nick_str, *reply_str;
	int nick_len, reply_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &nick_str, &nick_len, &reply_str, &reply_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_ctcp_reply(obj->sess, nick_str, reply_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doCtcpRequest, 0, 0, 2)
	ZEND_ARG_INFO(0, nick)
	ZEND_ARG_INFO(0, request)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doCtcpRequest(string nick, string request)
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doCtcpRequest)
{
	char *nick_str, *request_str;
	int nick_len, request_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &nick_str, &nick_len, &request_str, &request_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_ctcp_request(obj->sess, nick_str, request_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

ZEND_BEGIN_ARG_INFO_EX(ai_Session_doRaw, 0, 0, 1)
	ZEND_ARG_INFO(0, message)
ZEND_END_ARG_INFO()
/* {{{ proto bool Session::doRaw(string message)
	Returns TRUE when the command was sent successfully. */
PHP_METHOD(Session, doRaw)
{
	char *msg_str;
	int msg_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &msg_str, &msg_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_send_raw(obj->sess, "%.*s", msg_len, msg_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}
/* }}} */

/* {{{ event_callbacks */
ZEND_BEGIN_ARG_INFO_EX(ai_Session_event, 0, 0, 2)
	ZEND_ARG_INFO(0, origin)
	ZEND_ARG_ARRAY_INFO(0, args, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(ai_Session_event_code, 0, 0, 3)
	ZEND_ARG_INFO(0, origin)
	ZEND_ARG_INFO(0, event)
	ZEND_ARG_ARRAY_INFO(0, args, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(ai_Session_event_dcc_chat, 0, 0, 3)
	ZEND_ARG_INFO(0, nick)
	ZEND_ARG_INFO(0, remote_addr)
	ZEND_ARG_INFO(0, dccid)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(ai_Session_event_dcc_send, 0, 0, 5)
	ZEND_ARG_INFO(0, nick)
	ZEND_ARG_INFO(0, remote_addr)
	ZEND_ARG_INFO(0, filename)
	ZEND_ARG_INFO(0, size)
	ZEND_ARG_INFO(0, dccid)
ZEND_END_ARG_INFO()

static void call_closure(INTERNAL_FUNCTION_PARAMETERS, /* stupid non-const API */ char *prop_str, size_t prop_len)
{
	zval **params = ecalloc(ZEND_NUM_ARGS(), sizeof(zval *));
	php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

	if (obj->opts & LIBIRC_OPTION_DEBUG) {
		zval za;

		INIT_PZVAL(&za);
		array_init(&za);

		if (SUCCESS == zend_copy_parameters_array(ZEND_NUM_ARGS(), &za TSRMLS_CC)) {
			php_printf("ircclient: %s - ", prop_str);
			zend_print_flat_zval_r(&za TSRMLS_CC);
			php_printf("\n");
		}
		zval_dtor(&za);
	}

	if (SUCCESS == zend_get_parameters_array(ZEND_NUM_ARGS(), ZEND_NUM_ARGS(), params)) {
		zval *prop = zend_read_property(Z_OBJCE_P(getThis()), getThis(), prop_str, prop_len, 0 TSRMLS_CC);

		if (Z_TYPE_P(prop) != IS_NULL) {
			call_user_function(NULL, NULL, prop, return_value, ZEND_NUM_ARGS(), params TSRMLS_CC);
		}
	}

	efree(params);
}

PHP_METHOD(Session, onConnect) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onConnect")); }
PHP_METHOD(Session, onNick) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onNick")); }
PHP_METHOD(Session, onQuit) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onQuit")); }
PHP_METHOD(Session, onJoin) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onJoin")); }
PHP_METHOD(Session, onPart) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onPart")); }
PHP_METHOD(Session, onMode) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onMode")); }
PHP_METHOD(Session, onUmode) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onUmode")); }
PHP_METHOD(Session, onTopic) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onTopic")); }
PHP_METHOD(Session, onKick) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onKick")); }
PHP_METHOD(Session, onChannel) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onChannel")); }
PHP_METHOD(Session, onPrivmsg) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onPrivmsg")); }
PHP_METHOD(Session, onNotice) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onNotice")); }
PHP_METHOD(Session, onChannelNotice) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onChannelNotice")); }
PHP_METHOD(Session, onInvite) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onInvite")); }
PHP_METHOD(Session, onCtcpReq) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onCtcpReq")); }
PHP_METHOD(Session, onCtcpRep) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onCtcpRep")); }
PHP_METHOD(Session, onAction) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onAction")); }
PHP_METHOD(Session, onUnknown) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onUnknown")); }
PHP_METHOD(Session, onNumeric) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onNumeric")); }
PHP_METHOD(Session, onDccChatReq) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onDccChatReq")); }
PHP_METHOD(Session, onDccSendReq) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onDccSendReq")); }
PHP_METHOD(Session, onError) { call_closure(INTERNAL_FUNCTION_PARAM_PASSTHRU, ZEND_STRL("onError")); }
/* }}} */

#define ME(m, ai) PHP_ME(Session, m, ai, ZEND_ACC_PUBLIC)

zend_function_entry php_ircclient_session_method_entry[] = {
	ME(__construct, ai_Session___construct)
	ME(doConnect, ai_Session_doConnect)
	ME(isConnected, NULL)
	ME(disconnect, NULL)
	ME(run, ai_Session_run)
	ME(setOption, ai_Session_setOption)

	ME(doJoin, ai_Session_doJoin)
	ME(doPart, ai_Session_doPart)
	ME(doInvite, ai_Session_doInvite)
	ME(doNames, ai_Session_doNames)
	ME(doList, ai_Session_doList)
	ME(doTopic, ai_Session_doTopic)
	ME(doChannelMode, ai_Session_doChannelMode)
	ME(doKick, ai_Session_doKick)

	ME(doMsg, ai_Session_doMsg)
	ME(doMe, ai_Session_doMe)
	ME(doNotice, ai_Session_doNotice)

	ME(doQuit, ai_Session_doQuit)
	ME(doUserMode, ai_Session_doUserMode)
	ME(doNick, ai_Session_doNick)
	ME(doWhois, ai_Session_doWhois)

	ME(doCtcpReply, ai_Session_doCtcpReply)
	ME(doCtcpRequest, ai_Session_doCtcpRequest)

	ME(doRaw, ai_Session_doRaw)

	ME(onConnect, ai_Session_event)
	ME(onNick, ai_Session_event)
	ME(onQuit, ai_Session_event)
	ME(onJoin, ai_Session_event)
	ME(onPart, ai_Session_event)
	ME(onMode, ai_Session_event)
	ME(onUmode, ai_Session_event)
	ME(onTopic, ai_Session_event)
	ME(onKick, ai_Session_event)
	ME(onChannel, ai_Session_event)
	ME(onPrivmsg, ai_Session_event)
	ME(onNotice, ai_Session_event)
	ME(onChannelNotice, ai_Session_event)
	ME(onInvite, ai_Session_event)
	ME(onCtcpReq, ai_Session_event)
	ME(onCtcpRep, ai_Session_event)
	ME(onAction, ai_Session_event)
	ME(onUnknown, ai_Session_event)
	ME(onNumeric, ai_Session_event_code)
	ME(onDccChatReq, ai_Session_event_dcc_chat)
	ME(onDccSendReq, ai_Session_event_dcc_send)
	ME(onError, ai_Session_event)
	{0}
};

PHP_MINIT_FUNCTION(ircclient)
{
	zend_class_entry ce;

	memset(&ce, 0, sizeof(zend_class_entry));
	INIT_NS_CLASS_ENTRY(ce, "irc\\client", "Session", php_ircclient_session_method_entry);
	ce.create_object = php_ircclient_session_object_create;
	php_ircclient_session_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);

	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("nick"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("user"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("real"), ZEND_ACC_PUBLIC TSRMLS_CC);

	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onConnect"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onNick"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onQuit"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onJoin"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onPart"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onMode"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onUmode"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onTopic"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onKick"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onChannel"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onPrivmsg"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onNotice"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onChannelNotice"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onInvite"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onCtcpReq"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onCtcpRep"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onAction"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onUnknown"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onNumeric"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onDccChatReq"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onDccSendReq"), ZEND_ACC_PUBLIC TSRMLS_CC);
	zend_declare_property_null(php_ircclient_session_class_entry, ZEND_STRL("onError"), ZEND_ACC_PUBLIC TSRMLS_CC);

	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_WELCOME", 001, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_YOURHOST", 002, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_CREATED", 003, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_MYINFO", 004, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_BOUNCE", 005, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_USERHOST", 302, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ISON", 303, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_AWAY", 301, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_UNAWAY", 305, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_NOWAWAY", 306, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_WHOISUSER", 311, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_WHOISSERVER", 312, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_WHOISOPERATOR", 313, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_WHOISIDLE", 317, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ENDOFWHOIS", 318, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_WHOISCHANNELS", 319, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_WHOWASUSER", 314, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ENDOFWHOWAS", 369, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_LIST", 322, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_LISTEND", 323, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_UNIQOPIS", 325, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_CHANNELMODEIS", 324, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_NOTOPIC", 331, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_TOPIC", 332, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_INVITING", 341, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_SUMMONING", 342, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_INVITELIST", 346, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ENDOFINVITELIST", 347, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_EXCEPTLIST", 348, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ENDOFEXCEPTLIST", 349, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_VERSION", 351, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_WHOREPLY", 352, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ENDOFWHO", 315, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_NAMREPLY", 353, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ENDOFNAMES", 366, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_LINKS", 364, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ENDOFLINKS", 365, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_BANLIST", 367, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ENDOFBANLIST", 368, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_INFO", 371, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ENDOFINFO", 374, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_MOTDSTART", 375, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_MOTD", 372, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ENDOFMOTD", 376, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_YOUREOPER", 381, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_REHASHING", 382, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_YOURESERVICE", 383, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_TIME", 391, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_USERSSTART", 392, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_USERS", 393, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ENDOFUSERS", 394, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_NOUSERS", 395, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_TRACELINK", 200, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_TRACECONNECTING", 201, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_TRACEHANDSHAKE", 202, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_TRACEUNKNOWN", 203, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_TRACEOPERATOR", 204, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_TRACEUSER", 205, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_TRACESERVER", 206, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_TRACESERVICE", 207, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_TRACENEWTYPE", 208, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_TRACECLASS", 209, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_TRACELOG", 261, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_TRACEEND", 262, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_STATSLINKINFO", 211, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_STATSCOMMANDS", 212, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ENDOFSTATS", 219, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_STATSUPTIME", 242, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_STATSOLINE", 243, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_UMODEIS", 221, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_SERVLIST", 234, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_SERVLISTEND", 235, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_LUSERCLIENT", 251, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_LUSEROP", 252, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_LUSERUNKNOWN", 253, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_LUSERCHANNELS", 254, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_LUSERME", 255, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ADMINME", 256, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ADMINLOC1", 257, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ADMINLOC2", 258, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_ADMINEMAIL", 259, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "RPL_TRYAGAIN", 263, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOSUCHNICK", 401, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOSUCHSERVER", 402, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOSUCHCHANNEL", 403, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_CANNOTSENDTOCHAN", 404, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_TOOMANYCHANNELS", 405, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_WASNOSUCHNICK", 406, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_TOOMANYTARGETS", 407, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOSUCHSERVICE", 408, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOORIGIN", 409, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NORECIPIENT", 411, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOTEXTTOSEND", 412, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOTOPLEVEL", 413, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_WILDTOPLEVEL", 414, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_BADMASK", 415, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_UNKNOWNCOMMAND", 421, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOMOTD", 422, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOADMININFO", 423, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_FILEERROR", 424, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NONICKNAMEGIVEN", 431, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_ERRONEUSNICKNAME", 432, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NICKNAMEINUSE", 433, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NICKCOLLISION", 436, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_UNAVAILRESOURCE", 437, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_USERNOTINCHANNEL", 441, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOTONCHANNEL", 442, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_USERONCHANNEL", 443, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOLOGIN", 444, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_SUMMONDISABLED", 445, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_USERSDISABLED", 446, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOTREGISTERED", 451, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NEEDMOREPARAMS", 461, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_ALREADYREGISTRED", 462, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOPERMFORHOST", 463, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_PASSWDMISMATCH", 464, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_YOUREBANNEDCREEP", 465, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_YOUWILLBEBANNED", 466, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_KEYSET", 467, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_CHANNELISFULL", 471, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_UNKNOWNMODE", 472, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_INVITEONLYCHAN", 473, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_BANNEDFROMCHAN", 474, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_BADCHANNELKEY", 475, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_BADCHANMASK", 476, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOCHANMODES", 477, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_BANLISTFULL", 478, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOPRIVILEGES", 481, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_CHANOPRIVSNEEDED", 482, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_CANTKILLSERVER", 483, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_RESTRICTED", 484, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_UNIQOPPRIVSNEEDED", 485, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_NOOPERHOST", 491, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_UMODEUNKNOWNFLAG", 501, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("irc\\client", "ERR_USERSDONTMATCH", 502, CONST_CS|CONST_PERSISTENT);

	return SUCCESS;
}

PHP_MINFO_FUNCTION(ircclient)
{
	unsigned int high, low;
	char *version[2];
	char *lt16 = "<=1.6";

	irc_get_version(&high, &low);
	spprintf(&version[1], 0, "%u.%u", high, low);
#if PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_HIGH
	spprintf(&version[0], 0, "%u.%u", PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_HIGH, PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_LOW);
#else
	/* version <= 1.6 doesn't expose its version */
	version[0] = lt16;
#endif
	php_info_print_table_start();
	php_info_print_table_header(2, "IRC client support", "enabled");
	php_info_print_table_row(2, "Version", PHP_IRCCLIENT_VERSION);
	php_info_print_table_end();

	php_info_print_table_start();
	php_info_print_table_header(3, "Used Library", "compiled", "linked");
	php_info_print_table_row(3,
		"libircclient",
		version[0],
		version[1]
	);
	php_info_print_table_end();

	if (version[0] != lt16) {
		efree(version[0]);
	}
	efree(version[1]);
}





