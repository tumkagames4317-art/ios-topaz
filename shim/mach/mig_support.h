/*
 * mig_support.h -- the pieces GNU mig's generated code expects from Apple's
 * libsyscall SDK, which is not present when building XNU itself.
 *
 * WHY THIS IS NEEDED
 *
 * GNU mig unconditionally emits
 *
 *     server.c:47   fprintf(file, "#include <mach/mig_support.h>\n");
 *     user.c:91     fprintf(file, "#include <mach/mig_support.h>\n");
 *
 * in every file it generates. Apple's mig does not, and upstream XNU therefore
 * never has to ship or find <mach/mig_support.h>: on a real macOS build that
 * header comes from the SDK's MachSupport, and the kernel build does not use
 * it. Here it is simply absent, so all 38 generated translation units died:
 *
 *     mach/clock_server.c:12:10: fatal error: 'mach/mig_support.h' file not found
 *     gssd/gssd_mach.c:13:10: fatal error: 'mach/mig_support.h' file not found
 *
 * WHAT IT PROVIDES
 *
 * Everything mig_support.h needs to supply already exists inside XNU; the
 * only missing piece is a header that declares it. kern/mig.h (osfmk/mach/
 * mig.h) already declares every runtime entry point the generated code calls:
 *
 *     mig_get_reply_port / mig_put_reply_port / mig_dealloc_reply_port
 *     mig_strncpy / mig_strncpy_zerofill
 *
 * and those have real kernel implementations in osfmk/kern/ipc_mig.c (the
 * kernel-side stubs that return MACH_PORT_NULL) and libsyscall. So this header
 * is a forwarder, not a reimplementation: it includes the existing declaration
 * file rather than duplicating prototypes that could drift from it.
 *
 * The only genuine additions are two compiler-side helpers Apple defines in
 * its SDK headers and XNU does not carry, because Apple's mig never references
 * them when building the kernel:
 *
 *     MIG_EXTERNAL / mig_external  -- linkage marker on generated prototypes
 *     mig_unlikely / mig_likely    -- branch hints wrapping error checks
 *
 * Both are defined only if not already defined, so a real SDK header, if one
 * is ever present, still wins.
 */

#ifndef _MACH_MIG_SUPPORT_H_
#define _MACH_MIG_SUPPORT_H_

#include <mach/mig.h>
#include <mach/mig_errors.h>

/*
 * `mig_external` prefixes the generated server/client prototypes. Apple
 * defines it as `__attribute__((__visibility__("default")))` for kexts and
 * empty for the kernel. The kernel is the only consumer here.
 */
#ifndef mig_external
#define mig_external
#endif

/*
 * Apple's MIG_DEFS / MACH_SDK headers provide these; __builtin_expect is
 * available in every clang and GCC that can build XNU.
 */
#ifndef mig_unlikely
#define mig_unlikely(x) __builtin_expect(!!(x), 0)
#endif

#ifndef mig_likely
#define mig_likely(x) __builtin_expect(!!(x), 1)
#endif

#endif /* _MACH_MIG_SUPPORT_H_ */
