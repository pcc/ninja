// Copyright 2014 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "watcher.h"
#include <iostream>

int main(int argc, char **argv) {
  Watcher w;
  for (int i = 1; i != argc; ++i) {
    w.AddPath(argv[i], argv[i]);
  }

  while (1) {
    w.WaitForEvents();
    for (Watcher::key_set_type::iterator i = w.added_keys_.begin();
         i != w.added_keys_.end(); ++i) {
      std::cout << "added " << static_cast<char *>(*i) << std::endl;
    }
    for (Watcher::key_set_type::iterator i = w.changed_keys_.begin();
         i != w.changed_keys_.end(); ++i) {
      std::cout << "changed " << static_cast<char *>(*i) << std::endl;
    }
    for (Watcher::key_set_type::iterator i = w.deleted_keys_.begin();
         i != w.deleted_keys_.end(); ++i) {
      std::cout << "deleted " << static_cast<char *>(*i) << std::endl;
    }
    w.Reset();
  }
}
