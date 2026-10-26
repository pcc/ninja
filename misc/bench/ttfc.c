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
// commands through /bin/sh).  On Windows, where ninja runs commands without
// a shell, it measures spawning fakecc instead.

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#else
#define _GNU_SOURCE
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
// The same clock as fakecc's.
static long long Now(void) {
  LARGE_INTEGER count, frequency;
  QueryPerformanceCounter(&count);
  QueryPerformanceFrequency(&frequency);
  return (long long)(count.QuadPart / frequency.QuadPart) * 1000000000LL +
         (long long)(count.QuadPart % frequency.QuadPart) * 1000000000LL /
             frequency.QuadPart;
}
#else
extern char** environ;

static long long Now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
#endif

// Sets the mtime of a file to now.
static int Touch(const char* path) {
#ifdef _WIN32
  // (_utime() has a resolution of seconds.)
  HANDLE file = CreateFileA(path, FILE_WRITE_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE)
    return -1;
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  BOOL ok = SetFileTime(file, NULL, NULL, &now);
  CloseHandle(file);
  return ok ? 0 : -1;
#else
  return utimensat(AT_FDCWD, path, NULL, 0);
#endif
}

static int Cmp(const void* a, const void* b) {
  long long x = *(const long long*)a, y = *(const long long*)b;
  return x < y ? -1 : x > y;
}

static char stamp_path[4096];

#ifdef _WIN32
// Appends \a arg to \a cmd, quoted the way the C runtime parses it.
static void AppendArg(char* cmd, size_t size, const char* arg) {
  size_t len = strlen(cmd);
  if (len)
    cmd[len++] = ' ';
  int quote = !*arg || strpbrk(arg, " \t\"") != NULL;
  if (quote)
    cmd[len++] = '"';
  for (const char* p = arg; *p && len + 4 < size; ++p) {
    size_t backslashes = 0;
    while (*p == '\\') {
      ++backslashes;
      ++p;
    }
    if (*p == '"' || (!*p && quote))
      backslashes *= 2;
    while (backslashes-- && len + 4 < size)
      cmd[len++] = '\\';
    if (!*p)
      break;
    if (*p == '"')
      cmd[len++] = '\\';
    cmd[len++] = *p;
  }
  if (quote)
    cmd[len++] = '"';
  cmd[len] = '\0';
}

// Spawns argv, waits for it, and returns the stamp time minus the spawn time,
// or -1 if no stamp was written. *total receives the total wall time.
static long long RunOnce(char** argv, long long* total) {
  _unlink(stamp_path);
  static char cmd[32768];
  cmd[0] = '\0';
  for (char** arg = argv; *arg; ++arg)
    AppendArg(cmd, sizeof(cmd), *arg);
  SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
  HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit,
                           OPEN_EXISTING, 0, NULL);
  STARTUPINFOA startup;
  memset(&startup, 0, sizeof(startup));
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = nul;
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION process;
  long long t0 = Now();
  if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &startup,
                      &process)) {
    fprintf(stderr, "ttfc: CreateProcess(%s) failed: %lu\n", cmd,
            GetLastError());
    exit(1);
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  *total = Now() - t0;
  DWORD status = 1;
  GetExitCodeProcess(process.hProcess, &status);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  CloseHandle(nul);
  if (status != 0) {
    fprintf(stderr, "ttfc: %s failed\n", argv[0]);
    exit(1);
  }
#else
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
#endif
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
  int i = 1;
  for (; i < argc; ++i) {
    if (strcmp(argv[i], "--") == 0) {
      ++i;
      break;
    }
    if (argv[i][0] != '-')
      break;
    if (i + 1 < argc && strcmp(argv[i], "-n") == 0) {
      runs = atoi(argv[++i]);
    } else if (i + 1 < argc && strcmp(argv[i], "-C") == 0) {
      dir = argv[++i];
    } else if (i + 1 < argc && strcmp(argv[i], "-t") == 0) {
      touch = argv[++i];
    } else if (i + 1 < argc && strcmp(argv[i], "-f") == 0) {
      fakecc = argv[++i];
    } else {
      fprintf(stderr,
              "usage: ttfc [-n runs] -C dir -t touchfile [-f fakecc] -- "
              "ninja [args...]\n");
      return 1;
    }
  }
  if (!dir || !touch || i >= argc) {
    fprintf(stderr, "ttfc: need -C, -t and a command\n");
    return 1;
  }
#ifdef _WIN32
  if (_chdir(dir) < 0) {
    perror(dir);
    return 1;
  }
  char cwd[4096];
  if (!_getcwd(cwd, sizeof(cwd)))
    return 1;
  snprintf(stamp_path, sizeof(stamp_path), "%s\\.bench_stamp", cwd);
  SetEnvironmentVariableA("BENCH_STAMP", stamp_path);
#else
  if (chdir(dir) < 0) {
    perror(dir);
    return 1;
  }
  char cwd[4096];
  if (!getcwd(cwd, sizeof(cwd)))
    return 1;
  snprintf(stamp_path, sizeof(stamp_path), "%s/.bench_stamp", cwd);
  setenv("BENCH_STAMP", stamp_path, 1);
#endif

  long long* ttfc = calloc(runs, sizeof(long long));
  long long* total = calloc(runs, sizeof(long long));

  if (fakecc) {
#ifdef _WIN32
    // Ninja starts commands without a shell on Windows.
    char* floor_argv[] = { (char*)fakecc, "-o", "NUL", NULL };
    const char* floor_name = "spawn-floor";
#else
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "%s -o /dev/null", fakecc);
    char* floor_argv[] = { "/bin/sh", "-c", cmd, NULL };
    const char* floor_name = "sh-floor";
#endif
    for (int j = 0; j < runs; ++j)
      ttfc[j] = RunOnce(floor_argv, &total[j]);
    Report(floor_name, ttfc, runs);
  }

  char** command = argv + i;
  for (i = 0; i < runs; ++i) {
    if (Touch(touch) < 0) {
      fprintf(stderr, "ttfc: can't touch %s\n", touch);
      return 1;
    }
    ttfc[i] = RunOnce(command, &total[i]);
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
