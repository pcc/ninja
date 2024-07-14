// Copyright 2024 Google LLC. All Rights Reserved.
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

#include <fcntl.h>
#include <linux/prctl.h>
#include <poll.h>
#include <spawn.h>
#include <stdarg.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <deque>
#include <list>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "oneapi/tbb/concurrent_hash_map.h"
#include "oneapi/tbb/concurrent_vector.h"
#include "oneapi/tbb/parallel_for_each.h"
#include "oneapi/tbb/task_group.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#ifdef __aarch64__
#include <arm_neon.h>
#elif defined(__x86_64__)
#include <emmintrin.h>
#include <xmmintrin.h>
#endif

#include "depfile_parser.h"

using namespace oneapi;

// This implementation of Ninja uses SIMD and parallelism to achieve
// significantly faster time-to-first-build-command than the existing
// implementation. On the author's machine, an M2 Max Macbook Pro running Linux,
// we can start executing build commands in Chromium's GN based build system
// ("chrome" target) in 80ms while the existing Ninja implementation takes 3.5
// seconds. The null build time has not been measured for Chromium because it
// doesn't build out of the box on Linux/arm64, but here are the null build
// times for LLVM:
//
//                         Ninja  Ninja-SIMD
// llvm-ar (CMake build)   175ms     24ms
// clang (GN build)         70ms     21ms
//
// FIXME: The implementation currently does not support the following:
// - Non-POSIX operating systems (only tested on Linux).
// - Generator rules, pools, dyndeps, and likely several other features and
//   parsing corner cases that GN/CMake/Meson do not use (at least for C++).
//
// Here's an overview of an example parallel task execution flow:
//
// clang-format off
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcomment"
//
//         /--- parse build log --------------------------------------------------------------------------------------------\
//         |                                                              /- parse subninja build statements (batch 1/2) ----\
//         | /- scan root manifest (slice 1/2) -\ /-- scan subninja file --- parse subninja build statements (batch 2/2) ------- identify build commands
//  START ----- scan root manifest (slice 2/2) ----- parse root manifest build statements (batch 1/2) -----------------------/
//                                                \- parse root manifest build statements (batch 2/2) ----------------------/
//
#pragma GCC diagnostic pop
// clang-format on
//
// In the "scan" phase, we split the input file into chunks (minimum chunk size
// 1MB, maximum 128 chunks), use SIMD instructions to rapidly search the input
// file for top-level entities, such as build statements, rules and subninja
// statements, parse certain top-level entities, and compute hashes for
// top-level entities that may be involved in the evaluation of a build
// statement command. In the "parse" phase, we add variables to a hash map, and
// then parse build statements as well as subninja files. Batches of build
// statements identified during the "scan" phase are processed in parallel
// during the "parse" phase, and so are any identified subninja files. Build
// statements are parsed using SIMD for tokenization, and we add them to a
// custom concurrent hash map implementation that maps from paths to node
// pointers.
//
// In parallel with parsing build manifests, we also parse the build log. For
// details, including an overview of the format, please see the comment at the
// top of the read_build_log() function.
//
// After parsing is complete, we are ready to identify an initial set of build
// commands. This is achieved with a post-order traversal of the build graph,
// starting with the nodes that the user specified on the command line, in which
// we compute a Merkle hash of each edge based on the mtimes of the input
// and output files and the hashes of top-level entities that were computed
// previously. This is done in a specific way so that we can stat() nodes in
// parallel as well as starting commands for dirty nodes as soon as they are
// identified. For details, please see the comment at the top of the
// classify_edges() function.
//
// After the initial set of build commands is identified and started, we just
// need to wait for the commands to complete and start any commands that were
// unblocked by commands that exit. This is done single-threaded, in a similar
// way to the existing implementation of Ninja, because at this point the
// performance of Ninja is no longer a bottleneck.
//
// Special thanks:
// - Evan Martin for the initial implementation of Ninja and for the idea of
//   using hashes to detect dirty nodes, as implemented in n2 [1].
// - John Keiser and Daniel Lemire for the SIMD vectorized character
//   classification technique first used in simdjson [2].
// - Rui Ueyama for the suggestion to use simdjson-like techniques for parsing
//   Ninja files and for speeding up the the stuff that happens at the end of a
//   build so we have no excuse not to speed up the stuff that happens at the
//   beginning as well.
//
// [1] https://neugierig.org/software/blog/2022/03/n2.html
// [2] https://arxiv.org/pdf/1902.08318

struct HashResult {
  uint64_t lo, hi;
  bool operator==(const HashResult& other) const {
    return lo == other.lo && hi == other.hi;
  }
};

HashResult hash_buf(const void* buf, size_t size) {
  auto h = XXH3_128bits(buf, size);
  HashResult result;
  result.lo = h.low64;
  result.hi = h.high64;
  return result;
}

struct Rule {
  char* begin;
  HashResult hash;
};

struct Var {
  std::string_view value;
  bool simple;
};

struct ToplevelVar : Var {
  HashResult hash;
};

using rules_t = std::unordered_map<std::string_view, Rule>;
using vars_t = std::unordered_map<std::string_view, Var>;
using toplevel_vars_t = std::unordered_map<std::string_view, ToplevelVar>;

struct Node;
struct Scope;

struct Edge {
  Scope* scope;
  std::vector<Node*> outputs, inputs;
  size_t first_implicit_output = -1ul;
  size_t first_implicit_input = -1ul;
  size_t first_order_only_input = -1ul;
  std::span<Node*> explicit_outputs() {
    return std::span<Node*>(outputs).subspan(0, first_implicit_output);
  }
  std::span<Node*> explicit_inputs() {
    return std::span<Node*>(inputs).subspan(0, first_implicit_input);
  }
  std::span<Node*> non_order_only_inputs() {
    return std::span<Node*>(inputs).subspan(0, first_order_only_input);
  }
  std::string_view rule_name;
  char* vars;
  HashResult hash;
  bool dirty = false;
  bool needed = false;
  bool started = false;
};

struct Node {
  Node* next = nullptr;
  std::string_view path;
  std::string path_buf;
  std::vector<Edge*> out_edges;
  Edge* in_edge = nullptr;
  bool nonexistent = false;
  std::atomic<bool> statted = false;
  struct timespec mtime;
  uint32_t build_log_index = -1u;
  std::optional<HashResult> build_log_hash;
  std::vector<Node*> depfile_inputs;
};

// The BigMap is the hash table used for the path to node mapping. Because it
// is a performance critical data structure, we use our own implementation that
// only supports the operations that we need. In particular, because we do
// not support resizing, the data structure can be made lock-free.
//
// Each bucket is an atomic pointer. The assumption is that the number of nodes
// will be large, so the data structure consists of a fixed size array of 1M
// bucket. (In the future we may consider dynamically sizing the array based on
// a node count recorded in the build log.) Insertion operations add the new
// node onto the head of the linked list stored in the bucket. Our first
// compare-exchange assumes the bucket to be empty (given the array size, this
// is likely to be true) and if that operation fails, we search the linked list
// for an existing node and compare-exchange the old head with a new one if it
// fails.
struct BigMap {
  static constexpr size_t array_size = 1 << 20;
  std::atomic<Node*> nodes[array_size] = {};

  Node* operator[](std::string_view path) const {
    HashResult hash = hash_buf(path.begin(), path.size());
    Node* node = nodes[hash.lo & (BigMap::array_size - 1)];
    while (node) {
      if (node->path == path)
        return node;
      node = node->next;
    }
    return nullptr;
  }

  // Finds an existing node with path == tmp_node->path and returns it,
  // otherwise inserts tmp_node into the map and returns it. If the insert
  // operation succeeds, tmp_node will be replaced with a newly allocated node.
  Node* get_or_insert(Node*& tmp_node) {
    HashResult hash = hash_buf(tmp_node->path.begin(), tmp_node->path.size());
    std::atomic<Node*>& slot = nodes[hash.lo & (BigMap::array_size - 1)];
    Node* value = nullptr;
    while (1) {
      if (slot.compare_exchange_strong(value, tmp_node,
                                       std::memory_order_acq_rel)) {
        Node* inserted_node = tmp_node;
        tmp_node = new Node;
        return inserted_node;
      }
      Node* search = value;
      while (search) {
        if (search->path == tmp_node->path) {
          tmp_node->next = nullptr;
          tmp_node->path_buf.clear();
          return search;
        }
        search = search->next;
      }
      tmp_node->next = value;
    }
  }

  size_t size() const {
    size_t size = 0;
    for (Node* node : nodes) {
      while (node) {
        ++size;
        node = node->next;
      }
    }
    return size;
  }
};

struct Global {
  tbb::concurrent_hash_map<std::string_view, char*> pool;
  BigMap nodes;
};
struct Scope {
  Scope* parent;
  toplevel_vars_t vars;
  rules_t rule;
};
struct ScannedVar {
  std::string_view name;
  ToplevelVar value;
};
struct ScannedFileRange {
  std::vector<Rule> build, rule;
  std::vector<ScannedVar> var;
  std::vector<char*> include, subninja, default_;
};

struct ExpansionScope {
  ExpansionScope(Scope* subninja_scope) : subninja_scope(subninja_scope) {}
  Scope* subninja_scope;
  Edge* build_edge = nullptr;
  vars_t* build_vars = nullptr;
  vars_t* rule_vars = nullptr;
};

void error(const char* err) {
  fprintf(stderr, "error: %s\n", err);
  exit(1);
}

void append_expansion(std::string& buf, std::string_view token,
                      ExpansionScope es, size_t recursion_depth = 0);

void append_var_expansion(std::string& buf, Var& v, ExpansionScope es,
                          size_t recursion_depth = 0) {
  if (v.simple)
    buf += v.value;
  else
    append_expansion(buf, v.value, es, recursion_depth + 1);
}

std::string var_expansion(Var& v, ExpansionScope es) {
  std::string buf;
  append_var_expansion(buf, v, es);
  return buf;
}

void append_named_var_expansion(std::string& buf, std::string_view name,
                                ExpansionScope es, size_t recursion_depth) {
  if (es.build_edge) {
    auto expand_node_list = [&](std::span<Node*> nodes, char sep) {
      if (nodes.empty())
        return;
      for (Node* n : nodes) {
        // FIXME: Need shell quoting.
        buf.append(n->path);
        buf.push_back(sep);
      }
      buf.pop_back();
    };
    // FIXME: "in", "in_newline" and "out" are supposed to be expanded as
    // written in the manifest file (after variable expansion), rather than
    // after canonicalization. In the original implementation of Ninja, this is
    // implemented by storing enough information about each path name in the
    // Node to allow them to be "decanonicalized" (see the Node::slash_bits_
    // field). Since the decanonicalized form is only needed if we actually need
    // to run a command, this seems suboptimal as well as being more
    // complicated, especially with this implementation of Ninja where there are
    // also concurrency concerns, since multiple threads could be creating the
    // same node. A better approach would probably be to use the edge's pointer
    // into the build manifest to lex the input/output file tokens again and
    // re-expand variable references.
    if (name == "in") {
      expand_node_list(es.build_edge->explicit_inputs(), ' ');
      return;
    }
    if (name == "in_newline") {
      expand_node_list(es.build_edge->explicit_inputs(), '\n');
      return;
    }
    if (name == "out") {
      expand_node_list(es.build_edge->explicit_outputs(), ' ');
      return;
    }

    auto i = es.build_vars->find(name);
    if (i != es.build_vars->end()) {
      append_var_expansion(buf, i->second, es, recursion_depth);
      return;
    }

    i = es.rule_vars->find(name);
    if (i != es.rule_vars->end()) {
      append_var_expansion(buf, i->second, es, recursion_depth);
      return;
    }
  }

  for (Scope* scope = es.subninja_scope; scope; scope = scope->parent) {
    auto i = scope->vars.find(name);
    if (i != scope->vars.end()) {
      append_var_expansion(buf, i->second, scope, recursion_depth);
      return;
    }
  }
}

void append_expansion(std::string& buf, std::string_view token,
                      ExpansionScope es, size_t recursion_depth) {
  if (recursion_depth == 16)
    error("recursion limit reached during variable expansion");
  size_t i = 0;
  while (1) {
    while (1) {
      if (i == token.size())
        return;
      if (token[i] == '$')
        break;
      buf.push_back(token[i]);
      i++;
    }
    if (i + 1 == token.size())
      return;
    if (token[i + 1] == ' ' || token[i + 1] == ':' || token[i + 1] == '$') {
      buf.push_back(token[i + 1]);
      i += 2;
    } else if (token[i + 1] == '\n') {
      i += 2;
      while (i != token.size() && token[i] == ' ')
        ++i;
    } else {
      std::string_view name;
      if (token[i + 1] == '{') {
        size_t name_begin = i + 2;
        size_t name_end = name_begin;
        while (name_end != token.size() && token[name_end] != '}')
          name_end++;
        name = token.substr(name_begin, name_end - name_begin);
        i = name_end + 1;
      } else {
        size_t name_begin = i + 1;
        size_t name_end = name_begin;
        while (name_end != token.size() &&
               ((token[name_end] >= '0' && token[name_end] <= '9') ||
                (token[name_end] >= 'A' && token[name_end] <= 'Z') ||
                (token[name_end] >= 'a' && token[name_end] <= 'z') ||
                token[name_end] == '_'))
          name_end++;
        name = token.substr(name_begin, name_end - name_begin);
        i = name_end;
      }
      append_named_var_expansion(buf, name, es, recursion_depth);
    }
  }
}

enum {
  EqualsIsToken = 1,
  ColonIsToken = 2,
  SpaceIsSeparator = 4,
};

// "$:" should be treated as a literal ":", but "$$:" and "$$$$:" expand to "$"
// and "$$" respectively followed by a ":" token. This means that if we identify
// a ":" or some other special character and it is preceded by "$" we need to
// look backwards and count the number of "$" characters to determine whether
// the character is a literal or not.
static bool is_unescaped_dollar(char* begin, char* pos) {
  size_t num_dollars = 0;
  while (pos >= begin && *pos-- == '$')
    num_dollars++;
  return num_dollars % 2 == 1;
}

#if defined(__aarch64__) && !defined(__AARCH64EB__)
using SIMDVec = uint8x16_t;

static SIMDVec vec_load(char* c) {
  return *(SIMDVec*)c;
}

static SIMDVec vec_dup(uint8_t c) {
  return vdupq_n_u8(c);
}

static SIMDVec vec_eq(SIMDVec v1, SIMDVec v2) {
  return vceqq_u8(v1, v2);
}

static SIMDVec vec_or(SIMDVec v1, SIMDVec v2) {
  return vorrq_u8(v1, v2);
}

static uint8_t first_all_ones(SIMDVec v) {
  // Reinterpret a vector of 16 8-bit masks as a vector of 8 16-bit mask pairs,
  // shift each element right by 4 and truncate each element to 8 bits. This
  // effectively transforms a vector of 8-bit masks into a vector of 4-bit
  // masks. For example, the mask pair 1111111100000000 becomes 11110000.
  uint64_t mask = vget_lane_u64(
      vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(v), 4)), 0);
  return mask ? __builtin_ctzl(mask) / 4 : 16;
}

static bool has_all_ones(SIMDVec v) {
  return vmaxvq_u8(v);
}
#elif defined(__x86_64__)
using SIMDVec = __m128i;

static SIMDVec vec_load(char* c) {
  return _mm_loadu_si128((__m128i_u*)c);
}

static SIMDVec vec_dup(uint8_t c) {
  return _mm_set1_epi8(c);
}

static SIMDVec vec_eq(SIMDVec v1, SIMDVec v2) {
  return _mm_cmpeq_epi8(v1, v2);
}

static SIMDVec vec_or(SIMDVec v1, SIMDVec v2) {
  return _mm_or_si128(v1, v2);
}

static uint8_t first_all_ones(SIMDVec v) {
  uint16_t mask = _mm_movemask_epi8(v);
  return __builtin_ctz(0x10000 | mask);
}

static bool has_all_ones(SIMDVec v) {
  return _mm_movemask_epi8(v);
}
#else
using SIMDVec = uint8_t;

static SIMDVec vec_load(char* c) {
  return *c;
}

static SIMDVec vec_dup(uint8_t c) {
  return c;
}

static SIMDVec vec_eq(SIMDVec v1, SIMDVec v2) {
  return v1 == v2;
}

static SIMDVec vec_or(SIMDVec v1, SIMDVec v2) {
  return v1 | v2;
}

static uint8_t first_all_ones(SIMDVec v) {
  return v ? 0 : 1;
}

static bool has_all_ones(SIMDVec v) {
  return v;
}
#endif

// Consume a token and its following whitespace. Returns the token (without
// whitespace).
//
// A "simple" token is one that does not contain a "$" character. This means
// that it does not need to be expanded and we do not need to search for
// dependencies when computing a hash.
template <unsigned Args>
inline std::string_view token(char*& pos, bool& simple) {
  char* begin = pos;
  char* end;
  simple = true;
  if (((Args & ColonIsToken) && *pos == ':') ||
      ((Args & EqualsIsToken) && *pos == '=')) {
    pos++;
    end = pos;
  } else if ((Args & SpaceIsSeparator) && *pos == ' ') {
    end = pos;
    pos++;
  } else if (*pos == '\n' || *pos == '\0') {
    return "";
  } else {
    // The end-of-token identifier SIMD loop works like this:
    //
    // A subset of the characters '=', ':', ' ', '\n' and '\0' (end-of-file)
    // ends a token (depending on Args) and terminates the loop. However, '$'
    // acts as an escape character for the next character, so if we see '$'
    // preceding a character other than end-of-file it should not terminate the
    // loop, unless the '$' is itself escaped with another '$'. We handle it
    // like this (in this example, I only handle the ' ' token separator for
    // brevity, but the other characters are handled in a similar way).
    //
    // Read 16 bytes from the current position into register chars.
    //             e.g. chars = 'fo$ bar bazzzzzzz'
    //
    // Now we use the CMEQ instruction to compare each character with
    // ' ' in parallel, using the spaces register which contains duplicated ' '
    // characters. Here we use '0' and '1' to represent an 8-bit mask of
    // all-zeros and all-ones respectively.
    //
    // mask = chars == spaces = '0001000100000000'
    //
    // We can see that we've identified the first ' ', which is the first 1 in
    // the vector. We call this the potential token terminating character
    // (PTTC). Now we need to locate it. We can do that in an
    // architecture-specific way (see first_all_ones()). Note that we also
    // identified the second ' ' but we ignore it for now and handle it during
    // the next iteration. If the maximum was 0 it means there was no PTTC and
    // we move to the next 16 characters.
    //
    // We then need to determine whether the PTTC is escaped, which means that
    // it does not terminate the token. We do that by checking whether it is
    // preceded by an odd number of '$' characters (see is_unescaped_dollar). If
    // so, we continue the loop, starting from the character following the
    // escaped character. Otherwise, we've found the end of the token and we
    // stop. In the example, we would find the preceding '$', restart from the
    // first 'b', identify the second ' ' and stop because of the lack of a
    // preceding '$'.
    //
    // We keep track of whether the token is simple (does not contain '$'
    // characters) by parallel comparisons of loaded characters against '$' into
    // a dollar mask, and accumulate (via OR) positive comparisons during loop
    // iterations without a PTTC into an accumulated dollar mask. When we
    // encounter a PTTC, we check for a preceding '$' by determining the
    // location of the first '$' in the same way as we handle the other
    // characters and checking if it is before the PTTC. At this point we don't
    // care where in the token the '$' is so when we terminate the loop we just
    // check for a non-zero accumulated dollar mask by using UMAXV.
    SIMDVec dollars = vec_dup('$');
    SIMDVec spaces = vec_dup(' ');
    SIMDVec colons = vec_dup(':');
    SIMDVec equals = vec_dup('=');
    SIMDVec newlines = vec_dup('\n');
    SIMDVec zeroes = vec_dup('\0');
    SIMDVec acc_dollar_mask = zeroes;
    while (1) {
      SIMDVec chars = vec_load(pos);
      SIMDVec dollar_mask = vec_eq(chars, dollars);
      SIMDVec space_mask = vec_eq(chars, spaces);
      SIMDVec colon_mask = vec_eq(chars, colons);
      SIMDVec equal_mask = vec_eq(chars, equals);
      SIMDVec newline_mask = vec_eq(chars, newlines);
      SIMDVec zero_mask = vec_eq(chars, zeroes);
      SIMDVec mask = newline_mask;
      if (Args & EqualsIsToken)
        mask = vec_or(mask, equal_mask);
      if (Args & ColonIsToken)
        mask = vec_or(mask, colon_mask);
      if (Args & SpaceIsSeparator)
        mask = vec_or(mask, space_mask);
      mask = vec_or(mask, zero_mask);
      uint8_t first = first_all_ones(mask);
      if (__builtin_expect(first == sizeof(SIMDVec), 1)) {
        pos += sizeof(SIMDVec);
        acc_dollar_mask = vec_or(acc_dollar_mask, dollar_mask);
        continue;
      }
      uint8_t dollar_first = first_all_ones(dollar_mask);
      simple &= dollar_first > first;
      pos += first;
      if (*pos && is_unescaped_dollar(begin, pos - 1)) {
        pos++;
        continue;
      }
      simple &= !has_all_ones(acc_dollar_mask);
      if ((Args & SpaceIsSeparator) && *pos == ' ') {
        end = pos;
        pos++;
        break;
      }
      return std::string_view(begin, pos - begin);
    }
  }
  std::string_view retval(begin, end - begin);
  while (1) {
    if (*pos == '$') {
      if (*(pos + 1) == '\n') {
        pos += 2;
        continue;
      }
    }
    if (*pos == ' ') {
      pos++;
      continue;
    }
    break;
  }
  return retval;
}

template <unsigned Args>
inline Var var_token(char*& pos) {
  Var v;
  v.value = token<Args>(pos, v.simple);
  return v;
}

using ScannedFileRangeVec =
    tbb::concurrent_vector<std::shared_ptr<ScannedFileRange>>;

void scan_file(tbb::task_group& tg, Global& global, Scope& scope,
               ScannedFileRangeVec& scanned_file_ranges, std::string_view path);

vars_t parse_indented_vars(char* pos);

void resolve_build(Global& global, Scope& scope, char* pos, HashResult hash,
                   Node*& tmp_node);

timespec now() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts;
}

timespec prog_begin = now();

void print_difference(timespec a, timespec b) {
  uint64_t a64 = a.tv_sec * 1000000000 + a.tv_nsec;
  uint64_t b64 = b.tv_sec * 1000000000 + b.tv_nsec;
  fprintf(stderr, "[%lu.%06lu] ", (a64 - b64) / 1000000000,
          ((a64 - b64) % 1000000000) / 1000);
}

void dbg(const char* format, ...) {
  static bool debug_enabled = getenv("POM_DEBUG");
  if (!debug_enabled)
    return;
  print_difference(now(), prog_begin);
  va_list ap;
  va_start(ap, format);
  vfprintf(stderr, format, ap);
}

void scan_file_range(tbb::task_group& tg, Global& global, Scope& scope,
                     ScannedFileRangeVec& scanned_file_ranges, char* begin,
                     char* end, char* file_end) {
  auto scanned_file_range = std::make_shared<ScannedFileRange>();
  auto* tmp_node = new Node;
  char* pos = begin;
  bool cur_build = false;
  bool cur_rule = false;
  char* cur_toplevel;
  auto finish_toplevel = [&](char* pos) {
    if (cur_build) {
      HashResult hash = hash_buf(cur_toplevel, pos - cur_toplevel);
      scanned_file_range->build.push_back({ cur_toplevel, hash });
      cur_build = false;
    } else if (cur_rule) {
      HashResult hash = hash_buf(cur_toplevel, pos - cur_toplevel);
      scanned_file_range->rule.push_back({ cur_toplevel, hash });
      cur_rule = false;
    }
  };
  auto parse_toplevel = [&](char*& pos) -> bool {
    bool simple;
    auto word =
        token<EqualsIsToken | ColonIsToken | SpaceIsSeparator>(pos, simple);
    if (word == "build") {
      cur_build = true;
      cur_toplevel = pos;
    } else if (word == "rule") {
      cur_rule = true;
      cur_toplevel = pos;
    } else if (word == "pool") {
      decltype(global.pool)::accessor a;
      bool simple;
      global.pool.insert(
          a,
          token<EqualsIsToken | ColonIsToken | SpaceIsSeparator>(pos, simple));
      a->second = pos;
    } else if (word == "include") {
      char* path_pos = pos;
      Var path = var_token<ColonIsToken | SpaceIsSeparator>(pos);
      if (path.simple)
        scan_file(tg, global, scope, scanned_file_ranges, path.value);
      else
        scanned_file_range->include.push_back(path_pos);
    } else if (word == "subninja")
      scanned_file_range->subninja.push_back(pos);
    else if (word == "default")
      scanned_file_range->default_.push_back(pos);
    else {
      bool simple;
      std::string_view equals =
          token<EqualsIsToken | ColonIsToken | SpaceIsSeparator>(pos, simple);
      if (equals != "=")
        error("invalid variable declaration");
      ToplevelVar v;
      static_cast<Var&>(v) = var_token<0>(pos);
      v.hash = hash_buf(v.value.data(), v.value.size());
      scanned_file_range->var.push_back({ word, v });
    }
    return false;
  };
  auto move_to_next_toplevel = [&]() {
    // FIXME: This misclassifies "# $\nfoo = bar" as a non-toplevel because
    // "$" at the end of a comment does not count as a continuation character.
    // We may need to keep track of whether the previous line is a comment.
    SIMDVec newlines = vec_dup('\n');
    while (pos < file_end) {
      SIMDVec chars_m1 = vec_load(pos - 1);
      SIMDVec mask = vec_eq(chars_m1, newlines);
      uint8_t first = first_all_ones(mask);
      if (__builtin_expect(first == sizeof(SIMDVec), 1)) {
        pos += sizeof(SIMDVec);
        continue;
      }
      pos += first;
      // A blank line ends a toplevel. CMake leaves comments before each
      // toplevel, we don't want them to be included in the hash for the
      // previous one.
      if (*pos == '\n') {
        finish_toplevel(pos);
        pos++;
        continue;
      }
      // These could also be classified with SIMD but it turns out to be slower.
      if (*pos == ' ' || *pos == '#' || is_unescaped_dollar(begin, pos - 2)) {
        pos++;
        continue;
      }
      break;
    }
    finish_toplevel(pos);
  };
  // For the beginning of the file we can't use SIMD because that would read
  // unmapped/uninitialized memory. We need to either classify the start of the
  // file as a toplevel or move past the first two characters and call
  // move_to_next_toplevel().
  while (*pos == '\n')
    pos++;
  switch (*pos) {
  case ' ':  // Normally a space at the start is an error but this could be an
             // indented comment.
  case '#':
    // The shortest character sequence before a toplevel is "#\n" so we can
    // advance by 2.
    pos += 2;
    move_to_next_toplevel();
    break;
  default:
    break;
  }
  while (pos < end) {
    parse_toplevel(pos);
    move_to_next_toplevel();
  }
  delete tmp_node;
  scanned_file_ranges.push_back(std::move(scanned_file_range));
}

void scan_file(tbb::task_group& tg, Global& global, Scope& scope,
               ScannedFileRangeVec& scanned_file_ranges,
               std::string_view path) {
  // The parser uses '\0' as an end-of-file marker. In the SIMD code paths, we
  // unconditionally read 16 bytes from an arbitrary position in the file.
  // This simplifies the SIMD code paths because it means that we don't also
  // need a scalar code path to handle data near the end of the file without
  // faulting. So we need to ensure that there are at least 16 '\0'
  // characters between the end of the file and the end of the mapping.
  int fd = open(std::string(path).c_str(), O_RDONLY);
  if (fd == -1)
    error("failed to open file");
  size_t size = lseek(fd, 0, SEEK_END);

  static size_t page_size = sysconf(_SC_PAGESIZE);
  void* addr;
  if (size % page_size > page_size - sizeof(SIMDVec)) {
    void* nulls_addr =
        mmap(0, size + page_size, PROT_READ, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (nulls_addr == MAP_FAILED)
      error("failed to map nulls");
    addr = mmap(nulls_addr, size, PROT_READ, MAP_FIXED | MAP_PRIVATE, fd, 0);
  } else {
    addr = mmap(0, size, PROT_READ, MAP_PRIVATE, fd, 0);
  }
  if (addr == MAP_FAILED)
    error("failed to map file");
  close(fd);
  char* begin = (char*)addr;
  char* end = begin + size;
  size_t chunk_size = std::max(size_t(1) << 20, (size / 128) + 1);
  for (size_t chunk = 0; chunk <= size / chunk_size; ++chunk) {
    char* chunk_begin = begin + chunk * chunk_size;
    char* chunk_end = std::min(end, begin + (chunk + 1) * chunk_size);
    if (chunk != 0)
      while (*(chunk_begin - 1) != '\n')
        chunk_begin++;
    tg.run([&tg, &global, &scope, &scanned_file_ranges, chunk_begin, chunk_end,
            end]() {
      scan_file_range(tg, global, scope, scanned_file_ranges, chunk_begin,
                      chunk_end, end);
    });
  }
}

std::string_view canonicalize(std::string_view path, std::string& buf) {
  std::string tmp_buf;
  bool moved_to_tmp_buf = false;
  size_t pos = 0;
  auto move_to_tmp_buf = [&]() {
    if (!moved_to_tmp_buf) {
      moved_to_tmp_buf = true;
      if (buf.empty()) {
        buf = path.substr(0, pos);
      } else {
        tmp_buf = path;
        buf = tmp_buf.substr(0, pos);
        path = buf;
      }
    }
  };
  if (path.starts_with("./")) {
    do
      path = path.substr(2);
    while (path.starts_with("./"));
    while (path.starts_with('/'))
      path = path.substr(1);
  }
  // FIXME: Canonicalize "/..", "/.", "//" (none seem to be needed by GN at
  // least).
  return path;
}

void resolve_build(Global& global, Scope& scope, char* pos, HashResult hash,
                   Node*& tmp_node) {
  auto* e = new Edge;
  e->scope = &scope;
  e->hash = hash;
  auto get_or_create_node = [&](Var v) {
    if (v.simple) {
      tmp_node->path = v.value;
    } else {
      append_expansion(tmp_node->path_buf, v.value, &scope);
      tmp_node->path = tmp_node->path_buf;
    }
    tmp_node->path = canonicalize(tmp_node->path, tmp_node->path_buf);
    return global.nodes.get_or_insert(tmp_node);
  };
  while (1) {
    Var out = var_token<ColonIsToken | SpaceIsSeparator>(pos);
    if (out.value == ":")
      break;
    if (out.value == "|") {
      e->first_implicit_output = e->outputs.size();
      continue;
    }
    Node* out_node = get_or_create_node(out);
    out_node->in_edge = e;
    e->outputs.push_back(out_node);
  }
  if (e->first_implicit_output == -1ul)
    e->first_implicit_output = e->outputs.size();
  bool simple;
  std::string_view rule = token<ColonIsToken | SpaceIsSeparator>(pos, simple);
  if (rule != "phony")
    e->rule_name = rule;
  while (1) {
    Var in = var_token<ColonIsToken | SpaceIsSeparator>(pos);
    if (in.value == "|") {
      e->first_implicit_input = e->inputs.size();
      continue;
    }
    if (in.value == "||") {
      e->first_order_only_input = e->inputs.size();
      continue;
    }
    if (in.value == "")
      break;
    e->inputs.push_back(get_or_create_node(in));
  }
  if (e->first_order_only_input == -1ul)
    e->first_order_only_input = e->inputs.size();
  if (e->first_implicit_input == -1ul)
    e->first_implicit_input = e->first_order_only_input;
  e->vars = pos;
}

void parse_scope(tbb::task_group& tg, Global& global, Scope* parent,
                 std::string_view path) {
  auto* s = new Scope;
  s->parent = parent;

  tbb::task_group scan_tg;
  ScannedFileRangeVec scanned_file_ranges;
  scan_file(scan_tg, global, *s, scanned_file_ranges, path);
  scan_tg.wait();

  for (auto& scanned_file_range : scanned_file_ranges) {
    for (ScannedVar& var : scanned_file_range->var) {
      s->vars[var.name] = var.value;
    }
  }

  for (auto& scanned_file_range : scanned_file_ranges) {
    if (!scanned_file_range->include.empty())
      error("FIXME: can't handle non-simple includes yet");
    for (char* inc : scanned_file_range->subninja) {
      Var path_token = var_token<ColonIsToken | SpaceIsSeparator>(inc);
      std::string path = var_expansion(path_token, s);
      tg.run([&tg, &global, s, path]() { parse_scope(tg, global, s, path); });
    }
    tg.run([&global, s, scanned_file_range]() {
      Node* tmp_node = new Node;
      for (Rule& rule : scanned_file_range->build)
        resolve_build(global, *s, rule.begin, rule.hash, tmp_node);
      delete tmp_node;
    });
  }

  for (auto& scanned_file_range : scanned_file_ranges) {
    for (Rule& rule : scanned_file_range->rule) {
      bool simple;
      char* pos = rule.begin;
      std::string_view name =
          token<EqualsIsToken | ColonIsToken | SpaceIsSeparator>(pos, simple);
      s->rule[name] = { pos, rule.hash };
    }
  }
}

struct Subprocess {
  Edge* edge;
  int fd;
  int pid;
  std::string stdout;
  std::string depfile;
  std::string rspfile;
};

struct BuildState {
  size_t parallelism = sysconf(_SC_NPROCESSORS_ONLN) + 2;
  int log_fd;
  uint32_t build_log_next_index = 0;
  std::list<Subprocess> subprocesses;
  std::deque<Edge*> pending_edges;
  size_t completed_edges = 0;
  size_t total_edges = 0;
  bool total_edges_known = false;
  std::string last_description;
};

std::string ElideMiddle(const std::string& str, size_t width) {
  switch (width) {
  case 0:
    return "";
  case 1:
    return ".";
  case 2:
    return "..";
  case 3:
    return "...";
  }
  const int kMargin = 3;  // Space for "...".
  const static std::regex ansi_escape("\\x1b[^m]*m");
  std::string result = std::regex_replace(str, ansi_escape, "");
  if (result.size() <= width) {
    return str;
  }
  int32_t elide_size = (width - kMargin) / 2;

  std::vector<std::pair<int32_t, std::string>> escapes;
  size_t added_len = 0;  // total number of characters

  std::sregex_iterator it(str.begin(), str.end(), ansi_escape);
  std::sregex_iterator end;
  while (it != end) {
    escapes.emplace_back(it->position() - added_len, it->str());
    added_len += it->str().size();
    ++it;
  }

  std::string new_status =
      result.substr(0, elide_size) + "..." +
      result.substr(result.size() - elide_size - ((width - kMargin) % 2));

  added_len = 0;
  // We need to put all ANSI escape codes back in:
  for (const auto& escape : escapes) {
    int32_t pos = escape.first;
    if (pos > elide_size) {
      pos -= result.size() - width;
      if (pos < static_cast<int32_t>(width) - elide_size) {
        pos = width - elide_size - (width % 2 == 0 ? 1 : 0);
      }
    }
    pos += added_len;
    new_status.insert(pos, escape.second);
    added_len += escape.second.size();
  }
  return new_status;
}

void update_build_line(BuildState& state) {
  std::string line = "[" + std::to_string(state.completed_edges) + "/";
  if (state.total_edges_known)
    line += std::to_string(state.total_edges);
  else
    line += "???";
  line += "] " + state.last_description;

  winsize size;
  if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) && size.ws_col) {
    line = ElideMiddle(line, size.ws_col);
  }
  printf("\r%s\x1B[K", line.c_str());
  fflush(stdout);
}

// Equivalent to mkdir -p $(dirname path)
void mkdirs(std::string_view path) {
  size_t slash_pos = 0;
  while (1) {
    slash_pos = path.find('/', slash_pos + 1);
    if (slash_pos == std::string_view::npos)
      break;
    int ret = mkdir(std::string(path.substr(0, slash_pos)).c_str(), 0755);
    if (ret != 0 && errno != EEXIST)
      error("mkdirs failed");
  }
}

Rule* find_rule(Edge* e) {
  Scope* cur_scope = e->scope;
  std::string_view rule_name = e->rule_name;
  rules_t::iterator i;
  while (cur_scope) {
    i = cur_scope->rule.find(rule_name);
    if (i != cur_scope->rule.end())
      break;
    cur_scope = cur_scope->parent;
  }
  if (!cur_scope)
    error("could not find rule");
  return &i->second;
}

void schedule_subprocess(BuildState& state, Edge* e) {
  if (state.subprocesses.size() >= state.parallelism) {
    state.pending_edges.push_back(e);
    return;
  }

  Rule* rule = find_rule(e);
  vars_t rule_vars = parse_indented_vars(rule->begin);
  vars_t build_vars = parse_indented_vars(e->vars);

  ExpansionScope es(e->scope);
  es.build_edge = e;
  es.rule_vars = &rule_vars;
  es.build_vars = &build_vars;

  auto command_var = rule_vars.find("command");
  if (command_var == rule_vars.end())
    error("rule missing command");
  std::string command = var_expansion(command_var->second, es);

  std::string description;
  auto description_var = rule_vars.find("description");
  if (description_var == rule_vars.end())
    description = command;
  else
    description = var_expansion(description_var->second, es);
  state.last_description = description;
  update_build_line(state);

  Subprocess proc;

  for (Node* out : e->outputs)
    mkdirs(out->path);

  auto depfile_var = rule_vars.find("depfile");
  if (depfile_var != rule_vars.end()) {
    proc.depfile = var_expansion(depfile_var->second, es);
    mkdirs(proc.depfile);
  }

  auto rspfile_var = rule_vars.find("rspfile");
  if (rspfile_var != rule_vars.end()) {
    proc.rspfile = var_expansion(rspfile_var->second, es);
    mkdirs(proc.rspfile);

    auto rspfile_content_var = rule_vars.find("rspfile_content");
    if (rspfile_content_var == rule_vars.end())
      error("rspfile rule missing rspfile_content");
    std::string rspfile_content =
        var_expansion(rspfile_content_var->second, es);
    int fd = open(proc.rspfile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
      error("rspfile open failed");
    ssize_t size;
    while ((size = write(fd, rspfile_content.c_str(), rspfile_content.size())) <
               0 &&
           errno == EINTR)
      ;
    if (size < 0)
      error("rspfile write failed");
    close(fd);
  }

  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0)
    error("pipe2 failed");

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0)
    error("posix_spawn_file_actions_init failed");
  if (posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO) != 0)
    error("posix_spawn_file_actions_adddup2 failed");
  if (posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO) != 0)
    error("posix_spawn_file_actions_adddup2 failed");
  const char* argv[] = { "sh", "-c", command.c_str(), nullptr };
  if (posix_spawnp(&proc.pid, "sh", &actions, nullptr, (char* const*)argv,
                   environ) != 0)
    error("posix_spawnp failed");
  if (posix_spawn_file_actions_destroy(&actions) != 0)
    error("posix_spawn_file_actions_destroy failed");
  close(fds[1]);
  proc.edge = e;
  proc.fd = fds[0];

  dbg("schedule '%s' (pid %d)\n", command.c_str(), proc.pid);
  state.subprocesses.push_back(std::move(proc));
}

std::optional<HashResult> compute_edge_hash(Edge* e);

void compute_edge_dirty(Edge* e) {
  if (!e->rule_name.empty()) {
    std::optional<HashResult> hash = compute_edge_hash(e);
    std::optional<HashResult> bl_hash = e->outputs[0]->build_log_hash;
    e->dirty = (hash && bl_hash) ? *hash != *bl_hash : true;
  } else {
    e->dirty = false;
  }
}

// The build log is a combination of the build log and the deps log from the
// existing implementation of Ninja. Like the build log and deps log, it's
// designed so that incremental builds can just append to the file. It should
// also be possible to implement recompaction, but that's not done yet. It
// consists of a list of concatenated build log entries. Each build log entry
// takes one of the following forms:
// 1. A node introducer, consisting of a canonicalized path name followed by a
//    null terminator. Each node introducer causes a node slot number to be
//    allocated starting from 0.
// 2. A node manifest. Each node manifest consists of the following items:
//    a. A null byte. Because paths always contain at least one character,
//       a node manifest can always be distinguished from a node introducer.
//    b. A 4-byte node slot number.
//    c. A 16-byte XXH128 hash that combines the hashes of the text of the build
//       statement, the rule, and (TODO) the referenced variables, the
//       mtimes of the input and output files and the mtimes of the filenames
//       read from the depfile.
//    d. A list of slot numbers for the filenames read from the depfile.
//
// Filling in the node data from the build log turns out to be expensive enough
// to be a build bottleneck in a null build of Clang (GN build) unless it is
// parallelized. Therefore we save a list of observed node manifest pointers and
// use them to complete the node data in a parallel loop.
//
// FIXME: This file format is not self-synchronizing so it can't be trivially
// processed in parallel. It may turn out to be necessary to redesign the format
// and/or the parser to better support parallel processing.
void read_build_log(Global& global, BuildState& state) {
  state.log_fd = open(".pom_log", O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (state.log_fd < 0)
    error("failed to open build log");
  size_t size = lseek(state.log_fd, 0, SEEK_END);
  if (size == 0)
    return;

  void* addr = mmap(0, size, PROT_READ, MAP_PRIVATE, state.log_fd, 0);
  if (addr == MAP_FAILED)
    error("failed to map file");
  char* pos = (char*)addr;
  char* end = pos + size;

  std::vector<Node*> nodes;
  std::vector<char*> data_pos;
  auto* tmp_node = new Node;
  while (pos < end) {
    size_t node_len = strnlen(pos, end - pos);
    if (pos + node_len == end)
      error("invalid build log file");
    if (node_len == 0) {
      if (end - pos < 25)
        error("invalid build log file");
      uint32_t idx;
      memcpy(&idx, pos + 1, 4);
      if (idx >= nodes.size())
        error("invalid build log file");
      if (data_pos.size() <= idx)
        data_pos.resize(idx + 1);
      data_pos[idx] = pos + 5;
      uint32_t depfile_idx_count;
      memcpy(&depfile_idx_count, pos + 21, 4);
      if (end - pos < 25 + 4 * depfile_idx_count)
        error("invalid build log file");
      pos += 25 + 4 * depfile_idx_count;
    } else {
      tmp_node->path = std::string_view(pos, node_len);
      Node* n = global.nodes.get_or_insert(tmp_node);
      n->build_log_index = nodes.size();
      nodes.push_back(n);
      pos += node_len + 1;
    }
  }
  delete tmp_node;
  dbg("done scanning build log\n");

  tbb::parallel_for_each(data_pos, [&](char*& pos) {
    if (!pos)
      return;
    size_t idx = &pos - data_pos.data();
    Node* node = nodes[idx];
    HashResult hash;
    memcpy(&hash, pos, 16);
    node->build_log_hash = hash;
    uint32_t depfile_idx_count;
    memcpy(&depfile_idx_count, pos + 16, 4);
    node->depfile_inputs.resize(depfile_idx_count);
    for (uint32_t i = 0; i != depfile_idx_count; ++i) {
      uint32_t depfile_idx;
      memcpy(&depfile_idx, pos + 20 + 4 * i, 4);
      if (depfile_idx >= nodes.size())
        error("invalid build log file");
      node->depfile_inputs[i] = nodes[depfile_idx];
    }
  });

  state.build_log_next_index = nodes.size();
  dbg("done reading build log\n");
}

void write_build_log(BuildState& state, Edge* e) {
  auto introduce_node = [&](Node* n) {
    if (n->build_log_index != -1u)
      return n->build_log_index;
    if (n->path.empty())
      error("attempt to introduce an empty path");
    write(state.log_fd, n->path.data(), n->path.size());
    write(state.log_fd, "", 1);
    return n->build_log_index = state.build_log_next_index++;
  };
  std::vector<uint32_t> depfile_idxs;
  for (Node* n : e->outputs[0]->depfile_inputs)
    depfile_idxs.push_back(introduce_node(n));
  std::optional<HashResult> hash = compute_edge_hash(e);
  if (hash) {
    uint32_t idx = introduce_node(e->outputs[0]);
    write(state.log_fd, "", 1);
    write(state.log_fd, &idx, 4);
    write(state.log_fd, &*hash, 16);
    uint32_t depfile_size = depfile_idxs.size();
    write(state.log_fd, &depfile_size, 4);
    for (uint32_t depfile_idx : depfile_idxs)
      write(state.log_fd, &depfile_idx, 4);
  }
}

void monitor_subprocesses(BuildState& state, Global& global) {
  auto handle_termination = [&](decltype(state.subprocesses)::iterator i) {
    if (!i->stdout.empty())
      printf("\n%s", i->stdout.c_str());
    int wstatus;
    if (waitpid(i->pid, &wstatus, 0) < 0)
      error("waitpid failed");
    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0)
      error("subprocess exited abnormally");
    dbg("pid %d exited\n", i->pid);
    ++state.completed_edges;
    Edge* e = i->edge;
    std::string depfile = std::move(i->depfile);
    std::string rspfile = std::move(i->rspfile);
    close(i->fd);
    state.subprocesses.erase(i);

    if (!state.pending_edges.empty()) {
      Edge* next = state.pending_edges.front();
      state.pending_edges.pop_front();
      schedule_subprocess(state, next);
    }

    e->dirty = false;
    for (Node* n : e->outputs)
      n->statted = false;

    std::deque<Edge*> cleaned_edges;
    cleaned_edges.push_back(e);
    while (!cleaned_edges.empty()) {
      Edge* e = cleaned_edges.front();
      cleaned_edges.pop_front();
      for (Node* output : e->outputs) {
        for (Edge* out_edge : output->out_edges) {
          if (!out_edge->dirty || out_edge->started)
            continue;
          bool cleaned_all_deps = true;
          for (Node* input : out_edge->inputs) {
            if (input->in_edge && input->in_edge->dirty) {
              cleaned_all_deps = false;
              break;
            }
          }
          if (cleaned_all_deps) {
            out_edge->started = true;
            compute_edge_dirty(out_edge);
            if (out_edge->dirty) {
              schedule_subprocess(state, out_edge);
            } else {
              cleaned_edges.push_back(out_edge);
              if (!out_edge->rule_name.empty())
                --state.total_edges;
            }
          }
        }
      }
    }

    if (!depfile.empty()) {
      dbg("read depfile: %s\n", depfile.c_str());
      int depfile_fd = open(depfile.c_str(), O_RDONLY);
      if (depfile_fd < 0)
        error("failed to open depfile");
      size_t depfile_size = lseek(depfile_fd, 0, SEEK_END);
      std::string depfile_content(depfile_size, '\0');
      size_t size;
      while ((size = pread(depfile_fd, depfile_content.data(), depfile_size,
                           0)) < 0 &&
             errno == EINTR)
        ;
      close(depfile_fd);
      unlink(depfile.c_str());

      DepfileParser parser;
      std::string err;
      if (!parser.Parse(&depfile_content, &err))
        error("depfile parser failed");

      auto& depfile_inputs = e->outputs[0]->depfile_inputs;
      depfile_inputs.clear();
      auto* tmp_node = new Node;
      for (std::string_view input : parser.ins_) {
        dbg("found depfile entry: %s\n", std::string(input).c_str());
        std::string buf;
        tmp_node->path_buf = canonicalize(input, buf);
        tmp_node->path = tmp_node->path_buf;
        Node* n = global.nodes.get_or_insert(tmp_node);
        depfile_inputs.push_back(n);
      }
      delete tmp_node;
    } else {
      e->outputs[0]->depfile_inputs.clear();
    }
    write_build_log(state, e);

    if (!rspfile.empty())
      unlink(rspfile.c_str());
  };
  if (state.subprocesses.empty()) {
    puts("ninja: no work to do.");
    return;
  }
  update_build_line(state);
  while (!state.subprocesses.empty()) {
    std::vector<decltype(state.subprocesses)::iterator> procs;
    std::vector<pollfd> pfds;
    for (auto i = state.subprocesses.begin(); i != state.subprocesses.end();
         ++i) {
      procs.push_back(i);
      pfds.push_back({ i->fd, POLLIN, 0 });
    }
    if (poll(pfds.data(), pfds.size(), -1) < 0)
      error("poll failed");
    for (size_t i = 0; i != pfds.size(); ++i) {
      char buf[4096];
      if (pfds[i].revents) {
        size_t size;
        while ((size = read(pfds[i].fd, buf, 4096)) < 0 && errno == EINTR)
          ;
        if (size < 0)
          error("read failed");
        procs[i]->stdout.append(buf, size);
        if (size == 0)
          handle_termination(procs[i]);
      }
    }
  }
  update_build_line(state);
  puts("");
}

std::optional<HashResult> compute_edge_hash(Edge* e) {
  // Skip phony edges. These are handled recursively when computing the Merkle
  // tree for the referents (see add_inputs below).
  if (e->rule_name.empty())
    return std::nullopt;
  std::vector<uint64_t> merkle;
  auto stat_node = [](Node* n) {
    // This function returns true if the file does not exist. This leads
    // to an early return without computing the hash. This is because there is
    // no point in computing the hash if a file is missing, because that means
    // that we'll need to rebuild anyway.
    //
    // Note that this function allows multiple tasks to begin stat'ing the
    // same file. Although it's racy, it's a benign race because each stat
    // will read the same mtime.
    if (n->statted.load(std::memory_order_acquire))
      return n->nonexistent;
    struct stat s;
    if (stat(std::string(n->path).c_str(), &s) == 0) {
      n->mtime = s.st_mtim;
      n->nonexistent = false;
    } else {
      n->nonexistent = true;
    }
    n->statted.store(true, std::memory_order_release);
    return n->nonexistent;
  };
  auto add_node = [&](Node* n) {
    if (stat_node(n))
      return true;
    merkle.push_back(n->mtime.tv_sec);
    merkle.push_back(n->mtime.tv_nsec);
    return false;
  };
  for (Node* n : e->outputs)
    if (add_node(n))
      return std::nullopt;
  std::function<bool(Edge*)> add_inputs;
  add_inputs = [&](Edge* e) {
    merkle.push_back(e->hash.lo);
    merkle.push_back(e->hash.hi);
    for (Node* n : e->non_order_only_inputs()) {
      if (n->in_edge && n->in_edge->rule_name.empty()) {
        if (add_inputs(n->in_edge))
          return true;
      } else if (add_node(n)) {
        if (!n->in_edge)
          error("missing input file");
        return true;
      }
    }
    return false;
  };
  if (add_inputs(e))
    return std::nullopt;
  for (Node* n : e->outputs[0]->depfile_inputs)
    if (add_node(n))
      return std::nullopt;
  Rule* rule = find_rule(e);
  merkle.push_back(rule->hash.lo);
  merkle.push_back(rule->hash.hi);
  return hash_buf(merkle.data(), 8 * merkle.size());
}

// The work identification phase of Ninja can be thought of as requiring the
// following tasks to be performed on each node that is reachable from the
// requested target:
//
// 1. Call stat() on files that are inputs or outputs to the edges, and compute
//    the edge's Merkle hash if necessary.
// 2. Identify an edge as being dirty or clean, as well as whether its
//    subprocess may be started immediately (whether it is an "initial edge").
//    An edge is considered dirty if its input edges are dirty or its Merkle
//    hash does not match the one from the previous run, and an initial edge is
//    a dirty edge whose input edges are clean.
//
// Note that step 2 requires a post-order traversal of the build graph, because
// it must know whether the inputs are dirty in order to classify the edge as
// clean, dirty or initial. Naively it would look like this:
//
// for each edge e in post-order traversal of G {
//   for each node n that is an input/output of e:
//     stat(n);
//   compute_hash(e);
//   classify_edge(e);
//   if (is_initial_edge(e))
//     schedule_subprocess(e);
// }
//
// But we would like to stat() and compute hashes in parallel, so it's not as
// simple as that. By stat()ing files in parallel we give an opportunity to the
// kernel to parallelize the work of resolving path names. This can save several
// hundred milliseconds in a build of Chromium.
//
// To set up stat() to be parallelized, we create a pipeline between the
// post-order traversal, the stat() calls and hash computation and the
// dirty/clean/initial edge classification. The post-order traversal is
// performed by the mark() function. After it has collected a batch of edges of
// size 1024, it starts a task to call the classify_edges() function, which
// handles steps 1 and 2. The work for step 1 can happen in any order relative
// to other tasks because it is independent of other edges, but step 2 must
// preserve the post-order traversal ordering. We handle this by using an atomic
// variable to keep track of how many previous tasks have finished step 2. After
// step 1 we spin reading from the atomic variable, and once the number of tasks
// reaches our task identifier, which is assigned sequentially by mark(), we
// perform our work on step 2 and increment the variable to unblock the next
// task.
//
// When an edge subprocess terminates, we need to schedule any edges that were
// blocked by that edge. Such edges are identified using the out_edges array on
// Node. Since this is the only case where we need the out_edges array, and it
// is expensive to fill in out_edges for each node unconditionally, step 2 is
// also responsible for filling in out_edges on any nodes that are identified as
// outputs of dirty edges. This also implies that although we can start edge
// subprocesses during classify_edges(), handling subprocess completion needs to
// wait until after all tasks are finished with step 2.
void classify_edges(std::span<Edge* const> edges, size_t task_id,
                    std::atomic<size_t>& task_count, BuildState& state) {
  for (Edge* e : edges)
    compute_edge_dirty(e);
  while (task_count.load(std::memory_order_acquire) != task_id)
    ;
  for (Edge* e : edges) {
    bool has_dirty_dep = false;
    for (Node* dep : e->inputs) {
      if (dep->in_edge && dep->in_edge->dirty) {
        dep->out_edges.push_back(e);
        has_dirty_dep = true;
      }
    }
    if (has_dirty_dep)
      e->dirty = true;
    if (e->dirty && !e->rule_name.empty())
      ++state.total_edges;
    if (e->dirty && !has_dirty_dep) {
      static bool first = false;
      if (!first) {
        first = true;
        dbg("schedule first task\n");
      }
      schedule_subprocess(state, e);
    }
  }
  task_count.store(task_id + 1, std::memory_order_release);
}

void mark(tbb::task_group& tg, Node* n, std::vector<Edge*>& needed_edges,
          size_t& task_id, std::atomic<size_t>& task_count, BuildState& state) {
  if (!n->in_edge)
    return;
  Edge* e = n->in_edge;
  if (e->needed)
    return;
  e->needed = true;
  for (Node* dep : e->inputs)
    mark(tg, dep, needed_edges, task_id, task_count, state);
  needed_edges.push_back(e);
  constexpr size_t chunk_size = 1024;
  if (needed_edges.size() == chunk_size) {
    tg.run([needed_edges, task_id, &task_count, &state]() {
      classify_edges(needed_edges, task_id, task_count, state);
    });
    ++task_id;
    needed_edges.clear();
  }
}

vars_t parse_indented_vars(char* pos) {
  vars_t result;
  pos++;
  while (*pos != '\n') {
    if (*pos == ' ') {
      while (*++pos == ' ')
        ;
    } else {
      break;
    }
    if (*pos == '#') {
      while (*pos++ != '\n')
        ;
      continue;
    }
    bool simple;
    std::string_view var =
        token<EqualsIsToken | ColonIsToken | SpaceIsSeparator>(pos, simple);
    std::string_view equals =
        token<EqualsIsToken | ColonIsToken | SpaceIsSeparator>(pos, simple);
    if (equals != "=")
      error("invalid variable declaration");
    result[var] = var_token<0>(pos);
    pos++;
  }
  return result;
}

void parse(Global& global, std::string_view path,
           BuildState* state_for_build_log) {
  tbb::task_group tg;
  if (state_for_build_log) {
    tg.run([&]() { read_build_log(global, *state_for_build_log); });
  }
  parse_scope(tg, global, nullptr, path);
  tg.wait();
  dbg("%zu nodes\n", global.nodes.size());
}

void print_command(Edge* e) {
  if (e->rule_name.empty())
    return;
  Rule* rule = find_rule(e);
  vars_t rule_vars = parse_indented_vars(rule->begin);
  vars_t build_vars = parse_indented_vars(e->vars);

  auto command_var = rule_vars.find("command");
  if (command_var == rule_vars.end())
    error("rule missing command");
  Var& v = command_var->second;
  ExpansionScope es(e->scope);
  es.build_edge = e;
  es.rule_vars = &rule_vars;
  es.build_vars = &build_vars;
  std::string command = var_expansion(v, es);
  puts(command.c_str());
}

void print_commands(Node* n) {
  if (!n->in_edge || n->in_edge->needed)
    return;
  n->in_edge->needed = true;
  for (Node* input : n->in_edge->inputs)
    print_commands(input);
  print_command(n->in_edge);
}

int main(int argc, char** argv) {
  // the perf tool still has some pac bugs
  syscall(__NR_prctl, PR_PAC_SET_ENABLED_KEYS,
          PR_PAC_APDAKEY | PR_PAC_APDBKEY | PR_PAC_APIAKEY | PR_PAC_APIBKEY, 0,
          0, 0);

  std::string_view manifest_path = "build.ninja";
  std::vector<std::string_view> targets;
  std::string_view tool;
  for (int i = 1; i != argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "-f") {
      manifest_path = argv[++i];
    } else if (arg == "-C") {
      chdir(argv[++i]);
    } else if (arg == "-t") {
      tool = argv[++i];
    } else {
      targets.push_back(arg);
    }
  }

  bool build_log_required = tool == "" || tool == "loadlog";
  BuildState state;
  auto global = std::make_unique<Global>();
  parse(*global, manifest_path, build_log_required ? &state : nullptr);
  if (tool == "") {
    std::vector<Edge*> needed_edges;
    tbb::task_group tg;
    size_t task_id = 0;
    std::atomic<size_t> task_count = 0;
    for (auto target : targets) {
      Node* n = global->nodes[target];
      if (!n)
        error("unknown target");
      mark(tg, n, needed_edges, task_id, task_count, state);
    }
    classify_edges(needed_edges, task_id, task_count, state);
    tg.wait();
    state.total_edges_known = true;
    dbg("monitoring subprocesses\n");
    monitor_subprocesses(state, *global);
    dbg("done\n");
  } else if (tool == "commands") {
    for (auto target : targets) {
      Node* n = global->nodes[target];
      if (!n)
        error("unknown target");
      print_commands(n);
    }
  } else if (tool == "loadlog") {
  } else {
    error("unsupported tool");
  }
  fflush(stdout);
  _exit(0);
}
