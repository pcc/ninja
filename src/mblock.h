#ifndef NINJA_MBLOCK_H_
#define NINJA_MBLOCK_H_

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

struct mblock {
  void* begin;
  void* end;

  mblock(void *begin, void *end) : begin(begin), end(end) {}

  void* allocate(size_t size, size_t align) {
    void* mem = begin;

    uintptr_t begin_uptr = reinterpret_cast<uintptr_t>(begin);
    begin_uptr = (begin_uptr + size + align - 1) & -align;
    begin = reinterpret_cast<void *>(begin_uptr);

    assert(begin < end);

    return mem;
  }
};

void* operator new(size_t size, mblock &mb) {
  return mb.allocate(size, 16);
}

void* operator new[](size_t size, mblock &mb) {
  return mb.allocate(size, 16);
}

template <class T>
struct mblock_allocator {
  mblock *mb;

  mblock_allocator() : mb(0) {}
  mblock_allocator(mblock *mb) : mb(mb) {}
  template <class U>
  mblock_allocator(const mblock_allocator<U> &other) : mb(other.mb) {}
  T* allocate(size_t n) {
    return static_cast<T *>(mb->allocate(n, __alignof__(T)));
  }
  void deallocate(T* p, size_t n) {}

  template <class U>
  bool operator==(const mblock_allocator<U> &other) const {
    return mb == other.mb;
  }
  template <class U>
  bool operator!=(const mblock_allocator<U> &other) const {
    return mb != other.mb;
  }
  typedef T value_type;
  typedef size_t size_type;
  typedef ptrdiff_t difference_type;
  typedef T& reference;
  typedef const T& const_reference;
  typedef T* pointer;
  typedef const T* const_pointer;

  template <class U>
  struct rebind {
    typedef mblock_allocator<U> other;
  };
};

#endif
