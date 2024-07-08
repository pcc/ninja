project('ninja-simd', 'cpp', default_options: ['cpp_std=c++20'])
tbb = dependency('tbb')
phash = include_directories('parallel-hashmap/parallel_hashmap')
executable('ninja-simd', 'depfile_parser.cc', 'ninja-simd.cpp', dependencies: [tbb], include_directories: [phash], cpp_args: ['-mno-outline-atomics'])
