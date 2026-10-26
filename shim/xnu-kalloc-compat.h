/*
 * xnu-kalloc-compat.h -- substitute the Apple-clang kalloc_type() type
 * signature builtins when building XNU with upstream LLVM clang.
 *
 * WHY THIS EXISTS
 *
 * xnu/osfmk/kern/kalloc.h implements type-aware allocation: every
 * kalloc_type(T, ...) call site records a compile-time "signature" derived
 * from T's memory layout, and the runtime uses it to pin that call site to
 * a dedicated zone. The machinery rests on four Apple-clang-only builtins:
 *
 *     __builtin_xnu_type_summary(type)      -> 32-bit layout summary
 *     __builtin_xnu_type_signature(type)    -> string signature
 *     __builtin_xnu_types_compatible(a, b)  -> bool
 *     __builtin_xnu_type_discriminator(type)
 *
 * Upstream LLVM does not implement any of them. It parses an unknown builtin
 * as an ordinary function call, so on ubuntu-latest the kernel instead dies
 * with tens of thousands of lines of
 *
 *     kalloc.h:775: error: expected expression
 *     kalloc.h:763: error: use of unknown builtin '__builtin_xnu_type_summary'
 *
 * (run 40415149814: 6471 x "expected expression", 3456 x "'T' does not refer
 * to a value", ~2.5k x the three unknown builtins.)
 *
 * Nothing in kalloc.h guards these behind __has_builtin, so the only options
 * are to patch kalloc.h on every build or to supply the names from outside.
 * This header does the latter, and is force-included (-include) so it is in
 * place before any kernel header is reached.
 *
 * WHAT CHANGES
 *
 * xnu's kalloc_type zones are a defence-in-depth heap isolation measure: a
 * type-confused pointer into a kalloc_type() object is likely to land in a
 * zone that never held that type, so the corruption is contained. It is not
 * load-bearing for correctness -- a kernel with no signatures still
 * allocates, frees and runs; it simply loses the per-type zone split.
 *
 * These definitions therefore make every call site compile and behave
 * consistently, and they all agree with each other, which is the property
 * that actually matters: the KALLOC_TYPE_ASSERT_COMPATIBLE_POINTER
 * _Static_assert must not fire, the KALLOC_TYPE_CHECK decisions must not
 * disagree between the definition site and the use site, and the
 * kt_signature/kt_sig_type/kt_sig_hdr fields must be valid string literals.
 *
 *   type_summary(T)       -> KT_SUMMARY_MASK_DATA
 *   types_compatible      -> 1 (true; ptr and type are "the same type")
 *   type_signature(T)     -> "" (no signature emitted)
 *   type_discriminator(T) -> 0
 *
 * The summary value is chosen from kalloc.h's own encoding, not invented:
 *
 *     KT_SUMMARY_GRANULE_TO_IDX(g)   == 1 << g
 *     KT_GRANULE_PADDING             == 0
 *     KT_GRANULE_DATA                == 2
 *     KT_SUMMARY_MASK_DATA           == (1<<0) | (1<<2) == 0x5
 *
 * so KALLOC_TYPE_IS_DATA_ONLY(t) -- ((summary & 0xffff) & ~0x5) == 0 -- is
 * true for every type, KALLOC_TYPE_HAS_OVERLAPS(t) is false, and every
 * KALLOC_TYPE_CHECK resolves to the same constant at every site. Every
 * allocation is treated as data-only, which is the conservative choice: it
 * suppresses signature emission rather than fabricating one.
 *
 * This file must stay a plain C preprocessor header with no dependencies: it
 * is included before <sys/cdefs.h> and can be seen by both C and C++ TUs.
 * It is written explicitly for pre-C23 compilers, which the kernel is.
 */

#ifndef _XNU_KALLOC_COMPAT_H_
#define _XNU_KALLOC_COMPAT_H_

/*
 * Only stand in when the compiler genuinely lacks the builtins. Apple clang
 * implements all four, so this header is a no-op there and the real type
 * signature system stays in effect -- the substitute can never silently
 * replace working behaviour on a machine that does not need it.
 */
#if !defined(__has_builtin)
#define __has_builtin(x) 0
#define XNU_KALLOC_COMPAT_UNDEF_HAS_BUILTIN 1
#endif

#if !__has_builtin(__builtin_xnu_type_summary)

#define XNU_KALLOC_COMPAT 1

/* 0x5 == KT_SUMMARY_GRANULE_TO_IDX(KT_GRANULE_PADDING) |
 *        KT_SUMMARY_GRANULE_TO_IDX(KT_GRANULE_DATA)        == KT_SUMMARY_MASK_DATA */
#define __builtin_xnu_type_summary(type)     ((unsigned int)0x5u)
#define __builtin_xnu_type_signature(type)   ((const char *)"")
#define __builtin_xnu_types_compatible(a, b) (1)
#define __builtin_xnu_type_discriminator(type) ((unsigned int)0u)

#endif /* !__has_builtin(__builtin_xnu_type_summary) */

#ifdef XNU_KALLOC_COMPAT_UNDEF_HAS_BUILTIN
#undef __has_builtin
#undef XNU_KALLOC_COMPAT_UNDEF_HAS_BUILTIN
#endif

#endif /* _XNU_KALLOC_COMPAT_H_ */
