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

// A fake compiler/archiver/linker for the time-to-first-command benchmark.
//
// fakecc [-MF depfile] [-c source] -o output [ignored args...]
//
// - If $BENCH_STAMP is set and names a file that does not exist yet, the
//   CLOCK_MONOTONIC (on Windows, QueryPerformanceCounter()) time at which this
//   process started is written to it, in nanoseconds (so the first command of
//   a build records its start time).
// - If -c and -MF are given, the source file is read and every line of the
//   form '#include "path"' contributes 'path' to the depfile, in the format
//   that clang/gcc -MMD would write.
// - The -o output is created/truncated.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define open _open
#define write _write
#define close _close
#else
#include <time.h>
#include <unistd.h>
#endif

static void Stamp(void) {
#ifdef _WIN32
  // The same clock as ttfc's (QueryPerformanceCounter(), in nanoseconds).
  LARGE_INTEGER count, frequency;
  QueryPerformanceCounter(&count);
  QueryPerformanceFrequency(&frequency);
  long long now =
      (long long)(count.QuadPart / frequency.QuadPart) * 1000000000LL +
      (long long)(count.QuadPart % frequency.QuadPart) * 1000000000LL /
          frequency.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  long long now = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
  const char* stamp = getenv("BENCH_STAMP");
  if (!stamp)
    return;
  int fd = open(stamp, O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0)
    return;
  char buf[64];
  int len = snprintf(buf, sizeof(buf), "%lld\n", now);
  if (write(fd, buf, len) != len)
    perror("write");
  close(fd);
}

int main(int argc, char** argv) {
  Stamp();
  const char* depfile = NULL;
  const char* source = NULL;
  const char* output = NULL;
  for (int i = 1; i < argc; ++i) {
    if (i + 1 < argc && strcmp(argv[i], "-MF") == 0)
      depfile = argv[++i];
    else if (i + 1 < argc && strcmp(argv[i], "-c") == 0)
      source = argv[++i];
    else if (i + 1 < argc && strcmp(argv[i], "-o") == 0)
      output = argv[++i];
  }
  if (!output) {
    fprintf(stderr, "fakecc: no -o\n");
    return 1;
  }
  if (depfile && source) {
    FILE* in = fopen(source, "r");
    if (!in) {
      perror(source);
      return 1;
    }
    FILE* out = fopen(depfile, "w");
    if (!out) {
      perror(depfile);
      return 1;
    }
    fprintf(out, "%s: %s", output, source);
    char line[4096];
    while (fgets(line, sizeof(line), in)) {
      if (strncmp(line, "#include \"", 10) != 0)
        continue;
      char* end = strchr(line + 10, '"');
      if (!end)
        continue;
      *end = '\0';
      fprintf(out, " \\\n  %s", line + 10);
    }
    fputc('\n', out);
    fclose(in);
    fclose(out);
  }
  int fd = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    perror(output);
    return 1;
  }
  close(fd);
  return 0;
}
