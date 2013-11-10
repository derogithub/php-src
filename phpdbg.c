/*
   +----------------------------------------------------------------------+
   | PHP Version 5                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2013 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Joe Watkins <joe.watkins@live.co.uk>                        |
   +----------------------------------------------------------------------+
*/

#include "phpdbg.h"
#include "phpdbg_prompt.h"

ZEND_DECLARE_MODULE_GLOBALS(phpdbg);

void (*zend_execute_old)(zend_execute_data *execute_data TSRMLS_DC);
void (*zend_execute_internal_old)(zend_execute_data *execute_data_ptr, zend_fcall_info *fci, int return_value_used TSRMLS_DC);

static inline void php_phpdbg_globals_ctor(zend_phpdbg_globals *pg) /* {{{ */
{
    pg->exec = NULL;
    pg->exec_len = 0;
    pg->ops = NULL;
    pg->stepping = 0;
    pg->vmret = 0;
    pg->quitting = 0;
    pg->bp_count = 0;
    pg->quiet = 0;
    pg->last = NULL;
    pg->last_params = NULL;
    pg->last_params_len = 0;
} /* }}} */

static PHP_MINIT_FUNCTION(phpdbg) /* {{{ */
{
    ZEND_INIT_MODULE_GLOBALS(phpdbg, php_phpdbg_globals_ctor, NULL);

    zend_execute_old = zend_execute_ex;
    zend_execute_ex = phpdbg_execute_ex;

    return SUCCESS;
} /* }}} */

static void php_phpdbg_destroy_break(void *brake) /* {{{ */
{
	zend_llist_destroy((zend_llist*)brake);
} /* }}} */

static PHP_RINIT_FUNCTION(phpdbg) /* {{{ */
{
	zend_hash_init(&PHPDBG_G(bp_files),   8, NULL, php_phpdbg_destroy_break, 0);
	zend_hash_init(&PHPDBG_G(bp_symbols), 8, NULL, php_phpdbg_destroy_break, 0);

	return SUCCESS;
} /* }}} */

static PHP_RSHUTDOWN_FUNCTION(phpdbg) /* {{{ */
{
    zend_hash_destroy(&PHPDBG_G(bp_files));
    zend_hash_destroy(&PHPDBG_G(bp_symbols));

    if (PHPDBG_G(exec)) {
        efree(PHPDBG_G(exec));
    }

    if (PHPDBG_G(ops)) {
        destroy_op_array(PHPDBG_G(ops) TSRMLS_CC);
        efree(PHPDBG_G(ops));
    }
    return SUCCESS;
} /* }}} */

static zend_module_entry sapi_phpdbg_module_entry = {
	STANDARD_MODULE_HEADER,
	"phpdbg",
	NULL,
	PHP_MINIT(phpdbg),
	NULL,
	PHP_RINIT(phpdbg),
	PHP_RSHUTDOWN(phpdbg),
	NULL,
	"0.1",
	STANDARD_MODULE_PROPERTIES
};

static inline int php_sapi_phpdbg_module_startup(sapi_module_struct *module) /* {{{ */
{
    if (php_module_startup(module, &sapi_phpdbg_module_entry, 1) == FAILURE) {
		return FAILURE;
	}
	return SUCCESS;
} /* }}} */

/* {{{ sapi_module_struct phpdbg_sapi_module
 */
static sapi_module_struct phpdbg_sapi_module = {
	"phpdbg",						/* name */
	"phpdbg",					    /* pretty name */

	php_sapi_phpdbg_module_startup,	/* startup */
	php_module_shutdown_wrapper,    /* shutdown */

	NULL,		                    /* activate */
	NULL,		                    /* deactivate */

	NULL,			                /* unbuffered write */
	NULL,				            /* flush */
	NULL,							/* get uid */
	NULL,				            /* getenv */

	php_error,						/* error handler */

	NULL,							/* header handler */
	NULL,			                /* send headers handler */
	NULL,							/* send header handler */

	NULL,				            /* read POST data */
	NULL,			                /* read Cookies */

	NULL,	                        /* register server variables */
	NULL,			                /* Log message */
	NULL,							/* Get request time */
	NULL,							/* Child terminate */
	STANDARD_SAPI_MODULE_PROPERTIES
};
/* }}} */

const opt_struct OPTIONS[] = { /* }}} */
    {'c', 1, "ini path override"},
    {'d', 1, "define ini entry on command line"},
    {'-', 0, NULL}
}; /* }}} */

/* overwriteable ini defaults must be set in phpdbg_ini_defaults() */
#define INI_DEFAULT(name,value)\
	Z_SET_REFCOUNT(tmp, 0);\
	Z_UNSET_ISREF(tmp);	\
	ZVAL_STRINGL(&tmp, zend_strndup(value, sizeof(value)-1), sizeof(value)-1, 0);\
	zend_hash_update(configuration_hash, name, sizeof(name), &tmp, sizeof(zval), NULL);\

void phpdbg_ini_defaults(HashTable *configuration_hash) { /* {{{ */
    zval tmp;
	INI_DEFAULT("report_zend_debug", "0");
	INI_DEFAULT("display_errors", "1");
} /* }}} */

int main(int argc, char **argv) /* {{{ */
{
	sapi_module_struct *phpdbg = &phpdbg_sapi_module;
	char *ini_file = NULL;
	char *ini_entries = NULL;
	int   ini_entries_len = 0;
	char *ini_path_override = NULL;
	char *php_optarg = NULL;
    int php_optind = 0;
    int opt;

#ifdef ZTS
	void ***tsrm_ls;
	tsrm_startup(1, 1, 0, NULL);

	tsrm_ls = ts_resource(0);
#endif

#ifdef PHP_WIN32
	_fmode = _O_BINARY;                 /* sets default for file streams to binary */
	setmode(_fileno(stdin), O_BINARY);  /* make the stdio mode be binary */
	setmode(_fileno(stdout), O_BINARY); /* make the stdio mode be binary */
	setmode(_fileno(stderr), O_BINARY); /* make the stdio mode be binary */
#endif

    while ((opt = php_getopt(argc, argv, OPTIONS, &php_optarg, &php_optind, 1, 1)) != -1) {
        switch (opt) {
            case 'c':
                if (ini_path_override) {
                    free(ini_path_override);
                }
                ini_path_override = strdup(php_optarg);
            break;
            case 'd': {
                int len = strlen(php_optarg);
			    char *val;

			    if ((val = strchr(php_optarg, '='))) {
				    val++;
				    if (!isalnum(*val) && *val != '"' && *val != '\'' && *val != '\0') {
					    ini_entries = realloc(ini_entries, ini_entries_len + len + sizeof("\"\"\n\0"));
					    memcpy(ini_entries + ini_entries_len, php_optarg, (val - php_optarg));
					    ini_entries_len += (val - php_optarg);
					    memcpy(ini_entries + ini_entries_len, "\"", 1);
					    ini_entries_len++;
					    memcpy(ini_entries + ini_entries_len, val, len - (val - php_optarg));
					    ini_entries_len += len - (val - php_optarg);
					    memcpy(ini_entries + ini_entries_len, "\"\n\0", sizeof("\"\n\0"));
					    ini_entries_len += sizeof("\n\0\"") - 2;
				    } else {
					    ini_entries = realloc(ini_entries, ini_entries_len + len + sizeof("\n\0"));
					    memcpy(ini_entries + ini_entries_len, php_optarg, len);
					    memcpy(ini_entries + ini_entries_len + len, "\n\0", sizeof("\n\0"));
					    ini_entries_len += len + sizeof("\n\0") - 2;
				    }
			    } else {
				    ini_entries = realloc(ini_entries, ini_entries_len + len + sizeof("=1\n\0"));
				    memcpy(ini_entries + ini_entries_len, php_optarg, len);
				    memcpy(ini_entries + ini_entries_len + len, "=1\n\0", sizeof("=1\n\0"));
				    ini_entries_len += len + sizeof("=1\n\0") - 2;
			    }
            } break;
        }
    }

    phpdbg->ini_defaults = phpdbg_ini_defaults;
    phpdbg->php_ini_path_override = ini_path_override;
	phpdbg->phpinfo_as_text = 1;
	phpdbg->php_ini_ignore_cwd = 1;

	sapi_startup(phpdbg);

    phpdbg->executable_location = argv[0];
    phpdbg->phpinfo_as_text = 1;
    phpdbg->php_ini_ignore_cwd = 0;
    phpdbg->php_ini_ignore = 0;
    phpdbg->ini_entries = ini_entries;

	if (phpdbg->startup(phpdbg) == SUCCESS) {
		zend_activate(TSRMLS_C);

#ifdef ZEND_SIGNALS
		zend_try {
			zend_signals_activate(TSRMLS_C);
		} zend_end_try();
#endif

		PG(modules_activated) = 0;

		zend_try {
			zend_activate_modules(TSRMLS_C);
		} zend_end_try();

		zend_try {
			do {
			    phpdbg_interactive(argc, argv TSRMLS_CC);
			} while(!PHPDBG_G(quitting));
		} zend_end_try();

		if (ini_file) {
		    pefree(ini_file, 1);
		}

		if (ini_entries) {
		    free(ini_entries);
		}

		if (PG(modules_activated)) {
			zend_try {
				zend_deactivate_modules(TSRMLS_C);
			} zend_end_try();
		}

		zend_deactivate(TSRMLS_C);

		zend_try {
			zend_post_deactivate_modules(TSRMLS_C);
		} zend_end_try();

#ifdef ZEND_SIGNALS
		zend_try {
			zend_signal_deactivate(TSRMLS_C);
		} zend_end_try();
#endif

		php_module_shutdown(TSRMLS_C);

		sapi_shutdown();
	}

#ifdef ZTS
	tsrm_shutdown();
#endif

	return 0;
} /* }}} */
