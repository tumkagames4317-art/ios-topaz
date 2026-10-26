/*
 * TargetConditionals.h -- the platform-detection header Apple's SDK provides
 * and upstream XNU cannot ship, because on a real macOS build it comes from
 * the SDK and the kernel build does not need it.
 *
 * osfmk/kern/debug.h:43 includes it unconditionally:
 *
 *     #include <TargetConditionals.h>
 *
 * so on this runner every translation unit that reaches kern/debug.h died:
 *
 *     debug.h:43:10: fatal error: 'TargetConditionals.h' file not found
 *     sweet_pthread_init.c:4 -> sweet_libpthread/kern/kern_init.c:10 -> debug.h
 *
 * 38 occurrences in 40420909266.
 *
 * The targets below describe what is actually being built here: an XNU
 * kernel, not a user-space program on Darwin. The handful of call sites that
 * consult these macros in the kernel are asking "which Darwin platform am I
 * compiling for", and the answer for this port is a macOS-family kernel
 * (XNU_TARGET_OS_OSX is what the build passes on the command line) being
 * built for a device-class (iPhone-family) target.
 *
 * Only the macros XNU actually references are provided. Defining the complete
 * Apple set would invent claims about platforms this build never targets.
 */

#ifndef _TARGET_CONDITIONALS_H_
#define _TARGET_CONDITIONALS_H_

/* This is a Darwin-family kernel build, not a host-platform build. */
#define TARGET_OS_MAC               1

/*
 * The kernel is compiled the way a macOS XNU build compiles it: the makefiles
 * pass -DXNU_TARGET_OS_OSX, and kalloc.h/bsd headers key off that. Keep the
 * user-space view consistent with it rather than with the Linux host.
 */
#define TARGET_OS_OSX               1
#define TARGET_OS_IPHONE            0

/* Never true for a kernel build: there is no simulator or watch/tv/vision. */
#define TARGET_OS_SIMULATOR         0
#define TARGET_OS_EMBEDDED          0
#define TARGET_OS_IOS               0
#define TARGET_OS_TV                0
#define TARGET_OS_WATCH             0
#define TARGET_OS_VISION            0
#define TARGET_OS_MACCATALYST       0

#define TARGET_OS_WIN32             0
#define TARGET_OS_UNIX              0

/* Architecture is fixed for this port; the build passes -DARM64. */
#define TARGET_CPU_ARM64            1
#define TARGET_CPU_ARM              0
#define TARGET_CPU_X86_64           0
#define TARGET_CPU_X86              0
#define TARGET_CPU_PPC              0
#define TARGET_CPU_PPC64            0

/* Little-endian ARM64 only. */
#define TARGET_RT_LITTLE_ENDIAN     1
#define TARGET_RT_BIG_ENDIAN        0
#define TARGET_RT_64_BIT            1

#endif /* _TARGET_CONDITIONALS_H_ */
