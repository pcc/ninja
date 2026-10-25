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

"""Generates a large, realistic (GN/Chromium-shaped) build tree.

Usage:
  gen_bench.py [--targets N] [--seed S] rootdir

Writes:
  rootdir/<dir>/*.cc, *.h, BUILD.gn       sources, headers and "build files"
  rootdir/build/linux/sysroot/...,
  rootdir/third_party/libc++/...         "system" headers shared by everything
  rootdir/out/Default/build.ninja        the root manifest, GN style:
                                         generator rule with a depfile, one
                                         subninja per toolchain
  rootdir/out/Default/toolchain.ninja    rules, one subninja per target
  rootdir/out/Default/obj/**.ninja       one file per target, with
                                         per-target variables and some
                                         per-target action rules
  rootdir/out/Default/build.ninja.d      depfile for the generator rule

Every compile step uses deps=gcc depfiles (written by fakecc from the
'#include' lines in the source), so a built tree has a large .ninja_deps.
The output is deterministic for a given seed.
"""

import argparse
import os
import random
import sys
from typing import Dict, List, Set


def paretoint(avg: float, alpha: float) -> int:
    return int(random.paretovariate(alpha) * avg / (alpha / (alpha - 1)))


K_START = ['render', 'web', 'browser', 'tab', 'content', 'extension', 'url',
           'file', 'sync', 'http', 'profile', 'net', 'gpu', 'media', 'ui',
           'base', 'cc', 'blink', 'viz', 'mojo', 'storage', 'device']
K_OPTION = ['view', 'host', 'holder', 'container', 'impl', 'ref', 'delegate',
            'widget', 'proxy', 'stub', 'context', 'manager', 'controller',
            'watcher', 'service', 'file', 'data', 'resource', 'device', 'info',
            'provider', 'internals', 'tracker', 'api', 'layer', 'client',
            'observer', 'factory', 'util', 'helper', 'handler', 'registry']
K_OS = ['win', 'mac', 'aura', 'linux', 'android', 'unittest', 'browsertest',
        'fuzzer', 'perftest']


class Names:
    def __init__(self) -> None:
        self.seen: Set[str] = set()

    def unique(self, avg_options: float, p_suffix: float) -> str:
        while True:
            n = min(paretoint(avg_options, alpha=4), 5)
            name = [random.choice(K_START)] + random.sample(K_OPTION, n)
            if random.random() < p_suffix:
                name.append(random.choice(K_OS))
            s = '_'.join(name)
            if s not in self.seen:
                self.seen.add(s)
                return s


class Target:
    def __init__(self, idx: int, names: Names, dirs: List[str],
                 kind: str) -> None:
        self.idx = idx
        self.kind = kind  # 'source_set', 'static_library' or 'executable'
        self.name = names.unique(1.3, 0.1)
        # Targets cluster into directories, like real projects.
        if dirs and random.random() < 0.6:
            self.dir = random.choice(dirs)
        else:
            depth = 1 + min(paretoint(0.8, alpha=3), 4)
            self.dir = '/'.join(names.unique(0.8, 0) for _ in range(depth))
            dirs.append(self.dir)
        n = min(paretoint(20, alpha=2) + 1, 400)
        src_names = Names()
        self.sources = [src_names.unique(2.5, 0.3) for _ in range(n)]
        self.deps: List['Target'] = []
        self.public_headers: List[str] = []
        self.inc_set: List[str] = []
        self.action = None
        self.defines = ['-D' + d for d in random.sample(DEFINES, 30)]
        self.defines += ['-DTARGET_%s_IMPL' % self.name.upper()]

    @property
    def out_dir(self) -> str:
        return 'obj/' + self.dir

    @property
    def output(self) -> str:
        if self.kind == 'static_library':
            return 'obj/%s/lib%s.a' % (self.dir, self.name)
        if self.kind == 'executable':
            return './' + self.name
        return 'obj/%s/%s.stamp' % (self.dir, self.name)

    def obj(self, src: str) -> str:
        return 'obj/%s/%s/%s.o' % (self.dir, self.name, src)

    def src(self, src: str) -> str:
        return '../../%s/%s.cc' % (self.dir, src)

    def hdr(self, src: str) -> str:
        return '../../%s/%s.h' % (self.dir, src)


DEFINES = ['USE_UDEV', 'USE_AURA=1', 'USE_GLIB=1', 'USE_OZONE=1',
           'OFFICIAL_BUILD', 'NDEBUG', 'NVALGRIND', '_FILE_OFFSET_BITS=64',
           '_LARGEFILE_SOURCE', '_LARGEFILE64_SOURCE', '__STDC_CONSTANT_MACROS',
           '__STDC_FORMAT_MACROS', '_FORTIFY_SOURCE=2', '_GNU_SOURCE',
           'CR_CLANG_REVISION=\\"llvmorg-19-init-2941-ga0b3dbaf-22\\"',
           'COMPONENT_BUILD', 'CR_LIBCXX_REVISION=e3b94d0e5b86883fd77696bf10dc33ba250ba99b',
           'DYNAMIC_ANNOTATIONS_ENABLED=0', 'PROTOBUF_USE_DLLS',
           'GOOGLE_PROTOBUF_NO_RTTI', 'HAVE_PTHREAD', 'BORINGSSL_SHARED_LIBRARY',
           'V8_USE_EXTERNAL_STARTUP_DATA', 'WEBRTC_POSIX', 'WEBRTC_LINUX',
           'ABSL_CONSUME_DLL', 'U_USING_ICU_NAMESPACE=0', 'U_ENABLE_DYLOAD=0',
           'USE_CHROMIUM_ICU=1', 'U_ENABLE_TRACING=1', 'U_ENABLE_RESOURCE_TRACING=0',
           'ICU_UTIL_DATA_IMPL=ICU_UTIL_DATA_FILE', 'UCHAR_TYPE=uint16_t',
           'GL_GLEXT_PROTOTYPES', 'USE_GLX', 'USE_EGL', 'VK_USE_PLATFORM_XCB_KHR',
           'SK_ENABLE_SKSL', 'SK_UNTIL_CRBUG_1187654_IS_FIXED', 'SK_GANESH',
           'SK_USE_PERFETTO', 'SK_GL', 'SK_VULKAN=1', 'LEVELDB_PLATFORM_CHROMIUM=1',
           'GOOGLE_PROTOBUF_INTERNAL_DONATE_STEAL_INLINE=0', 'XML_STATIC',
           'LIBXML_STATIC', 'CHROMIUM_BUILD', 'ENABLE_IPC_FUZZER',
           'FULL_SAFE_BROWSING', 'SAFE_BROWSING_CSD', 'SAFE_BROWSING_DB_LOCAL']

CFLAGS = ('-fno-delete-null-pointer-checks -fno-ident -fno-strict-aliasing '
          '-fstack-protector -funwind-tables -fPIC -pthread '
          '-fcolor-diagnostics -fmerge-all-constants '
          '-fcrash-diagnostics-dir=../../tools/clang/crashreports '
          '-mllvm -instcombine-lower-dbg-declare=0 '
          '-mllvm -split-threshold-for-reg-with-hint=0 -ffp-contract=off '
          '-fcomplete-member-pointers -m64 -msse3 -Wno-builtin-macro-redefined '
          '-D__DATE__= -D__TIME__= -D__TIMESTAMP__= -ffile-compilation-dir=. '
          '-no-canonical-prefixes -ftrivial-auto-var-init=pattern -O2 '
          '-fdata-sections -ffunction-sections -fno-unique-section-names '
          '-fno-omit-frame-pointer -g0 -fvisibility=hidden -Wheader-hygiene '
          '-Wstring-conversion -Wtautological-overlap-compare -Wall -Wextra '
          '-Wimplicit-fallthrough -Wextra-semi -Wunreachable-code-aggressive '
          '-Wthread-safety -Wunguarded-availability -Wno-missing-field-initializers '
          '-Wno-unused-parameter -Wno-psabi -Wloop-analysis '
          '-Wno-unneeded-internal-declaration -Wno-cast-function-type '
          '-Wno-ignored-pragma-optimize -Wno-deprecated-builtins '
          '-Wno-bitfield-constant-conversion -Wno-deprecated-this-capture '
          '-Wno-invalid-offsetof -Wno-vla-extension -Wno-thread-safety-reference-return '
          '-Wshadow -Werror')
CFLAGS_CC = ('-Wno-undefined-bool-conversion -Wno-tautological-undefined-compare '
             '-std=c++20 -Wno-trigraphs -gsimple-template-names -fno-exceptions '
             '-fno-rtti -nostdinc++ -isystem../../third_party/libc++/src/include '
             '-isystem../../third_party/libc++abi/src/include '
             '--sysroot=../../build/linux/debian_bullseye_amd64-sysroot '
             '-fvisibility-inlines-hidden')


def write(path: str, content: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(content)


def make_system_headers(root: str) -> List[str]:
    hdrs = []
    groups = [
        ('third_party/libc++/src/include', 220),
        ('third_party/libc++/src/include/__algorithm', 180),
        ('third_party/libc++/src/include/__iterator', 90),
        ('third_party/libc++/src/include/__memory', 80),
        ('third_party/libc++/src/include/__type_traits', 200),
        ('third_party/libc++/src/include/__utility', 50),
        ('build/linux/debian_bullseye_amd64-sysroot/usr/include', 300),
        ('build/linux/debian_bullseye_amd64-sysroot/usr/include/x86_64-linux-gnu/bits', 250),
        ('build/linux/debian_bullseye_amd64-sysroot/usr/include/x86_64-linux-gnu/sys', 80),
        ('build/linux/debian_bullseye_amd64-sysroot/usr/include/linux', 150),
        ('third_party/abseil-cpp/absl/base', 60),
        ('third_party/abseil-cpp/absl/strings', 60),
        ('third_party/abseil-cpp/absl/container', 60),
        ('third_party/perfetto/include/perfetto/tracing', 120),
        ('third_party/protobuf/src/google/protobuf', 100),
    ]
    names = Names()
    for d, n in groups:
        for _ in range(n):
            hdrs.append('../../%s/%s.h' % (d, names.unique(1.0, 0.1)))
    for h in hdrs:
        write(os.path.join(root, 'out/Default', h), '')
    return hdrs


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--targets', type=int, default=2000)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('root')
    args = parser.parse_args()
    random.seed(args.seed)
    root = args.root
    out = os.path.join(root, 'out/Default')

    sys_hdrs = make_system_headers(root)
    core = sys_hdrs[:150]
    rest = sys_hdrs[150:]

    names = Names()
    dirs: List[str] = []
    n_exe_small = max(args.targets // 100, 1)
    targets: List[Target] = []
    for i in range(args.targets):
        kind = 'source_set' if random.random() < 0.6 else 'static_library'
        targets.append(Target(i, names, dirs, kind))
    for i, t in enumerate(targets):
        # Depend mostly on "low-level" targets (low indices).
        if i:
            n = min(paretoint(5, alpha=2), i)
            t.deps = sorted({targets[int(random.random() ** 2 * i)]
                             for _ in range(n)}, key=lambda d: d.idx)
        t.public_headers = random.sample(t.sources, min(len(t.sources), 6))
        inherited: Set[str] = set()
        for d in t.deps:
            inherited.update(d.hdr(s) for s in d.public_headers[:4])
            inherited.update(random.sample(d.inc_set, min(len(d.inc_set), 25)))
        t.inc_set = sorted(inherited)
        if random.random() < 0.1:
            t.action = 'gen/%s/%s_buildflags.h' % (t.dir, t.name)
    exes = []
    for name, frac in [('chrome', 0.9), ('unit_tests', 0.8),
                       ('browser_tests', 0.85)]:
        e = Target(len(targets) + len(exes), names, dirs, 'executable')
        e.name = name
        e.dir = name
        e.deps = [t for t in targets if random.random() < frac]
        e.inc_set = []
        exes.append(e)
    for i in range(n_exe_small):
        e = Target(len(targets) + len(exes), names, dirs, 'executable')
        e.deps = random.sample(targets, 8)
        e.inc_set = []
        exes.append(e)
    all_targets = targets + exes

    # Sources and headers.
    for t in all_targets:
        sysset = core + random.sample(rest, 80)
        own = [t.hdr(s) for s in t.sources]
        for s in t.sources:
            incs = list(sysset)
            incs += random.sample(own, min(len(own), 8))
            incs += t.inc_set
            if t.action:
                incs.append(t.action)
            write(os.path.join(out, t.hdr(s)), '')
            write(os.path.join(out, t.src(s)),
                  ''.join('#include "%s"\n' % h for h in incs))
        write(os.path.join(root, t.dir, 'BUILD.gn'), '# %s\n' % t.name)

    def transitive(t: Target, seen: Set[int], acc: List[Target]) -> None:
        for d in t.deps:
            if d.idx not in seen:
                seen.add(d.idx)
                transitive(d, seen, acc)
                acc.append(d)

    # Per-target subninjas.
    for t in all_targets:
        lines = []
        if t.action:
            rule = '__%s_%s_buildflags___build_toolchain_linux_x64__rule' % (
                t.dir.replace('/', '_'), t.name)
            lines += [
                'rule %s' % rule,
                '  command = ../../tools/fakecc ../../build/write_buildflag_header.py '
                '--output ${out} --rulename //%s:%s_buildflags --gen-dir gen '
                '--definitions __%s_buildflags.rsp -o ${out}' % (
                    t.dir, t.name, t.name),
                '  description = ACTION //%s:%s_buildflags(//build/toolchain/'
                'linux:x64)' % (t.dir, t.name),
                '  restat = 1',
                '',
                'build %s: %s ../../build/write_buildflag_header.py '
                '../../build/gn_helpers.py' % (t.action, rule),
                '',
                'build obj/%s/%s_buildflags.stamp: stamp %s' % (
                    t.dir, t.name, t.action),
                '',
            ]
        lines += [
            'defines = ' + ' '.join(t.defines),
            'include_dirs = -I../.. -Igen -I../../third_party/perfetto/include '
            '-Igen/third_party/perfetto/build_config -Igen/third_party/perfetto '
            '-I../../third_party/abseil-cpp -I../../third_party/protobuf/src '
            '-I../../third_party/boringssl/src/include',
            'cflags = ' + CFLAGS,
            'cflags_cc = ' + CFLAGS_CC,
            'label_name = ' + t.name,
            'target_out_dir = obj/' + t.dir,
            'target_output_name = ' + t.name,
            '',
        ]
        order_only = [d.output for d in t.deps if d.kind != 'executable']
        if t.action:
            order_only.append('obj/%s/%s_buildflags.stamp' % (t.dir, t.name))
        oo = (' || ' + ' '.join(order_only)) if order_only else ''
        for s in t.sources:
            lines.append('build %s: cxx %s%s' % (t.obj(s), t.src(s), oo))
            lines.append('  source_file_part = %s.cc' % s)
            lines.append('  source_name_part = %s' % s)
        lines.append('')
        objs = ' '.join(t.obj(s) for s in t.sources)
        if t.kind == 'source_set':
            lines.append('build %s: stamp %s%s' % (t.output, objs, oo))
        elif t.kind == 'static_library':
            lines.append('build %s: alink %s%s' % (t.output, objs, oo))
            lines.append('  arflags = -T -S')
            lines.append('  output_extension = .a')
            lines.append('  output_dir = obj/' + t.dir)
        else:
            closure: List[Target] = []
            transitive(t, set(), closure)
            link_objs = [objs] + [d.obj(s) for d in closure
                                  if d.kind == 'source_set' for s in d.sources]
            libs = [d.output for d in closure if d.kind == 'static_library']
            stamps = [d.output for d in closure if d.kind == 'source_set']
            lines.append('build %s: link %s | %s || %s' % (
                t.output, ' '.join(link_objs), ' '.join(libs),
                ' '.join(stamps)))
            lines.append('  ldflags = -Wl,--fatal-warnings -Wl,--build-id '
                         '-fPIC -Wl,-z,noexecstack -Wl,-z,relro -Wl,-z,now '
                         '-Wl,--icf=all -Wl,--color-diagnostics '
                         '-Wl,--gdb-index -Wl,-O2 -Wl,--gc-sections -rdynamic '
                         '-pie -Wl,--disable-new-dtags')
            lines.append('  libs = -ldl -lpthread -lrt -lgmodule-2.0 '
                         '-lgobject-2.0 -lglib-2.0 -lnss3 -lnssutil3 '
                         '-lsmime3 -lplds4 -lplc4 -lnspr4 -latk-1.0 '
                         '-latk-bridge-2.0 -ldbus-1 -lexpat -lm -lz')
            lines.append('  solibs = ' + ' '.join(libs))
            lines.append('  output_extension = ')
            lines.append('  output_dir = .')
        lines.append('')
        write(os.path.join(out, 'obj', t.dir, t.name + '.ninja'),
              '\n'.join(lines))

    tool = '../../tools/fakecc'
    toolchain = [
        'rule cc',
        '  command = %s -MMD -MF ${out}.d ${defines} ${include_dirs} ${cflags} '
        '${cflags_c} -c ${in} -o ${out}' % tool,
        '  description = CC ${out}',
        '  depfile = ${out}.d',
        '  deps = gcc',
        'rule cxx',
        '  command = %s -MMD -MF ${out}.d ${defines} ${include_dirs} ${cflags} '
        '${cflags_cc} -c ${in} -o ${out}' % tool,
        '  description = CXX ${out}',
        '  depfile = ${out}.d',
        '  deps = gcc',
        'rule alink',
        '  command = rm -f ${out} && %s --ar ${arflags} -o ${out} @\"${rspfile}\"'
        % tool,
        '  description = AR ${out}',
        '  rspfile = ${out}.rsp',
        '  rspfile_content = ${in}',
        'rule link',
        '  command = %s --link ${ldflags} -o \"${output_dir}/${target_output_name}'
        '${output_extension}\" -Wl,--start-group @\"${output_dir}/'
        '${target_output_name}${output_extension}.rsp\" -Wl,--end-group '
        '${solibs} ${libs}' % tool,
        '  description = LINK ${output_dir}/${target_output_name}'
        '${output_extension}',
        '  rspfile = ${output_dir}/${target_output_name}${output_extension}.rsp',
        '  rspfile_content = ${in}',
        '  pool = link_pool',
        'rule stamp',
        '  command = touch ${out}',
        '  description = STAMP ${out}',
        'rule copy',
        '  command = ln -f ${in} ${out} 2>/dev/null || (rm -rf ${out} && cp -af '
        '${in} ${out})',
        '  description = COPY ${in} ${out}',
        '',
        'pool link_pool',
        '  depth = 4',
        '',
    ]
    for t in all_targets:
        toolchain.append('subninja obj/%s/%s.ninja' % (t.dir, t.name))
    write(os.path.join(out, 'toolchain.ninja'), '\n'.join(toolchain) + '\n')

    build = [
        'ninja_required_version = 1.7.2',
        '',
        'rule gn',
        '  command = echo regeneration should not run && false',
        '  pool = console',
        '  description = Regenerating ninja files',
        '',
        'build build.ninja.stamp: gn',
        '  generator = 1',
        '  depfile = build.ninja.d',
        '',
        'build build.ninja: phony build.ninja.stamp',
        '  generator = 1',
        '',
        'subninja toolchain.ninja',
        '',
    ]
    seen_short: Set[str] = set()
    for t in all_targets:
        if t.name not in seen_short and t.kind != 'executable':
            seen_short.add(t.name)
            build.append('build %s: phony %s' % (t.name, t.output))
        build.append('build %s$:%s: phony %s' % (t.dir, t.name, t.output))
    build.append('')
    build.append('build all: phony ' +
                 ' $\n    '.join(t.output for t in all_targets))
    build.append('')
    build.append('default all')
    write(os.path.join(out, 'build.ninja'), '\n'.join(build) + '\n')

    gn_files = ['../../BUILD.gn', '../../build/config/BUILDCONFIG.gn'] + [
        '../../%s/BUILD.gn' % d for d in sorted(set(t.dir for t in all_targets))]
    for f in gn_files[:2]:
        write(os.path.join(out, f), '')
    write(os.path.join(out, 'build.ninja.d'),
          'build.ninja.stamp: ' + ' '.join(gn_files) + '\n')
    for f in ['build/write_buildflag_header.py', 'build/gn_helpers.py']:
        write(os.path.join(root, f), '')

    # gn writes this last, so that the generator step is clean.
    write(os.path.join(out, 'build.ninja.stamp'), '')

    # A good file to touch: a source in a mid-level library.
    t = targets[len(targets) // 2]
    write(os.path.join(root, 'touch_file'), t.src(t.sources[0]) + '\n')

    n_edges = sum(len(t.sources) for t in all_targets)
    print('%d targets, %d compile steps' % (len(all_targets), n_edges))


if __name__ == '__main__':
    main()
