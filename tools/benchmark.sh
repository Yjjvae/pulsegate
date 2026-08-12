#!/usr/bin/env bash
# Run a reproducible local /healthz baseline. Results are raw evidence, not a
# cross-machine performance claim; see docs/benchmarks/v0.9.3-healthz-baseline.md.
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/benchmark.sh [options]

Options:
  --workers LIST       Comma-separated io_context worker counts (default: 1,2,4,8)
  --trials N           Measured trials per worker count (default: 3)
  --connections N      wrk connections (default: 100)
  --load-threads N     wrk threads (default: 2)
  --warmup DURATION    wrk warmup duration, e.g. 5s (default: 5s)
  --duration DURATION  wrk measured duration, e.g. 15s (default: 15s)
  --port N             Loopback port (default: 18082)
  --results-dir PATH   Output directory (default: benchmarks/results/<timestamp>)
  --help               Show this help

Environment:
  WRK_BIN              Path to wrk (default: wrk from PATH)
  PULSEGATE_BIN        Release server (default: build/release/app/pulsegate)

The script generates a temporary YAML configuration with logging set to
critical. This prevents access-log output from becoming the measured bottleneck.
EOF
}

workers='1,2,4,8'
trials=3
connections=100
load_threads=2
warmup='5s'
duration='15s'
port=18082
results_dir=''

while (($# > 0)); do
  case "$1" in
    --workers) workers="$2"; shift 2 ;;
    --trials) trials="$2"; shift 2 ;;
    --connections) connections="$2"; shift 2 ;;
    --load-threads) load_threads="$2"; shift 2 ;;
    --warmup) warmup="$2"; shift 2 ;;
    --duration) duration="$2"; shift 2 ;;
    --port) port="$2"; shift 2 ;;
    --results-dir) results_dir="$2"; shift 2 ;;
    --help) usage; exit 0 ;;
    *) printf 'Unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done

for value in "$trials" "$connections" "$load_threads" "$port"; do
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || { printf 'Expected positive integer: %s\n' "$value" >&2; exit 2; }
done

wrk_bin="${WRK_BIN:-wrk}"
pulsegate_bin="${PULSEGATE_BIN:-build/release/app/pulsegate}"
command -v "$wrk_bin" >/dev/null || { printf 'wrk not found: %s\n' "$wrk_bin" >&2; exit 2; }
[[ -x "$pulsegate_bin" ]] || { printf 'Release server not executable: %s\n' "$pulsegate_bin" >&2; exit 2; }

if [[ -z "$results_dir" ]]; then
  results_dir="benchmarks/results/$(date -u +%Y%m%dT%H%M%SZ)"
fi
mkdir -p "$results_dir"
results_dir=$(cd "$results_dir" && pwd)
config_path="$results_dir/server.yaml"
server_log="$results_dir/server.log"
server_pid=''
sampler_pid=''

shutdown_server() {
  if [[ -n "$sampler_pid" ]] && kill -0 "$sampler_pid" 2>/dev/null; then
    kill "$sampler_pid" 2>/dev/null || true
    wait "$sampler_pid" 2>/dev/null || true
  fi
  sampler_pid=''
  if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  server_pid=''
}
trap shutdown_server EXIT INT TERM

sample_server() {
  local output_path="$1"
  printf 'timestamp_utc cpu_percent_since_start rss_kib\n' >"$output_path"
  while kill -0 "$server_pid" 2>/dev/null; do
    printf '%s ' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$output_path"
    ps -o %cpu=,rss= -p "$server_pid" | awk 'NF == 2 {print $1, $2}' >>"$output_path"
    sleep 1
  done
}

{
  printf 'date_utc=%s\n' "$(date -u --iso-8601=seconds)"
  printf 'git_commit=%s\n' "$(git rev-parse HEAD)"
  printf 'git_dirty=%s\n' "$(git status --porcelain | wc -l)"
  printf 'compiler=%s\n' "$(c++ --version | head -1)"
  printf 'cmake=%s\n' "$(cmake --version | head -1)"
  printf 'kernel=%s\n' "$(uname -srmo)"
  printf 'cpu=%s\n' "$(lscpu | awk -F: '/Model name/ {gsub(/^ +/, "", $2); print $2}')"
  printf 'logical_cores=%s\n' "$(getconf _NPROCESSORS_ONLN)"
  printf 'memory=%s\n' "$(free -h | awk '/^Mem:/ {print $2}')"
  printf 'wrk=%s\n' "$("$wrk_bin" --version 2>&1 | head -1)"
  printf 'server_binary=%s\n' "$pulsegate_bin"
  printf 'workers=%s\ntrials=%s\nconnections=%s\nload_threads=%s\nwarmup=%s\nduration=%s\nport=%s\n' \
    "$workers" "$trials" "$connections" "$load_threads" "$warmup" "$duration" "$port"
} >"$results_dir/environment.txt"

IFS=',' read -r -a worker_counts <<<"$workers"
for worker_count in "${worker_counts[@]}"; do
  [[ "$worker_count" =~ ^[1-9][0-9]*$ ]] || { printf 'Invalid worker count: %s\n' "$worker_count" >&2; exit 2; }
  cat >"$config_path" <<EOF
server:
  listen_host: 127.0.0.1
  listen_port: $port
  io_threads: $worker_count
logging:
  level: critical
  format: json
EOF

  "$pulsegate_bin" --config "$config_path" >"$server_log" 2>&1 &
  server_pid=$!
  for _ in {1..100}; do
    if curl --noproxy '*' --fail --silent "http://127.0.0.1:$port/healthz" >/dev/null; then break; fi
    sleep 0.05
  done
  if ! kill -0 "$server_pid" 2>/dev/null || ! curl --noproxy '*' --fail --silent "http://127.0.0.1:$port/healthz" >/dev/null; then
    printf 'Server did not become ready; inspect %s\n' "$server_log" >&2
    exit 1
  fi

  "$wrk_bin" -t"$load_threads" -c"$connections" -d"$warmup" --latency \
    "http://127.0.0.1:$port/healthz" >"$results_dir/workers-${worker_count}-warmup.txt"
  sample_server "$results_dir/workers-${worker_count}-server-stats.txt" &
  sampler_pid=$!
  for trial in $(seq 1 "$trials"); do
    "$wrk_bin" -t"$load_threads" -c"$connections" -d"$duration" --latency \
      "http://127.0.0.1:$port/healthz" >"$results_dir/workers-${worker_count}-trial-${trial}.txt"
  done
  shutdown_server
done

printf 'Benchmark data written to %s\n' "$results_dir"
