# AGENTS.md

Notes for agents (and humans) working on this tree.

## Goal of the current work

Make ninja's **time to first command** as small as possible on arm64 and
x86-64: for a large, realistic build directory where one source file has been
touched (deps=gcc depfiles, large `.ninja_deps`), how long from exec'ing ninja
until the first build command is started. The `simd/` directory contains a
standalone prototype (`ninja-simd.cpp`) with ideas (SIMD lexing, parallel
parsing, parallel stat) that may be borrowed.

Constraints:
- `.ninja_log` / `.ninja_deps` formats may change.
- Error messages may be dropped for constructs we can avoid parsing.
- Internal data structures may change.
- All other user-visible behavior must stay the same.
- One commit per improvement; keep this file's results table current.

## Building

    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
    ./build/ninja_test           # unit tests
    python3 misc/output_test.py  # needs ./ninja in the repo root or $PATH

(`./configure.py --bootstrap` also works.)

## Benchmark

Tools live in `misc/bench/`:
- `gen_bench.py` – deterministic generator of a GN/Chromium-shaped tree:
  ~2000 targets, ~42k compile steps, one subninja per target, ~39 MB of
  manifests, ~300 headers per source (13M deps entries, ~60 MB
  `.ninja_deps`), a GN-style generator rule with `build.ninja.d`.
- `fakecc.c` – fake compiler/linker: writes a depfile from the source's
  `#include` lines, creates the output, and records its start time in
  `$BENCH_STAMP` if that file does not exist yet.
- `ttfc.c` – harness: touches a source, spawns ninja, reads the stamp and
  reports time to first command (and total time). `sh-floor` is the time to
  spawn `/bin/sh -c fakecc` directly, i.e. the floor for any ninja.
- `bench.py` – driver: `setup` (generate + full build), `rebuild` (redo the
  full build, e.g. after a log format change), `run`.

Usage:

    misc/bench/bench.py setup --ninja build/ninja /mnt/tb/bench
    misc/bench/bench.py run -n 10 --ninja build/ninja /mnt/tb/bench

Put the tree on a tmpfs (`mount -t tmpfs -o size=2500m tmpfs /mnt/tb`): the
VM's rootfs is virtiofs, where stat() is slow and very noisy, which is not
representative of a local disk with a warm page cache.

If the log formats change, run `bench.py rebuild` with the new binary before
measuring it.

Machines used:
- arm64: this VM, Apple M2 Max, 12 vCPUs, 16 GB RAM.
- x86-64: `ssh x86` companion VM, AMD EPYC, 16 vCPUs, 16 GB RAM. Copy the
  tree over with `tar cf - --exclude=.git . | ssh x86 'cd /root/ninja && tar xf -'`.

## Results

Median time to first command (ms), tree on tmpfs. Run-to-run noise is
large (medians move by ±5 ms on arm64, ±0.5 ms on x86-64), so compare
builds with interleaved runs (A B A B ...) rather than back-to-back
batches. The arm64 column comes from one interleaved run of all the
commits' binaries (40-130 runs each); the x86-64 one from comparisons
against the previous commit, made over several sessions, so its rows
don't compare exactly.

| Change                         | arm64 | x86-64 |
|--------------------------------|------:|-------:|
| sh-floor                       |    13 |      9 |
| baseline (upstream ninja)      |   670 |    626 |

Baseline breakdown (`ninja -d stats -n`, arm64 / x86-64 ms): manifest parse
~180 / 121, `.ninja_log` load 14 / 10, `.ninja_deps` load 72 / 65, node stat
84 / 165 (131k stats); the remainder is dependency scan bookkeeping, plan
setup and ParsePreviousElapsedTimes.
