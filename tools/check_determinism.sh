#!/usr/bin/env bash
# The parallel enemy update must give bit-identical results to the sequential
# one. There's no cross-enemy dependency in the update, so chunking the range
# differently must not change a single float. If this ever fails, parallelFor
# has broken the simulation.
#
# One run per process on purpose: Game.cpp keeps its RNG in a file-static that
# init() doesn't reseed, so two Games in one process start from different
# random streams and the comparison would be meaningless.
set -euo pipefail
BIN="${1:-./build/lf_playtest}"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

"$BIN" --dump-state --sequential > "$tmp/seq.bin"
"$BIN" --dump-state              > "$tmp/par.bin"

if cmp -s "$tmp/seq.bin" "$tmp/par.bin"; then
    echo "PASS - parallel path is bit-identical to sequential ($(wc -c < "$tmp/seq.bin") bytes)"
    exit 0
fi
echo "FAIL - parallel and sequential diverged"
cmp -l "$tmp/seq.bin" "$tmp/par.bin" | head -20
exit 1
