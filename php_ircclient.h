/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2011 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author:                                                              |
  +----------------------------------------------------------------------+
*/

/* $Id: header 310447 2011-04-23 21:14:10Z bjori $ */

#ifndef PHP_IRCCLIENT_H
#define PHP_IRCCLIENT_H

extern zend_module_entry ircclient_module_entry;
#define phpext_ircclient_ptr &ircclient_module_entry

#ifdef PHP_WIN32
#	define PHP_IRCCLIENT_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#	define PHP_IRCCLIENT_API __attribute__ ((visibility("default")))
#else
#	define PHP_IRCCLIENT_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

PHP_MINIT_FUNCTION(ircclient);
PHP_MSHUTDOWN_FUNCTION(ircclient);
PHP_RINIT_FUNCTION(ircclient);
PHP_RSHUTDOWN_FUNCTION(ircclient);
PHP_MINFO_FUNCTION(ircclient);




#ifdef ZTS
#define IRCCLIENT_G(v) TSRMG(ircclient_globals_id, zend_ircclient_globals *, v)
#else
#define IRCCLIENT_G(v) (ircclient_globals.v)
#endif

#endif


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
