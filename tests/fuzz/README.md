# HTTP Parser fuzzing

The target is intentionally built in a separate Clang build tree because
libFuzzer owns `main()`. The `fuzz` preset enables AddressSanitizer and
UndefinedBehaviorSanitizer for both the driver and the gateway libraries.

```bash
cmake --preset fuzz
cmake --build --preset fuzz

# Deterministic short smoke run, suitable before a PR.  libFuzzer evolves its
# input corpus, so run it against a temporary copy rather than changing tracked
# seeds.
corpus_dir=$(mktemp -d)
cp -a tests/testdata/http/. "$corpus_dir"
./build/fuzz/tests/fuzz/pulsegate_http_parser_fuzz \
  "$corpus_dir" -dict=tests/fuzz/http_parser.dict \
  -artifact_prefix="$corpus_dir/" -runs=10000 -max_len=65536
rm -rf "$corpus_dir"

# Keep fuzzing until interrupted.  Preserve a crash artifact and add a
# deterministic regression before deleting the temporary corpus.
./build/fuzz/tests/fuzz/pulsegate_http_parser_fuzz \
  /path/to/writable-corpus -dict=tests/fuzz/http_parser.dict \
  -artifact_prefix=/path/to/writable-artifacts/
```

Any crash input must be minimized with libFuzzer and copied into
`tests/testdata/http/` together with a deterministic unit regression test.
The corpus contains only public, synthetic HTTP requests; do not add captured
production traffic or credentials.
