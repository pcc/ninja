// Copyright 2026 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Time-to-first-command benchmark harness.
//
// ttfc [-n runs] -C builddir -t file-to-touch -- ninja [args...]
//
// For each run: touch the given file, start ninja in builddir with
// $BENCH_STAMP pointing at a stamp file, and wait for it to exit. The fake
// compiler (fakecc) writes its start time into the stamp file, so the
// difference between that time and the time ninja was spawned is the time to
// first command. Prints the per-run results and the min/median.
//
// As a calibration, the harness also measures the time it takes to spawn
// "/bin/sh -c fakecc" directly, which is the floor for any ninja (ninja runs
// commands through /bin/sh).

#define _GNU_SOURCE
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

static long long Now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int Cmp(const void* a, const void* b) {
  long long x = *(const long long*)a, y = *(const long long*)b;
  return x < y ? -1 : x > y;
}

static char stamp_path[4096];

// Spawns argv, waits for it, and returns the stamp time minus the spawn time,
// or -1 if no stamp was written. *total receives the total wall time.
static long long RunOnce(char** argv, long long* total) {
  unlink(stamp_path);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  long long t0 = Now();
  pid_t pid;
  if (posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ) != 0) {
    perror("posix_spawn");
    exit(1);
  }
  int status;
  waitpid(pid, &status, 0);
  *total = Now() - t0;
  posix_spawn_file_actions_destroy(&fa);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "ttfc: %s failed\n", argv[0]);
    exit(1);
  }
  FILE* f = fopen(stamp_path, "r");
  if (!f)
    return -1;
  long long stamp = -1;
  if (fscanf(f, "%lld", &stamp) != 1)
    stamp = -1;
  fclose(f);
  return stamp < 0 ? -1 : stamp - t0;
}

static void Report(const char* name, long long* v, int n) {
  qsort(v, n, sizeof(*v), Cmp);
  printf("%-8s min %8.2f ms  median %8.2f ms  max %8.2f ms\n", name,
         v[0] / 1e6, v[n / 2] / 1e6, v[n - 1] / 1e6);
}

int main(int argc, char** argv) {
  int runs = 10;
  const char* dir = NULL;
  const char* touch = NULL;
  const char* fakecc = NULL;
  int opt;
  while ((opt = getopt(argc, argv, "n:C:t:f:")) != -1) {
    switch (opt) {
    case 'n': runs = atoi(optarg); break;
    case 'C': dir = optarg; break;
    case 't': touch = optarg; break;
    case 'f': fakecc = optarg; break;
    default:
      fprintf(stderr,
              "usage: ttfc [-n runs] -C dir -t touchfile [-f fakecc] -- "
              "ninja [args...]\n");
      return 1;
    }
  }
  if (!dir || !touch || optind >= argc) {
    fprintf(stderr, "ttfc: need -C, -t and a command\n");
    return 1;
  }
  if (chdir(dir) < 0) {
    perror(dir);
    return 1;
  }
  char cwd[4096];
  if (!getcwd(cwd, sizeof(cwd)))
    return 1;
  snprintf(stamp_path, sizeof(stamp_path), "%s/.bench_stamp", cwd);
  setenv("BENCH_STAMP", stamp_path, 1);

  long long* ttfc = calloc(runs, sizeof(long long));
  long long* total = calloc(runs, sizeof(long long));

  if (fakecc) {
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "%s -o /dev/null", fakecc);
    char* sh_argv[] = { "/bin/sh", "-c", cmd, NULL };
    for (int i = 0; i < runs; ++i)
      ttfc[i] = RunOnce(sh_argv, &total[i]);
    Report("sh-floor", ttfc, runs);
  }

  for (int i = 0; i < runs; ++i) {
    if (utimensat(AT_FDCWD, touch, NULL, 0) < 0) {
      perror(touch);
      return 1;
    }
    ttfc[i] = RunOnce(argv + optind, &total[i]);
    if (ttfc[i] < 0) {
      fprintf(stderr, "ttfc: no command was run\n");
      return 1;
    }
    fprintf(stderr, "run %d: first command %.2f ms, total %.2f ms\n", i,
            ttfc[i] / 1e6, total[i] / 1e6);
  }
  Report("ttfc", ttfc, runs);
  Report("total", total, runs);
  return 0;
}
