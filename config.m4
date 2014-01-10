PHP_ARG_WITH(ircclient, for ircclient support,
	[  --with-ircclient[=LIBIRCCLIENTDIR]   Include ircclient support])

if test "$PHP_IRCCLIENT" != "no"; then
	AC_PATH_PROG(GREP, grep, no)
	AC_PATH_PROG(SED, sed, no)
	
	AC_MSG_CHECKING([for libircclient.h])
	for d in $PHP_IRCCLIENT /usr /usr/local /opt; do
		if test -f $d/include/libircclient.h; then
			IRCCLIENT_INCDIR=$d/include
			IRCCLIENT_LIBDIR=$d/$PHP_LIBDIR
			AC_MSG_RESULT([found in $d])
			break
		elif test -f $d/include/libircclient/libircclient.h; then
			AC_MSG_RESULT([found in $d])
			IRCCLIENT_INCDIR=$d/include/libircclient
			IRCCLIENT_LIBDIR=$d/$PHP_LIBDIR
			AC_MSG_RESULT([found in $d])
			break
		fi
	done
	if test "x$IRCCLIENT_INCDIR" = "x"; then
		AC_MSG_ERROR([not found])
	fi
	AC_MSG_CHECKING([libircclient version])
	if test -x $GREP && test -x $SED && test -f "$IRCCLIENT_INCDIR/libirc_params.h"; then
		PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_HIGH=`$GREP "define LIBIRC_VERSION_HIGH" $IRCCLIENT_INCDIR/libirc_params.h | $SED -e 's/[[^0-9\x]]//g'`
		PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_LOW=`$GREP "define LIBIRC_VERSION_LOW" $IRCCLIENT_INCDIR/libirc_params.h | $SED -e 's/[[^0-9\x]]//g'`
		AC_MSG_RESULT([$PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_HIGH $PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_LOW])
	else
		PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_HIGH=0
		PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_LOW=0
		AC_MSG_RESULT([unkown])
	fi
	AC_DEFINE_UNQUOTED([PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_HIGH], [$PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_HIGH], [ ])
	AC_DEFINE_UNQUOTED([PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_LOW], [$PHP_IRCCLIENT_LIBIRCCLIENT_VERSION_LOW], [ ])
	
	PHP_ADD_INCLUDE($IRCCLIENT_INCDIR)
	AC_CHECK_MEMBER([irc_callbacks_t.event_channel_notice], [
		AC_DEFINE(HAVE_LIBIRCCLIENT_EVENT_CHANNEL_NOTICE, 1, [ ])
	], [], [
		[#include <libircclient.h>]
	])
	PHP_CHECK_LIBRARY(ircclient, irc_create_session,
	[
		PHP_ADD_LIBRARY_WITH_PATH(ircclient, $IRCCLIENT_LIBDIR, IRCCLIENT_SHARED_LIBADD)
		AC_DEFINE(HAVE_LIBIRCCLIENT,1,[ ])
	],[
		AC_MSG_ERROR([libircclient not found])
	],[
		-L$IRCCLIENT_LIBDIR -lm
	])
	PHP_SUBST([IRCCLIENT_SHARED_LIBADD])
	PHP_NEW_EXTENSION([ircclient], [php_ircclient.c], [$ext_shared])
fi
