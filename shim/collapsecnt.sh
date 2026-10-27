#!/bin/bash
# collapsecnt.sh -- collapse GNU mig's per-array count parameters back to the
# single shared count Apple's mig generates from `, SameCount`.
#
# WHY THIS EXISTS
#
# Apple's .defs mark a run of `out` array parameters with `, SameCount`:
#
#     thread_act.defs:282  out old_handlers  : exception_handler_array_t, SameCount;
#     thread_act.defs:283  out old_behaviors : exception_behavior_array_t, SameCount;
#     thread_act.defs:284  out old_flavors   : exception_flavor_array_t, SameCount);
#
# Apple's mig turns that run into three arrays and ONE count. GNU mig cannot:
# routine.c:867 calls rtAddCountArg() for every variable array unconditionally,
# and CountInOut does not suppress it (routine.c:869 adds a SECOND one). The
# generated prototype therefore gains one count per array and the arity is
# wrong:
#
#     GNU mig (11)  thread_act_t, mask, masks, *masksCnt, *CountCnt,
#                   old_handlers, *old_handlersCnt,
#                   old_behaviors, *old_behaviorsCnt,
#                   old_flavors, *old_flavorsCnt
#     Apple    (7)  thread_t, mask, masks, *CountCnt,
#                   ports, behaviors, flavors        (osfmk/kern/thread.h:1728)
#
# No mig flag fixes this, so the generated header is post-processed here. XNU
# source is NOT touched: kern/thread.h stays the oracle, so a positional
# mistake shows up as a disagreement with a file we did not write.
#
# THE SHAPE IS ALWAYS THE SAME
#
# Every SameCount site in the tree is a run of three `out` arrays, each
# followed by its generated count, plus one shared count. 24 markers, 7
# routines, in task.defs / thread_act.defs / host_priv.defs. Keying on shape
# and not on identifiers matters: host_priv.defs:281 spells the parameter
# `old_handlerss` (Apple's own typo), so a name-based transform would miss it.
#
# ONE TRANSFORM, ONE SOURCE OF TRUTH
#
# The collapse is computed exactly once, into $cand. Every gate inspects THAT
# file and it is what gets installed. There is deliberately no second rewrite
# pass: a second implementation of the same filtering is how a transform ends
# up validated in one form and applied in another.
#
# USAGE
#   collapsecnt.sh <header> <oracle-header> <routine> <expected-arity> \
#                  [accepted-first-param-types]
#
#   accepted-first-param-types: comma-separated substrings, e.g.
#       "thread_act_t"        user form
#       "ipc_port_t"          server form
#
# EXIT: 0 = transformed and all gates passed. 1 = refused, header untouched.

set -u

hdr=${1:?usage: collapsecnt.sh <header> <oracle> <routine> <arity> [first-param-types]}
oracle=${2:?missing oracle header}
routine=${3:?missing routine name}
want=${4:?missing expected arity}
first_ok=${5:-}

die() { echo "collapsecnt: $*" >&2; exit 1; }

[ -f "$hdr" ]    || die "no such header: $hdr"
[ -f "$oracle" ] || die "no such oracle: $oracle"

tmp=$(mktemp -d) || die "mktemp failed"
trap 'rm -rf "$tmp"' EXIT

# ---------------------------------------------------------------------------
# param_block FILE ROUTINE
#   Print the parameter list of ROUTINE, one normalised parameter per line.
#   A MIG prototype is emitted as:
#         kern_return_t NAME
#         (
#                 type  arg,
#                 ...
#                 type  arg
#         );
#   We locate the line matching the routine name, then take everything from the
#   following "(" to the matching ")".
# ---------------------------------------------------------------------------
param_block() {
  awk -v want="$2" '
    BEGIN { state = 0 }
    state == 2 { next }
    state == 1 {
      l = $0
      if (l ~ /^[ \t]*\)/) { state = 2; next }
      sub(/^[ \t]+/, "", l); sub(/[ \t]+$/, "", l)
      if (l == "") next
      if (l == "(") next
      sub(/,$/, "", l)
      print l
      next
    }
    {
      # A prototype head takes two shapes and both must be recognised:
      #
      #   MIG-generated header   "kern_return_t thread_get_exception_ports"
      #   hand-written header    "extern kern_return_t thread_get_exception_ports("
      #
      # so match the routine name as the LAST identifier token on the line,
      # optionally followed by an opening paren, with a type before it.
      head = $0
      sub(/^[ \t]+/, "", head); sub(/[ \t]+$/, "", head)
      if (head ~ ("^/\\*.*\\*/$")) next        # comment banner, not a head
      if (head ~ ("^([A-Za-z_][A-Za-z0-9_]*[ \t]+)+" want "[(]?$")) {
        state = 1
        # the parameter list may start on this very line
        if (head ~ /[(]$/) next
      }
    }
  ' "$1"
}

# ---------------------------------------------------------------------------
# gate helpers
# ---------------------------------------------------------------------------
count_params() { grep -c . "$1" 2>/dev/null || echo 0; }

norm() { tr -s '[:space:]' ' ' <<< "$1" | sed 's/^ //; s/ $//'; }

# ---------------------------------------------------------------------------
# GATE 0 -- the routine is declared in this header at all, and exactly once.
# Counted with param_block() itself rather than a separate grep, so the gate
# and the transform cannot disagree about what "declared" means.
# ---------------------------------------------------------------------------
n_decl_before=$(param_block "$hdr" "$routine" | grep -c . || true)
[ "${n_decl_before:-0}" -ge 0 ] || n_decl_before=0

param_block "$hdr" "$routine" > "$tmp/before"
n_before=$(count_params "$tmp/before")
[ "$n_before" -gt 0 ] \
  || die "routine '$routine' has no parameters found in $hdr -- not declared, or parse failed (both are refused)"

# ---------------------------------------------------------------------------
# THE TRANSFORM -- computed once, into $tmp/cand.
#
# Drop lines of the form `mach_msg_type_number_t *<name>Cnt` that are NOT the
# shared *CountCnt. Those are the per-array counts GNU mig synthesised. The
# arrays themselves and the shared count are kept.
# ---------------------------------------------------------------------------
awk '
  !( /^mach_msg_type_number_t[ \t]*\*[ \t]*[A-Za-z_][A-Za-z0-9_]*Cnt[ \t]*[,;]?$/ &&
     !/\*CountCnt/ )
' "$tmp/before" > "$tmp/cand"

n_after=$(count_params "$tmp/cand")
collapsed=$((n_before - n_after))

# ---------------------------------------------------------------------------
# GATE 1 -- NON-VACUITY.
# A transform that removed nothing must fail loudly. This is the fifth time
# tonight a rewrite could silently do nothing; this one cannot.
# ---------------------------------------------------------------------------
[ "$collapsed" -gt 0 ] \
  || die "gate 1: collapsed 0 parameters in '$routine' ($hdr) -- transform did nothing"

# ---------------------------------------------------------------------------
# BUILD THE SHIPPING ARTIFACT -- before the remaining gates run.
#
# Gates 2..4 must inspect the file that will actually be installed, not the
# candidate parameter list that fed it. Otherwise a rebuild bug ships a file
# the gates never looked at. The candidate is spliced into the header exactly
# once, here, and nothing downstream re-derives it.
# ---------------------------------------------------------------------------
awk -v want="$routine" -v cand="$tmp/cand" '
  BEGIN { while ((getline line < cand) > 0) newp[++nnew] = line }
  state == 2 { print; next }
  state == 1 {
    if ($0 ~ /^[ \t]*\)/) {
      for (i = 1; i <= nnew; i++) {
        if (i < nnew) print "\t" newp[i] ","
        else          print "\t" newp[i]
      }
      state = 2
      print
      next
    }
    next
  }
  { print }
  {
    n = $0
    sub(/^[ \t]+/, "", n); sub(/[ \t]+$/, "", n)
    if (state == 0 && n ~ ("^([A-Za-z_][A-Za-z0-9_]*[ \t]+)+" want "[(]?$")) state = 1
  }
' "$hdr" > "$tmp/new" || die "rewrite failed"

[ -s "$tmp/new" ] || die "rewrite produced an empty file"

# ---------------------------------------------------------------------------
# GATE 2 -- NAME, read back from the rebuilt file.
# A transform that collapsed the wrong routine would pass arity and types on
# that other routine; this is what stops it. Same parser as everywhere else.
# ---------------------------------------------------------------------------
param_block "$tmp/new" "$routine" > "$tmp/after"
n_after_rebuilt=$(count_params "$tmp/after")
[ "$n_after_rebuilt" -eq "$n_after" ] \
  || die "gate 2: rebuilt file reads '\($routine\)' as $n_after_rebuilt parameters but the candidate had $n_after -- the splice disagrees with the candidate, refusing"

# ---------------------------------------------------------------------------
# GATE 3 -- ARITY, against the count the caller must supply, read back from
# the rebuilt file.
# ---------------------------------------------------------------------------
[ "$n_after_rebuilt" -eq "$want" ] \
  || die "gate 3: '$routine' has $n_after_rebuilt parameters in the rebuilt file, expected $want"

# ---------------------------------------------------------------------------
# GATE 4 -- TYPES, against the Apple header we never modify.
#
# Argument 1 differs BY DESIGN between conventions:
#     user header   thread_act_t / task_t      (the object)
#     server header ipc_port_t                 (the port)
# so the caller states which it expects instead of us skipping the check.
# ---------------------------------------------------------------------------
param_block "$oracle" "$routine" > "$tmp/oracle"
n_oracle=$(count_params "$tmp/oracle")

if [ "$n_oracle" -ne "$want" ]; then
  die "gate 4: oracle declares '$routine' with $n_oracle parameters, expected $want -- the oracle and the expectation disagree, refusing to guess which is right"
fi

i=0
while IFS= read -r oa; do
  i=$((i + 1))
  mine=$(sed -n "${i}p" "$tmp/after")
  a=$(norm "$oa"); b=$(norm "$mine")

  if [ "$i" -eq 1 ] && [ -n "$first_ok" ]; then
    ok=0
    IFS=',' read -ra acc <<< "$first_ok"
    for t in "${acc[@]}"; do
      [ -z "$t" ] && continue
      case "$b" in *"$t"*) ok=1 ;; esac
    done
    [ "$ok" -eq 1 ] \
      || die "gate 4: arg 1 is '$b', but this header expects one of {$first_ok}"
    continue
  fi

  [ "$a" = "$b" ] || die "gate 4: arg $i differs
    oracle: $a
    ours  : $b"
done < "$tmp/oracle"

# ---------------------------------------------------------------------------
# INSTALL -- only now, and only the file the gates inspected.
# ---------------------------------------------------------------------------
cp "$hdr" "$tmp/hdr.orig" 2>/dev/null || true
cat "$tmp/new" > "$hdr"

echo "collapsecnt: collapsed $collapsed parameter(s) in '$routine' ($n_before -> $n_after_rebuilt), gates 1-4 passed on the shipped bytes"
