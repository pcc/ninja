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

"""Checks that two ninja binaries agree on what a benchmark tree needs.

  equivalence.py [--build-dir D] DIR REFERENCE_NINJA NINJA

Changes the tree of bench.py in a few ways (a source or a header touched,
an object missing, a command changed, the manifest cache or the build log
missing or broken), and compares the output of 'ninja -n' (run twice for
NINJA, so that its manifest cache is both missed and used).  Afterwards,
the tree is restored and rebuilt with NINJA.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import time


def dry_run(ninja, out_dir):
    result = subprocess.run([ninja, '-n'], cwd=out_dir,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return result.returncode, result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', default='out/Default')
    parser.add_argument('dir')
    parser.add_argument('reference')
    parser.add_argument('ninja')
    args = parser.parse_args()
    root = os.path.abspath(args.dir)
    out_dir = os.path.join(root, args.build_dir)
    reference = os.path.abspath(args.reference)
    ninja = os.path.abspath(args.ninja)
    failures = 0

    def check(name):
        nonlocal failures
        expected = dry_run(reference, out_dir)
        results = [dry_run(ninja, out_dir), dry_run(ninja, out_dir)]
        lines = expected[1].count(b'\n')
        if all(r == expected for r in results):
            print('OK   %s (%d lines)' % (name, lines))
        else:
            failures += 1
            print('DIFF %s' % name)

    def touch(path):
        os.utime(path)

    def in_out(path):
        return os.path.join(out_dir, path)

    source = in_out(open(os.path.join(root, 'touch_file')).read().strip())
    touch(source)
    check('touched source')
    header = sorted(glob.glob(os.path.join(root, 'base', '*.h')))[0]
    touch(header)
    check('touched header')
    objects = sorted(glob.glob(in_out('obj/**/*.o'), recursive=True))
    if objects:
        shutil.move(objects[0], objects[0] + '.moved')
        check('missing object')
        shutil.move(objects[0] + '.moved', objects[0])
    # A changed command (the manifest cache must be missed); files changed
    # within 2 seconds are not cached.
    manifests = sorted(glob.glob(in_out('obj/*/*.ninja')))
    for manifest in manifests:
        text = open(manifest).read()
        if 'defines = ' in text:
            shutil.copy2(manifest, manifest + '.saved')
            open(manifest, 'w').write(
                text.replace('defines = ', 'defines = -DEQUIVALENCE ', 1))
            time.sleep(2.1)
            check('changed command')
            shutil.move(manifest + '.saved', manifest)
            touch(manifest)
            time.sleep(2.1)
            break
    cache = in_out('.ninja_manifest_cache')
    if os.path.exists(cache):
        os.unlink(cache)
    check('no manifest cache')
    open(cache, 'w').write('garbage')
    check('broken manifest cache')
    log = in_out('.ninja_log')
    shutil.move(log, log + '.moved')
    check('no build log')
    shutil.move(log + '.moved', log)
    check('restored')

    subprocess.run([ninja, '--quiet'], cwd=out_dir, stdout=subprocess.DEVNULL,
                   check=True)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
