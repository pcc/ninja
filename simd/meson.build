project('ninja-simd', 'cpp', default_options: ['cpp_std=c++20'])

tbb = dependency('tbb')

cflags = ['-fno-omit-frame-pointer', '-fno-strict-aliasing']
if target_machine.cpu_family() == 'aarch64'
  cflags += ['-mno-outline-atomics']
endif
if target_machine.cpu_family() == 'x86_64'
  cflags += ['-mcx16']
endif

executable('ninja-simd',
           'depfile_parser.cc',
           'ninja-simd.cpp',
           dependencies: [tbb],
           cpp_args: cflags,
           # TODO(pcc): get rid of this, it's silly that we have to call a function just to use cmpxchg16b
           link_args: ['-l:libatomic.a'])
