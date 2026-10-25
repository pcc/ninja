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

"""Time-to-first-command benchmark driver.

  bench.py setup [--targets N] [--ninja PATH] DIR
      Generates the build tree in DIR (see gen_bench.py), builds the helper
      tools and does a full build with the given ninja, producing
      .ninja_log and .ninja_deps in the format of that ninja.

  bench.py rebuild [--ninja PATH] DIR
      Redoes only the full build (for when the log formats changed).

  bench.py run [-n RUNS] [--ninja PATH] DIR [-- extra ninja args]
      Touches one source file and measures the time until ninja starts the
      first command, RUNS times.
"""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def build_tools(root: str) -> None:
    tools = os.path.join(root, 'tools')
    os.makedirs(tools, exist_ok=True)
    cc = os.environ.get('CC', 'cc')
    for t in ['fakecc', 'ttfc']:
        subprocess.check_call([cc, '-O2', '-o', os.path.join(tools, t),
                               os.path.join(HERE, t + '.c')])


def full_build(root: str, ninja: str) -> None:
    out = os.path.join(root, 'out/Default')
    for f in ['.ninja_log', '.ninja_deps']:
        if os.path.exists(os.path.join(out, f)):
            os.unlink(os.path.join(out, f))
    for d in ['obj', 'gen']:
        objs = os.path.join(out, d)
        # Keep the subninjas under obj/, remove everything else.
        for dirpath, _, files in os.walk(objs):
            for f in files:
                if not f.endswith('.ninja'):
                    os.unlink(os.path.join(dirpath, f))
    subprocess.check_call([ninja, '-C', out, '--quiet'])
    log = subprocess.check_output([ninja, '-C', out]).decode()
    if 'no work to do' not in log:
        sys.exit('bench: build is not clean after a full build:\n' + log)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('cmd', choices=['setup', 'rebuild', 'run'])
    parser.add_argument('--ninja', default='ninja')
    parser.add_argument('--targets', type=int, default=2000)
    parser.add_argument('-n', '--runs', type=int, default=10)
    parser.add_argument('dir')
    parser.add_argument('extra', nargs='*')
    args = parser.parse_args()
    root = os.path.abspath(args.dir)
    ninja = os.path.abspath(args.ninja) if os.sep in args.ninja else args.ninja

    if args.cmd == 'setup':
        if os.path.exists(root):
            shutil.rmtree(root)
        subprocess.check_call([sys.executable, os.path.join(HERE,
                               'gen_bench.py'), '--targets',
                               str(args.targets), root])
        build_tools(root)
        full_build(root, ninja)
    elif args.cmd == 'rebuild':
        build_tools(root)
        full_build(root, ninja)
    else:
        out = os.path.join(root, 'out/Default')
        touch = open(os.path.join(root, 'touch_file')).read().strip()
        sys.exit(subprocess.call(
            [os.path.join(root, 'tools/ttfc'), '-n', str(args.runs),
             '-C', out, '-t', touch, '-f', os.path.join(root, 'tools/fakecc'),
             '--', ninja] + args.extra))


if __name__ == '__main__':
    main()
