#!/bin/bash
# ============================================================================
# migcom shim around Debian's x86_64-gnu-migcom (package: mig-x86-64-gnu).
#
# mig.sh (Apple's driver, unmodified) forwards its switches straight to
# migcom, and feeds it preprocessed .defs on STDIN.
#
# WHY THIS EXISTS -- measured, not assumed:
#
# 1. CALLING CONVENTION. migcom reads .defs from STDIN. It takes NO input
#    path argument. mig.sh:183 pipes cpp output into "$M". Passing a path
#    makes migcom exit with `bad argument: '<path>'`. Every direct migcom
#    call in this project was wrong about this for many runs; the FLAGS
#    probe's "INCONCLUSIVE" rows were this and nothing else.
#
# 2. APPLE-ONLY SWITCHES. mig.sh forwards -novouchers/-mach_msg2/-split/
#    -maxonstack; GNU migcom rejects them with "fatal: unknown flag". Each
#    drop is echoed to stderr so this is never a silent behaviour change.
#
# 3. MACHTYPE SPLIT. Apple's machine/machine_types.defs declares
#        type float  = MACH_MSG_TYPE_REAL_32;
#        type double = MACH_MSG_TYPE_REAL_64;
#    GNU migcom's built-in type table (read out of the binary's strings)
#    has MACH_MSG_TYPE_REAL and NO _REAL_32 / _REAL_64. So
#        type 'MACH_MSG_TYPE_REAL_32' not defined
#    is fatal and the header is never written. Apple split REAL into 32/64;
#    Hurd never did. We rewrite those two tokens to MACH_MSG_TYPE_REAL on
#    the way in. This is a genuine semantic difference, so it is only
#    applied to those exact tokens, and the substitution is echoed once.
# ============================================================================
set -u

real="__MIGCOM_REAL__"

args=()
dropped=""
while [ $# -gt 0 ]; do
  case "$1" in
    -novouchers|-mach_msg2|-split)
      dropped="$dropped $1"; shift ;;
    -maxonstack|-max_descrs|-max_reply_descrs)
      dropped="$dropped $1 $2"; shift 2 ;;
    *)
      args+=("$1"); shift ;;
  esac
done

[ -n "$dropped" ] && echo "migcom-shim: dropped:$dropped" >&2

inp=$(mktemp)
out=$(mktemp)
trap 'rm -f "$inp" "$out"' EXIT

cat > "$inp"

# --- MACHTYPE compatibility ------------------------------------------------
# MACH_MSG_TYPE_REAL_32 / _64 do not exist in GNU mig; MACH_MSG_TYPE_REAL
# does. Rewrite only those two exact tokens, preserving #line directives so
# diagnostics still point at the real .defs lines.
n_real=$(grep -cE 'MACH_MSG_TYPE_REAL_(32|64)\b' "$inp" || true)
if [ "$n_real" -gt 0 ]; then
  sed -E 's/MACH_MSG_TYPE_REAL_(32|64)\b/MACH_MSG_TYPE_REAL/g' "$inp" > "$out"
  echo "migcom-shim: rewrote $n_real MACH_MSG_TYPE_REAL_{32,64} -> MACH_MSG_TYPE_REAL (GNU mig has no split REAL)" >&2
else
  cp "$inp" "$out"
fi

exec "$real" "${args[@]}" < "$out"
