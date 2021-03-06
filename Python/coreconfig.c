#include "Python.h"
#include "osdefs.h"       /* DELIM */
#include "pycore_coreconfig.h"
#include "pycore_fileutils.h"
#include "pycore_getopt.h"
#include "pycore_pylifecycle.h"
#include "pycore_pymem.h"
#include "pycore_pathconfig.h"
#include <locale.h>       /* setlocale() */
#ifdef HAVE_LANGINFO_H
#  include <langinfo.h>   /* nl_langinfo(CODESET) */
#endif
#if defined(MS_WINDOWS) || defined(__CYGWIN__)
#  include <windows.h>    /* GetACP() */
#  ifdef HAVE_IO_H
#    include <io.h>
#  endif
#  ifdef HAVE_FCNTL_H
#    include <fcntl.h>    /* O_BINARY */
#  endif
#endif


/* --- Command line options --------------------------------------- */

/* Short usage message (with %s for argv0) */
static const char usage_line[] =
"usage: %ls [option] ... [-c cmd | -m mod | file | -] [arg] ...\n";

/* Long usage message, split into parts < 512 bytes */
static const char usage_1[] = "\
Options and arguments (and corresponding environment variables):\n\
-b     : issue warnings about str(bytes_instance), str(bytearray_instance)\n\
         and comparing bytes/bytearray with str. (-bb: issue errors)\n\
-B     : don't write .pyc files on import; also PYTHONDONTWRITEBYTECODE=x\n\
-c cmd : program passed in as string (terminates option list)\n\
-d     : debug output from parser; also PYTHONDEBUG=x\n\
-E     : ignore PYTHON* environment variables (such as PYTHONPATH)\n\
-h     : print this help message and exit (also --help)\n\
";
static const char usage_2[] = "\
-i     : inspect interactively after running script; forces a prompt even\n\
         if stdin does not appear to be a terminal; also PYTHONINSPECT=x\n\
-I     : isolate Python from the user's environment (implies -E and -s)\n\
-m mod : run library module as a script (terminates option list)\n\
-O     : remove assert and __debug__-dependent statements; add .opt-1 before\n\
         .pyc extension; also PYTHONOPTIMIZE=x\n\
-OO    : do -O changes and also discard docstrings; add .opt-2 before\n\
         .pyc extension\n\
-q     : don't print version and copyright messages on interactive startup\n\
-s     : don't add user site directory to sys.path; also PYTHONNOUSERSITE\n\
-S     : don't imply 'import site' on initialization\n\
";
static const char usage_3[] = "\
-u     : force the stdout and stderr streams to be unbuffered;\n\
         this option has no effect on stdin; also PYTHONUNBUFFERED=x\n\
-v     : verbose (trace import statements); also PYTHONVERBOSE=x\n\
         can be supplied multiple times to increase verbosity\n\
-V     : print the Python version number and exit (also --version)\n\
         when given twice, print more information about the build\n\
-W arg : warning control; arg is action:message:category:module:lineno\n\
         also PYTHONWARNINGS=arg\n\
-x     : skip first line of source, allowing use of non-Unix forms of #!cmd\n\
-X opt : set implementation-specific option\n\
--check-hash-based-pycs always|default|never:\n\
    control how Python invalidates hash-based .pyc files\n\
";
static const char usage_4[] = "\
file   : program read from script file\n\
-      : program read from stdin (default; interactive mode if a tty)\n\
arg ...: arguments passed to program in sys.argv[1:]\n\n\
Other environment variables:\n\
PYTHONSTARTUP: file executed on interactive startup (no default)\n\
PYTHONPATH   : '%lc'-separated list of directories prefixed to the\n\
               default module search path.  The result is sys.path.\n\
";
static const char usage_5[] =
"PYTHONHOME   : alternate <prefix> directory (or <prefix>%lc<exec_prefix>).\n"
"               The default module search path uses %s.\n"
"PYTHONCASEOK : ignore case in 'import' statements (Windows).\n"
"PYTHONIOENCODING: Encoding[:errors] used for stdin/stdout/stderr.\n"
"PYTHONFAULTHANDLER: dump the Python traceback on fatal errors.\n";
static const char usage_6[] =
"PYTHONHASHSEED: if this variable is set to 'random', a random value is used\n"
"   to seed the hashes of str, bytes and datetime objects.  It can also be\n"
"   set to an integer in the range [0,4294967295] to get hash values with a\n"
"   predictable seed.\n"
"PYTHONMALLOC: set the Python memory allocators and/or install debug hooks\n"
"   on Python memory allocators. Use PYTHONMALLOC=debug to install debug\n"
"   hooks.\n"
"PYTHONCOERCECLOCALE: if this variable is set to 0, it disables the locale\n"
"   coercion behavior. Use PYTHONCOERCECLOCALE=warn to request display of\n"
"   locale coercion and locale compatibility warnings on stderr.\n"
"PYTHONBREAKPOINT: if this variable is set to 0, it disables the default\n"
"   debugger. It can be set to the callable of your debugger of choice.\n"
"PYTHONDEVMODE: enable the development mode.\n"
"PYTHONPYCACHEPREFIX: root directory for bytecode cache (pyc) files.\n";

#if defined(MS_WINDOWS)
#  define PYTHONHOMEHELP "<prefix>\\python{major}{minor}"
#else
#  define PYTHONHOMEHELP "<prefix>/lib/pythonX.X"
#endif


/* --- Global configuration variables ----------------------------- */

/* UTF-8 mode (PEP 540): if equals to 1, use the UTF-8 encoding, and change
   stdin and stdout error handler to "surrogateescape". It is equal to
   -1 by default: unknown, will be set by Py_Main() */
int Py_UTF8Mode = -1;
int Py_DebugFlag = 0; /* Needed by parser.c */
int Py_VerboseFlag = 0; /* Needed by import.c */
int Py_QuietFlag = 0; /* Needed by sysmodule.c */
int Py_InteractiveFlag = 0; /* Needed by Py_FdIsInteractive() below */
int Py_InspectFlag = 0; /* Needed to determine whether to exit at SystemExit */
int Py_OptimizeFlag = 0; /* Needed by compile.c */
int Py_NoSiteFlag = 0; /* Suppress 'import site' */
int Py_BytesWarningFlag = 0; /* Warn on str(bytes) and str(buffer) */
int Py_FrozenFlag = 0; /* Needed by getpath.c */
int Py_IgnoreEnvironmentFlag = 0; /* e.g. PYTHONPATH, PYTHONHOME */
int Py_DontWriteBytecodeFlag = 0; /* Suppress writing bytecode files (*.pyc) */
int Py_NoUserSiteDirectory = 0; /* for -s and site.py */
int Py_UnbufferedStdioFlag = 0; /* Unbuffered binary std{in,out,err} */
int Py_HashRandomizationFlag = 0; /* for -R and PYTHONHASHSEED */
int Py_IsolatedFlag = 0; /* for -I, isolate from user's env */
#ifdef MS_WINDOWS
int Py_LegacyWindowsFSEncodingFlag = 0; /* Uses mbcs instead of utf-8 */
int Py_LegacyWindowsStdioFlag = 0; /* Uses FileIO instead of WindowsConsoleIO */
#endif


PyObject *
_Py_GetGlobalVariablesAsDict(void)
{
    PyObject *dict, *obj;

    dict = PyDict_New();
    if (dict == NULL) {
        return NULL;
    }

#define SET_ITEM(KEY, EXPR) \
        do { \
            obj = (EXPR); \
            if (obj == NULL) { \
                return NULL; \
            } \
            int res = PyDict_SetItemString(dict, (KEY), obj); \
            Py_DECREF(obj); \
            if (res < 0) { \
                goto fail; \
            } \
        } while (0)
#define SET_ITEM_INT(VAR) \
    SET_ITEM(#VAR, PyLong_FromLong(VAR))
#define FROM_STRING(STR) \
    ((STR != NULL) ? \
        PyUnicode_FromString(STR) \
        : (Py_INCREF(Py_None), Py_None))
#define SET_ITEM_STR(VAR) \
    SET_ITEM(#VAR, FROM_STRING(VAR))

    SET_ITEM_STR(Py_FileSystemDefaultEncoding);
    SET_ITEM_INT(Py_HasFileSystemDefaultEncoding);
    SET_ITEM_STR(Py_FileSystemDefaultEncodeErrors);
    SET_ITEM_INT(_Py_HasFileSystemDefaultEncodeErrors);

    SET_ITEM_INT(Py_UTF8Mode);
    SET_ITEM_INT(Py_DebugFlag);
    SET_ITEM_INT(Py_VerboseFlag);
    SET_ITEM_INT(Py_QuietFlag);
    SET_ITEM_INT(Py_InteractiveFlag);
    SET_ITEM_INT(Py_InspectFlag);

    SET_ITEM_INT(Py_OptimizeFlag);
    SET_ITEM_INT(Py_NoSiteFlag);
    SET_ITEM_INT(Py_BytesWarningFlag);
    SET_ITEM_INT(Py_FrozenFlag);
    SET_ITEM_INT(Py_IgnoreEnvironmentFlag);
    SET_ITEM_INT(Py_DontWriteBytecodeFlag);
    SET_ITEM_INT(Py_NoUserSiteDirectory);
    SET_ITEM_INT(Py_UnbufferedStdioFlag);
    SET_ITEM_INT(Py_HashRandomizationFlag);
    SET_ITEM_INT(Py_IsolatedFlag);

#ifdef MS_WINDOWS
    SET_ITEM_INT(Py_LegacyWindowsFSEncodingFlag);
    SET_ITEM_INT(Py_LegacyWindowsStdioFlag);
#endif

    return dict;

fail:
    Py_DECREF(dict);
    return NULL;

#undef FROM_STRING
#undef SET_ITEM
#undef SET_ITEM_INT
#undef SET_ITEM_STR
}


/* --- _Py_wstrlist ----------------------------------------------- */

void
_Py_wstrlist_clear(int len, wchar_t **list)
{
    for (int i=0; i < len; i++) {
        PyMem_RawFree(list[i]);
    }
    PyMem_RawFree(list);
}


wchar_t**
_Py_wstrlist_copy(int len, wchar_t * const *list)
{
    assert((len > 0 && list != NULL) || len == 0);
    size_t size = len * sizeof(list[0]);
    wchar_t **list_copy = PyMem_RawMalloc(size);
    if (list_copy == NULL) {
        return NULL;
    }
    for (int i=0; i < len; i++) {
        wchar_t* arg = _PyMem_RawWcsdup(list[i]);
        if (arg == NULL) {
            _Py_wstrlist_clear(i, list_copy);
            return NULL;
        }
        list_copy[i] = arg;
    }
    return list_copy;
}


_PyInitError
_Py_wstrlist_append(int *len, wchar_t ***list, const wchar_t *str)
{
    if (*len == INT_MAX) {
        /* len+1 would overflow */
        return _Py_INIT_NO_MEMORY();
    }
    wchar_t *str2 = _PyMem_RawWcsdup(str);
    if (str2 == NULL) {
        return _Py_INIT_NO_MEMORY();
    }

    size_t size = (*len + 1) * sizeof(list[0]);
    wchar_t **list2 = (wchar_t **)PyMem_RawRealloc(*list, size);
    if (list2 == NULL) {
        PyMem_RawFree(str2);
        return _Py_INIT_NO_MEMORY();
    }
    list2[*len] = str2;
    *list = list2;
    (*len)++;
    return _Py_INIT_OK();
}


PyObject*
_Py_wstrlist_as_pylist(int len, wchar_t **list)
{
    assert(list != NULL || len < 1);

    PyObject *pylist = PyList_New(len);
    if (pylist == NULL) {
        return NULL;
    }

    for (int i = 0; i < len; i++) {
        PyObject *v = PyUnicode_FromWideChar(list[i], -1);
        if (v == NULL) {
            Py_DECREF(pylist);
            return NULL;
        }
        PyList_SET_ITEM(pylist, i, v);
    }
    return pylist;
}


/* --- Py_SetStandardStreamEncoding() ----------------------------- */

/* Helper to allow an embedding application to override the normal
 * mechanism that attempts to figure out an appropriate IO encoding
 */

static char *_Py_StandardStreamEncoding = NULL;
static char *_Py_StandardStreamErrors = NULL;

int
Py_SetStandardStreamEncoding(const char *encoding, const char *errors)
{
    if (Py_IsInitialized()) {
        /* This is too late to have any effect */
        return -1;
    }

    int res = 0;

    /* Py_SetStandardStreamEncoding() can be called before Py_Initialize(),
       but Py_Initialize() can change the allocator. Use a known allocator
       to be able to release the memory later. */
    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    /* Can't call PyErr_NoMemory() on errors, as Python hasn't been
     * initialised yet.
     *
     * However, the raw memory allocators are initialised appropriately
     * as C static variables, so _PyMem_RawStrdup is OK even though
     * Py_Initialize hasn't been called yet.
     */
    if (encoding) {
        _Py_StandardStreamEncoding = _PyMem_RawStrdup(encoding);
        if (!_Py_StandardStreamEncoding) {
            res = -2;
            goto done;
        }
    }
    if (errors) {
        _Py_StandardStreamErrors = _PyMem_RawStrdup(errors);
        if (!_Py_StandardStreamErrors) {
            if (_Py_StandardStreamEncoding) {
                PyMem_RawFree(_Py_StandardStreamEncoding);
            }
            res = -3;
            goto done;
        }
    }
#ifdef MS_WINDOWS
    if (_Py_StandardStreamEncoding) {
        /* Overriding the stream encoding implies legacy streams */
        Py_LegacyWindowsStdioFlag = 1;
    }
#endif

done:
    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    return res;
}


void
_Py_ClearStandardStreamEncoding(void)
{
    /* Use the same allocator than Py_SetStandardStreamEncoding() */
    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    /* We won't need them anymore. */
    if (_Py_StandardStreamEncoding) {
        PyMem_RawFree(_Py_StandardStreamEncoding);
        _Py_StandardStreamEncoding = NULL;
    }
    if (_Py_StandardStreamErrors) {
        PyMem_RawFree(_Py_StandardStreamErrors);
        _Py_StandardStreamErrors = NULL;
    }

    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);
}


/* --- Py_GetArgcArgv() ------------------------------------------- */

/* For Py_GetArgcArgv(); set by _Py_SetArgcArgv() */
static int orig_argc = 0;
static wchar_t **orig_argv = NULL;


void
_Py_ClearArgcArgv(void)
{
    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    _Py_wstrlist_clear(orig_argc, orig_argv);
    orig_argc = 0;
    orig_argv = NULL;

    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);
}


static int
_Py_SetArgcArgv(int argc, wchar_t * const *argv)
{
    int res;

    PyMemAllocatorEx old_alloc;
    _PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, &old_alloc);

    wchar_t **argv_copy = _Py_wstrlist_copy(argc, argv);
    if (argv_copy != NULL) {
        _Py_ClearArgcArgv();
        orig_argc = argc;
        orig_argv = argv_copy;
        res = 0;
    }
    else {
        res = -1;
    }

    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &old_alloc);
    return res;
}


/* Make the *original* argc/argv available to other modules.
   This is rare, but it is needed by the secureware extension. */
void
Py_GetArgcArgv(int *argc, wchar_t ***argv)
{
    *argc = orig_argc;
    *argv = orig_argv;
}


/* --- _PyCoreConfig ---------------------------------------------- */

#define DECODE_LOCALE_ERR(NAME, LEN) \
    (((LEN) == -2) \
     ? _Py_INIT_USER_ERR("cannot decode " NAME) \
     : _Py_INIT_NO_MEMORY())

/* Free memory allocated in config, but don't clear all attributes */
void
_PyCoreConfig_Clear(_PyCoreConfig *config)
{
    _PyPreConfig_Clear(&config->preconfig);

#define CLEAR(ATTR) \
    do { \
        PyMem_RawFree(ATTR); \
        ATTR = NULL; \
    } while (0)
#define CLEAR_WSTRLIST(LEN, LIST) \
    do { \
        _Py_wstrlist_clear(LEN, LIST); \
        LEN = 0; \
        LIST = NULL; \
    } while (0)

    CLEAR(config->pycache_prefix);
    CLEAR(config->module_search_path_env);
    CLEAR(config->home);
    CLEAR(config->program_name);
    CLEAR(config->program);

    CLEAR_WSTRLIST(config->argc, config->argv);
    config->argc = -1;

    CLEAR_WSTRLIST(config->nwarnoption, config->warnoptions);
    CLEAR_WSTRLIST(config->nxoption, config->xoptions);
    CLEAR_WSTRLIST(config->nmodule_search_path, config->module_search_paths);
    config->nmodule_search_path = -1;

    CLEAR(config->executable);
    CLEAR(config->prefix);
    CLEAR(config->base_prefix);
    CLEAR(config->exec_prefix);
#ifdef MS_WINDOWS
    CLEAR(config->dll_path);
#endif
    CLEAR(config->base_exec_prefix);

    CLEAR(config->filesystem_encoding);
    CLEAR(config->filesystem_errors);
    CLEAR(config->stdio_encoding);
    CLEAR(config->stdio_errors);
    CLEAR(config->run_command);
    CLEAR(config->run_module);
    CLEAR(config->run_filename);
#undef CLEAR
#undef CLEAR_WSTRLIST
}


int
_PyCoreConfig_Copy(_PyCoreConfig *config, const _PyCoreConfig *config2)
{
    _PyCoreConfig_Clear(config);

    if (_PyPreConfig_Copy(&config->preconfig, &config2->preconfig) < 0) {
        return -1;
    }

#define COPY_ATTR(ATTR) config->ATTR = config2->ATTR
#define COPY_STR_ATTR(ATTR) \
    do { \
        if (config2->ATTR != NULL) { \
            config->ATTR = _PyMem_RawStrdup(config2->ATTR); \
            if (config->ATTR == NULL) { \
                return -1; \
            } \
        } \
    } while (0)
#define COPY_WSTR_ATTR(ATTR) \
    do { \
        if (config2->ATTR != NULL) { \
            config->ATTR = _PyMem_RawWcsdup(config2->ATTR); \
            if (config->ATTR == NULL) { \
                return -1; \
            } \
        } \
    } while (0)
#define COPY_WSTRLIST(LEN, LIST) \
    do { \
        if (config2->LIST != NULL) { \
            config->LIST = _Py_wstrlist_copy(config2->LEN, config2->LIST); \
            if (config->LIST == NULL) { \
                return -1; \
            } \
        } \
        config->LEN = config2->LEN; \
    } while (0)

    COPY_ATTR(install_signal_handlers);
    COPY_ATTR(use_hash_seed);
    COPY_ATTR(hash_seed);
    COPY_ATTR(_install_importlib);
    COPY_ATTR(faulthandler);
    COPY_ATTR(tracemalloc);
    COPY_ATTR(import_time);
    COPY_ATTR(show_ref_count);
    COPY_ATTR(show_alloc_count);
    COPY_ATTR(dump_refs);
    COPY_ATTR(malloc_stats);

    COPY_WSTR_ATTR(pycache_prefix);
    COPY_WSTR_ATTR(module_search_path_env);
    COPY_WSTR_ATTR(home);
    COPY_WSTR_ATTR(program_name);
    COPY_WSTR_ATTR(program);

    COPY_WSTRLIST(argc, argv);
    COPY_WSTRLIST(nwarnoption, warnoptions);
    COPY_WSTRLIST(nxoption, xoptions);
    COPY_WSTRLIST(nmodule_search_path, module_search_paths);

    COPY_WSTR_ATTR(executable);
    COPY_WSTR_ATTR(prefix);
    COPY_WSTR_ATTR(base_prefix);
    COPY_WSTR_ATTR(exec_prefix);
#ifdef MS_WINDOWS
    COPY_WSTR_ATTR(dll_path);
#endif
    COPY_WSTR_ATTR(base_exec_prefix);

    COPY_ATTR(site_import);
    COPY_ATTR(bytes_warning);
    COPY_ATTR(inspect);
    COPY_ATTR(interactive);
    COPY_ATTR(optimization_level);
    COPY_ATTR(parser_debug);
    COPY_ATTR(write_bytecode);
    COPY_ATTR(verbose);
    COPY_ATTR(quiet);
    COPY_ATTR(user_site_directory);
    COPY_ATTR(buffered_stdio);
    COPY_STR_ATTR(filesystem_encoding);
    COPY_STR_ATTR(filesystem_errors);
    COPY_STR_ATTR(stdio_encoding);
    COPY_STR_ATTR(stdio_errors);
#ifdef MS_WINDOWS
    COPY_ATTR(legacy_windows_stdio);
#endif
    COPY_ATTR(skip_source_first_line);
    COPY_WSTR_ATTR(run_command);
    COPY_WSTR_ATTR(run_module);
    COPY_WSTR_ATTR(run_filename);
    COPY_ATTR(_check_hash_pycs_mode);
    COPY_ATTR(_frozen);

#undef COPY_ATTR
#undef COPY_STR_ATTR
#undef COPY_WSTR_ATTR
#undef COPY_WSTRLIST
    return 0;
}


const char*
_PyCoreConfig_GetEnv(const _PyCoreConfig *config, const char *name)
{
    return _PyPreConfig_GetEnv(&config->preconfig, name);
}


int
_PyCoreConfig_GetEnvDup(const _PyCoreConfig *config,
                        wchar_t **dest,
                        wchar_t *wname, char *name)
{
    assert(config->preconfig.use_environment >= 0);

    if (!config->preconfig.use_environment) {
        *dest = NULL;
        return 0;
    }

#ifdef MS_WINDOWS
    const wchar_t *var = _wgetenv(wname);
    if (!var || var[0] == '\0') {
        *dest = NULL;
        return 0;
    }

    wchar_t *copy = _PyMem_RawWcsdup(var);
    if (copy == NULL) {
        return -1;
    }

    *dest = copy;
#else
    const char *var = getenv(name);
    if (!var || var[0] == '\0') {
        *dest = NULL;
        return 0;
    }

    size_t len;
    wchar_t *wvar = Py_DecodeLocale(var, &len);
    if (!wvar) {
        if (len == (size_t)-2) {
            return -2;
        }
        else {
            return -1;
        }
    }
    *dest = wvar;
#endif
    return 0;
}


void
_PyCoreConfig_GetGlobalConfig(_PyCoreConfig *config)
{
    _PyPreConfig_GetGlobalConfig(&config->preconfig);

#define COPY_FLAG(ATTR, VALUE) \
        if (config->ATTR == -1) { \
            config->ATTR = VALUE; \
        }
#define COPY_NOT_FLAG(ATTR, VALUE) \
        if (config->ATTR == -1) { \
            config->ATTR = !(VALUE); \
        }

    COPY_FLAG(bytes_warning, Py_BytesWarningFlag);
    COPY_FLAG(inspect, Py_InspectFlag);
    COPY_FLAG(interactive, Py_InteractiveFlag);
    COPY_FLAG(optimization_level, Py_OptimizeFlag);
    COPY_FLAG(parser_debug, Py_DebugFlag);
    COPY_FLAG(verbose, Py_VerboseFlag);
    COPY_FLAG(quiet, Py_QuietFlag);
#ifdef MS_WINDOWS
    COPY_FLAG(legacy_windows_stdio, Py_LegacyWindowsStdioFlag);
#endif
    COPY_FLAG(_frozen, Py_FrozenFlag);

    COPY_NOT_FLAG(buffered_stdio, Py_UnbufferedStdioFlag);
    COPY_NOT_FLAG(site_import, Py_NoSiteFlag);
    COPY_NOT_FLAG(write_bytecode, Py_DontWriteBytecodeFlag);
    COPY_NOT_FLAG(user_site_directory, Py_NoUserSiteDirectory);

#undef COPY_FLAG
#undef COPY_NOT_FLAG
}


/* Set Py_xxx global configuration variables from 'config' configuration. */
void
_PyCoreConfig_SetGlobalConfig(const _PyCoreConfig *config)
{
#define COPY_FLAG(ATTR, VAR) \
        if (config->ATTR != -1) { \
            VAR = config->ATTR; \
        }
#define COPY_NOT_FLAG(ATTR, VAR) \
        if (config->ATTR != -1) { \
            VAR = !config->ATTR; \
        }

    COPY_FLAG(bytes_warning, Py_BytesWarningFlag);
    COPY_FLAG(inspect, Py_InspectFlag);
    COPY_FLAG(interactive, Py_InteractiveFlag);
    COPY_FLAG(optimization_level, Py_OptimizeFlag);
    COPY_FLAG(parser_debug, Py_DebugFlag);
    COPY_FLAG(verbose, Py_VerboseFlag);
    COPY_FLAG(quiet, Py_QuietFlag);
#ifdef MS_WINDOWS
    COPY_FLAG(legacy_windows_stdio, Py_LegacyWindowsStdioFlag);
#endif
    COPY_FLAG(_frozen, Py_FrozenFlag);

    COPY_NOT_FLAG(buffered_stdio, Py_UnbufferedStdioFlag);
    COPY_NOT_FLAG(site_import, Py_NoSiteFlag);
    COPY_NOT_FLAG(write_bytecode, Py_DontWriteBytecodeFlag);
    COPY_NOT_FLAG(user_site_directory, Py_NoUserSiteDirectory);

    /* Random or non-zero hash seed */
    Py_HashRandomizationFlag = (config->use_hash_seed == 0 ||
                                config->hash_seed != 0);

#undef COPY_FLAG
#undef COPY_NOT_FLAG
}


/* Get the program name: use PYTHONEXECUTABLE and __PYVENV_LAUNCHER__
   environment variables on macOS if available. */
static _PyInitError
config_init_program_name(_PyCoreConfig *config)
{
    assert(config->program_name == NULL);

    /* If Py_SetProgramName() was called, use its value */
    const wchar_t *program_name = _Py_path_config.program_name;
    if (program_name != NULL) {
        config->program_name = _PyMem_RawWcsdup(program_name);
        if (config->program_name == NULL) {
            return _Py_INIT_NO_MEMORY();
        }
        return _Py_INIT_OK();
    }

#ifdef __APPLE__
    /* On MacOS X, when the Python interpreter is embedded in an
       application bundle, it gets executed by a bootstrapping script
       that does os.execve() with an argv[0] that's different from the
       actual Python executable. This is needed to keep the Finder happy,
       or rather, to work around Apple's overly strict requirements of
       the process name. However, we still need a usable sys.executable,
       so the actual executable path is passed in an environment variable.
       See Lib/plat-mac/bundlebuiler.py for details about the bootstrap
       script. */
    const char *p = _PyCoreConfig_GetEnv(config, "PYTHONEXECUTABLE");
    if (p != NULL) {
        size_t len;
        wchar_t* program_name = Py_DecodeLocale(p, &len);
        if (program_name == NULL) {
            return DECODE_LOCALE_ERR("PYTHONEXECUTABLE environment "
                                     "variable", (Py_ssize_t)len);
        }
        config->program_name = program_name;
        return _Py_INIT_OK();
    }
#ifdef WITH_NEXT_FRAMEWORK
    else {
        const char* pyvenv_launcher = getenv("__PYVENV_LAUNCHER__");
        if (pyvenv_launcher && *pyvenv_launcher) {
            /* Used by Mac/Tools/pythonw.c to forward
             * the argv0 of the stub executable
             */
            size_t len;
            wchar_t* program_name = Py_DecodeLocale(pyvenv_launcher, &len);
            if (program_name == NULL) {
                return DECODE_LOCALE_ERR("__PYVENV_LAUNCHER__ environment "
                                         "variable", (Py_ssize_t)len);
            }
            config->program_name = program_name;
            return _Py_INIT_OK();
        }
    }
#endif   /* WITH_NEXT_FRAMEWORK */
#endif   /* __APPLE__ */

    /* Use argv[0] by default, if available */
    if (config->program != NULL) {
        config->program_name = _PyMem_RawWcsdup(config->program);
        if (config->program_name == NULL) {
            return _Py_INIT_NO_MEMORY();
        }
        return _Py_INIT_OK();
    }

    /* Last fall back: hardcoded string */
#ifdef MS_WINDOWS
    const wchar_t *default_program_name = L"python";
#else
    const wchar_t *default_program_name = L"python3";
#endif
    config->program_name = _PyMem_RawWcsdup(default_program_name);
    if (config->program_name == NULL) {
        return _Py_INIT_NO_MEMORY();
    }
    return _Py_INIT_OK();
}

static _PyInitError
config_init_executable(_PyCoreConfig *config)
{
    assert(config->executable == NULL);

    /* If Py_SetProgramFullPath() was called, use its value */
    const wchar_t *program_full_path = _Py_path_config.program_full_path;
    if (program_full_path != NULL) {
        config->executable = _PyMem_RawWcsdup(program_full_path);
        if (config->executable == NULL) {
            return _Py_INIT_NO_MEMORY();
        }
        return _Py_INIT_OK();
    }

    return _Py_INIT_OK();
}


static const wchar_t*
config_get_xoption(const _PyCoreConfig *config, wchar_t *name)
{
    return _Py_get_xoption(config->nxoption, config->xoptions, name);
}


static _PyInitError
config_init_home(_PyCoreConfig *config)
{
    wchar_t *home;

    /* If Py_SetPythonHome() was called, use its value */
    home = _Py_path_config.home;
    if (home) {
        config->home = _PyMem_RawWcsdup(home);
        if (config->home == NULL) {
            return _Py_INIT_NO_MEMORY();
        }
        return _Py_INIT_OK();
    }

    int res = _PyCoreConfig_GetEnvDup(config, &home,
                                      L"PYTHONHOME", "PYTHONHOME");
    if (res < 0) {
        return DECODE_LOCALE_ERR("PYTHONHOME", res);
    }
    config->home = home;
    return _Py_INIT_OK();
}


static _PyInitError
config_init_hash_seed(_PyCoreConfig *config)
{
    const char *seed_text = _PyCoreConfig_GetEnv(config, "PYTHONHASHSEED");

    Py_BUILD_ASSERT(sizeof(_Py_HashSecret_t) == sizeof(_Py_HashSecret.uc));
    /* Convert a text seed to a numeric one */
    if (seed_text && strcmp(seed_text, "random") != 0) {
        const char *endptr = seed_text;
        unsigned long seed;
        errno = 0;
        seed = strtoul(seed_text, (char **)&endptr, 10);
        if (*endptr != '\0'
            || seed > 4294967295UL
            || (errno == ERANGE && seed == ULONG_MAX))
        {
            return _Py_INIT_USER_ERR("PYTHONHASHSEED must be \"random\" "
                                     "or an integer in range [0; 4294967295]");
        }
        /* Use a specific hash */
        config->use_hash_seed = 1;
        config->hash_seed = seed;
    }
    else {
        /* Use a random hash */
        config->use_hash_seed = 0;
        config->hash_seed = 0;
    }
    return _Py_INIT_OK();
}


static int
config_wstr_to_int(const wchar_t *wstr, int *result)
{
    const wchar_t *endptr = wstr;
    errno = 0;
    long value = wcstol(wstr, (wchar_t **)&endptr, 10);
    if (*endptr != '\0' || errno == ERANGE) {
        return -1;
    }
    if (value < INT_MIN || value > INT_MAX) {
        return -1;
    }

    *result = (int)value;
    return 0;
}


static _PyInitError
config_read_env_vars(_PyCoreConfig *config)
{
    _PyPreConfig *preconfig = &config->preconfig;

    /* Get environment variables */
    _Py_get_env_flag(preconfig, &config->parser_debug, "PYTHONDEBUG");
    _Py_get_env_flag(preconfig, &config->verbose, "PYTHONVERBOSE");
    _Py_get_env_flag(preconfig, &config->optimization_level, "PYTHONOPTIMIZE");
    _Py_get_env_flag(preconfig, &config->inspect, "PYTHONINSPECT");

    int dont_write_bytecode = 0;
    _Py_get_env_flag(preconfig, &dont_write_bytecode, "PYTHONDONTWRITEBYTECODE");
    if (dont_write_bytecode) {
        config->write_bytecode = 0;
    }

    int no_user_site_directory = 0;
    _Py_get_env_flag(preconfig, &no_user_site_directory, "PYTHONNOUSERSITE");
    if (no_user_site_directory) {
        config->user_site_directory = 0;
    }

    int unbuffered_stdio = 0;
    _Py_get_env_flag(preconfig, &unbuffered_stdio, "PYTHONUNBUFFERED");
    if (unbuffered_stdio) {
        config->buffered_stdio = 0;
    }

#ifdef MS_WINDOWS
    _Py_get_env_flag(preconfig, &config->legacy_windows_stdio,
                 "PYTHONLEGACYWINDOWSSTDIO");
#endif

    if (_PyCoreConfig_GetEnv(config, "PYTHONDUMPREFS")) {
        config->dump_refs = 1;
    }
    if (_PyCoreConfig_GetEnv(config, "PYTHONMALLOCSTATS")) {
        config->malloc_stats = 1;
    }

    wchar_t *path;
    int res = _PyCoreConfig_GetEnvDup(config, &path,
                                      L"PYTHONPATH", "PYTHONPATH");
    if (res < 0) {
        return DECODE_LOCALE_ERR("PYTHONPATH", res);
    }
    config->module_search_path_env = path;

    if (config->use_hash_seed < 0) {
        _PyInitError err = config_init_hash_seed(config);
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }

    return _Py_INIT_OK();
}


static _PyInitError
config_init_tracemalloc(_PyCoreConfig *config)
{
    int nframe;
    int valid;

    const char *env = _PyCoreConfig_GetEnv(config, "PYTHONTRACEMALLOC");
    if (env) {
        if (!_Py_str_to_int(env, &nframe)) {
            valid = (nframe >= 0);
        }
        else {
            valid = 0;
        }
        if (!valid) {
            return _Py_INIT_USER_ERR("PYTHONTRACEMALLOC: invalid number "
                                     "of frames");
        }
        config->tracemalloc = nframe;
    }

    const wchar_t *xoption = config_get_xoption(config, L"tracemalloc");
    if (xoption) {
        const wchar_t *sep = wcschr(xoption, L'=');
        if (sep) {
            if (!config_wstr_to_int(sep + 1, &nframe)) {
                valid = (nframe >= 0);
            }
            else {
                valid = 0;
            }
            if (!valid) {
                return _Py_INIT_USER_ERR("-X tracemalloc=NFRAME: "
                                         "invalid number of frames");
            }
        }
        else {
            /* -X tracemalloc behaves as -X tracemalloc=1 */
            nframe = 1;
        }
        config->tracemalloc = nframe;
    }
    return _Py_INIT_OK();
}


static _PyInitError
config_init_pycache_prefix(_PyCoreConfig *config)
{
    assert(config->pycache_prefix == NULL);

    const wchar_t *xoption = config_get_xoption(config, L"pycache_prefix");
    if (xoption) {
        const wchar_t *sep = wcschr(xoption, L'=');
        if (sep && wcslen(sep) > 1) {
            config->pycache_prefix = _PyMem_RawWcsdup(sep + 1);
            if (config->pycache_prefix == NULL) {
                return _Py_INIT_NO_MEMORY();
            }
        }
        else {
            // -X pycache_prefix= can cancel the env var
            config->pycache_prefix = NULL;
        }
    }
    else {
        wchar_t *env;
        int res = _PyCoreConfig_GetEnvDup(config, &env,
                                          L"PYTHONPYCACHEPREFIX",
                                          "PYTHONPYCACHEPREFIX");
        if (res < 0) {
            return DECODE_LOCALE_ERR("PYTHONPYCACHEPREFIX", res);
        }

        if (env) {
            config->pycache_prefix = env;
        }
    }
    return _Py_INIT_OK();
}


static _PyInitError
config_read_complex_options(_PyCoreConfig *config)
{
    /* More complex options configured by env var and -X option */
    if (config->faulthandler < 0) {
        if (_PyCoreConfig_GetEnv(config, "PYTHONFAULTHANDLER")
           || config_get_xoption(config, L"faulthandler")) {
            config->faulthandler = 1;
        }
    }
    if (_PyCoreConfig_GetEnv(config, "PYTHONPROFILEIMPORTTIME")
       || config_get_xoption(config, L"importtime")) {
        config->import_time = 1;
    }

    _PyInitError err;
    if (config->tracemalloc < 0) {
        err = config_init_tracemalloc(config);
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }

    if (config->pycache_prefix == NULL) {
        err = config_init_pycache_prefix(config);
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }
    return _Py_INIT_OK();
}


static const char *
get_stdio_errors(const _PyCoreConfig *config)
{
#ifndef MS_WINDOWS
    const char *loc = setlocale(LC_CTYPE, NULL);
    if (loc != NULL) {
        /* surrogateescape is the default in the legacy C and POSIX locales */
        if (strcmp(loc, "C") == 0 || strcmp(loc, "POSIX") == 0) {
            return "surrogateescape";
        }

#ifdef PY_COERCE_C_LOCALE
        /* surrogateescape is the default in locale coercion target locales */
        if (_Py_IsLocaleCoercionTarget(loc)) {
            return "surrogateescape";
        }
#endif
    }

    return "strict";
#else
    /* On Windows, always use surrogateescape by default */
    return "surrogateescape";
#endif
}


static _PyInitError
get_locale_encoding(char **locale_encoding)
{
#ifdef MS_WINDOWS
    char encoding[20];
    PyOS_snprintf(encoding, sizeof(encoding), "cp%d", GetACP());
#elif defined(__ANDROID__) || defined(__VXWORKS__)
    const char *encoding = "UTF-8";
#else
    const char *encoding = nl_langinfo(CODESET);
    if (!encoding || encoding[0] == '\0') {
        return _Py_INIT_USER_ERR("failed to get the locale encoding: "
                                 "nl_langinfo(CODESET) failed");
    }
#endif
    *locale_encoding = _PyMem_RawStrdup(encoding);
    if (*locale_encoding == NULL) {
        return _Py_INIT_NO_MEMORY();
    }
    return _Py_INIT_OK();
}


static _PyInitError
config_init_stdio_encoding(_PyCoreConfig *config)
{
    /* If Py_SetStandardStreamEncoding() have been called, use these
        parameters. */
    if (config->stdio_encoding == NULL && _Py_StandardStreamEncoding != NULL) {
        config->stdio_encoding = _PyMem_RawStrdup(_Py_StandardStreamEncoding);
        if (config->stdio_encoding == NULL) {
            return _Py_INIT_NO_MEMORY();
        }
    }

    if (config->stdio_errors == NULL && _Py_StandardStreamErrors != NULL) {
        config->stdio_errors = _PyMem_RawStrdup(_Py_StandardStreamErrors);
        if (config->stdio_errors == NULL) {
            return _Py_INIT_NO_MEMORY();
        }
    }

    if (config->stdio_encoding != NULL && config->stdio_errors != NULL) {
        return _Py_INIT_OK();
    }

    /* PYTHONIOENCODING environment variable */
    const char *opt = _PyCoreConfig_GetEnv(config, "PYTHONIOENCODING");
    if (opt) {
        char *pythonioencoding = _PyMem_RawStrdup(opt);
        if (pythonioencoding == NULL) {
            return _Py_INIT_NO_MEMORY();
        }

        char *err = strchr(pythonioencoding, ':');
        if (err) {
            *err = '\0';
            err++;
            if (!err[0]) {
                err = NULL;
            }
        }

        /* Does PYTHONIOENCODING contain an encoding? */
        if (pythonioencoding[0]) {
            if (config->stdio_encoding == NULL) {
                config->stdio_encoding = _PyMem_RawStrdup(pythonioencoding);
                if (config->stdio_encoding == NULL) {
                    PyMem_RawFree(pythonioencoding);
                    return _Py_INIT_NO_MEMORY();
                }
            }

            /* If the encoding is set but not the error handler,
               use "strict" error handler by default.
               PYTHONIOENCODING=latin1 behaves as
               PYTHONIOENCODING=latin1:strict. */
            if (!err) {
                err = "strict";
            }
        }

        if (config->stdio_errors == NULL && err != NULL) {
            config->stdio_errors = _PyMem_RawStrdup(err);
            if (config->stdio_errors == NULL) {
                PyMem_RawFree(pythonioencoding);
                return _Py_INIT_NO_MEMORY();
            }
        }

        PyMem_RawFree(pythonioencoding);
    }

    /* UTF-8 Mode uses UTF-8/surrogateescape */
    if (config->preconfig.utf8_mode) {
        if (config->stdio_encoding == NULL) {
            config->stdio_encoding = _PyMem_RawStrdup("utf-8");
            if (config->stdio_encoding == NULL) {
                return _Py_INIT_NO_MEMORY();
            }
        }
        if (config->stdio_errors == NULL) {
            config->stdio_errors = _PyMem_RawStrdup("surrogateescape");
            if (config->stdio_errors == NULL) {
                return _Py_INIT_NO_MEMORY();
            }
        }
    }

    /* Choose the default error handler based on the current locale. */
    if (config->stdio_encoding == NULL) {
        _PyInitError err = get_locale_encoding(&config->stdio_encoding);
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }
    if (config->stdio_errors == NULL) {
        const char *errors = get_stdio_errors(config);
        config->stdio_errors = _PyMem_RawStrdup(errors);
        if (config->stdio_errors == NULL) {
            return _Py_INIT_NO_MEMORY();
        }
    }

    return _Py_INIT_OK();
}


static _PyInitError
config_init_fs_encoding(_PyCoreConfig *config)
{
#ifdef MS_WINDOWS
    if (config->preconfig.legacy_windows_fs_encoding) {
        /* Legacy Windows filesystem encoding: mbcs/replace */
        if (config->filesystem_encoding == NULL) {
            config->filesystem_encoding = _PyMem_RawStrdup("mbcs");
            if (config->filesystem_encoding == NULL) {
                return _Py_INIT_NO_MEMORY();
            }
        }
        if (config->filesystem_errors == NULL) {
            config->filesystem_errors = _PyMem_RawStrdup("replace");
            if (config->filesystem_errors == NULL) {
                return _Py_INIT_NO_MEMORY();
            }
        }
    }

    /* Windows defaults to utf-8/surrogatepass (PEP 529).

       Note: UTF-8 Mode takes the same code path and the Legacy Windows FS
             encoding has the priortiy over UTF-8 Mode. */
    if (config->filesystem_encoding == NULL) {
        config->filesystem_encoding = _PyMem_RawStrdup("utf-8");
        if (config->filesystem_encoding == NULL) {
            return _Py_INIT_NO_MEMORY();
        }
    }

    if (config->filesystem_errors == NULL) {
        config->filesystem_errors = _PyMem_RawStrdup("surrogatepass");
        if (config->filesystem_errors == NULL) {
            return _Py_INIT_NO_MEMORY();
        }
    }
#else
    if (config->filesystem_encoding == NULL) {
        if (config->preconfig.utf8_mode) {
            /* UTF-8 Mode use: utf-8/surrogateescape */
            config->filesystem_encoding = _PyMem_RawStrdup("utf-8");
            /* errors defaults to surrogateescape above */
        }
        else if (_Py_GetForceASCII()) {
            config->filesystem_encoding = _PyMem_RawStrdup("ascii");
        }
        else {
            /* macOS and Android use UTF-8,
               other platforms use the locale encoding. */
#if defined(__APPLE__) || defined(__ANDROID__)
            config->filesystem_encoding = _PyMem_RawStrdup("utf-8");
#else
            _PyInitError err = get_locale_encoding(&config->filesystem_encoding);
            if (_Py_INIT_FAILED(err)) {
                return err;
            }
#endif
        }

        if (config->filesystem_encoding == NULL) {
            return _Py_INIT_NO_MEMORY();
        }
    }

    if (config->filesystem_errors == NULL) {
        /* by default, use the "surrogateescape" error handler */
        config->filesystem_errors = _PyMem_RawStrdup("surrogateescape");
        if (config->filesystem_errors == NULL) {
            return _Py_INIT_NO_MEMORY();
        }
    }
#endif
    return _Py_INIT_OK();
}


static _PyInitError
_PyCoreConfig_ReadPreConfig(_PyCoreConfig *config)
{
    _PyInitError err;
    _PyPreConfig local_preconfig = _PyPreConfig_INIT;
    _PyPreConfig_GetGlobalConfig(&local_preconfig);

    if (_PyPreConfig_Copy(&local_preconfig, &config->preconfig) < 0) {
        err = _Py_INIT_NO_MEMORY();
        goto done;
    }

    err = _PyPreConfig_Read(&local_preconfig);
    if (_Py_INIT_FAILED(err)) {
        goto done;
    }

    if (_PyPreConfig_Copy(&config->preconfig, &local_preconfig) < 0) {
        err = _Py_INIT_NO_MEMORY();
        goto done;
    }
    err = _Py_INIT_OK();

done:
    _PyPreConfig_Clear(&local_preconfig);
    return err;
}


/* Read the configuration into _PyCoreConfig from:

   * Environment variables
   * Py_xxx global configuration variables

   See _PyCoreConfig_ReadFromArgv() to parse also command line arguments. */
_PyInitError
_PyCoreConfig_Read(_PyCoreConfig *config, const _PyPreConfig *preconfig)
{
    _PyInitError err;

    _PyCoreConfig_GetGlobalConfig(config);

    if (preconfig != NULL) {
        if (_PyPreConfig_Copy(&config->preconfig, preconfig) < 0) {
            return _Py_INIT_NO_MEMORY();
        }
    }
    else {
        err = _PyCoreConfig_ReadPreConfig(config);
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }

    assert(config->preconfig.use_environment >= 0);

    if (config->preconfig.isolated > 0) {
        config->user_site_directory = 0;
    }

    if (config->preconfig.use_environment) {
        err = config_read_env_vars(config);
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }

    /* -X options */
    if (config_get_xoption(config, L"showrefcount")) {
        config->show_ref_count = 1;
    }
    if (config_get_xoption(config, L"showalloccount")) {
        config->show_alloc_count = 1;
    }

    err = config_read_complex_options(config);
    if (_Py_INIT_FAILED(err)) {
        return err;
    }

    if (config->home == NULL) {
        err = config_init_home(config);
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }

    if (config->program_name == NULL) {
        err = config_init_program_name(config);
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }

    if (config->executable == NULL) {
        err = config_init_executable(config);
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }

    if (config->_install_importlib) {
        err = _PyCoreConfig_InitPathConfig(config);
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }

    /* default values */
    if (config->preconfig.dev_mode) {
        if (config->faulthandler < 0) {
            config->faulthandler = 1;
        }
    }
    if (config->use_hash_seed < 0) {
        config->use_hash_seed = 0;
        config->hash_seed = 0;
    }
    if (config->faulthandler < 0) {
        config->faulthandler = 0;
    }
    if (config->tracemalloc < 0) {
        config->tracemalloc = 0;
    }
    if (config->argc < 0) {
        config->argc = 0;
    }

    if (config->filesystem_encoding == NULL || config->filesystem_errors == NULL) {
        err = config_init_fs_encoding(config);
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }

    err = config_init_stdio_encoding(config);
    if (_Py_INIT_FAILED(err)) {
        return err;
    }

    assert(config->preconfig.use_environment >= 0);
    assert(config->filesystem_encoding != NULL);
    assert(config->filesystem_errors != NULL);
    assert(config->stdio_encoding != NULL);
    assert(config->stdio_errors != NULL);
    assert(config->_check_hash_pycs_mode != NULL);

    return _Py_INIT_OK();
}


static void
config_init_stdio(const _PyCoreConfig *config)
{
#if defined(MS_WINDOWS) || defined(__CYGWIN__)
    /* don't translate newlines (\r\n <=> \n) */
    _setmode(fileno(stdin), O_BINARY);
    _setmode(fileno(stdout), O_BINARY);
    _setmode(fileno(stderr), O_BINARY);
#endif

    if (!config->buffered_stdio) {
#ifdef HAVE_SETVBUF
        setvbuf(stdin,  (char *)NULL, _IONBF, BUFSIZ);
        setvbuf(stdout, (char *)NULL, _IONBF, BUFSIZ);
        setvbuf(stderr, (char *)NULL, _IONBF, BUFSIZ);
#else /* !HAVE_SETVBUF */
        setbuf(stdin,  (char *)NULL);
        setbuf(stdout, (char *)NULL);
        setbuf(stderr, (char *)NULL);
#endif /* !HAVE_SETVBUF */
    }
    else if (config->interactive) {
#ifdef MS_WINDOWS
        /* Doesn't have to have line-buffered -- use unbuffered */
        /* Any set[v]buf(stdin, ...) screws up Tkinter :-( */
        setvbuf(stdout, (char *)NULL, _IONBF, BUFSIZ);
#else /* !MS_WINDOWS */
#ifdef HAVE_SETVBUF
        setvbuf(stdin,  (char *)NULL, _IOLBF, BUFSIZ);
        setvbuf(stdout, (char *)NULL, _IOLBF, BUFSIZ);
#endif /* HAVE_SETVBUF */
#endif /* !MS_WINDOWS */
        /* Leave stderr alone - it should be unbuffered anyway. */
    }
}


/* Write the configuration:

   - set Py_xxx global configuration variables
   - initialize C standard streams (stdin, stdout, stderr) */
void
_PyCoreConfig_Write(const _PyCoreConfig *config)
{
    _PyCoreConfig_SetGlobalConfig(config);
    config_init_stdio(config);
}


PyObject *
_PyCoreConfig_AsDict(const _PyCoreConfig *config)
{
    PyObject *dict;

    dict = PyDict_New();
    if (dict == NULL) {
        return NULL;
    }

    if (_PyPreConfig_AsDict(&config->preconfig, dict) < 0) {
        Py_DECREF(dict);
        return NULL;
    }

#define SET_ITEM(KEY, EXPR) \
        do { \
            PyObject *obj = (EXPR); \
            if (obj == NULL) { \
                goto fail; \
            } \
            int res = PyDict_SetItemString(dict, (KEY), obj); \
            Py_DECREF(obj); \
            if (res < 0) { \
                goto fail; \
            } \
        } while (0)
#define FROM_STRING(STR) \
    ((STR != NULL) ? \
        PyUnicode_FromString(STR) \
        : (Py_INCREF(Py_None), Py_None))
#define SET_ITEM_INT(ATTR) \
    SET_ITEM(#ATTR, PyLong_FromLong(config->ATTR))
#define SET_ITEM_UINT(ATTR) \
    SET_ITEM(#ATTR, PyLong_FromUnsignedLong(config->ATTR))
#define SET_ITEM_STR(ATTR) \
    SET_ITEM(#ATTR, FROM_STRING(config->ATTR))
#define FROM_WSTRING(STR) \
    ((STR != NULL) ? \
        PyUnicode_FromWideChar(STR, -1) \
        : (Py_INCREF(Py_None), Py_None))
#define SET_ITEM_WSTR(ATTR) \
    SET_ITEM(#ATTR, FROM_WSTRING(config->ATTR))
#define SET_ITEM_WSTRLIST(NOPTION, OPTIONS) \
    SET_ITEM(#OPTIONS, _Py_wstrlist_as_pylist(config->NOPTION, config->OPTIONS))

    SET_ITEM_INT(install_signal_handlers);
    SET_ITEM_INT(use_hash_seed);
    SET_ITEM_UINT(hash_seed);
    SET_ITEM_INT(faulthandler);
    SET_ITEM_INT(tracemalloc);
    SET_ITEM_INT(import_time);
    SET_ITEM_INT(show_ref_count);
    SET_ITEM_INT(show_alloc_count);
    SET_ITEM_INT(dump_refs);
    SET_ITEM_INT(malloc_stats);
    SET_ITEM_STR(filesystem_encoding);
    SET_ITEM_STR(filesystem_errors);
    SET_ITEM_WSTR(pycache_prefix);
    SET_ITEM_WSTR(program_name);
    SET_ITEM_WSTRLIST(argc, argv);
    SET_ITEM_WSTR(program);
    SET_ITEM_WSTRLIST(nxoption, xoptions);
    SET_ITEM_WSTRLIST(nwarnoption, warnoptions);
    SET_ITEM_WSTR(module_search_path_env);
    SET_ITEM_WSTR(home);
    SET_ITEM_WSTRLIST(nmodule_search_path, module_search_paths);
    SET_ITEM_WSTR(executable);
    SET_ITEM_WSTR(prefix);
    SET_ITEM_WSTR(base_prefix);
    SET_ITEM_WSTR(exec_prefix);
    SET_ITEM_WSTR(base_exec_prefix);
#ifdef MS_WINDOWS
    SET_ITEM_WSTR(dll_path);
#endif
    SET_ITEM_INT(site_import);
    SET_ITEM_INT(bytes_warning);
    SET_ITEM_INT(inspect);
    SET_ITEM_INT(interactive);
    SET_ITEM_INT(optimization_level);
    SET_ITEM_INT(parser_debug);
    SET_ITEM_INT(write_bytecode);
    SET_ITEM_INT(verbose);
    SET_ITEM_INT(quiet);
    SET_ITEM_INT(user_site_directory);
    SET_ITEM_INT(buffered_stdio);
    SET_ITEM_STR(stdio_encoding);
    SET_ITEM_STR(stdio_errors);
#ifdef MS_WINDOWS
    SET_ITEM_INT(legacy_windows_stdio);
#endif
    SET_ITEM_INT(skip_source_first_line);
    SET_ITEM_WSTR(run_command);
    SET_ITEM_WSTR(run_module);
    SET_ITEM_WSTR(run_filename);
    SET_ITEM_INT(_install_importlib);
    SET_ITEM_STR(_check_hash_pycs_mode);
    SET_ITEM_INT(_frozen);

    return dict;

fail:
    Py_DECREF(dict);
    return NULL;

#undef FROM_STRING
#undef FROM_WSTRING
#undef SET_ITEM
#undef SET_ITEM_INT
#undef SET_ITEM_UINT
#undef SET_ITEM_STR
#undef SET_ITEM_WSTR
#undef SET_ITEM_WSTRLIST
}


/* --- _PyCmdline ------------------------------------------------- */

typedef struct {
    const _PyArgv *args;
    int argc;
    wchar_t **argv;
    int nwarnoption;             /* Number of -W command line options */
    wchar_t **warnoptions;       /* Command line -W options */
    int nenv_warnoption;         /* Number of PYTHONWARNINGS environment variables */
    wchar_t **env_warnoptions;   /* PYTHONWARNINGS environment variables */
    int print_help;              /* -h, -? options */
    int print_version;           /* -V option */
} _PyCmdline;


static void
cmdline_clear(_PyCmdline *cmdline)
{
    _Py_wstrlist_clear(cmdline->nwarnoption, cmdline->warnoptions);
    cmdline->nwarnoption = 0;
    cmdline->warnoptions = NULL;

    _Py_wstrlist_clear(cmdline->nenv_warnoption, cmdline->env_warnoptions);
    cmdline->nenv_warnoption = 0;
    cmdline->env_warnoptions = NULL;

    if (cmdline->args->use_bytes_argv && cmdline->argv != NULL) {
        _Py_wstrlist_clear(cmdline->args->argc, cmdline->argv);
    }
    cmdline->argv = NULL;
}


/* --- _PyCoreConfig command line parser -------------------------- */

/* Parse the command line arguments */
static _PyInitError
config_parse_cmdline(_PyCoreConfig *config, _PyCmdline *cmdline,
                     int *need_usage)
{
    _PyInitError err;
    _PyOS_ResetGetOpt();
    do {
        int longindex = -1;
        int c = _PyOS_GetOpt(cmdline->args->argc, cmdline->argv, &longindex);
        if (c == EOF) {
            break;
        }

        if (c == 'c') {
            /* -c is the last option; following arguments
               that look like options are left for the
               command to interpret. */
            size_t len = wcslen(_PyOS_optarg) + 1 + 1;
            wchar_t *command = PyMem_RawMalloc(sizeof(wchar_t) * len);
            if (command == NULL) {
                return _Py_INIT_NO_MEMORY();
            }
            memcpy(command, _PyOS_optarg, (len - 2) * sizeof(wchar_t));
            command[len - 2] = '\n';
            command[len - 1] = 0;
            config->run_command = command;
            break;
        }

        if (c == 'm') {
            /* -m is the last option; following arguments
               that look like options are left for the
               module to interpret. */
            config->run_module = _PyMem_RawWcsdup(_PyOS_optarg);
            if (config->run_module == NULL) {
                return _Py_INIT_NO_MEMORY();
            }
            break;
        }

        switch (c) {
        case 0:
            // Handle long option.
            assert(longindex == 0); // Only one long option now.
            if (!wcscmp(_PyOS_optarg, L"always")) {
                config->_check_hash_pycs_mode = "always";
            } else if (!wcscmp(_PyOS_optarg, L"never")) {
                config->_check_hash_pycs_mode = "never";
            } else if (!wcscmp(_PyOS_optarg, L"default")) {
                config->_check_hash_pycs_mode = "default";
            } else {
                fprintf(stderr, "--check-hash-based-pycs must be one of "
                        "'default', 'always', or 'never'\n");
                *need_usage = 1;
                return _Py_INIT_OK();
            }
            break;

        case 'b':
            config->bytes_warning++;
            break;

        case 'd':
            config->parser_debug++;
            break;

        case 'i':
            config->inspect++;
            config->interactive++;
            break;

        case 'E':
        case 'I':
            /* option handled by _PyPreConfig_ReadFromArgv() */
            break;

        /* case 'J': reserved for Jython */

        case 'O':
            config->optimization_level++;
            break;

        case 'B':
            config->write_bytecode = 0;
            break;

        case 's':
            config->user_site_directory = 0;
            break;

        case 'S':
            config->site_import = 0;
            break;

        case 't':
            /* ignored for backwards compatibility */
            break;

        case 'u':
            config->buffered_stdio = 0;
            break;

        case 'v':
            config->verbose++;
            break;

        case 'x':
            config->skip_source_first_line = 1;
            break;

        case 'h':
        case '?':
            cmdline->print_help++;
            break;

        case 'V':
            cmdline->print_version++;
            break;

        case 'W':
            err = _Py_wstrlist_append(&cmdline->nwarnoption,
                                      &cmdline->warnoptions,
                                      _PyOS_optarg);
            if (_Py_INIT_FAILED(err)) {
                return err;
            }
            break;

        case 'X':
            err = _Py_wstrlist_append(&config->nxoption,
                                      &config->xoptions,
                                      _PyOS_optarg);
            if (_Py_INIT_FAILED(err)) {
                return err;
            }
            break;

        case 'q':
            config->quiet++;
            break;

        case 'R':
            config->use_hash_seed = 0;
            break;

        /* This space reserved for other options */

        default:
            /* unknown argument: parsing failed */
            *need_usage = 1;
            return _Py_INIT_OK();
        }
    } while (1);

    if (config->run_command == NULL && config->run_module == NULL
        && _PyOS_optind < cmdline->args->argc
        && wcscmp(cmdline->argv[_PyOS_optind], L"-") != 0)
    {
        config->run_filename = _PyMem_RawWcsdup(cmdline->argv[_PyOS_optind]);
        if (config->run_filename == NULL) {
            return _Py_INIT_NO_MEMORY();
        }
    }

    if (config->run_command != NULL || config->run_module != NULL) {
        /* Backup _PyOS_optind */
        _PyOS_optind--;
    }

    /* -c and -m options are exclusive */
    assert(!(config->run_command != NULL && config->run_module != NULL));

    return _Py_INIT_OK();
}


#ifdef MS_WINDOWS
#  define WCSTOK wcstok_s
#else
#  define WCSTOK wcstok
#endif

/* Get warning options from PYTHONWARNINGS environment variable. */
static _PyInitError
cmdline_init_env_warnoptions(_PyCmdline *cmdline, const _PyCoreConfig *config)
{
    wchar_t *env;
    int res = _PyCoreConfig_GetEnvDup(config, &env,
                                      L"PYTHONWARNINGS", "PYTHONWARNINGS");
    if (res < 0) {
        return DECODE_LOCALE_ERR("PYTHONWARNINGS", res);
    }

    if (env == NULL) {
        return _Py_INIT_OK();
    }


    wchar_t *warning, *context = NULL;
    for (warning = WCSTOK(env, L",", &context);
         warning != NULL;
         warning = WCSTOK(NULL, L",", &context))
    {
        _PyInitError err = _Py_wstrlist_append(&cmdline->nenv_warnoption,
                                               &cmdline->env_warnoptions,
                                               warning);
        if (_Py_INIT_FAILED(err)) {
            PyMem_RawFree(env);
            return err;
        }
    }
    PyMem_RawFree(env);
    return _Py_INIT_OK();
}


static _PyInitError
config_init_program(_PyCoreConfig *config, const _PyCmdline *cmdline)
{
    wchar_t *program;
    if (cmdline->args->argc >= 1 && cmdline->argv != NULL) {
        program = cmdline->argv[0];
    }
    else {
        program = L"";
    }
    config->program = _PyMem_RawWcsdup(program);
    if (config->program == NULL) {
        return _Py_INIT_NO_MEMORY();
    }

    return _Py_INIT_OK();
}


static _PyInitError
config_add_warnings_optlist(_PyCoreConfig *config,
                            int len, wchar_t * const *options)
{
    for (int i = 0; i < len; i++) {
        _PyInitError err = _Py_wstrlist_append(&config->nwarnoption,
                                               &config->warnoptions,
                                               options[i]);
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }
    return _Py_INIT_OK();
}


static _PyInitError
config_init_warnoptions(_PyCoreConfig *config, const _PyCmdline *cmdline)
{
    _PyInitError err;

    assert(config->nwarnoption == 0);

    /* The priority order for warnings configuration is (highest precedence
     * first):
     *
     * - the BytesWarning filter, if needed ('-b', '-bb')
     * - any '-W' command line options; then
     * - the 'PYTHONWARNINGS' environment variable; then
     * - the dev mode filter ('-X dev', 'PYTHONDEVMODE'); then
     * - any implicit filters added by _warnings.c/warnings.py
     *
     * All settings except the last are passed to the warnings module via
     * the `sys.warnoptions` list. Since the warnings module works on the basis
     * of "the most recently added filter will be checked first", we add
     * the lowest precedence entries first so that later entries override them.
     */

    if (config->preconfig.dev_mode) {
        err = _Py_wstrlist_append(&config->nwarnoption,
                                  &config->warnoptions,
                                  L"default");
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }

    err = config_add_warnings_optlist(config,
                                      cmdline->nenv_warnoption,
                                      cmdline->env_warnoptions);
    if (_Py_INIT_FAILED(err)) {
        return err;
    }

    err = config_add_warnings_optlist(config,
                                      cmdline->nwarnoption,
                                      cmdline->warnoptions);
    if (_Py_INIT_FAILED(err)) {
        return err;
    }

    /* If the bytes_warning_flag isn't set, bytesobject.c and bytearrayobject.c
     * don't even try to emit a warning, so we skip setting the filter in that
     * case.
     */
    if (config->bytes_warning) {
        wchar_t *filter;
        if (config->bytes_warning> 1) {
            filter = L"error::BytesWarning";
        }
        else {
            filter = L"default::BytesWarning";
        }
        err = _Py_wstrlist_append(&config->nwarnoption,
                                  &config->warnoptions,
                                  filter);
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }
    return _Py_INIT_OK();
}


static _PyInitError
config_init_argv(_PyCoreConfig *config, const _PyCmdline *cmdline)
{
    /* Copy argv to be able to modify it (to force -c/-m) */
    int argc = cmdline->args->argc - _PyOS_optind;
    wchar_t **argv;

    if (argc <= 0 || cmdline->argv == NULL) {
        /* Ensure at least one (empty) argument is seen */
        static wchar_t *empty_argv[1] = {L""};
        argc = 1;
        argv = _Py_wstrlist_copy(1, empty_argv);
    }
    else {
        argv = _Py_wstrlist_copy(argc, &cmdline->argv[_PyOS_optind]);
    }

    if (argv == NULL) {
        return _Py_INIT_NO_MEMORY();
    }

    wchar_t *arg0 = NULL;
    if (config->run_command != NULL) {
        /* Force sys.argv[0] = '-c' */
        arg0 = L"-c";
    }
    else if (config->run_module != NULL) {
        /* Force sys.argv[0] = '-m'*/
        arg0 = L"-m";
    }
    if (arg0 != NULL) {
        arg0 = _PyMem_RawWcsdup(arg0);
        if (arg0 == NULL) {
            _Py_wstrlist_clear(argc, argv);
            return _Py_INIT_NO_MEMORY();
        }

        assert(argc >= 1);
        PyMem_RawFree(argv[0]);
        argv[0] = arg0;
    }

    config->argc = argc;
    config->argv = argv;
    return _Py_INIT_OK();
}


static void
config_usage(int error, const wchar_t* program)
{
    FILE *f = error ? stderr : stdout;

    fprintf(f, usage_line, program);
    if (error)
        fprintf(f, "Try `python -h' for more information.\n");
    else {
        fputs(usage_1, f);
        fputs(usage_2, f);
        fputs(usage_3, f);
        fprintf(f, usage_4, (wint_t)DELIM);
        fprintf(f, usage_5, (wint_t)DELIM, PYTHONHOMEHELP);
        fputs(usage_6, f);
    }
}


/* Parse command line options and environment variables. */
static _PyInitError
config_from_cmdline(_PyCoreConfig *config, _PyCmdline *cmdline,
                    const _PyPreConfig *preconfig)
{
    int need_usage = 0;
    _PyInitError err;

    err = config_init_program(config, cmdline);
    if (_Py_INIT_FAILED(err)) {
        return err;
    }

    err = config_parse_cmdline(config, cmdline, &need_usage);
    if (_Py_INIT_FAILED(err)) {
        return err;
    }

    if (need_usage) {
        config_usage(1, config->program);
        return _Py_INIT_EXIT(2);
    }

    if (cmdline->print_help) {
        config_usage(0, config->program);
        return _Py_INIT_EXIT(0);
    }

    if (cmdline->print_version) {
        printf("Python %s\n",
               (cmdline->print_version >= 2) ? Py_GetVersion() : PY_VERSION);
        return _Py_INIT_EXIT(0);
    }

    err = config_init_argv(config, cmdline);
    if (_Py_INIT_FAILED(err)) {
        return err;
    }

    err = _PyCoreConfig_Read(config, preconfig);
    if (_Py_INIT_FAILED(err)) {
        return err;
    }

    if (config->preconfig.use_environment) {
        err = cmdline_init_env_warnoptions(cmdline, config);
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }

    err = config_init_warnoptions(config, cmdline);
    if (_Py_INIT_FAILED(err)) {
        return err;
    }

    if (_Py_SetArgcArgv(cmdline->args->argc, cmdline->argv) < 0) {
        return _Py_INIT_NO_MEMORY();
    }
    return _Py_INIT_OK();
}


/* Read the configuration into _PyCoreConfig from:

   * Command line arguments
   * Environment variables
   * Py_xxx global configuration variables */
_PyInitError
_PyCoreConfig_ReadFromArgv(_PyCoreConfig *config, const _PyArgv *args,
                           const _PyPreConfig *preconfig)
{
    _PyInitError err;

    _PyCmdline cmdline;
    memset(&cmdline, 0, sizeof(cmdline));
    cmdline.args = args;

    err = _PyArgv_Decode(cmdline.args, &cmdline.argv);
    if (_Py_INIT_FAILED(err)) {
        goto done;
    }

    err = config_from_cmdline(config, &cmdline, preconfig);
    if (_Py_INIT_FAILED(err)) {
        goto done;
    }
    err = _Py_INIT_OK();

done:
    cmdline_clear(&cmdline);
    return err;
}
