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

Unit tests: `./build/ninja_test`; end-to-end: `ln -s build/ninja ninja &&
python3 misc/output_test.py` (needs `script`, from util-linux-script).

Put the tree on a tmpfs (`mount -t tmpfs -o size=2500m tmpfs /mnt/tb`): the
VM's rootfs is virtiofs, where stat() is slow and very noisy, which is not
representative of a local disk with a warm page cache.

If the log formats change, run `bench.py rebuild` with the new binary before
measuring it.

Machines used:
- arm64: this VM, Apple M2 Max, 12 vCPUs, 16 GB RAM.
- x86-64: `ssh x86` companion VM, AMD EPYC, 16 vCPUs, 16 GB RAM. Copy the
  tree over with `tar cf - --exclude=.git . | ssh x86 'cd /root/ninja && tar xf -'`.

## Design notes

- `src/thread_pool.*`: process-wide pool (GetProcessorCount() threads incl.
  the caller); `TaskGroup`, `ParallelFor`, `RunTasksUntil`. Workers block
  all signals (the main loop relies on SIGCHLD/SIGINT interrupting ppoll).
- `src/char_scan.h`: `FindFirstOf<chars...>()` (NEON/SSE2/scalar); buffers
  need `kCharScanPadding` readable bytes after the terminating NUL.
- Manifest parser (`src/manifest_parser.cc`, see the overview comment
  there): files are lexed by a reimplementation of `lexer.in.cc` (which
  remains for dyndep files) and their effects on the State are recorded as
  items, merged in order on one thread. Subninjas are processed on other
  threads when no definitions follow them in the parent scopes. Error
  messages and positions are the same as before; the per-include-depth
  `ninja_required_version` state for `$^` is emulated.
- `DependencyScan::Prefetch()` (graph.cc): parallel stat + binding
  evaluation before the serial scan; only with thread-safe Stat() and
  without `-d explain`. It also summarizes each edge's deps-log deps
  without in-edges (dirty?, newest mtime) in `Edge::scan_deps_`; the
  serial scan uses that and only adds the deps to `inputs_` if the edge
  ends up not ready (`Edge::deps_pending_`). Summaries are invalidated
  when dyndep files are loaded (`dyndep_epoch_`).
- `.ninja_deps` is mmap()ed (read on Windows); `DepsLog::Deps::nodes` is
  a view mapping the ids stored in the file through the log's id->Node
  table, so loading doesn't allocate or convert per dependency.
- The logs are loaded while the manifest is parsed (NinjaMain::
  StartLoadingLogs), assuming the default location (no builddir) and
  without side effects; `DepsLog::Read()` is the State-independent half
  of `DepsLog::Load()`. Anything unusual falls back to a normal load.
- `State::paths_` is a `PathMap`: sharded (64 shards, mutex each) so nodes
  can be created concurrently. Parse tasks create the nodes for their paths
  (with a per-file cache in front), and the deps log creates its nodes while
  the manifest is parsed (after the top-level scope is done, which tells
  that builddir is empty). Nodes created out of order are "provisional":
  their first manifest reference in merge order sets their slash bits, and
  `default` treats them as nonexistent.
- Nodes and edges are never freed, and are allocated from per-thread
  blocks (state.cc AllocateForever). Parse tasks build complete Edge objects;
  the merge appends them (ids) and connects their nodes. The parsed files'
  temporary data is freed on a background thread (RunDetached).
- The deps log is loaded entirely while the manifest is parsed
  (DepsLog::LoadWithoutSideEffects, in an urgent task that skips the queue
  of parse tasks) unless that would have side effects (corrupt log).
- The deps log's nodes are stat()ed on other threads while the manifest is
  still parsed (DepsLog::StartEarlyStats, chunks of 256 re-queued so parse
  tasks interleave). Results stay in per-id arrays; Prefetch() applies them
  (DepsLog::TakeEarlyStat). RebuildManifest() forgets them before running
  any command.
- Leaf nodes loaded as deps don't get out-edges (flagged instead; dyndep
  recovers them if such a node gains an in-edge).

## Pitfalls found so far

- glibc grows heaps with an mprotect() per few pages; with many threads
  allocating that serializes everything on the mmap lock (osq_lock in the
  kernel profile). Hence M_TOP_PAD in main().
- Worker threads must block signals (see thread_pool.cc).
- Once there are threads, growing the fd table makes Linux wait for an RCU
  grace period each time (1000 opens: 0.3 ms single-threaded, 200 ms with
  a second thread). main() grows it up front. (Stat'ing relative to cached
  directory fds was tried: no gain once that is fixed.)
- The shell aliases cp/rm to -i: use `command cp`/`command rm -f`.
- `pkill -f` patterns can match the shell running them.

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
| baseline (upstream ninja)      |   670 |    636 |
| critical path: skip ready edges|   615 |    596 |
| no out-edges for leaf deps     |   539 |    517 |
| append-based evaluation        |   496 |    500 |
| parallel scan prefetch         |   428 |    329 |
| new lexer, parallel subninjas  |   327 |    251 |
| deps log: ids stay in the file |   291 |    209 |
| summarize leaf deps in prefetch|   215 |    139 |
| load logs while parsing        |   180 |    127 |
| concurrent node creation       |   175 |    113 |
| mallopt(M_TOP_PAD, 64MB)       |   173 |     92 |
| lazy previous elapsed times    |   175 |     89 |
| ReadFile without copies        |   177 |        |
| fast path for canonical paths  |   164 |        |
| edges built in parse tasks     |   159 |     85 |
| stat all deps-log nodes first  |   167 |        |
| faster $in lists (SIMD escape) |   161 |     81 |
| stat deps-log nodes while parsing |   118 |     74 |
| load deps log while parsing    |   118 |     71 |

Baseline breakdown (`ninja -d stats -n`, arm64 / x86-64 ms): manifest parse
~180 / 121, `.ninja_log` load 14 / 10, `.ninja_deps` load 72 / 65, node stat
84 / 165 (131k stats); the remainder is dependency scan bookkeeping, plan
setup and ParsePreviousElapsedTimes.
