#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/ipc_bench"
OUT="${1:-$ROOT/results/results.csv}"
REPEATS="${REPEATS:-5}"

mkdir -p "$(dirname "$OUT")"
if [[ ! -x "$BIN" ]]; then
  make -C "$ROOT"
fi

printf 'run,method,mode,message_size,iterations,elapsed_s,value_name,value\n' > "$OUT"

methods=(mmap-anon mmap-anon-spin mmap-file mmap-file-spin shm file pipe fifo socket mq)
sizes=(8 64 1024 4096 65536 1048576)

iters_for() {
  local mode="$1" size="$2"
  if [[ "$mode" == latency ]]; then
    if (( size <= 4096 )); then echo 20000; else echo 2000; fi
  else
    if (( size <= 4096 )); then echo 50000; elif (( size <= 65536 )); then echo 10000; else echo 1000; fi
  fi
}

for mode in latency throughput; do
  for size in "${sizes[@]}"; do
    for method in "${methods[@]}"; do
      # Typical Linux POSIX MQ msgsize_max is 8192; skip unsupported larger messages.
      if [[ "$method" == mq && "$size" -gt 8192 ]]; then
        continue
      fi
      iters="$(iters_for "$mode" "$size")"
      for ((r=1; r<=REPEATS; r++)); do
        line="$($BIN "$method" "$mode" "$size" "$iters" 2>/dev/null || true)"
        [[ -z "$line" ]] && continue
        method_v="$(sed -n 's/.*method=\([^,]*\).*/\1/p' <<<"$line")"
        elapsed="$(sed -n 's/.*elapsed_s=\([^,]*\).*/\1/p' <<<"$line")"
        if [[ "$mode" == latency ]]; then
          val="$(sed -n 's/.*latency_ns=\([^,]*\).*/\1/p' <<<"$line")"
          name="latency_ns"
        else
          val="$(sed -n 's/.*throughput_MiB_s=\([^,]*\).*/\1/p' <<<"$line")"
          name="throughput_MiB_s"
        fi
        printf '%d,%s,%s,%s,%s,%s,%s,%s\n' "$r" "$method_v" "$mode" "$size" "$iters" "$elapsed" "$name" "$val" >> "$OUT"
      done
    done
  done
done

echo "Wrote $OUT"
