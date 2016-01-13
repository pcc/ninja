#include "mblock.h"
#include <string>

typedef std::basic_string<char, std::char_traits<char>, mblock_allocator<char> >
    mblock_string;

int main() {
  char foo[128];
  mblock mb(foo, foo+128);

  mblock_string str("hello", &mb);
  str = "world";

  char *c = new (mb) char[128000];
}
