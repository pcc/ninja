#!/usr/bin/env python3
# Copyright 2026 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Compares two ninja binaries on a benchmark tree (see bench.py).

  compare.py [--rounds N] [--build-dir D] [--cpu] DIR NINJA_A NINJA_B

Runs the binaries alternately (A B A B ...), 5 runs at a time, since the
noise between batches is larger than the differences of interest, and
prints the median, 25th percentile and minimum time to first command of
each.  With --cpu, measures the CPU time (user + system) of 'ninja -n'
runs instead, which is less noisy on machines that are short of cores.
"""

import argparse
import os
import re
import resource
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def ttfc_runs(root, build_dir, ninja, runs):
    out = subprocess.run(
        [sys.executable, os.path.join(HERE, 'bench.py'), 'run', '-n',
         str(runs), '--build-dir', build_dir, '--ninja', ninja, root],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True).stdout
    return [float(m) for m in re.findall(rb'first command ([0-9.]+) ms', out)]


def cpu_runs(root, build_dir, ninja, runs):
    out_dir = os.path.join(root, build_dir)
    # The file to touch is relative to the build directory.
    touch = os.path.join(out_dir,
                         open(os.path.join(root, 'touch_file')).read().strip())
    times = []
    for _ in range(runs):
        os.utime(touch)
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        subprocess.run([ninja, '-C', out_dir, '-n'], stdout=subprocess.DEVNULL,
                       check=True)
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        times.append(1000 * (after.ru_utime - before.ru_utime +
                             after.ru_stime - before.ru_stime))
    return times


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rounds', type=int, default=8)
    parser.add_argument('--build-dir', default='out/Default')
    parser.add_argument('--cpu', action='store_true')
    parser.add_argument('dir')
    parser.add_argument('a')
    parser.add_argument('b')
    args = parser.parse_args()
    root = os.path.abspath(args.dir)
    binaries = [os.path.abspath(args.a), os.path.abspath(args.b)]
    measure = cpu_runs if args.cpu else ttfc_runs
    results = [[], []]
    for _ in range(args.rounds):
        for i, ninja in enumerate(binaries):
            results[i] += measure(root, args.build_dir, ninja, 5)
    what = 'cpu' if args.cpu else 'ttfc'
    for name, times in zip('AB', results):
        times.sort()
        print('%s %s median %.1f ms  p25 %.1f  min %.1f (n=%d)' %
              (name, what, statistics.median(times), times[len(times) // 4],
               times[0], len(times)))


if __name__ == '__main__':
    main()
