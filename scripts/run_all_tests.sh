#!/usr/bin/env bash
# Builds and runs every test configuration and prints a summary:
#   release: fast + long suites;  asan (ASan + UBSan): fast suite;  tsan: fast suite;
#   plus a short benchmark smoke run. Exit status is non-zero if anything failed.
set -uo pipefail
cd "$(dirname "$0")/.."
export DOCSTORE_LOG_LEVEL=${DOCSTORE_LOG_LEVEL:-off}
JOBS=$(nproc)
declare -a NAMES RESULTS
FAILED=0

step() {  # step <name> <command...>
  local name=$1
  shift
  echo "==> $name" >&2
  local start=$SECONDS
  if "$@" >"build/$name.log" 2>&1; then
    RESULTS+=("PASS ($((SECONDS - start)) s)")
  else
    RESULTS+=("FAIL ($((SECONDS - start)) s, see build/$name.log)")
    FAILED=1
  fi
  NAMES+=("$name")
}

mkdir -p build
for preset in release asan tsan; do
  step "build-$preset" bash -c "cmake --preset $preset -DDOCSTORE_WERROR=ON && cmake --build --preset $preset -j $JOBS"
  step "fast-$preset" ctest --preset "$preset" -L fast
done
step "long-release" ctest --preset release -L long --output-on-failure -V
step "bench-smoke" ./build/release/docstore_bench \
  --benchmarks=fillrandom,readrandom,readmissing --num=100000 --db="$PWD/build/bench_smoke"
rm -rf build/bench_smoke

echo
echo "Summary"
echo "-------"
for i in "${!NAMES[@]}"; do printf '%-16s %s\n' "${NAMES[$i]}" "${RESULTS[$i]}"; done
grep -h -E "power-loss:|process-crash:|stress:|doc-model:" build/long-release.log 2>/dev/null |
  sed 's/^[0-9]*: //' | sort -u
grep -h -E "tests passed|tests failed" build/fast-*.log build/long-release.log 2>/dev/null |
  sed "s/[[:space:]]*$//" | sort | uniq -c
exit $FAILED
