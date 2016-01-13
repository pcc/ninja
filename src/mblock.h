#ifndef NINJA_MBLOCK_H_
#define NINJA_MBLOCK_H_

#include <string>
#include <vector>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct mblock {
  void* begin;
  void* end;

  mblock(void *begin, void *end) : begin(begin), end(end) {}

  void* allocate(size_t size, size_t align) {
    uintptr_t begin_uptr = reinterpret_cast<uintptr_t>(begin);
    begin_uptr = (begin_uptr + align - 1) & -align;
    void *mem = reinterpret_cast<void *>(begin_uptr);

    begin_uptr += size;
    begin = reinterpret_cast<void *>(begin_uptr);

    assert(begin < end);

    return mem;
  }
};

inline void* operator new(size_t size, mblock &mb) {
  return mb.allocate(size, 16);
}

inline void* operator new[](size_t size, mblock &mb) {
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
    return static_cast<T *>(mb->allocate(n * sizeof(T), __alignof__(T)));
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

  template <class U, class A1>
  void construct(U *p, const A1 &arg) {
    new (p) U(arg);
  }

  template <class U>
  void destroy(U *p) {
    p->~U();
  }

  size_t max_size() const {
    return -1;
  }
};

typedef std::basic_string<char, std::char_traits<char>, mblock_allocator<char> >
    mblock_string;
template <typename T>
struct mblock_vector {
  typedef std::vector<T, mblock_allocator<T> > type;
};

#endif
