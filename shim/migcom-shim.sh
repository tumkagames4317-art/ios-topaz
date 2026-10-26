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
real_out2=$(mktemp)
final=$(mktemp)
hoisted=$(mktemp)
trap 'rm -f "$inp" "$real_out" "$real_out2" "$hoisted" "$final"' EXIT

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

# --- (3b) SameCount -> an explicit count argument --------------------------
# Apple's mig accepts the marker `SameCount` on an out array parameter and
# uses it to generate a count output. GNU mig has no such keyword: the
# lexer (lexxer.l) knows routine/simpleroutine/in/out/inout/array/of/... and
# nothing else, so `array_t, SameCount;` is a hard SYNTAX ERROR -- measured
# in run 40395508631 at task.defs:278-280, 295-296.
#
# Dropping the marker is NOT a fix. It would make the marker vanish AND
# silently delete the count parameter that Apple's mig generates from it.
# The kernel proves the parameter is real -- osfmk/kern/task.h:
#     extern kern_return_t task_get_exception_ports(
#         task_t task, exception_mask_t exception_mask,
#         exception_mask_array_t masks,
#         mach_msg_type_number_t *CountCnt,      <-- generated, not in .defs
#         exception_port_array_t ports,
#         exception_behavior_array_t behaviors,
#         thread_state_flavor_array_t flavors);
# and ipc_tt.c:4824 calls it with seven arguments. A stub missing CountCnt
# would fail to compile against that header.
#
# GNU mig has an exact equivalent: declaring the parameter yourself as
# `out name : mach_msg_type_number_t`. So SameCount is REWRITTEN into that
# form rather than removed. The emitted order then matches Apple's: Apple
# puts the count immediately before the parameter carrying the marker, so
# a count is inserted before the FIRST marked parameter of a routine and
# later markers in the same routine are simply dropped.
#
# Name: CountCnt. It appears in the kernel but is not part of the Mach
# message -- MIG's generated header uses the .defs name, and the external
# ABI is positional. Chosen over a descriptive name (e.g.
# old_handlers_count) precisely because it is not ABI.
#
# Applies to 24 markers across 8 routines in host_priv.defs, task.defs and
# thread_act.defs. The generated stubs are kernel-side, so this is only
# about compiling this tree; it is not a device ABI claim.
n_sc=$(grep -c 'SameCount' "$real_out" || true)
if [ "${n_sc:-0}" -gt 0 ]; then
  awk '
    function emit_count() {
      print "  out\tCountCnt\t\t: mach_msg_type_number_t;"
      inserted = 1
    }
    {
      line = $0
      trimmed = line
      sub(/^[ \t]+/, "", trimmed)

      # A new routine starts: this one has not had its count yet.
      if (trimmed ~ /^(simple)?routine([ \t]|$)/) inserted = 0

      if (trimmed !~ /SameCount/) { print line; next }

      # Drop the marker, keeping the terminating ; or ) of the parameter.
      had_paren = 0
      if (line ~ /\)[ \t]*;[ \t]*$/) had_paren = 1
      sub(/[ \t]*,[ \t]*SameCount/, "", line)
      if (!inserted) emit_count()
      print line
    }
  ' "$real_out" > "$real_out2"
  echo "migcom-shim: converted $n_sc SameCount markers into explicit count parameters" >&2
else
  cp "$real_out" "$real_out2"
fi

# --- (4) hoist the subsystem statement to the top --------------------------
if ! grep -qE '^[[:blank:]]*subsystem([[:blank:]]|$)' "$real_out2"; then
  exec "$real" "${args[@]}" < "$real_out2"
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
' "$real_out2" > "$hoisted"

awk_rc=$?
if [ "$awk_rc" -ne 0 ]; then
  echo "migcom-shim: WARNING: could not hoist subsystem (awk rc=$awk_rc); passing through unchanged" >&2
  exec "$real" "${args[@]}" < "$real_out2"
fi

echo "migcom-shim: hoisted subsystem statement to top of input" >&2

# --- (5) supply mach_msg_type_number_t, which Apple's mig has built in -----
# Run 40396625873 got past SameCount (9 markers converted) and then failed at
#     task.defs, line 278: type 'mach_msg_type_number_t' not defined
# twice more at :296 and :563 -- exactly the CountCnt parameters the SameCount
# rewrite above inserts.
#
# Why it is not defined: Apple's mig treats mach_msg_type_number_t as a
# BASIC TYPE. bootstrap_cmds migcom.tproj/type.c:653 sets it directly
#     it->itName = "mach_msg_type_number_t";
# and :623 calls itCheckDecl("mach_msg_type_number_t", itElement).
# GNU mig's built-ins (type.c init_type, 897-...) are only:
#     unsigned char, kern_return_t, char *, mach_port_t
# integer_t/natural_t are covered because machine_types.defs:80-81 declares
# them; mach_msg_type_number_t appears in NO .defs in the tree.
#
# The correct declaration is spelled exactly as machine_types.defs declares
# natural_t, so it lands in the same size/alignment family:
#     type mach_msg_type_number_t = natural_t;
# It is injected immediately after the hoisted `subsystem ...;` statement so
# it precedes every use and sits inside a valid basic-type context.
if ! grep -qE '^[[:blank:]]*type[[:blank:]]+mach_msg_type_number_t' "$hoisted"; then
  # Insert right after the semicolon that terminates `subsystem ...;`.
  # grab is set when the subsystem line is seen, and the insert happens on
  # the FIRST subsequent line containing ';' -- which is that terminator.
  awk '
    !grab && /^[ \t]*subsystem([ \t]|$)/ { grab = 1; print; next }
    grab && !done && /;/ { print; print "type mach_msg_type_number_t = natural_t;"; done = 1; next }
    { print }
  ' "$hoisted" > "$final" && cp "$final" "$hoisted"
  echo "migcom-shim: injected type mach_msg_type_number_t = natural_t (Apple built-in, absent from all .defs)" >&2
fi

exec "$real" "${args[@]}" < "$hoisted"
