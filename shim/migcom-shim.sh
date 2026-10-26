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
n_dropped=0
while [ $# -gt 0 ]; do
  case "$1" in
    -novouchers|-mach_msg2|-split)
      dropped="$dropped $1"; n_dropped=$((n_dropped+1)); shift ;;
    -maxonstack|-max_descrs|-max_reply_descrs)
      dropped="$dropped $1 $2"; n_dropped=$((n_dropped+1)); shift 2 ;;
    *)
      args+=("$1"); shift ;;
  esac
done

# Print the COUNT as well as the names. A bare name list cannot be told apart
# from a shim that never ran; a count plus the argument vector migcom will
# actually receive can be. Stage 6 needs this: run 40401122633 showed MIGFLAGS
# carrying -novouchers into the real build, and we need to see it removed here
# rather than infer it from the absence of a fatal later.
echo "migcom-shim: dropped $n_dropped Apple-only switch(es):${dropped:- (none)}" >&2
# NOTE: print ${args[@]}, NOT "$@". The while/shift loop above drains $#  to
# zero, so "$#" here is always 0 and "$@" always empty -- a first version of
# this diagnostic printed "(none)" even though migcom received
# `-header out.h -user out.c`. Another instrument that would have lied.
if [ "${#args[@]}" -gt 0 ]; then
  printf 'migcom-shim: migcom argv (%d after drop):' "${#args[@]}" >&2
  printf ' %s' "${args[@]}" >&2
  printf '\n' >&2
else
  echo "migcom-shim: migcom argv (0 after drop): (none)" >&2
fi

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

# --- (3c) ServerAuditToken -> in ------------------------------------------
# Apple marks a parameter carrying the caller's audit token with the direction
# keyword `ServerAuditToken`:
#     task_access.defs:50   ServerAuditToken  caller_cred : audit_token_t);
#     task_access.defs:65   ServerAuditToken  caller_cred : audit_token_t);
# GNU mig has no such token -- zero occurrences of "AuditToken" in lexxer.l,
# parser.y or any .c file in the GNU mig 1.8 tree. Its Direction production
# (parser.y:653-656) admits only:
#     /* empty */ | syIn | syOut | syInOut | ...
# so `ServerAuditToken` lands where a direction must be and is a syntax error:
#     task_access.defs, line 64: syntax error      (run 40403082377)
#
# Mapping to `in` is the faithful translation, not a workaround:
#   - the parameter IS an input (a credential handed to the server),
#   - Apple's special behaviour is that the SERVER receives the token even
#     though the client did not send it explicitly,
#   - and we generate no server code at all (Stage 6 invokes mig with
#     -server /dev/null -sheader <name>), so that server-side behaviour is
#     never emitted.
# The user-side declaration keeps its name and its audit_token_t type, so the
# generated header's parameter list is unchanged.
#
# ServerSecToken is the same shape (security_token_t instead of
# audit_token_t) and is absent from the GNU lexer for the same reason, so it
# gets the same treatment rather than being left to cascade into the
# parameter list that follows it.
n_sat=$(grep -cE '\b(ServerAuditToken|ServerSecToken)\b' "$real_out2" || true)
if [ "${n_sat:-0}" -gt 0 ]; then
  sed -i -E 's/^([[:space:]]*)(ServerAuditToken|ServerSecToken)([[:space:]])/\1in\3/' "$real_out2"
  echo "migcom-shim: rewrote $n_sat ServerAuditToken/ServerSecToken -> in (Apple-only direction; no server code generated)" >&2
fi

# --- (4) hoist the subsystem statement to the top --------------------------
if ! grep -qE '^[[:blank:]]*subsystem([[:blank:]]|$)' "$real_out2"; then
  exec "$real" "${args[@]}" < "$real_out2"
fi

awk '
  BEGIN { grab = 0; nb = 0; ss = ""; nl = 0 }
  {
    line = $0
    sub(/^[ \t]+/, "", line)
  }
  # A line-directive must NEVER enter the subsystem block. GNU migs grammar is
  #     Subsystem : SubsystemStart SubsystemMods SubsystemName SubsystemBase
  # and SubsystemMods (parser.y:229-245) accepts only syKernelUser /
  # syKernelServer. cpp emits `# 64 "exc.defs"` markers constantly, and exc.defs
  # really is
  #     subsystem
  #     #if KERNEL_SERVER || KOBJECT_SERVER   -> KernelServer
  #     #if KERNEL_USER                       -> KernelUser
  #              exc 2401;
  # so after cpp the block has # markers interleaved. Hoisting them verbatim
  # produced `subsystem # 64 ... KernelServer ...` and GNU mig filed no
  # subsystem at all, then every routine hit
  #     mig: fatal: no SubSystem declaration     (global.c:76, after yyparse)
  # which is what 12 MIG targets died on in run 40399841763 / a120648.
  # Keep the markers in the residual stream; drop them from the hoisted block.
  !grab && /^[ \t]*#/ { lines[++n] = $0; next }
  !grab && line ~ /^subsystem([ \t]|$)/ { grab = 1 }
  grab {
    if ($0 ~ /^[ \t]*#/) next      # line-directive inside the block: skip
    ss = ss $0 "\n"
    # The statement ends at the first semicolon, which may be on the
    # `subsystem` line itself or after the mode keywords and name.
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

# --- (4b) PROVENANCE for duplicate type declarations ------------------------
# GNU mig treats a repeated `type X = ...` as FATAL (parser.y:339 calls
# error(); error() prints with no severity prefix; and warn() at error.c:55 is
# suppressed once errors != 0, so the fatal lines look like the harmless ones).
# Stage 6 reports six of them, every run:
#     machine_types.defs:38 short   :39 int   :61 uintptr_t   :62 intptr_t
#     std_types.defs:84 int64       :90 char
# A count cannot distinguish the two causes, and a count is what let this
# survive several runs. The #line provenance can:
#   same file AND same line twice  -> the file really was included twice
#   same file, DIFFERENT lines     -> two revisions, or two files, same name
# So print, for each target type, every occurrence with the #line-attributed
# file that precedes it. Cheap: one awk over a file we already have.
{
  echo "migcom-shim: TYPE PROVENANCE (file@line for each declaration, on the hoisted stream):"
  awk '
    /^[ \t]*#/ && /"/ {
      # cpp line marker: # <num> "<file>" [flags]
      f = $0
      sub(/^[ \t]*#[ \t]*[0-9]+[ \t]+"/, "", f)
      sub(/".*$/, "", f)
      cur = f
      l = $0
      sub(/^[ \t]*#[ \t]*/, "", l)
      sub(/[ \t]+".*$/, "", l)
      curline = l
      next
    }
    /^[ \t]*type[ \t]+(short|int|uintptr_t|intptr_t|int64|char|natural_t)[ \t]*=/ {
      n = $0
      sub(/^[ \t]*type[ \t]+/, "", n)
      sub(/[ \t]*=.*$/, "", n)
      print "    " n " @ " cur ":" curline
      seen[cur ":" curline]++
    }
    END {
      d = 0
      for (k in seen) if (seen[k] > 1) { print "    DUPLICATE (same file:line " k " x" seen[k] ")" ; d++ }
      if (d == 0) print "    (no same-file:line duplicate in this stream)"
    }
  ' "$hoisted"
} >&2

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
# The declaration must name a type that is ALREADY DEFINED at the point of
# injection. Two candidates existed and the difference matters:
#
#   = natural_t   machine_types.defs:81. WRONG HERE. natural_t is itself
#                 declared inside machine_types.defs, which std_types.defs
#                 #includes; the injection point is right after the hoisted
#                 `subsystem ...;`, which is BEFORE those #includes are
#                 reached. It would forward-reference its own base type and
#                 fail exactly as mach_msg_type_number_t just did. Caught by
#                 simulating the deliverable locally and printing line
#                 numbers: injection at line 4, natural_t defined at line 11.
#
#   = uint32_t    std_types.defs:75. CORRECT. std_types.defs is the first
#                 .defs every subsystem pulls in, and every one of the eight
#                 affected routines is in a file that includes it before any
#                 routine. uint32_t is the same width as natural_t
#                 (machine_types.defs:81 spells it `type natural_t =
#                 uint32_t;`) and matches message.h:857
#                 `typedef natural_t mach_msg_type_number_t;`.
#
# So the injected line must use a PRIMITIVE that migcom knows from its own
# lexer -- not an alias some .defs declares. THREE attempts, all measured:
#   natural_t  -- declared in machine_types.defs:81; undefined if the
#                 injection precedes that include (include-order trap)
#   uint32_t   -- declared in std_types.defs:75. Reasoning above was right
#                 about includes being present, but WRONG about position:
#                 the subsystem hoist puts this injection ABOVE every
#                 std_types.defs declaration, so run 40397975034 failed
#                     "(unknown)", line 4: type 'uint32_t' not defined
#   MACH_MSG_TYPE_INTEGER_32 -- lexer token (lexxer.l:168) compiled into
#                 migcom itself; needs no declaration anywhere. 32 bits,
#                 matching message.h:857 typedef natural_t
#                 mach_msg_type_number_t and machine_types.defs:81
#                 `type natural_t = uint32_t;`. CORRECT.
#
# The lesson: at the top of the file nothing is declared yet, so only
# lexer-level primitives are safe. "The include is present" is not the same
# as "the declaration is above me".
#
# So the injected line is:
#   type mach_msg_type_number_t = MACH_MSG_TYPE_INTEGER_32;
if ! grep -qE '^[[:blank:]]*type[[:blank:]]+mach_msg_type_number_t' "$hoisted"; then
  # Insert right after the semicolon that terminates `subsystem ...;`.
  # grab is set when the subsystem line is seen, and the insert happens on
  # the FIRST subsequent line containing ';' -- which is that terminator.
  awk '
    !grab && /^[ \t]*subsystem([ \t]|$)/ { grab = 1; print; next }
    grab && !done && /;/ { print; print "type mach_msg_type_number_t = MACH_MSG_TYPE_INTEGER_32;"; done = 1; next }
    { print }
  ' "$hoisted" > "$final" && cp "$final" "$hoisted"
  echo "migcom-shim: injected type mach_msg_type_number_t = MACH_MSG_TYPE_INTEGER_32 (Apple built-in; primitive, no .defs needed)" >&2
fi

exec "$real" "${args[@]}" < "$hoisted"
