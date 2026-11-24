#!/usr/bin/env bash
# mayhem/test.sh — run roc-toolkit's own CppUTest suites (roc-test-*), built by mayhem/build.sh with
# normal flags (gcc) into mayhem/out/tests. Each suite asserts audio/packet/FEC/RTCP/URI results;
# counts come from CppUTest's summary line ("OK (N tests, R ran, ..., I ignored, ...)" or
# "Errors (F failures, N tests, R ran, ...)"). A suite that prints no summary counts as failed.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"
cd "$SRC"

emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

TESTDIR=mayhem/out/tests
shopt -s nullglob
suites=("$TESTDIR"/roc-test-*)
if [ ${#suites[@]} -eq 0 ]; then
  echo "test.sh: no roc-test-* runners in $TESTDIR (build.sh should have built them)" >&2
  emit_ctrf cpputest 0 1 0
  exit 1
fi

# roc-test-public-api: the receiver.configure* tests join the multicast group 224.0.0.1, which needs
# a network interface; the graded rebuild runs with --network none (lo only), where they fail. CppUTest
# 4.0 only excludes by name substring, so all 8 {sender,receiver}.configure* tests are filtered out
# (reported as skipped); the other 77 public-API tests still run.
args_for() {
  case "$(basename "$1")" in
    roc-test-public-api) echo "-xn configure" ;;
    roc-test-netio) echo "-xn multicast" ;; # udp_ports.multicast_* join a group: same reason
  esac
}

passed=0 failed=0 skipped=0
for t in "${suites[@]}"; do
  # shellcheck disable=SC2046
  out=$("$t" $(args_for "$t") 2>&1); rc=$?
  line=$(printf '%s\n' "$out" | grep -E '^(OK|Errors) \(' | tail -1)
  echo "$(basename "$t"): rc=$rc ${line:-<no CppUTest summary>}"
  if [[ "$line" =~ ^OK\ \(([0-9]+)\ tests,\ ([0-9]+)\ ran,.*\ ([0-9]+)\ ignored ]] && [ $rc -eq 0 ]; then
    passed=$(( passed + BASH_REMATCH[2] ))
    skipped=$(( skipped + BASH_REMATCH[1] - BASH_REMATCH[2] ))
  elif [[ "$line" =~ ^Errors\ \(([0-9]+)\ failures?,\ ([0-9]+)\ tests,\ ([0-9]+)\ ran ]]; then
    failed=$(( failed + BASH_REMATCH[1] ))
    passed=$(( passed + BASH_REMATCH[3] - BASH_REMATCH[1] ))
    skipped=$(( skipped + BASH_REMATCH[2] - BASH_REMATCH[3] ))
  else
    printf '%s\n' "$out" | tail -20
    failed=$(( failed + 1 ))
  fi
done
emit_ctrf cpputest "$passed" "$failed" "$skipped"
