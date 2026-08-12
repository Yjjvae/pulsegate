# Benchmark assets

`tools/benchmark.sh` starts a Release PulseGate instance with access logging
disabled, warms it up, and stores raw `wrk` output plus the environment in a
timestamped `results/` directory. The results are intentionally ignored: copy
the relevant numbers and full methodology into a versioned document under
`docs/benchmarks/`.

Run the Chapter 22 worker comparison after building Release:

```bash
cmake --preset release
cmake --build --preset release
tools/benchmark.sh --workers 1,2,4,8 --trials 3 \
  --connections 100 --load-threads 2 --warmup 5s --duration 15s
```

Set `WRK_BIN=/path/to/wrk` when `wrk` is not on `PATH`. Do not compare results
from different machines, kernels, compiler versions, or server configurations.
