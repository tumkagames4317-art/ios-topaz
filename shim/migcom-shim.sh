#!/bin/bash
# ============================================================================
# migcom shim around Debian's x86_64-gnu-migcom (package: mig-x86-64-gnu).
#
# mig.sh (Apple's driver, unmodified) forwards its switches straight to
# migcom, and feeds it preprocessed .defs on STDIN.
#
# WHY THIS EXISTS -- each item below was measured from a CI run, not assumed.
#
# 1. CALLING CONVENTION. migcom reads .defs from STDIN. It takes NO input
#    path argument. mig.sh:183 pipes cpp output into "$M". Passing a path
#    makes migcom exit with `bad argument: '<path>'`. Every direct migcom
#    call in this project was wrong about this for many runs; the deleted
#    FLAGS probe's "INCONCLUSIVE" rows were this and nothing else.
#
# 2. APPLE-ONLY SWITCHES. mig.sh forwards -novouchers/-mach_msg2/-split/
#    -maxonstack; GNU migcom rejects them with "fatal: unknown flag". Each
#    drop is echoed so this is never a silent behaviour change.
#
# 3. MACHTYPE SPLIT. Apple's machine/machine_types.defs declares
#        type float  = MACH_MSG_TYPE_REAL_32;
#        type double = MACH_MSG_TYPE_REAL_64;
#    GNU migcom's built-in table (every MACH_MSG_TYPE_* string in the
#    binary: 23 entries) has MACH_MSG_TYPE_REAL and NO _REAL_32/_REAL_64.
#    Apple split REAL into 32/64; Hurd never did.
#
#    The rewrite targets MACH_MSG_TYPE_INTEGER_32 / _INTEGER_64, NOT
#    MACH_MSG_TYPE_REAL. From Debian mig's lexxer.l:77-84, TRETURN's second
#    argument is the byte width:
#        <Normal>"MACH_MSG_TYPE_REAL"      TRETURN(MACH_MSG_TYPE_REAL,0);
#        <Normal>"MACH_MSG_TYPE_INTEGER_8" TRETURN(MACH_MSG_TYPE_INTEGER_8,8);
#    REAL's width is literally 0. A type with itSize 0 yields itTypeSize 0
#    (type.c:169), which trips type.c:195 `sizeof(%s) == 0`. Mapping to REAL
#    would therefore make every float/double a ZERO-WIDTH field in generated
#    RPC stubs -- it would compile and then be wrong. INTEGER_32/_64 carry
#    4/8 bytes, which is exactly what C float/double occupy.
#
#    Honest scope: no routine in this xnu passes float/double today (grep
#    for `: float;` / `: double;` across all 55 .defs finds nothing), so
#    this removes a latent trap rather than unblocking anything. It is here
#    because the earlier mapping to MACH_MSG_TYPE_REAL was actively wrong,
#    and wrong-but-silent is the worst outcome for a code generator.
#
# 4. SUBSYSTEM ORDERING. GNU mig resolves `type` declarations against the
#    subsystem's basic-type table, which only exists once the `subsystem`
#    statement has been seen. Apple's .defs put #includes FIRST and the
#    subsystem statement AFTER:
#        lockd_mach.defs:30  #include <mach/std_types.defs>
#        lockd_mach.defs:43  subsystem
#    so GNU mig fails with
#        std_types.defs, line 69: Basic types not initialized when looking
#        up type int8_t. Did you forget to define the subsystem?
#    Run 40394250479 measured exactly that, AFTER fix (3) removed the
#    earlier error -- so the error text itself names the remedy. The
#    `subsystem ...;` statement is hoisted to the top. Nothing else is
#    reordered, and #line directives are untouched so diagnostics still
#    name real .defs lines.
#    (An earlier revision carried this hoist, then dropped it when the
#    blocking error was a type-table lookup instead. Restored on evidence.)
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
real_out=$(mktemp)
hoisted=$(mktemp)
trap 'rm -f "$inp" "$real_out" "$hoisted"' EXIT

cat > "$inp"

# --- (3) MACH_MSG_TYPE_REAL_32/_64 -> MACH_MSG_TYPE_INTEGER_32/_64 ---------
n_real=$(grep -cE 'MACH_MSG_TYPE_REAL_(32|64)([^0-9_]|$)' "$inp" || true)
if [ "${n_real:-0}" -gt 0 ]; then
  sed -E -e 's/MACH_MSG_TYPE_REAL_32([^0-9_]|$)/MACH_MSG_TYPE_INTEGER_32\1/g' \
         -e 's/MACH_MSG_TYPE_REAL_64([^0-9_]|$)/MACH_MSG_TYPE_INTEGER_64\1/g' \
         "$inp" > "$real_out"
  echo "migcom-shim: rewrote $n_real MACH_MSG_TYPE_REAL_{32,64} -> INTEGER_{32,64} (widths 4/8 bytes)" >&2
else
  cp "$inp" "$real_out"
fi

# --- (4) hoist the subsystem statement to the top --------------------------
if ! grep -qE '^[[:blank:]]*subsystem([[:blank:]]|$)' "$real_out"; then
  exec "$real" "${args[@]}" < "$real_out"
fi

awk '
  BEGIN { grab = 0; nb = 0; ss = "" }
  {
    line = $0
    sub(/^[ \t]+/, "", line)
  }
  !grab && line ~ /^subsystem([ \t]|$)/ { grab = 1 }
  grab {
    ss = ss $0 "\n"
    # The statement ends at the first semicolon, which may be on the
    # `subsystem` line itself or after intervening #ifdef/#endif lines.
    if ($0 ~ /;/) { grab = 0; nb++ }
    next
  }
  { lines[++n] = $0 }
  END {
    if (nb != 1) exit 3
    printf "%s", ss
    for (i = 1; i <= n; i++) print lines[i]
  }
' "$real_out" > "$hoisted"

awk_rc=$?
if [ "$awk_rc" -ne 0 ]; then
  echo "migcom-shim: WARNING: could not hoist subsystem (awk rc=$awk_rc); passing through unchanged" >&2
  exec "$real" "${args[@]}" < "$real_out"
fi

echo "migcom-shim: hoisted subsystem statement to top of input" >&2
exec "$real" "${args[@]}" < "$hoisted"
