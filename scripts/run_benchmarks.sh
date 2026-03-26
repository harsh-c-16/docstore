#!/usr/bin/env bash
# Runs every benchmark workload and writes bench/results/<date>.md with the machine details.
# Usage: scripts/run_benchmarks.sh [num] [db_dir]
#   num     documents/keys per workload (default 1000000)
#   db_dir  where the benchmark database lives (default build/bench_db, on the same disk as the
#           repo; /tmp is often a RAM-backed tmpfs, which would hide fsync costs)
set -euo pipefail
cd "$(dirname "$0")/.."
NUM=${1:-1000000}
DB=${2:-$PWD/build/bench_db}
BENCH=./build/release/docstore_bench
OUT=bench/results/$(date +%F).md

cmake --preset release >/dev/null
cmake --build --preset release -j --target docstore_bench >/dev/null

run() {  # each invocation starts from an empty database directory
  rm -rf "$DB"
  echo "\$ $BENCH $* --db=$DB" >&2
  "$BENCH" "$@" --db="$DB" | grep -v -e '^| benchmark' -e '^|---'
}

ROWS=$(
  run --benchmarks=fillseq,readrandom,readmissing,readwhilewriting,scan --num="$NUM" --bloom_bits=10
  run --benchmarks=fillseq,readmissing --num="$NUM" --bloom_bits=0 | sed 's/^| \([a-z]*\) /| \1 (bloom off) /'
  run --benchmarks=fillrandom --num="$NUM"
  run --benchmarks=fillrandom --num=10000 --sync=1 | sed 's/^| fillrandom /| fillrandom (sync=1) /'
  run --benchmarks=overwrite --num="$NUM"
  run --benchmarks=doc_insert,find_collscan,find_indexed --num="$NUM"
)
rm -rf "$DB"

FS=$(df -T "$(dirname "$DB")" | awk 'NR==2 {print $2 " on " $1}')
DEV=$(lsblk -no PKNAME "$(df "$(dirname "$DB")" | awk 'NR==2 {print $1}')" 2>/dev/null | head -1)
MODEL=$( [ -n "$DEV" ] && lsblk -dno MODEL "/dev/$DEV" 2>/dev/null || echo unknown)
ROTA=$( [ -n "$DEV" ] && lsblk -dno ROTA "/dev/$DEV" 2>/dev/null || echo "?")
mkdir -p bench/results
{
  echo "# DocStore benchmark results, $(date +%F)"
  echo
  echo "## Machine"
  echo
  echo "| | |"
  echo "|---|---|"
  echo "| CPU | $(lscpu | sed -n 's/^Model name: *//p') |"
  echo "| Cores / threads | $(lscpu | sed -n 's/^Core(s) per socket: *//p') cores, $(nproc) threads |"
  echo "| RAM | $(free -h | awk '/^Mem:/ {print $2 " total, " $7 " available at start"}') |"
  echo "| Disk | $MODEL ($( [ "$ROTA" = 0 ] && echo SSD/NVMe || echo "rotational=$ROTA")), $FS |"
  echo "| OS | $(uname -sr) |"
  echo "| Compiler | $(c++ --version | head -1), CMake Release (-O3 -DNDEBUG), -std=c++20 |"
  echo
  echo "Single runs on a laptop with other applications open (no CPU pinning, frequency scaling"
  echo "on), so treat differences of ~10-20% as noise."
  echo
  echo "## Results (num=$NUM, value_size=200, block cache 8 MiB, memtable 4 MiB)"
  echo
  echo "| benchmark | ops | ops/s | MB/s | p50 us | p99 us | p99.9 us | notes |"
  echo "|---|---|---|---|---|---|---|---|"
  echo "$ROWS"
  echo
  echo "## How to reproduce"
  echo
  echo '```bash'
  echo "scripts/run_benchmarks.sh $NUM"
  echo "# which runs, each on an empty database directory:"
  echo "$BENCH --benchmarks=fillseq,readrandom,readmissing,readwhilewriting,scan --num=$NUM --bloom_bits=10 --db=<dir>"
  echo "$BENCH --benchmarks=fillseq,readmissing --num=$NUM --bloom_bits=0 --db=<dir>"
  echo "$BENCH --benchmarks=fillrandom --num=$NUM --db=<dir>"
  echo "$BENCH --benchmarks=fillrandom --num=10000 --sync=1 --db=<dir>"
  echo "$BENCH --benchmarks=overwrite --num=$NUM --db=<dir>"
  echo "$BENCH --benchmarks=doc_insert,find_collscan,find_indexed --num=$NUM --db=<dir>"
  echo '```'
} > "$OUT"
echo "wrote $OUT" >&2
