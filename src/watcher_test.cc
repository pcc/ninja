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
#include "util.h"
#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <iostream>

int main(int argc, char **argv) {
  Watcher w;
  for (int i = 1; i != argc; ++i) {
    w.AddPath(argv[i], argv[i]);
  }

  while (1) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(w.fd_, &fds);
    int ret = pselect(w.fd_ + 1, &fds, 0, 0, w.Timeout(), 0);

    switch (ret) {
      case 1:
        w.OnReady();
        break;

      case 0:
        for (Watcher::key_set_iterator i = w.added_keys_begin();
             i != w.added_keys_end(); ++i) {
          std::cout << "added " << static_cast<char *>(*i) << std::endl;
        }
        for (Watcher::key_set_iterator i = w.changed_keys_begin();
             i != w.changed_keys_end(); ++i) {
          std::cout << "changed " << static_cast<char *>(*i) << std::endl;
        }
        for (Watcher::key_set_iterator i = w.deleted_keys_begin();
             i != w.deleted_keys_end(); ++i) {
          std::cout << "deleted " << static_cast<char *>(*i) << std::endl;
        }
        w.Reset();
        break;

      case -1:
        Fatal("read: %s", strerror(errno));
    }
  }
}
