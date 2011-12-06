
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "php_network.h"
#include "ext/standard/php_string.h"
#include "ext/standard/info.h"

#include "zend_interfaces.h"

#include "php_ircclient.h"

#include <errno.h>
#include <ctype.h>
#include <libircclient/libircclient.h>

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

zend_module_entry ircclient_module_entry = {
	STANDARD_MODULE_HEADER,
	"ircclient",
	php_ircclient_function_entry,
	PHP_MINIT(ircclient),
	PHP_MSHUTDOWN(ircclient),
	PHP_RINIT(ircclient),	
	PHP_RSHUTDOWN(ircclient),
	PHP_MINFO(ircclient),
	"0.1.0",
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
	/* .event_channel_notice = php_ircclient_event_callback, */
	.event_invite = php_ircclient_event_callback,
	.event_ctcp_req = php_ircclient_event_callback,
	.event_ctcp_rep = php_ircclient_event_callback,
	.event_ctcp_action = php_ircclient_event_callback,
	.event_unknown = php_ircclient_event_callback,
	.event_numeric = php_ircclient_event_code_callback,
	.event_dcc_chat_req = php_ircclient_event_dcc_chat_callback,
	.event_dcc_send_req = php_ircclient_event_dcc_send_callback
};

typedef struct php_ircclient_session_object {
	zend_object zo;
	zend_object_value ov;
	irc_session_t *sess;
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
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(o);
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
	TSRMLS_SET_CTX(obj->ts);

	obj->ov.handle = zend_objects_store_put(obj, NULL, php_ircclient_session_object_free, NULL TSRMLS_CC);
	obj->ov.handlers = zend_get_std_object_handlers();

	return obj->ov;
}

static void php_ircclient_event_callback(irc_session_t *session, const char *event, const char *origin, const char **params, unsigned int count)
{
	char *fn_str;
	int i, fn_len;
	zval *zo, *zr, *za;
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

	MAKE_STD_ZVAL(zo);
	Z_TYPE_P(zo) = IS_OBJECT;
	zo->value.obj = obj->ov;
	zend_objects_store_add_ref(zo TSRMLS_CC);

	MAKE_STD_ZVAL(zr);
	if (origin) {
		ZVAL_STRING(zr, estrdup(origin), 0);
	} else {
		ZVAL_NULL(zr);
	}

	MAKE_STD_ZVAL(za);
	array_init(za);
	for (i = 0; i < count; ++i) {
		add_next_index_string(za, estrdup(params[i]), 0);
	}

	zend_call_method(&zo, NULL, NULL, fn_str, fn_len - 1, NULL, 2, zr, za TSRMLS_CC);

	zval_ptr_dtor(&za);
	zval_ptr_dtor(&zr);
	zval_ptr_dtor(&zo);

	efree(fn_str);
}

static void php_ircclient_event_code_callback(irc_session_t *session, unsigned int event, const char *origin, const char **params, unsigned int count)
{
	int i;
	zval *zo, *zr, *zp, *za;
	php_ircclient_session_object_t *obj = irc_get_ctx(session);
	TSRMLS_FETCH_FROM_CTX(obj->ts);

	MAKE_STD_ZVAL(zo);
	Z_TYPE_P(zo) = IS_OBJECT;
	zo->value.obj = obj->ov;
	zend_objects_store_add_ref(zo TSRMLS_CC);

	MAKE_STD_ZVAL(zr);
	if (origin) {
		ZVAL_STRING(zr, estrdup(origin), 0);
	} else {
		ZVAL_NULL(zr);
	}

	MAKE_STD_ZVAL(za);
	array_init(za);
	add_assoc_long_ex(za, ZEND_STRS("event"), event);

	MAKE_STD_ZVAL(zp);
	array_init(zp);
	for (i = 0; i < count; ++i) {
		add_next_index_string(zp, estrdup(params[i]), 0);
	}
	add_assoc_zval_ex(za, ZEND_STRS("params"), zp);

	zend_call_method(&zo, NULL, NULL, ZEND_STRL("onnumeric"), NULL, 2, zr, za TSRMLS_CC);

	zval_ptr_dtor(&zp);
	zval_ptr_dtor(&za);
	zval_ptr_dtor(&zr);
	zval_ptr_dtor(&zo);

}

static void php_ircclient_event_dcc_chat_callback(irc_session_t *session, const char *nick, const char *addr, irc_dcc_t dccid)
{
	zval *zo, *zp;
	php_ircclient_session_object_t *obj = irc_get_ctx(session);
	TSRMLS_FETCH_FROM_CTX(obj->ts);

	MAKE_STD_ZVAL(zo);
	Z_TYPE_P(zo) = IS_OBJECT;
	zo->value.obj = obj->ov;
	zend_objects_store_add_ref(zo TSRMLS_CC);

	MAKE_STD_ZVAL(zp);
	array_init(zp);
	add_assoc_string_ex(zp, ZEND_STRS("nick"), estrdup(nick), 0);
	add_assoc_string_ex(zp, ZEND_STRS("remote_addr"), estrdup(addr), 0);
	add_assoc_long_ex(zp, ZEND_STRS("dccid"), dccid);

	zend_call_method(&zo, NULL, NULL, ZEND_STRS("ondccchatreq"), NULL, 1, zp, NULL TSRMLS_CC);

	zval_ptr_dtor(&zp);
	zval_ptr_dtor(&zo);
}

static void php_ircclient_event_dcc_send_callback(irc_session_t *session, const char *nick, const char *addr, const char *filename, unsigned long size, irc_dcc_t dccid)
{
	zval *zo, *zp;
	php_ircclient_session_object_t *obj = irc_get_ctx(session);
	TSRMLS_FETCH_FROM_CTX(obj->ts);

	MAKE_STD_ZVAL(zo);
	Z_TYPE_P(zo) = IS_OBJECT;
	zo->value.obj = obj->ov;
	zend_objects_store_add_ref(zo TSRMLS_CC);

	MAKE_STD_ZVAL(zp);
	array_init(zp);
	add_assoc_string_ex(zp, ZEND_STRS("nick"), estrdup(nick), 0);
	add_assoc_string_ex(zp, ZEND_STRS("remote_addr"), estrdup(addr), 0);
	add_assoc_string_ex(zp, ZEND_STRS("filename"), estrdup(filename), 0);
	add_assoc_long_ex(zp, ZEND_STRS("filesize"), size);
	add_assoc_long_ex(zp, ZEND_STRS("dccid"), dccid);

	zend_call_method(&zo, NULL, NULL, ZEND_STRL("ondccsendreq"), NULL, 1, zp, NULL TSRMLS_CC);

	zval_ptr_dtor(&zp);
	zval_ptr_dtor(&zo);
}

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

PHP_METHOD(Session, doConnect)
{
	char *server_str, *passwd_str = NULL;
	int server_len, passwd_len = 0;
	long port = 6667;
	zend_bool ip6;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "bs|ls!", &ip6, &server_str, &server_len, &port, &passwd_str, &passwd_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
		char *nick = NULL, *user = NULL, *real = NULL;
		zval *znick, *zuser, *zreal, *tmp;

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

PHP_METHOD(Session, isConnected)
{
	if (SUCCESS == zend_parse_parameters_none()) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		RETURN_BOOL(irc_is_connected(obj->sess));
	}
}

PHP_METHOD(Session, disconnect)
{
	if (SUCCESS == zend_parse_parameters_none()) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		irc_disconnect(obj->sess);
	}
}

PHP_METHOD(Session, run)
{
	HashTable *ifds = NULL, *ofds = NULL;
	double to = 0.25;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|H!H!d", &ifds, &ofds, &to)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if ((ifds && zend_hash_num_elements(ifds)) || (ofds && zend_hash_num_elements(ofds))) {
			struct timeval t;
			fd_set i, o;
			int m = 0;
			zval **zfd, *zr, *zw;

			FD_ZERO(&i);
			FD_ZERO(&o);

			if (0 != irc_add_select_descriptors(obj->sess, &i, &o, &m)) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
				RETURN_FALSE;
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

			t.tv_sec = (time_t) to;
			t.tv_usec = (suseconds_t) ((to - t.tv_sec) * 1000000.0);

			if (0 > select(m + 1, &i, &o, NULL, &t) && errno != EINTR) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "select() error: %s", strerror(errno));
				RETURN_FALSE;
			}

			if (0 != irc_process_select_descriptors(obj->sess, &i, &o)) {
				int err = irc_errno(obj->sess);

				if (err) {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(err));
					RETURN_FALSE;
				}
			}

			array_init(return_value);
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
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(err));
					RETURN_FALSE;
				}
			}
		}

		RETURN_TRUE;
	}
}

PHP_METHOD(Session, setOption)
{
	long opt;
	zend_bool onoff = 1;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l|b", &opt, &onoff)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (onoff) {
			irc_option_set(obj->sess, opt);
		} else {
			irc_option_reset(obj->sess, opt);
		}
	}
}

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

PHP_METHOD(Session, doChannelMode)
{
	char *chan_str, *mode_str = NULL;
	int chan_len, mode_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|s!", &chan_str, &chan_len, &mode_str, &mode_len)) {
		php_ircclient_session_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (0 != irc_cmd_topic(obj->sess, chan_str, mode_str)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", irc_strerror(irc_errno(obj->sess)));
			RETVAL_FALSE;
		} else {
			RETVAL_TRUE;
		}
	}
}

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



PHP_METHOD(Session, onConnect) {}
PHP_METHOD(Session, onNick) {}
PHP_METHOD(Session, onQuit) {}
PHP_METHOD(Session, onJoin) {}
PHP_METHOD(Session, onPart) {}
PHP_METHOD(Session, onMode) {}
PHP_METHOD(Session, onUmode) {}
PHP_METHOD(Session, onTopic) {}
PHP_METHOD(Session, onKick) {}
PHP_METHOD(Session, onChannel) {}
PHP_METHOD(Session, onPrivmsg) {}
PHP_METHOD(Session, onNotice) {}
PHP_METHOD(Session, onChannelNotice) {}
PHP_METHOD(Session, onInvite) {}
PHP_METHOD(Session, onCtcpReq) {}
PHP_METHOD(Session, onCtcpRep) {}
PHP_METHOD(Session, onAction) {}
PHP_METHOD(Session, onUnknown) {}
PHP_METHOD(Session, onNumeric) {}
PHP_METHOD(Session, onDccChatReq) {}
PHP_METHOD(Session, onDccSendReq) {}
PHP_METHOD(Session, onError) {}

#define ME(m) PHP_ME(Session, m, NULL, ZEND_ACC_PUBLIC)

zend_function_entry php_ircclient_session_method_entry[] = {
	ME(__construct)
	ME(doConnect)
	ME(isConnected)
	ME(disconnect)
	ME(run)
	ME(setOption)

	ME(doJoin)
	ME(doPart)
	ME(doInvite)
	ME(doNames)
	ME(doList)
	ME(doTopic)
	ME(doChannelMode)
	ME(doKick)

	ME(doMsg)
	ME(doMe)
	ME(doNotice)

	ME(doQuit)
	ME(doUserMode)
	ME(doNick)
	ME(doWhois)

	ME(doCtcpReply)
	ME(doCtcpRequest)

	ME(onConnect)
	ME(onNick)
	ME(onQuit)
	ME(onJoin)
	ME(onPart)
	ME(onMode)
	ME(onUmode)
	ME(onTopic)
	ME(onKick)
	ME(onChannel)
	ME(onPrivmsg)
	ME(onNotice)
	ME(onChannelNotice)
	ME(onInvite)
	ME(onCtcpReq)
	ME(onCtcpRep)
	ME(onAction)
	ME(onUnknown)
	ME(onNumeric)
	ME(onDccChatReq)
	ME(onDccSendReq)
	ME(onError)
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


PHP_MSHUTDOWN_FUNCTION(ircclient)
{
	return SUCCESS;
}



PHP_RINIT_FUNCTION(ircclient)
{
	return SUCCESS;
}



PHP_RSHUTDOWN_FUNCTION(ircclient)
{
	return SUCCESS;
}


PHP_MINFO_FUNCTION(ircclient)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "ircclient support", "enabled");
	php_info_print_table_end();

}





