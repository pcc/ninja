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

#include "oneapi/tbb/concurrent_hash_map.h"
#include "oneapi/tbb/concurrent_vector.h"
#include "oneapi/tbb/parallel_for_each.h"
#include "oneapi/tbb/task_group.h"
#include <phmap.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

using namespace oneapi;

struct toplevel {
  char *begin;
  uint64_t hash;
};

struct var {
  std::string_view value;
  uint64_t hash;
  bool simple;
};

using rules_t = tbb::concurrent_hash_map<std::string_view, toplevel>;
using vars_t = tbb::concurrent_hash_map<std::string_view, var>;

struct node;

struct global {
  tbb::concurrent_hash_map<std::string_view, char *> pool;
  phmap::parallel_flat_hash_map<
      std::string_view, node *,
      phmap::priv::hash_default_hash<std::string_view>,
      phmap::priv::hash_default_eq<std::string_view>,
      phmap::priv::Allocator<phmap::priv::Pair<const std::string_view, node *>>,
      12, std::mutex>
      nodes;
};
struct Scope {
  Scope *parent;
  vars_t vars;
  rules_t rule;
};
struct phase1_file {};
struct phase1_scope {
  std::vector<toplevel> build;
  std::vector<char *> subninja, default_;
};

void error(const char *err) {
  fprintf(stderr, "error: %s\n", err);
  exit(1);
}

namespace std {
size_t _Hash_bytes(const void *ptr, size_t len, size_t seed) {
  return XXH3_64bits_withSeed(ptr, len, seed);
}
} // namespace std

std::string_view expand(std::string_view token, std::span<const vars_t *> scope,
                        std::string &buf, size_t recursion_depth = 0) {
  if (recursion_depth == 16)
    error("recursion limit reached during variable expansion");
  for (size_t i = 0; i != token.size(); ++i) {
    if (token[i] == '$') {
      buf.assign(token.begin(), i);
      while (1) {
        if (token[i + 1] == ' ' || token[i + 1] == ':') {
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
            vars_t::const_accessor a;
            if (vars->find(a, name)) {
              var v = a->second;
              a.release();
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

// A "simple" token is one that does not contain a "$" character.
// This means that it does not need to be expanded and we do not
// need to search for dependencies when computing a hash.
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
    if (*pos == '$') {
      simple = false;
      pos += 2;
    } else {
      pos++;
    }
    uint8x16_t dollars = vdupq_n_u8('$');
    uint8x16_t spaces = vdupq_n_u8(' ');
    uint8x16_t colons = vdupq_n_u8(':');
    uint8x16_t equals = vdupq_n_u8('=');
    uint8x16_t newlines = vdupq_n_u8('\n');
    uint8x16_t zeroes = vdupq_n_u8('\0');
    uint8x16_t acc_dollar_mask = zeroes;
    while (1) {
      uint8x16_t chars_m1 = *(uint8x16_t *)(pos - 1);
      uint8x16_t chars = *(uint8x16_t *)pos;
      uint8x16_t dollar_mask = vceqq_u8(chars_m1, dollars);
      uint64x2_t dollar_mask64 = vreinterpretq_u64_u8(dollar_mask);
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
      mask = vandq_u8(mask, vmvnq_u8(dollar_mask));
      mask = vorrq_u8(mask, zero_mask);
      uint64x2_t mask64 = vreinterpretq_u64_u8(mask);
      uint64_t lo = mask64[0];
      uint64_t hi = mask64[1];
      if (lo != 0) {
        int tz = __builtin_ctzl(lo);
        simple &= !dollar_mask64[0] || __builtin_ctzl(dollar_mask64[0]) > tz;
        pos += tz / 8;
      } else if (hi != 0) {
        int tz = __builtin_ctzl(hi);
        simple &= !dollar_mask64[0] &&
                  (!dollar_mask64[1] || __builtin_ctzl(dollar_mask64[1]) > tz);
        pos += 8 + tz / 8;
      } else {
        pos += 16;
        acc_dollar_mask = vorrq_u8(acc_dollar_mask, dollar_mask);
        continue;
      }
      uint64x2_t acc_dollar_mask64 = vreinterpretq_u64_u8(acc_dollar_mask);
      simple &= !acc_dollar_mask64[0] && !acc_dollar_mask64[1];
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

void parse_file(global &global, Scope &scope, phase1_scope &scope1,
                std::string_view path);

void parse_file_range(global &global, Scope &scope, phase1_scope &scope1,
                      char *begin, char *end) {
  char *pos = begin;
  bool cur_build = false;
  std::string_view cur_rule;
  char *cur_toplevel;
  auto hash_toplevel = [&](char *pos) {
    return XXH3_64bits(cur_toplevel, pos - cur_toplevel);
  };
  auto finish_toplevel = [&](char *pos) {
    if (cur_build) {
      uint64_t hash = hash_toplevel(pos);
      scope1.build.push_back({cur_toplevel, hash});
      cur_build = false;
    } else if (!cur_rule.empty()) {
      uint64_t hash = hash_toplevel(pos);
      decltype(scope.rule)::accessor a;
      scope.rule.insert(a, cur_rule);
      a->second.begin = cur_toplevel;
      a->second.hash = hash;
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
      var v;
      v.value = token<0>(pos, v.simple);
      v.hash = XXH3_64bits(v.value.data(), v.value.size());
      scope.vars.insert({word, v});
    }
    return false;
  };
  auto move_to_next_toplevel = [&]() {
    // FIXME: This misclassifies "# $\nfoo = bar" as a non-toplevel because
    // "$" at the end of a comment does not count as a continuation character.
    // We may need to keep track of whether the previous line is a comment.
    uint8x16_t newlines = vdupq_n_u8('\n');
    while (pos < end) {
      uint8x16_t chars_m1 = *(uint8x16_t *)(pos - 1);
      uint8x16_t mask = vceqq_u8(chars_m1, newlines);
      uint64x2_t mask64 = vreinterpretq_u64_u8(mask);
      uint64_t lo = mask64[0];
      uint64_t hi = mask64[1];
      if (lo != 0) {
        pos += __builtin_ctzl(lo) / 8;
      } else if (hi != 0) {
        pos += 8 + __builtin_ctzl(hi) / 8;
      } else {
        pos += 16;
        continue;
      }
      // A blank line ends a toplevel. CMake leaves comments before each
      // toplevel, we don't want them to be included in the hash for the
      // previous one.
      if (*pos == '\n') {
        finish_toplevel(pos);
        pos++;
        continue;
      }
      // These could also be classified with SIMD but it turns out to be slower.
      if (*pos == ' ' || *pos == '#' || *(pos - 2) == '$') {
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

void parse_file(global &global, Scope &scope, phase1_scope &scope1,
                std::string_view path) {
  int fd = open(std::string(path).c_str(), O_RDONLY);
  if (fd == -1)
    error("failed to open file");
  size_t size = lseek(fd, 0, SEEK_END);
  void *addr = mmap(0, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (addr == MAP_FAILED)
    error("failed to map file");
  close(fd);
  char *begin = (char *)addr;
  char *end = begin + size;
  parse_file_range(global, scope, scope1, begin, end);
}

struct edge {
  std::vector<node *> outputs, inputs;
  toplevel rule;
  size_t first_implicit, first_order_only;
  char *vars;
  uint64_t hash;
};

struct node {
  std::string path_buf;
  std::string_view path;
  edge *in_edge = nullptr;
};

void resolve_build(global &global, Scope &scope, std::span<toplevel> builds) {
  // keep a node allocated so we don't have to allocate one while holding the
  // lock
  auto *tmp_node = new node;
  for (toplevel &build : builds) {
    char *pos = build.begin;
    auto *e = new edge;
    e->hash = build.hash;
    auto get_or_create_node = [&](std::string_view token, bool simple) {
      const vars_t *var_scope = &scope.vars;
      if (simple)
        tmp_node->path = token;
      else
        tmp_node->path = expand(token, {&var_scope, 1}, tmp_node->path_buf);
      node *n;
      if (global.nodes.try_emplace_l(
              tmp_node->path, [&](auto &val) { n = val.second; }, tmp_node)) {
        n = tmp_node;
        tmp_node = new node;
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
      node *out_node = get_or_create_node(out, simple);
      out_node->in_edge = e;
      e->outputs.push_back(out_node);
    }
    bool simple;
    std::string_view rule = token<ColonIsToken | SpaceIsSeparator>(pos, simple);
    if (rule == "phony") {
      e->rule = {};
    } else {
      decltype(scope.rule)::const_accessor a;
      Scope *cur_scope = &scope;
      while (cur_scope) {
        if (cur_scope->rule.find(a, rule))
          break;
        cur_scope = cur_scope->parent;
      }
      if (!cur_scope)
        error("could not find rule");
      e->rule = a->second;
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

void parse_scope(tbb::task_group &tg, global &global, Scope *parent,
                 std::string_view path) {
  auto *s = new Scope;
  s->parent = parent;

  auto scope1 = std::make_shared<phase1_scope>();
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

void parse_top(std::string_view path) {
  global global;
  tbb::task_group tg;
  parse_scope(tg, global, nullptr, path);
  tg.wait();
  printf("%zu nodes\n", global.nodes.size());
  exit(0);
}

__attribute__((weak)) void sink(Scope *scope) {}

int main(int argc, char **argv) {
  // the perf tool still has some pac bugs
  syscall(__NR_prctl, PR_PAC_SET_ENABLED_KEYS,
          PR_PAC_APDAKEY | PR_PAC_APDBKEY | PR_PAC_APIAKEY | PR_PAC_APIBKEY, 0,
          0, 0);
  clock_gettime(CLOCK_MONOTONIC, &prog_begin);
  parse_top(argv[1]);
}
