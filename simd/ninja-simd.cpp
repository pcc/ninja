#include <arm_neon.h>
#include <ctime>
#include <fcntl.h>
#include <linux/prctl.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <sys/prctl.h>
#include <sys/stat.h>

#include "oneapi/tbb/concurrent_hash_map.h"
#include "oneapi/tbb/concurrent_vector.h"
#include "oneapi/tbb/parallel_for_each.h"
#include "oneapi/tbb/task_group.h"
#include <phmap.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

using namespace oneapi;

struct HashResult {
  uint64_t lo, hi;
};

struct Toplevel {
  char *begin;
  HashResult hash;
};

struct Var {
  std::string_view value;
  HashResult hash;
  bool simple;
};

using rules_t = std::unordered_map<std::string_view, Toplevel>;
using vars_t = std::unordered_map<std::string_view, Var>;

struct Node;

struct Edge {
  std::vector<Node *> outputs, inputs;
  Toplevel rule;
  size_t first_implicit, first_order_only;
  char *vars;
  HashResult hash;
};

struct Node {
  std::string path_buf;
  std::string_view path;
  Edge *in_edge = nullptr;
  bool needed = false;
};

struct Global {
  tbb::concurrent_hash_map<std::string_view, char *> pool;
  phmap::parallel_flat_hash_map<
      std::string_view, Node *,
      phmap::priv::hash_default_hash<std::string_view>,
      phmap::priv::hash_default_eq<std::string_view>,
      phmap::priv::Allocator<phmap::priv::Pair<const std::string_view, Node *>>,
      12, std::mutex>
      nodes;
};
struct Scope {
  Scope *parent;
  vars_t vars;
  rules_t rule;
};
struct Phase1Scope {
  std::vector<Toplevel> build;
  std::vector<char *> subninja, default_;
};

void error(const char *err) {
  fprintf(stderr, "error: %s\n", err);
  exit(1);
}

HashResult hash_buf(const void *buf, size_t size) {
  auto h = XXH3_128bits(buf, size);
  HashResult result;
  result.lo = h.low64;
  result.hi = h.high64;
  return result;
}

std::string_view expand(std::string_view token, std::span<const vars_t *> scope,
                        std::string &buf, size_t recursion_depth = 0) {
  if (recursion_depth == 16)
    error("recursion limit reached during variable expansion");
  for (size_t i = 0; i != token.size(); ++i) {
    if (token[i] == '$') {
      buf.assign(token.begin(), i);
      while (1) {
        if (token[i + 1] == ' ' || token[i + 1] == ':' || token[i + 1] == '$') {
          buf.push_back(token[i + 1]);
          i += 2;
        } else if (token[i + 1] == '\n') {
          i += 2;
          while (token[i] == ' ')
            ++i;
        } else {
          size_t name_begin = i + 1;
          size_t name_end = name_begin;
          while ((token[name_end] >= '0' && token[name_end] <= '9') ||
                 (token[name_end] >= 'A' && token[name_end] <= 'Z') ||
                 (token[name_end] >= 'a' && token[name_end] <= 'z') ||
                 token[name_end] == '_')
            name_end++;
          std::string_view name =
              token.substr(name_begin, name_end - name_begin);
          for (const vars_t *vars : scope) {
            auto i = vars->find(name);
            if (i != vars->end()) {
              const Var &v = i->second;
              if (v.simple) {
                buf += v.value;
              } else {
                std::string sub_buf;
                buf += expand(v.value, scope, sub_buf, recursion_depth + 1);
              }
              break;
            }
          }
          i = name_end;
        }
        while (1) {
          if (i == token.size() || token[i] == '\n')
            return buf;
          if (token[i] == '$')
            break;
          buf.push_back(token[i]);
          i++;
        }
      }
    }
    if (token[i] == '\n')
      return token.substr(0, i);
  }
  return token;
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
static bool is_unescaped_dollar(char *begin, char *pos) {
  size_t num_dollars = 0;
  while (pos >= begin && *pos-- == '$')
    num_dollars++;
  return num_dollars % 2 == 1;
}

// Returns a vector that may be ANDed with a vector of comparison results which
// may then be used as an input to UMAXV (max of all vector elements) to
// identify the first all-ones element. The max will be 0 in case of no matches,
// 16 if the first match was the first element, 15 if it's the second
// element, etc. It's noinline because GCC wants to move a load of this vector
// into the middle of our tight loops otherwise.
__attribute__((noinline)) uint8x16_t first_all_ones_mask_identifier() {
  uint8x16_t identifier = {16, 15, 14, 13, 12, 11, 10, 9,
                           8,  7,  6,  5,  4,  3,  2,  1};
  return identifier;
}

// Consume a token and its following whitespace. Returns the token (without
// whitespace).
//
// A "simple" token is one that does not contain a "$" character. This means
// that it does not need to be expanded and we do not need to search for
// dependencies when computing a hash.
template <unsigned Args>
inline std::string_view token(char *&pos, bool &simple) {
  char *begin = pos;
  char *end;
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
    // (PTTC). Now we need to locate it. We can do that by ANDing the register
    // with the magic "first_all_ones_mask_identifier" vector. In the example,
    // this will produce the following result:
    //   {0, 0, 0, 13, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0}
    // Now we use the UMAXV instruction to take the maximum of all elements of
    // the vector, which is 13. Subtracting 16 from that gives us our byte
    // position of 3. Note that we also identified the second ' ' but we ignore
    // it for now and handle it during the next iteration. If the maximum was 0
    // it means there was no PTTC and we move to the next 16 characters.
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
    uint8x16_t dollars = vdupq_n_u8('$');
    uint8x16_t spaces = vdupq_n_u8(' ');
    uint8x16_t colons = vdupq_n_u8(':');
    uint8x16_t equals = vdupq_n_u8('=');
    uint8x16_t newlines = vdupq_n_u8('\n');
    uint8x16_t zeroes = vdupq_n_u8('\0');
    uint8x16_t acc_dollar_mask = zeroes;
    uint8x16_t identifier = first_all_ones_mask_identifier();
    while (1) {
      uint8x16_t chars = *(uint8x16_t *)pos;
      uint8x16_t dollar_mask = vceqq_u8(chars, dollars);
      uint8x16_t space_mask = vceqq_u8(chars, spaces);
      uint8x16_t colon_mask = vceqq_u8(chars, colons);
      uint8x16_t equal_mask = vceqq_u8(chars, equals);
      uint8x16_t newline_mask = vceqq_u8(chars, newlines);
      uint8x16_t zero_mask = vceqq_u8(chars, zeroes);
      uint8x16_t mask = newline_mask;
      if (Args & EqualsIsToken)
        mask = vorrq_u8(mask, equal_mask);
      if (Args & ColonIsToken)
        mask = vorrq_u8(mask, colon_mask);
      if (Args & SpaceIsSeparator)
        mask = vorrq_u8(mask, space_mask);
      mask = vorrq_u8(mask, zero_mask);
      uint8_t max = vmaxvq_u8(vandq_u8(mask, identifier));
      if (__builtin_expect(!max, 1)) {
        pos += 16;
        acc_dollar_mask = vorrq_u8(acc_dollar_mask, dollar_mask);
        continue;
      }
      uint8_t dollar_max = vmaxvq_u8(vandq_u8(dollar_mask, identifier));
      simple &= dollar_max < max;
      pos += 16 - max;
      if (*pos && is_unescaped_dollar(begin, pos - 1)) {
        pos++;
        continue;
      }
      simple &= vmaxvq_u8(acc_dollar_mask) == 0;
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

void parse_file(Global &global, Scope &scope, Phase1Scope &scope1,
                std::string_view path);

void parse_file_range(Global &global, Scope &scope, Phase1Scope &scope1,
                      char *begin, char *end) {
  char *pos = begin;
  bool cur_build = false;
  std::string_view cur_rule;
  char *cur_toplevel;
  auto finish_toplevel = [&](char *pos) {
    if (cur_build) {
      HashResult hash = hash_buf(cur_toplevel, pos - cur_toplevel);
      scope1.build.push_back({cur_toplevel, hash});
      cur_build = false;
    } else if (!cur_rule.empty()) {
      HashResult hash = hash_buf(cur_toplevel, pos - cur_toplevel);
      auto &rule = scope.rule[cur_rule];
      rule.begin = cur_toplevel;
      rule.hash = hash;
      cur_rule = "";
    }
  };
  auto parse_toplevel = [&](char *&pos) -> bool {
    bool simple;
    auto word =
        token<EqualsIsToken | ColonIsToken | SpaceIsSeparator>(pos, simple);
    if (word == "build") {
      cur_build = true;
      cur_toplevel = pos;
    } else if (word == "rule") {
      bool simple;
      cur_rule =
          token<EqualsIsToken | ColonIsToken | SpaceIsSeparator>(pos, simple);
      cur_toplevel = pos;
    } else if (word == "pool") {
      decltype(global.pool)::accessor a;
      bool simple;
      global.pool.insert(
          a,
          token<EqualsIsToken | ColonIsToken | SpaceIsSeparator>(pos, simple));
      a->second = pos;
    } else if (word == "include") {
      bool simple;
      std::string_view path =
          token<EqualsIsToken | ColonIsToken | SpaceIsSeparator>(pos, simple);
      std::string buf;
      const vars_t *var_scope = &scope.vars;
      parse_file(global, scope, scope1, expand(path, {&var_scope, 1}, buf));
    } else if (word == "subninja")
      scope1.subninja.push_back(pos);
    else if (word == "default")
      scope1.default_.push_back(pos);
    else {
      bool simple;
      std::string_view equals =
          token<EqualsIsToken | ColonIsToken | SpaceIsSeparator>(pos, simple);
      if (equals != "=")
        error("invalid variable declaration");
      Var v;
      v.value = token<0>(pos, v.simple);
      v.hash = hash_buf(v.value.data(), v.value.size());
      scope.vars.insert({word, v});
    }
    return false;
  };
  auto move_to_next_toplevel = [&]() {
    // FIXME: This misclassifies "# $\nfoo = bar" as a non-toplevel because
    // "$" at the end of a comment does not count as a continuation character.
    // We may need to keep track of whether the previous line is a comment.
    uint8x16_t newlines = vdupq_n_u8('\n');
    uint8x16_t identifier = first_all_ones_mask_identifier();
    while (pos < end) {
      uint8x16_t chars_m1 = *(uint8x16_t *)(pos - 1);
      uint8x16_t mask = vceqq_u8(chars_m1, newlines);
      uint8_t max = vmaxvq_u8(vandq_u8(mask, identifier));
      if (__builtin_expect(!max, 1)) {
        pos += 16;
        continue;
      }
      pos += 16 - max;
      ;
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
      finish_toplevel(pos);
      break;
    }
  };
  // For the beginning of the file we can't use SIMD because that would read
  // unmapped/uninitialized memory. We need to either classify the start of the
  // file as a toplevel or move past the first two characters and call
  // move_to_next_toplevel().
  while (*pos == '\n')
    pos++;
  switch (*pos) {
  case ' ': // Normally a space at the start is an error but this could be an
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
}

void parse_file(Global &global, Scope &scope, Phase1Scope &scope1,
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
  void *addr;
  if (size % page_size > page_size - 16) {
    void *nulls_addr =
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
  char *begin = (char *)addr;
  char *end = begin + size;
  parse_file_range(global, scope, scope1, begin, end);
}

std::string_view canonicalize(std::string_view path, std::string &buf) {
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

__attribute__((noinline)) void here() {}

void resolve_build(Global &global, Scope &scope, std::span<Toplevel> builds) {
  // keep a node allocated so we don't have to allocate one while holding the
  // lock
  auto *tmp_node = new Node;
  for (Toplevel &build : builds) {
    char *pos = build.begin;
    auto *e = new Edge;
    e->hash = build.hash;
    auto get_or_create_node = [&](std::string_view token, bool simple) {
      const vars_t *var_scope = &scope.vars;
      if (simple)
        tmp_node->path = token;
      else
        tmp_node->path = expand(token, {&var_scope, 1}, tmp_node->path_buf);
      if (tmp_node->path == "./chrome")
        here();
      tmp_node->path = canonicalize(tmp_node->path, tmp_node->path_buf);
      Node *n;
      if (global.nodes.try_emplace_l(
              tmp_node->path, [&](auto &val) { n = val.second; }, tmp_node)) {
        n = tmp_node;
        tmp_node = new Node;
      } else {
        tmp_node->path_buf.clear();
      }
      return n;
    };
    while (1) {
      bool simple;
      std::string_view out =
          token<ColonIsToken | SpaceIsSeparator>(pos, simple);
      if (out == ":")
        break;
      Node *out_node = get_or_create_node(out, simple);
      out_node->in_edge = e;
      e->outputs.push_back(out_node);
    }
    bool simple;
    std::string_view rule = token<ColonIsToken | SpaceIsSeparator>(pos, simple);
    if (rule == "phony") {
      e->rule = {};
    } else {
      Scope *cur_scope = &scope;
      rules_t::const_iterator i;
      while (cur_scope) {
        i = cur_scope->rule.find(rule);
        if (i != cur_scope->rule.end())
          break;
        cur_scope = cur_scope->parent;
      }
      if (!cur_scope)
        error("could not find rule");
      e->rule = i->second;
    }
    while (1) {
      bool simple;
      std::string_view in = token<ColonIsToken | SpaceIsSeparator>(pos, simple);
      if (in == "|") {
        e->first_implicit = e->inputs.size();
        continue;
      }
      if (in == "||") {
        e->first_order_only = e->inputs.size();
        continue;
      }
      if (in == "")
        break;
      e->inputs.push_back(get_or_create_node(in, simple));
    }
    e->vars = pos;
  }
  delete tmp_node;
}

timespec prog_begin;

void print_difference(timespec a, timespec b) {
  uint64_t a64 = a.tv_sec * 1000000000 + a.tv_nsec;
  uint64_t b64 = b.tv_sec * 1000000000 + b.tv_nsec;
  printf("%lu.%06lu ", (a64 - b64) / 1000000000,
         ((a64 - b64) % 1000000000) / 1000);
}

void parse_scope(tbb::task_group &tg, Global &global, Scope *parent,
                 std::string_view path) {
  auto *s = new Scope;
  s->parent = parent;

  auto scope1 = std::make_shared<Phase1Scope>();
  parse_file(global, *s, *scope1, path);

  for (char *inc : scope1->subninja) {
    tg.run([&tg, &global, s, inc]() {
      std::string buf;
      const vars_t *var_scope = &s->vars;
      parse_scope(tg, global, s,
                  expand(std::string_view(inc, -1), {&var_scope, 1}, buf));
    });
  }
  constexpr size_t chunk_size = 1024;
  for (size_t i = 0; i < scope1->build.size(); i += chunk_size) {
    tg.run([&global, i, s, scope1]() {
      resolve_build(global, *s,
                    {scope1->build.begin() + i,
                     std::min(scope1->build.begin() + i + chunk_size,
                              scope1->build.end())});
    });
  }
}

void mark(Node *n) {
  if (n->needed)
    return;
  n->needed = true;
  if (n->in_edge)
    for (Node *dep : n->in_edge->inputs)
      mark(dep);
}

void stat_nodes(std::span<Node *> nodes) {
  for (Node *n : nodes) {
    struct stat s;
    stat(std::string(n->path).c_str(), &s);
  }
}
void parse_top(std::string_view path, std::string_view target) {
  Global global;
  tbb::task_group tg;
  parse_scope(tg, global, nullptr, path);
  tg.wait();
  printf("%zu nodes\n", global.nodes.size());
  Node *n = global.nodes[target];
  if (!n)
    error("unknown target");
  mark(n);
  std::vector<Node *> needed_nodes;
  for (auto &n : global.nodes)
    if (n.second->needed)
      needed_nodes.push_back(n.second);
  printf("%zu needed\n", needed_nodes.size());
  constexpr size_t chunk_size = 1024;
  for (size_t i = 0; i < needed_nodes.size(); i += chunk_size) {
    tg.run([i, &needed_nodes]() {
      stat_nodes({needed_nodes.begin() + i,
                  std::min(needed_nodes.begin() + i + chunk_size,
                           needed_nodes.end())});
    });
  }
  tg.wait();
  exit(0);
}

__attribute__((weak)) void sink(Scope *scope) {}

int main(int argc, char **argv) {
  // the perf tool still has some pac bugs
  syscall(__NR_prctl, PR_PAC_SET_ENABLED_KEYS,
          PR_PAC_APDAKEY | PR_PAC_APDBKEY | PR_PAC_APIAKEY | PR_PAC_APIBKEY, 0,
          0, 0);
  clock_gettime(CLOCK_MONOTONIC, &prog_begin);
  parse_top(argv[1], argv[2]);
}
