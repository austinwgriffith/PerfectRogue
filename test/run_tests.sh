#!/usr/bin/env bash
# Perfect Random regression tests: rebuild, run every test in test/tests.txt, and
# write test-results/latest.txt (what Claude reads). Run from the MSYS2 MINGW64 shell:
#
#   cd /c/Games/Recomps/pd-dabs-mod-x86_64-windows/PerfectRogue/source
#   ./test/run_tests.sh              # build, then all tests
#   ./test/run_tests.sh nobuild      # skip the build
#   ./test/run_tests.sh spawnedit    # only tests whose name contains "spawnedit"
#
# A game window opens for each test for about half a minute and closes itself.
set -u
cd "$(dirname "$0")/.."
SRC=$(pwd)
OUT="$SRC/test-results"
mkdir -p "$OUT"
RES="$OUT/latest.txt"
: > "$RES"
say() { echo "$@" | tee -a "$RES"; }

BUILD=1
FILTER=""
for a in "$@"; do
  if [ "$a" = "nobuild" ]; then BUILD=0; else FILTER="$a"; fi
done

say "run: $(date)"

if [ $BUILD = 1 ]; then
  if ! cmake --build build-win -j"$(nproc)" > "$OUT/build.log" 2>&1; then
    say "BUILD FAILED"
    grep -E "error|Error" "$OUT/build.log" | head -n 30 | tee -a "$RES"
    exit 2
  fi
  say "build ok"
fi

cp build-win/pd.x86_64.exe ..
cd ..

total_fail=0
while IFS='|' read -r name args; do
  name=$(echo "$name" | xargs)
  case "$name" in ""|\#*) continue;; esac
  if [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]]; then continue; fi
  args=$(echo "$args" | xargs)
  say ""
  say "== $name  ($args)"
  rm -f pd.log pd.crash.log
  timeout 300 ./pd.x86_64.exe --log --skip-intro --no-sound $args > /dev/null 2>&1
  code=$?
  cp pd.log "$OUT/$name.log" 2>/dev/null
  say "exit code $code"
  grep -E "SELFTEST" pd.log | tee -a "$RES"
  if grep -qE "RECOVERED|FATAL|EXCEPTION" pd.log; then
    say "CRASH or recovered fault:"
    grep -E "RECOVERED #1:|FATAL|EXCEPTION|^#0" pd.log | head -n 8 | tee -a "$RES"
    for off in $(grep -oE "pd.x86_64.exe\+0x[0-9a-f]+" pd.log | head -n 6 | sed 's/.*+//'); do
      addr2line -f -C -p -e "$SRC/build-win/pd.x86_64.exe" $(printf '0x%x' $((0x140000000 + off))) | sed 's|C:[^ ]*/src/|src/|' | tee -a "$RES"
    done
  fi
  if ! grep -q "SELFTEST RESULT" pd.log; then
    say "FAIL: no SELFTEST RESULT line (the test did not finish; code $code)"
    code=1
  fi
  if [ "$code" != 0 ]; then total_fail=$((total_fail + 1)); fi
done < "$SRC/test/tests.txt"

say ""
if [ $total_fail = 0 ]; then say "ALL TESTS PASSED"; else say "$total_fail TEST RUN(S) FAILED"; fi
exit $total_fail
