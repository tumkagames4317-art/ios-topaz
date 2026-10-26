/*
 * xnu-const-string-compat.h -- supply the `const_<T>` typedefs that GNU mig
 * emits but never defines.
 *
 * WHY THIS EXISTS
 *
 * mig qualifies a read-only ("in") pointer or string parameter with the
 * literal prefix `const_` glued to the type name, so that `const` qualifies
 * the pointed-to data rather than the pointer. That is unconditional -- it
 * does not depend on the .defs saying `const`:
 *
 *     utils.c:157  UserVarQualifier(const argument_t *arg)
 *         if (!UserVarConst(arg)) return "";
 *         if (arg->argType->itIndefinite ||
 *             arg->argType->itInName == MACH_MSG_TYPE_STRING_C ||
 *             !strcmp(arg->argType->itUserType, "string_t"))
 *             return "const_";      <- any indefinite/string input
 *         else return "const ";
 *
 * Apple's mig emits the same names and relies on Apple's SDK headers to have
 * already typedef'd them. GNU mig defines `const_string_t` only in its OWN
 * private headers, which are never emitted into generated code:
 *
 *     mig_string.h:34   typedef const char *const_string_t;
 *     types.h:32        typedef const char* const_string_t;
 *
 * Neither open-source XNU nor the generated headers supply the rest, so the
 * generated headers reference types that nothing defines:
 *
 *     ./device/device_server.h:54:2: error: unknown type name 'const_io_name_t'
 *     .../EXPORT_HDRS/osfmk/mach/vfs_nspace.h:26:2: error: unknown type name 'const_vfs_path_t'
 *     .../EXPORT_HDRS/osfmk/gssd/gssd_mach.h:30:2: error: unknown type name 'const_gssd_string'
 *
 * run 40416685987: 13 distinct such names, 240 occurrences.
 *
 * HOW THE TYPES WERE DERIVED -- read this before adding a 14th
 *
 * Every typedef below is derived from the BASE TYPE that mig was actually fed
 * for that name, read out of the captured migcom streams, NOT from the shape
 * of the name. That distinction is not pedantry: `const_` + name suggests
 * `const char *` for all thirteen, and that is wrong for gssd_byte_buffer,
 * whose base is `array [] of uint8_t` and therefore takes `const uint8_t *`.
 * A header built on the name pattern would compile and be wrong.
 *
 *     UNDKey                 c_string[*:128]      -> const char *
 *     UNDLabel               c_string[*:128]      -> const char *
 *     UNDMessage             c_string[*:1024]     -> const char *
 *     UNDPath                c_string[*:1024]     -> const char *
 *     gssd_byte_buffer       array [] of uint8_t  -> const uint8_t *
 *     gssd_string            c_string[*:1024]     -> const char *
 *     string_t               c_string [*:1024]    -> const char *
 *     io_name_t              c_string[*:128]      -> const char *
 *     io_string_t            c_string[*:512]      -> const char *
 *     io_string_inband_t     c_string[*:4096]     -> const char *
 *     nspace_name_t          c_string[8192]       -> const char *
 *     nspace_path_t          c_string[8192]       -> const char *
 *     vfs_path_t             c_string[4096]       -> const char *
 *
 * All thirteen bases are strings except gssd_byte_buffer. A `c_string[N]` is
 * a `char[N]` in the struct and a `char *` as a parameter, so the const_
 * form is `const char *`: that is what makes `const` bind to the data.
 *
 * INCOMPLETENESS IS LOUD, NOT SILENT
 *
 * If a build ever reports an `unknown type name 'const_...'` that is not
 * declared here, this file is incomplete and the log names the missing type.
 * That is deliberate: the failure mode of a compat header must be a visible
 * error, never a silently wrong definition.
 *
 * This file is a plain C preprocessor header with no dependencies. It is
 * force-included (-include) by cc-shim so it is in scope before any kernel
 * header, in both C and C++ translation units.
 */

#ifndef _XNU_CONST_STRING_COMPAT_H_
#define _XNU_CONST_STRING_COMPAT_H_

#include <stdint.h>

/* c_string[N] bases: const binds to the characters, not the pointer. */
typedef const char *const_UNDKey;
typedef const char *const_UNDLabel;
typedef const char *const_UNDMessage;
typedef const char *const_UNDPath;
typedef const char *const_gssd_string;
typedef const char *const_string_t;
typedef const char *const_io_name_t;
typedef const char *const_io_string_t;
typedef const char *const_io_string_inband_t;
typedef const char *const_nspace_name_t;
typedef const char *const_nspace_path_t;
typedef const char *const_vfs_path_t;

/*
 * NOT a string. Base is `array [] of uint8_t`, so the element type is
 * uint8_t and there is no NUL terminator.
 */
typedef const uint8_t *const_gssd_byte_buffer;

#endif /* _XNU_CONST_STRING_COMPAT_H_ */
