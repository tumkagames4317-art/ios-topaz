#!/bin/bash
# ============================================================================
# migcom shim around Debian's x86_64-gnu-migcom (package: mig-x86-64-gnu).
#
# mig.sh (Apple's driver, unmodified) forwards its switches straight to
# migcom, and feeds it preprocessed .defs on STDIN.
#
# WHY THIS EXISTS -- every item below was measured from a CI run or read out
# of mig's own source, not assumed.
#
# 1. CALLING CONVENTION. migcom reads .defs from STDIN. It takes NO input
#    path argument. mig.sh pipes cpp output into "$M". Passing a path makes
#    migcom exit with `bad argument: '<path>'`.
#
# 2. APPLE-ONLY SWITCHES. mig.sh forwards -novouchers/-mach_msg2/-split/
#    -maxonstack; GNU migcom rejects them with "fatal: unknown flag". Each
#    drop is echoed so this is never a silent behaviour change.
#
# 3. FLOATING POINT TYPES. Apple's machine/machine_types.defs:43-44 says
#        type float  = MACH_MSG_TYPE_REAL_32;
#        type double = MACH_MSG_TYPE_REAL_64;
#    GNU mig's lexer (lexxer.l:173) has
#        "MACH_MSG_TYPE_REAL"   TRETURN(MACH_MSG_TYPE_REAL, 0)
#    -- size ZERO, like STRING/POLYMORPHIC/UNSTRUCTURED, which are markers
#    rather than sized types. itShortDecl() (type.c:525) then fires
#        error("must use full IPC type decl")
#    because defsize == 0, and migcom dies on
#        it->itAlignment > 0   (type.c:165, itCalculateSizeInfo)
#    So MACH_MSG_TYPE_REAL is NOT a substitute for REAL_32. An earlier
#    revision of this shim made exactly that substitution, and run
#    40395401198 measured it to be wrong:
#        machine_types.defs: must use full IPC type decl
#        migcom-debian: ../type.c:165: itCalculateSizeInfo:
#                       Assertion `it->itAlignment > 0' failed.
#    GNU mig has no sized floating-point IPC type at all. The correct
#    translation is a sized INTEGER type of the same width, which is what a
#    Mach message carries on the wire (a bag of bits of a given size and
#    alignment). The C-side type still comes from the header's `import`,
#    so float/double stay float/double in the generated code.
#        MACH_MSG_TYPE_REAL_32 -> MACH_MSG_TYPE_INTEGER_32
#        MACH_MSG_TYPE_REAL_64 -> MACH_MSG_TYPE_INTEGER_64
#    This is a real semantic mapping and is echoed loudly once.
#
# 4. SUBSYSTEM ORDERING. GNU mig resolves `type` declarations against the
#    subsystem's basic-type table, which exists only after the `subsystem`
#    statement is seen. Apple's .defs put #includes first and the subsystem
#    statement after (lockd_mach.defs:30 vs :43), giving
#        "Basic types not initialized when looking up type int8_t.
#         Did you forget to define the subsystem?"
#    The `subsystem ...;` statement is hoisted to the top. Nothing else is
#    reordered, and #line directives are untouched so diagnostics still
#    name real .defs lines.
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

# --- (3) MACH_MSG_TYPE_REAL_32/_64 -> sized INTEGER equivalents -----------
# Word-bounded so a hypothetical REAL_320 is never touched.
n_real=$(grep -cE 'MACH_MSG_TYPE_REAL_(32|64)([^0-9_]|$)' "$inp" || true)
if [ "${n_real:-0}" -gt 0 ]; then
  sed -e 's/MACH_MSG_TYPE_REAL_32\([^0-9_]\|$\)/MACH_MSG_TYPE_INTEGER_32\1/g' \
      -e 's/MACH_MSG_TYPE_REAL_64\([^0-9_]\|$\)/MACH_MSG_TYPE_INTEGER_64\1/g' \
      "$inp" > "$real_out"
  echo "migcom-shim: mapped $n_real MACH_MSG_TYPE_REAL_{32,64} -> INTEGER_{32,64} (GNU mig has no sized REAL)" >&2
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
