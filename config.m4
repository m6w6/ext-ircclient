PHP_ARG_WITH(ircclient, for ircclient support,
	[  --with-ircclient[=LIBIRCCLIENTDIR]   Include ircclient support])

if test "$PHP_IRCCLIENT" != "no"; then
	AC_PROG_EGREP
	AC_PROG_SED
	
	AC_MSG_CHECKING([for libircclient/libircclient.h])
	for d in $PHP_IRCCLIENT /usr /usr/local /opt; do
		if test -f $d/include/libircclient/libircclient.h; then
			AC_MSG_RESULT([found in $d])
			IRCCLIENT_DIR=$d
			break
		fi
	done
	if test "x$IRCCLIENT_DIR" = "x"; then
		AC_MSG_ERROR([not found])
	fi
	if test -f "$IRCCLIENT_DIR/include/libircclient/libirc_params.h"; then
		PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_HIGH=`$EGREP "define LIBIRC_VERSION_HIGH" $IRCCLIENT_DIR/include/libircclient/libirc_params.h | $SED -e 's/[[^0-9\x]]//g'`
		PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_LOW=`$EGREP "define LIBIRC_VERSION_LOW" $IRCCLIENT_DIR/include/libircclient/libirc_params.h | $SED -e 's/[[^0-9\x]]//g'`
	else
		PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_HIGH=0
		PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_LOW=0
	fi
	AC_DEFINE_UNQUOTED([PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_HIGH], [$PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_HIGH], [ ])
	AC_DEFINE_UNQUOTED([PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_LOW], [$PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_LOW], [ ])
	
	PHP_ADD_INCLUDE($IRCCLIENT_DIR/include)
	AC_CHECK_MEMBER([irc_callbacks_t.event_channel_notice], [
		AC_DEFINE(HAVE_LIBIRCCLIENT_EVENT_CHANNEL_NOTICE, 1, [ ])
	], [], [
		[#include <libircclient/libircclient.h>]
	])
	PHP_CHECK_LIBRARY(ircclient, irc_create_session,
	[
		PHP_ADD_LIBRARY_WITH_PATH(ircclient, $IRCCLIENT_DIR/lib, IRCCLIENT_SHARED_LIBADD)
		AC_DEFINE(HAVE_LIBIRCCLIENT,1,[ ])
	],[
		AC_MSG_ERROR([libircclient not found])
	],[
		-L$IRCCLIENT_DIR/lib -lm
	])
	PHP_SUBST([IRCCLIENT_SHARED_LIBADD])
	PHP_NEW_EXTENSION([ircclient], [php_ircclient.c], [$ext_shared])
fi
