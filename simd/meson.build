project('ninja-simd', 'cpp', default_options: ['cpp_std=c++20'])

tbb = dependency('tbb')

cflags = ['-fno-omit-frame-pointer']
if target_machine.cpu_family() == 'aarch64'
  cflags += ['-mno-outline-atomics']
endif

executable('ninja-simd',
           'depfile_parser.cc',
           'ninja-simd.cpp',
           dependencies: [tbb],
           cpp_args: cflags)
