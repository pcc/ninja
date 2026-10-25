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
- `gen_bench.py` – deterministic generator of a GN/Chromium-shaped tree
  (out/Default; also the same build CMake-style in out/CMake):
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
- `compare.py` – A/B comparison of two binaries, interleaved (A B A B ...)
  in batches of 5 runs; `--cpu` compares the CPU time of `ninja -n` runs,
  which is steadier when the machine is short of cores.

Usage:

    misc/bench/bench.py setup --ninja build/ninja /mnt/tb/bench
    misc/bench/bench.py run -n 10 --ninja build/ninja /mnt/tb/bench
    misc/bench/compare.py --rounds 12 /mnt/tb/bench /tmp/ninja-prev build/ninja

Unit tests: `./build/ninja_test`; end-to-end: `ln -s build/ninja ninja &&
python3 misc/output_test.py` (needs `script`, from util-linux-script).

Put the tree on a tmpfs (`mount -t tmpfs -o size=2500m tmpfs /mnt/tb`): the
VM's rootfs is virtiofs, where stat() is slow and very noisy, which is not
representative of a local disk with a warm page cache.

If the log formats change, run `bench.py rebuild` with the new binary before
measuring it.

The CMake build links ninja statically on Linux when it can (NINJA_STATIC;
needs `dnf install glibc-static libstdc++-static`, done on both machines):
that saves 2-4 ms of process startup.

Machines used:
- arm64: this VM, Apple M2 Max, 12 vCPUs, 16 GB RAM.
- x86-64: `ssh x86` companion VM, AMD EPYC, 16 vCPUs, 16 GB RAM. Copy the
  tree over with `tar cf - --exclude=.git . | ssh x86 'cd /root/ninja && tar xf -'`.

## Design notes

- `src/thread_pool.*`: process-wide pool (GetProcessorCount() threads incl.
  the caller); `TaskGroup`, `ParallelFor`, `RunTasksUntil`. A FIFO queue:
  `TaskGroup::Wait()` runs whatever is queued, but `ParallelFor()` only
  waits for its own chunks (running unrelated tasks there delayed the
  critical path by milliseconds). Workers block
  all signals (the main loop relies on SIGCHLD/SIGINT interrupting ppoll).
- `src/char_scan.h`: `FindFirstOf<chars...>()` (NEON/SSE2/scalar); buffers
  need `kCharScanPadding` readable bytes after the terminating NUL.
- Manifest parser (`src/manifest_parser.cc`, see the overview comment
  there): files are lexed by a reimplementation of `lexer.in.cc` (which
  remains for dyndep files) and their effects on the State are recorded as
  items, merged in order on one thread. Subninjas are processed on other
  threads when no definitions follow them in the parent scopes, and the
  rest of a big file (>256 KB) after its last definition is split into
  ~128 KB chunks at "build" lines (after an unescaped newline) that are
  processed on other threads. Error
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
- `State::paths_` is a `PathMap`: sharded (256 shards, spin lock each) so nodes
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
  (DepsLog::CreateNodes + LoadWithoutSideEffects) unless that would have
  side effects (corrupt log). This starts right away, assuming no builddir;
  if the manifest sets one, DepsLog::UndoSpeculativeLoad() removes the
  (still provisional) nodes it created and forgets the log.
  `DepsLog::Read()` walks the records in parallel: finding a record needs
  the previous one, so a serial walk is a chain of cache misses (~9 ms for
  63 MB). Chunks are walked from the first position where 8 records in a
  row look valid (consecutive checksums), and joined where the true chain
  meets them (else it's walked serially). The checksum and id checks run
  in parallel afterwards; `DepsLogTest.BigLogCorruptions` compares the
  result with a serial reference.
- The deps log's nodes are stat()ed on other threads while the manifest is
  still loaded (DepsLog::StartEarlyStats, one task per thread, chunks of 256
  re-queued so other tasks interleave). Results stay in per-id arrays; Prefetch() applies them
  (DepsLog::TakeEarlyStat). RebuildManifest() forgets them before running
  any command.
- `BindingEnv` stores bindings as (key, value) StringPieces in a small
  vector (indexed once there are more than 16); the parser adds literal
  values as views into the manifest text, which `State::manifest_contents_`
  keeps alive. Rules have a hash index (CMake has two rules per target).
- Manifest cache (`src/manifest_cache.*`, `.ninja_manifest_cache` next to
  the logs, only without builddir and not for tools): after a parse without
  warnings, the graph (nodes, scopes, rules, pools, edges, defaults) is
  saved with the status (dev, ino, size, mtime, ctime) of every manifest
  file; it's used while they all have the same status. Files changed less
  than 2 s before saving make it skip saving (timestamp granularity).
  Records are fixed-size arrays used in place (checked in parallel first;
  nodes have their path hash and their in/out edges, so they're connected
  in parallel by node),
  strings are deduplicated (the CMake cache is 22 MB, not 120). Loading
  takes ~25 ms of wall time on arm64 (node creation dominates); saving
  costs ~50 ms on a cache miss. The cache also has what the scan needs
  from each edge's bindings (`Edge::ScanBindings`: restat, generator,
  deps, depfile, command hash), which only depend on the manifest; the
  prefetch then doesn't evaluate them. `ManifestCacheTest.External` compares a
  cache-loaded graph with a parsed one for any manifest (see the test).
- Simple commands run without /bin/sh (subprocess-posix.cc
  SplitSimpleCommand, glibc only): words of characters that aren't special
  to the shell, with quotes and backslashes removed as the shell would, the
  first word a path containing '/' (so no builtins, keywords, assignments,
  PATH lookup). Anything else, or a failed posix_spawn() (missing program,
  script without #!), goes through the shell. Starting bash costs 4-7 ms.
  $PWD is made right (ninja -C) as the shell would; the only remaining
  differences in the environment are bash's `_` (and SHLVL when unset).
  `SubprocessTest.CommandsWithAndWithoutShell` compares the arguments with
  the shell's.
- Leaf nodes loaded as deps don't get out-edges (flagged instead; dyndep
  recovers them if such a node gains an in-edge).

## Where the time goes now (GN tree)

Until the dirty scan, x86-64 is CPU-bound: ~380 ms of CPU per run (16
vCPUs, where syscalls and page faults cost more); arm64 uses ~420 ms (12
vCPUs). About 35-40% of it is the ~133k stat() calls (the floor: ~1 us
each on x86-64, ~0.6 us on arm64); the rest is loading the
manifest cache and the logs (node creation, ~27k page faults), and the
prefetch's deps summaries. Timeline on x86-64 (ms): loads done ~15, the
deps-log ids are all stat()ed at ~25 (that gates everything), edges pass
~2.5, serial scan ~3.4, plan ~0.7, then the command starts (~2 ms to
exec). (Timelines like this came from a copy of the tree with timestamps
printed at those points, and phases of CPU time from `perf record -g` of
`ninja -n` runs, grouped by the function they're under.)

Tried without gain (or worse): starting the early stats right after reading
the deps log (they compete with the loads), urgent ParallelFor helpers,
spinning thread-pool workers, io_uring statx (much more CPU), stat()ing
relative to cached directory handles (x86-64: -7% CPU but at best -0.8 ms
with 3000 handles, which needs a higher RLIMIT_NOFILE; nothing on arm64),
MAP_POPULATE
or THP for node/edge blocks, a smaller memory footprint (BindingEnv rules
out of line, 16-byte deps-log records, depfile views), parallelizing the
deps log's id assignment, and a parallel bottom-up dirty scan (synchronizing
the ~480k edge-to-edge dependencies, mostly order-only, costs more than the
serial scan).

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
| (chunks, mapped manifests)     |   121 |     72 |
| bindings as views, flat envs   |   115 |     70 |
| canonical paths not copied     |   111 |     69 |
| (small cleanups: log entries, name masks, command buffer) |   111 |     67 |
| spin locks for the node map    |   107 |     62 |
| manifest cache                 |   107 |     58 |
| parallel deps log walk         |   102 |     55 |
| scan bindings in the cache     |    89 |     52 |
| (parallel build log parse)     |    84 |   (~0) |
| nodes' edges in the cache      |    84 |     50 |
| ParallelFor waits, doesn't steal |    79 |     49 |
| parallel edge collection       |    85 |   48.6 |
| early stats on all threads     |    87 |     47 |
| fast path for deps summaries   |    86 |     46 |
| static linking (CMake, Linux)  |    83 |     43 |
| simple commands without /bin/sh|    70 |     35 |
| 256 node map shards            |    73 |   34.9 |
| leaner serial scan loop        |    65 |   34.6 |
| nodes made outside map locks   |    71 |   34.4 |

CMake-style build (`--build-dir out/CMake`: one 125 MB build.ninja, same
graph):

| Change                         | arm64 | x86-64 |
|--------------------------------|------:|-------:|
| baseline (upstream ninja)      |   817 |    757 |
| (all of the above)             |   315 |    284 |
| process big files in chunks    |   225 |    185 |
| map big manifests, no copies   |   108 |     82 |
| bindings as views, flat envs   |   101 |     73 |
| (small cleanups since)         |    76 |     69 |
| manifest cache                 |    75 |     55 |
| (deps walk, scan bindings)     |    71 |     51 |
| (all since, before no-shell)   |    58 |     42 |
| simple commands without /bin/sh|    52 |     35 |
| rules made in parallel (cache) |    46 |   32.8 |

Baseline breakdown (`ninja -d stats -n`, arm64 / x86-64 ms): manifest parse
~180 / 121, `.ninja_log` load 14 / 10, `.ninja_deps` load 72 / 65, node stat
84 / 165 (131k stats); the remainder is dependency scan bookkeeping, plan
setup and ParsePreviousElapsedTimes.
