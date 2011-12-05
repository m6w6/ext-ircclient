PHP_ARG_WITH(ircclient, for ircclient support,
	[  --with-ircclient[=LIBIRCCLIENTDIR]   Include ircclient support])

if test "$PHP_IRCCLIENT" != "no"; then
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
	
	PHP_ADD_INCLUDE($IRCCLIENT_DIR/include)
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
