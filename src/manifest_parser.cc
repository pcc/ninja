// Copyright 2011 Google Inc. All Rights Reserved.
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

#include "manifest_parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <memory>
#include <vector>

#include "char_scan.h"
#include "disk_interface.h"
#include "graph.h"
#include "hash_map.h"
#include "metrics.h"
#include "state.h"
#include "thread_pool.h"
#include "util.h"
#include "version.h"

using namespace std;

// Overview
// ========
//
// Parsing a manifest has effects of two kinds:
//
// - Effects on scopes: variable and rule definitions.  Paths and edge
//   bindings are evaluated as they are parsed, so they depend on the
//   definitions that precede them in their scope and its parents.
// - Effects on the State: edges, nodes, pools and defaults.  Their order
//   matters (edge ids, which rule generates an output first, errors).
//
// Files are processed by "file tasks" (ProcessFile()) that lex the file,
// apply the effects on their own scope directly, and record the effects on
// the State as a list of items, without touching the State.  The items are
// then applied ("merged") in order by a single thread, which also reports
// any error at the place where a serial parse would have reported it.
//
// A subninja file gets a new scope whose parent is the scope of the file
// containing the subninja statement.  The subninja file is processed on
// another thread if that cannot change the outcome, which is the case if
// no more definitions are made in the parent scope (or its parents) after
// the subninja statement: then the parent scopes look exactly the same to
// the subninja file as they would in a serial parse.  To check that,
// files are scanned for lines that may start definitions ("rule", "include"
// or a variable name at the start of a line); see LastDefinition().
// Otherwise the subninja file is processed on the spot.
//
// The lexer is a reimplementation of the one in lexer.in.cc (which is
// still used for dyndep files) that records unevaluated strings as raw
// slices of the input and uses SIMD to skip over plain text.  It produces
// the same tokens and errors (including their positions).

namespace {

// $^ supported starting this version
const int kMinNewlineEscapeVersionMajor = 1;
const int kMinNewlineEscapeVersionMinor = 14;

/// Characters allowed in variable names (and rule and pool names).
bool IsVarnameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

/// Characters allowed in $name references.
bool IsSimpleVarnameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/// An unevaluated string from the manifest, as a slice of the input.  The
/// lexer has validated it, so evaluating it can't fail.
struct RawString {
  const char* begin = nullptr;
  const char* end = nullptr;
  /// Whether the string contains no $-escapes, i.e. the text is the value.
  bool literal = true;
  /// Whether the string is non-empty in the sense of EvalString::empty():
  /// whether it contains any text or variable references.
  bool has_tokens = false;

  bool empty() const { return !has_tokens; }
};

/// Append the value of \a raw evaluated in \a env to \a out.
void Evaluate(const RawString& raw, Env* env, std::string* out) {
  if (raw.literal) {
    out->append(raw.begin, raw.end);
    return;
  }
  const char* p = raw.begin;
  const char* end = raw.end;
  while (p < end) {
    const char* dollar =
        static_cast<const char*>(memchr(p, '$', end - p));
    if (!dollar) {
      out->append(p, end);
      return;
    }
    out->append(p, dollar);
    p = dollar + 1;
    switch (*p) {
    case '$':
    case ' ':
    case ':':
      out->push_back(*p);
      ++p;
      break;
    case '^':
      out->push_back('\n');
      ++p;
      break;
    case '\r':  // $\r\n
      p += 2;
      while (p < end && *p == ' ')
        ++p;
      break;
    case '\n':
      ++p;
      while (p < end && *p == ' ')
        ++p;
      break;
    case '{': {
      const char* name = p + 1;
      const char* name_end = name;
      while (*name_end != '}')
        ++name_end;
      env->AppendVariable(StringPiece(name, name_end - name), out);
      p = name_end + 1;
      break;
    }
    default: {
      const char* name_end = p;
      while (IsSimpleVarnameChar(*name_end))
        ++name_end;
      env->AppendVariable(StringPiece(p, name_end - p), out);
      p = name_end;
      break;
    }
    }
  }
}

std::string Evaluate(const RawString& raw, Env* env) {
  std::string result;
  Evaluate(raw, env, &result);
  return result;
}

/// Convert \a raw to an EvalString, as Lexer::ReadEvalString() would have
/// produced it.
void ToEvalString(const RawString& raw, EvalString* eval) {
  const char* p = raw.begin;
  const char* end = raw.end;
  const char* text = p;
  auto flush_text = [&](const char* text_end) {
    if (text_end != text)
      eval->AddText(StringPiece(text, text_end - text));
  };
  while (p < end) {
    if (*p != '$') {
      ++p;
      continue;
    }
    flush_text(p);
    ++p;
    switch (*p) {
    case '$':
    case ' ':
    case ':':
      eval->AddText(StringPiece(p, 1));
      ++p;
      break;
    case '^':
      eval->AddText(StringPiece("\n", 1));
      ++p;
      break;
    case '\r':
      p += 2;
      while (p < end && *p == ' ')
        ++p;
      break;
    case '\n':
      ++p;
      while (p < end && *p == ' ')
        ++p;
      break;
    case '{': {
      const char* name = p + 1;
      const char* name_end = name;
      while (*name_end != '}')
        ++name_end;
      eval->AddSpecial(StringPiece(name, name_end - name));
      p = name_end + 1;
      break;
    }
    default: {
      const char* name_end = p;
      while (IsSimpleVarnameChar(*name_end))
        ++name_end;
      eval->AddSpecial(StringPiece(p, name_end - p));
      p = name_end;
      break;
    }
    }
    text = p;
  }
  flush_text(p);
}

/// Format an error message like Lexer::Error() does, for a position in a
/// file (the start of the last token read, or null).
std::string FormatError(StringPiece filename, const char* input,
                        const char* pos, const std::string& message) {
  // Compute line/column.
  int line = 1;
  const char* line_start = input;
  for (const char* p = input; p < pos; ++p) {
    if (*p == '\n') {
      ++line;
      line_start = p + 1;
    }
  }
  int col = pos ? (int)(pos - line_start) : 0;

  char buf[1024];
  snprintf(buf, sizeof(buf), "%s:%d: ", filename.AsString().c_str(), line);
  std::string err = buf;
  err += message + "\n";

  // Add some context to the message.
  const int kTruncateColumn = 72;
  if (col > 0 && col < kTruncateColumn) {
    int len;
    bool truncated = true;
    for (len = 0; len < kTruncateColumn; ++len) {
      if (line_start[len] == 0 || line_start[len] == '\n') {
        truncated = false;
        break;
      }
    }
    err += std::string(line_start, len);
    if (truncated)
      err += "...";
    err += "\n";
    err += std::string(col, ' ');
    err += "^ near here";
  }
  return err;
}

/// The manifest lexer.  See lexer.h for the meaning of the methods; the
/// differences are that strings are returned as RawStrings, and that
/// uses of the $^ escape are reported to the caller (because whether they
/// are allowed depends on state that is only known when merging).
struct ManifestLexer {
  enum Token {
    ERROR,
    BUILD,
    COLON,
    DEFAULT,
    EQUALS,
    IDENT,
    INCLUDE,
    INDENT,
    NEWLINE,
    PIPE,
    PIPE2,
    PIPEAT,
    POOL,
    RULE,
    SUBNINJA,
    TEOF,
  };

  static const char* TokenName(Token t) {
    switch (t) {
    case ERROR:    return "lexing error";
    case BUILD:    return "'build'";
    case COLON:    return "':'";
    case DEFAULT:  return "'default'";
    case EQUALS:   return "'='";
    case IDENT:    return "identifier";
    case INCLUDE:  return "'include'";
    case INDENT:   return "indent";
    case NEWLINE:  return "newline";
    case PIPE2:    return "'||'";
    case PIPE:     return "'|'";
    case PIPEAT:   return "'|@'";
    case POOL:     return "'pool'";
    case RULE:     return "'rule'";
    case SUBNINJA: return "'subninja'";
    case TEOF:     return "eof";
    }
    return NULL;  // not reached
  }

  static const char* TokenErrorHint(Token expected) {
    switch (expected) {
    case COLON:
      return " ($ also escapes ':')";
    default:
      return "";
    }
  }

  /// \a input must be NUL-terminated and padded (see char_scan.h).
  void Start(StringPiece filename, const char* input) {
    filename_ = filename;
    input_ = input;
    ofs_ = input;
    last_token_ = NULL;
  }

  std::string DescribeLastError() {
    if (last_token_) {
      switch (last_token_[0]) {
      case '\t':
        return "tabs are not allowed, use spaces";
      }
    }
    return "lexing error";
  }

  bool Error(const std::string& message, std::string* err) {
    *err = FormatError(filename_, input_, last_token_, message);
    return false;
  }

  void UnreadToken() { ofs_ = last_token_; }

  Token ReadToken() {
    const char* p = ofs_;
    const char* start;
    Token token;
    for (;;) {
      start = p;
      const char* s = p;
      while (*s == ' ')
        ++s;
      if (*s == '#') {
        // [ ]*"#"[^\000\n]*"\n" is a comment.
        const char* end = FindFirstOf<'\n', '\0'>(s);
        if (*end == '\n') {
          p = end + 1;
          continue;
        }
      }
      if (*s == '\n') {
        p = s + 1;
        token = NEWLINE;
        break;
      }
      if (*s == '\r' && s[1] == '\n') {
        p = s + 2;
        token = NEWLINE;
        break;
      }
      if (s != p) {
        p = s;
        token = INDENT;
        break;
      }
      if (IsVarnameChar(*p)) {
        const char* e = p + 1;
        while (IsVarnameChar(*e))
          ++e;
        token = IDENT;
        switch (e - p) {
        case 4:
          if (memcmp(p, "pool", 4) == 0)
            token = POOL;
          else if (memcmp(p, "rule", 4) == 0)
            token = RULE;
          break;
        case 5:
          if (memcmp(p, "build", 5) == 0)
            token = BUILD;
          break;
        case 7:
          if (memcmp(p, "default", 7) == 0)
            token = DEFAULT;
          else if (memcmp(p, "include", 7) == 0)
            token = INCLUDE;
          break;
        case 8:
          if (memcmp(p, "subninja", 8) == 0)
            token = SUBNINJA;
          break;
        }
        p = e;
        break;
      }
      switch (*p) {
      case '=':
        token = EQUALS;
        ++p;
        break;
      case ':':
        token = COLON;
        ++p;
        break;
      case '|':
        if (p[1] == '@') {
          token = PIPEAT;
          p += 2;
        } else if (p[1] == '|') {
          token = PIPE2;
          p += 2;
        } else {
          token = PIPE;
          ++p;
        }
        break;
      case '\0':
        token = TEOF;
        ++p;
        break;
      default:
        token = ERROR;
        ++p;
        break;
      }
      break;
    }

    last_token_ = start;
    ofs_ = p;
    if (token != NEWLINE && token != TEOF)
      EatWhitespace();
    return token;
  }

  bool PeekToken(Token token) {
    Token t = ReadToken();
    if (t == token)
      return true;
    UnreadToken();
    return false;
  }

  bool ReadIdent(StringPiece* out) {
    const char* start = ofs_;
    const char* p = start;
    while (IsVarnameChar(*p))
      ++p;
    last_token_ = start;
    if (p == start)
      return false;
    *out = StringPiece(start, p - start);
    ofs_ = p;
    EatWhitespace();
    return true;
  }

  bool ReadPath(RawString* path, std::string* err) {
    return ReadEvalString(path, true, err);
  }

  bool ReadVarValue(RawString* value, std::string* err) {
    return ReadEvalString(value, false, err);
  }

  bool ExpectToken(Token expected, std::string* err) {
    Token token = ReadToken();
    if (token != expected) {
      std::string message = std::string("expected ") + TokenName(expected);
      message += std::string(", got ") + TokenName(token);
      message += TokenErrorHint(expected);
      return Error(message, err);
    }
    return true;
  }

  /// Uses of the $^ escape since the last call to TakeNewlineEscapes(), as
  /// error messages to report if they turn out not to be allowed.
  std::vector<std::string> newline_escapes_;

  StringPiece filename_;
  const char* input_;
  const char* ofs_;
  const char* last_token_;

 private:
  void EatWhitespace() {
    const char* p = ofs_;
    for (;;) {
      if (*p == ' ') {
        while (*p == ' ')
          ++p;
      } else if (p[0] == '$' && p[1] == '\n') {
        p += 2;
      } else if (p[0] == '$' && p[1] == '\r' && p[2] == '\n') {
        p += 3;
      } else {
        break;
      }
    }
    ofs_ = p;
  }

  bool ReadEvalString(RawString* out, bool path, std::string* err) {
    const char* p = ofs_;
    const char* start;
    const char* text_end;
    out->begin = p;
    out->literal = true;
    out->has_tokens = false;
    for (;;) {
      start = p;
      // [^$ :\r\n|\000]+ is text.  Outside paths, ' ', ':' and '|' are
      // single character text tokens, so skip them as well.
      const char* q = path ? FindFirstOf<'$', ' ', ':', '\r', '\n', '|', '\0'>(p)
                           : FindFirstOf<'$', '\r', '\n', '\0'>(p);
      if (q != p) {
        out->has_tokens = true;
        p = q;
        continue;
      }
      char c = *p;
      if (c == '\r' && p[1] == '\n') {
        text_end = p;
        if (!path)
          p += 2;
        break;
      }
      if (c == ' ' || c == ':' || c == '|' || c == '\n') {
        // Only '\n' gets here outside paths.
        text_end = p;
        if (!path)
          ++p;
        break;
      }
      if (c == '$') {
        char d = p[1];
        out->literal = false;
        if (d == '$' || d == ' ' || d == ':') {
          out->has_tokens = true;
          p += 2;
          continue;
        }
        if (d == '\n' || (d == '\r' && p[2] == '\n')) {
          p += d == '\n' ? 2 : 3;
          while (*p == ' ')
            ++p;
          continue;
        }
        if (d == '{') {
          const char* e = p + 2;
          while (IsVarnameChar(*e))
            ++e;
          if (e != p + 2 && *e == '}') {
            out->has_tokens = true;
            p = e + 1;
            continue;
          }
        }
        if (IsSimpleVarnameChar(d)) {
          const char* e = p + 2;
          while (IsSimpleVarnameChar(*e))
            ++e;
          out->has_tokens = true;
          p = e;
          continue;
        }
        if (d == '^') {
          // Lexer::ReadEvalString() reports this error at the last token.
          newline_escapes_.push_back(FormatError(
              filename_, input_, last_token_,
              "using $^ escape requires specifying 'ninja_required_version' "
              "with version greater or equal 1.14"));
          out->has_tokens = true;
          p += 2;
          continue;
        }
        last_token_ = start;
        return Error("bad $-escape (literal $ must be written as $$)", err);
      }
      last_token_ = start;
      if (c == '\0')
        return Error("unexpected EOF", err);
      return Error(DescribeLastError(), err);
    }
    out->end = text_end;
    last_token_ = start;
    ofs_ = p;
    if (path)
      EatWhitespace();
    // Non-path strings end in newlines, so there's no whitespace to eat.
    return true;
  }
};

typedef ManifestLexer Lex;

/// A bump allocator for path strings.
struct Arena {
  char* Allocate(size_t size) {
    if (size > remaining_) {
      size_t block = std::max<size_t>(size, 64 << 10);
      blocks_.emplace_back(new char[block]);
      next_ = blocks_.back().get();
      remaining_ = block;
    }
    char* result = next_;
    next_ += size;
    remaining_ -= size;
    return result;
  }

 private:
  std::vector<std::unique_ptr<char[]>> blocks_;
  char* next_ = nullptr;
  size_t remaining_ = 0;
};

/// An evaluated and canonicalized path.
struct PathRef {
  const char* str;
  size_t len;
  uint64_t slash_bits;
  /// The node for the path (created if needed), unless it's empty.
  Node* node;

  StringPiece piece() const { return StringPiece(str, len); }
};

/// A build statement, to be added to the State when merging.
struct PendingEdge {
  const Rule* rule;
  BindingEnv* env;
  /// The value of the "pool" binding, unless pool_deferred.
  std::string pool;
  bool pool_deferred = false;
  /// The value of the "dyndep" binding, canonicalized, unless
  /// dyndep_deferred.  Its slash bits are in dyndep_slash_bits.
  std::string dyndep;
  uint64_t dyndep_slash_bits = 0;
  bool dyndep_deferred = false;
  /// Indexes into ParsedFile::paths_.
  size_t paths_begin;
  int outs, implicit_outs, ins, implicit_ins, order_only_ins, validations;
  /// Where errors found when adding the edge are reported.
  const char* error_pos;
  /// The edge, with its inputs, outputs and validations, unless one of the
  /// paths is empty (an error when adding the edge).
  Edge* edge = nullptr;
};

struct ParsedFile;

/// An effect on the State, or an error, to be applied in order.
struct Item {
  enum Kind {
    kEdge,
    kPool,
    kDefault,
    /// The items of an included or subninja file.
    kFile,
    /// An error message: stop parsing.
    kError,
    /// A ninja_required_version definition.
    kVersion,
    /// A use of the $^ escape: check that it is allowed.
    kNewlineEscape,
  };
  Kind kind;
  /// Index into the vector for the kind in ParsedFile (unused for kFile).
  size_t index;
  std::unique_ptr<ParsedFile> file;
};

struct PendingPool {
  std::string name;
  int depth;
  /// Whether to add the pool, or only check that it's not a duplicate
  /// (because there was an error in the pool statement).
  bool add;
  const char* error_pos;
};

struct PendingDefault {
  /// Paths, and where errors for them are reported.  A path with empty
  /// str is an error: "empty path".
  std::vector<std::pair<PathRef, const char*> > paths;
};

/// A manifest file being processed, with the effects to apply to the State.
struct ParsedFile {
  std::string filename;
  /// The contents of the file, NUL-terminated and padded.  Bindings point
  /// into them, so they end up in the State.
  std::unique_ptr<FileContents> contents{new FileContents};
  /// The text of the file: input.data(), or the text of the file that this
  /// is a chunk of.
  const char* text = nullptr;
  /// The size of the text (without padding).
  size_t size = 0;
  /// For chunks: the part of the text to process.
  const char* chunk_begin = nullptr;
  const char* chunk_end = nullptr;
  /// The depth of nesting of includes and subninjas.  The original parser
  /// used a Lexer per depth, and the state of a Lexer that matters for
  /// $^ escapes persisted across files.
  int depth = 0;

  std::vector<Item> items;
  std::vector<PendingEdge> edges;
  std::vector<PathRef> paths;
  std::vector<PendingPool> pools;
  std::vector<PendingDefault> defaults;
  /// Error messages for kError and kNewlineEscape items, and versions for
  /// kVersion items.
  std::vector<std::string> strings;
  Arena arena;

  /// Set once the file has been processed (by some thread).
  std::atomic<bool> done{false};



  /// The offset of the start of the last line in the file that may start a
  /// definition (see the overview), or -1 if not computed yet.
  ptrdiff_t last_definition = -1;

  void AddError(const std::string& message) {
    items.push_back({ Item::kError, strings.size(), nullptr });
    strings.push_back(message);
  }
};

/// Return whether the line starting at \a line may start a definition: if
/// it starts with something that is not "build", "pool", "default" or
/// "subninja" followed by a non-identifier character, and does not start
/// with ' ', '#', '\r' or '\n'.  This is conservative: lines that continue
/// a previous line are also considered.
bool MayStartDefinition(const char* line) {
  if (!IsVarnameChar(*line))
    return false;
  const char* e = line;
  while (IsVarnameChar(*e))
    ++e;
  StringPiece word(line, e - line);
  return word != "build" && word != "pool" && word != "default" &&
         word != "subninja";
}

/// Return the offset in \a text of the start of the last line that may
/// start a definition among the lines starting in [begin, end), or -1.
ptrdiff_t LastDefinitionIn(const char* text, const char* begin,
                           const char* end) {
  ptrdiff_t last = -1;
  const char* line = begin;
  if (line != text && line[-1] != '\n') {
    line = static_cast<const char*>(memchr(line, '\n', end - line));
    if (!line)
      return -1;
    ++line;
  }
  while (line < end) {
    if (MayStartDefinition(line))
      last = line - text;
    // The text is NUL-terminated and padded.
    const char* newline = FindFirstOf<'\n', '\0'>(line);
    if (*newline != '\n')
      break;
    line = newline + 1;
  }
  return last;
}

/// Return the offset of the start of the last line of the file that may
/// start a definition (see MayStartDefinition()), or -1.
ptrdiff_t LastDefinition(const char* text, size_t size, bool parallel) {
  const size_t kSegment = 1 << 20;
  if (!parallel || size < 2 * kSegment)
    return LastDefinitionIn(text, text, text + size);
  size_t segments = (size + kSegment - 1) / kSegment;
  std::vector<ptrdiff_t> last(segments);
  ParallelFor(segments, 1, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i)
      last[i] = LastDefinitionIn(text, text + i * kSegment,
                                 text + std::min(size, (i + 1) * kSegment));
  });
  return *std::max_element(last.begin(), last.end());
}

/// Return whether the newline at \a newline (in \a text) is escaped (as
/// far as one can tell from the text before it: in comments, '$' does not
/// escape newlines).
bool MayBeEscapedNewline(const char* text, const char* newline) {
  const char* p = newline;
  if (p > text && p[-1] == '\r')
    --p;
  int dollars = 0;
  while (p > text && p[-1] == '$') {
    --p;
    ++dollars;
  }
  return dollars % 2 == 1;
}

/// State shared by the tasks processing the files of one manifest.
struct ParseContext {
  State* state;
  FileReader* file_reader;
  /// Whether to process subninjas on other threads.
  bool parallel;
  /// The tasks processing files on other threads.  Whoever merges a file
  /// waits for it specifically; this is to wait for all of them in the end.
  TaskGroup tasks;
};

/// Process the statements of \a file in scope \a env.  \a frozen tells
/// whether \a env's parent scopes won't be modified anymore.  Returns false
/// if there was an error.
bool ProcessFile(ParseContext* ctx, ParsedFile* file, BindingEnv* env,
                 bool frozen);

/// Read and process an included or subninja file, mentioned at \a pos in
/// \a parent.  Returns false if there was an error.
bool LoadAndProcessFile(ParseContext* ctx, ParsedFile* parent,
                        const char* pos, ParsedFile* file, BindingEnv* env,
                        bool frozen) {
  std::string read_err;
  if (ctx->file_reader->LoadFile(file->filename, kCharScanPadding + 1,
                                 file->contents.get(),
                                 &read_err) != FileReader::Okay) {
    // Errors loading the file are reported in the context of the parent.
    file->AddError(FormatError(parent->filename, parent->text, pos,
                               "loading '" + file->filename + "': " +
                                   read_err));
    file->done.store(true, std::memory_order_release);
    return false;
  }
  return ProcessFile(ctx, file, env, frozen);
}

struct FileProcessor {
  FileProcessor(ParseContext* ctx, ParsedFile* file, BindingEnv* env,
                bool frozen)
      : ctx_(ctx), file_(file), env_(env), frozen_(frozen) {}

  /// Process the file; returns false if there was an error (recorded as
  /// the last item).
  bool Run();

 private:
  bool ParsePool(std::string* err);
  bool ParseRule(std::string* err);
  bool ParseLet(StringPiece* key, RawString* value, std::string* err);
  bool ParseEdge(std::string* err);
  bool ParseDefault(std::string* err);
  bool ParseFileInclude(bool new_scope, std::string* err);

  /// Record uses of $^ seen by the lexer, to be checked when merging.
  void FlushNewlineEscapes() {
    for (std::string& message : lexer_.newline_escapes_) {
      file_->items.push_back(
          { Item::kNewlineEscape, file_->strings.size(), nullptr });
      file_->strings.push_back(std::move(message));
    }
    lexer_.newline_escapes_.clear();
  }

  /// Evaluate and canonicalize a path into the arena, and look up its node
  /// (if \a with_node).
  PathRef EvaluatePath(const RawString& raw, Env* env, bool with_node) {
    PathRef path;
    path.slash_bits = 0;
    path.node = nullptr;
    if (raw.literal && IsCanonicalPath(raw.begin, raw.end - raw.begin)) {
      // The path can stay in the input.
      path.str = raw.begin;
      path.len = raw.end - raw.begin;
      if (path.len && with_node)
        path.node = GetNode(path);
      return path;
    }
    if (raw.literal) {
      path.len = raw.end - raw.begin;
      char* str = file_->arena.Allocate(path.len);
      memcpy(str, raw.begin, path.len);
      path.str = str;
    } else {
      scratch_.clear();
      Evaluate(raw, env, &scratch_);
      path.len = scratch_.size();
      char* str = file_->arena.Allocate(path.len);
      memcpy(str, scratch_.data(), path.len);
      path.str = str;
    }
    if (path.len) {
      CanonicalizePath(const_cast<char*>(path.str), &path.len,
                       &path.slash_bits);
      if (with_node)
        path.node = GetNode(path);
    }
    return path;
  }

  /// Return the node for \a path, creating it if needed.  Paths tend to
  /// repeat within files, so look in a cache before the State.
  Node* GetNode(const PathRef& path) {
    StringPiece piece = path.piece();
    uint64_t hash = PathMap::Hash(piece);
    auto i = node_cache_.find(CacheKey{ piece, hash });
    if (i != node_cache_.end())
      return i->second;
    // Nodes are created in a different order than they would be by a
    // serial parse, which matters for their slash bits.
    Node* node = ctx_->state->paths_.GetOrCreate(piece, hash, path.slash_bits,
                                                 /*provisional=*/true);
    node_cache_.emplace(CacheKey{ piece, hash }, node);
    return node;
  }
  struct CacheKey {
    StringPiece path;
    uint64_t hash;
    bool operator==(const CacheKey& other) const { return path == other.path; }
  };
  struct CacheKeyHash {
    size_t operator()(const CacheKey& key) const { return key.hash; }
  };
  /// Keys point into the file's arena.
  emhash8::HashMap<CacheKey, Node*, CacheKeyHash> node_cache_;

  /// Whether no definitions follow \a pos in this file (and the parent
  /// scopes are frozen), so that a subninja at \a pos can be processed
  /// on another thread.
  bool FrozenAt(const char* pos) {
    if (!frozen_)
      return false;
    if (file_->last_definition == -1) {
      file_->last_definition =
          LastDefinition(file_->text, file_->size, ctx_->parallel) + 1;
    }
    return pos - file_->text >= file_->last_definition;
  }

  /// If what remains of the file after \a pos is large, and only has
  /// statements that don't change the scope, process it in chunks on
  /// other threads.  Returns whether it did.
  bool ProcessRestInChunks(const char* pos);

  ParseContext* ctx_;
  ParsedFile* file_;
  BindingEnv* env_;
  bool frozen_;
  Lex lexer_;
  std::vector<RawString> outs_, ins_, validations_;
  std::string scratch_;
};

bool ProcessFile(ParseContext* ctx, ParsedFile* file, BindingEnv* env,
                 bool frozen) {
  if (!file->text) {
    // FileReader::LoadFile() padded the contents for the lexer.
    file->text = file->contents->data;
    file->size = file->contents->size;
  }
  FileProcessor processor(ctx, file, env, frozen);
  bool ok = processor.Run();
  file->done.store(true, std::memory_order_release);
  return ok;
}

bool FileProcessor::Run() {
  lexer_.Start(file_->filename, file_->text);
  if (file_->chunk_begin)
    lexer_.ofs_ = file_->chunk_begin;
  // Whether this processor may split the rest of its file into chunks.
  bool may_chunk = ctx_->parallel && !file_->chunk_begin;
  std::string err;
  bool ok = true;
  while (ok) {
    if (may_chunk && ProcessRestInChunks(lexer_.ofs_))
      return true;
    Lex::Token token = lexer_.ReadToken();
    // Chunks end at the start of a statement.
    if (file_->chunk_end && lexer_.last_token_ >= file_->chunk_end &&
        token != Lex::TEOF) {
      lexer_.UnreadToken();
      return true;
    }
    switch (token) {
    case Lex::POOL:
      ok = ParsePool(&err);
      break;
    case Lex::BUILD:
      ok = ParseEdge(&err);
      break;
    case Lex::RULE:
      ok = ParseRule(&err);
      break;
    case Lex::DEFAULT:
      ok = ParseDefault(&err);
      break;
    case Lex::IDENT: {
      lexer_.UnreadToken();
      StringPiece name;
      RawString let_value;
      ok = ParseLet(&name, &let_value, &err);
      FlushNewlineEscapes();
      if (!ok)
        break;
      std::string value = Evaluate(let_value, env_);
      // Check ninja_required_version immediately (when merging) so we can
      // exit before encountering any syntactic surprises.
      if (name == "ninja_required_version") {
        file_->items.push_back(
            { Item::kVersion, file_->strings.size(), nullptr });
        file_->strings.push_back(value);
      }
      // The manifest's text is kept as long as the State.
      env_->AddBindingView(name, let_value.literal
                                     ? StringPiece(let_value.begin,
                                                   let_value.end -
                                                       let_value.begin)
                                     : env_->Keep(std::move(value)));
      break;
    }
    case Lex::INCLUDE:
      ok = ParseFileInclude(false, &err);
      break;
    case Lex::SUBNINJA:
      ok = ParseFileInclude(true, &err);
      break;
    case Lex::ERROR:
      ok = lexer_.Error(lexer_.DescribeLastError(), &err);
      break;
    case Lex::TEOF:
      return true;
    case Lex::NEWLINE:
      break;
    default:
      ok = lexer_.Error(std::string("unexpected ") + Lex::TokenName(token),
                        &err);
      break;
    }
  }
  // An error in an included file was recorded there.
  if (!err.empty())
    file_->AddError(err);
  return false;
}

bool FileProcessor::ProcessRestInChunks(const char* pos) {
  const size_t kMinSize = 256 << 10;
  const size_t kChunkSize = 128 << 10;
  const char* end = file_->text + file_->size;
  if (end - pos < static_cast<ptrdiff_t>(kMinSize) || !FrozenAt(pos))
    return false;
  // The first chunk is the rest of the current line, plus some.  Chunks
  // start at "build" statements (the start of a line, after an unescaped
  // newline): there, a serial parse is at the start of a statement too.
  std::vector<const char*> starts(1, pos);
  for (const char* target = pos + kChunkSize; target < end;
       target += kChunkSize) {
    const char* line = target;
    while (line < end) {
      const char* newline =
          static_cast<const char*>(memchr(line, '\n', end - line));
      if (!newline)
        break;
      line = newline + 1;
      if (memcmp(line, "build ", 6) == 0 &&
          !MayBeEscapedNewline(file_->text, newline))
        break;
    }
    if (line >= end)
      break;
    if (line > starts.back())
      starts.push_back(line);
    target = line;
  }
  if (starts.size() < 2)
    return false;

  for (size_t i = 0; i < starts.size(); ++i) {
    std::unique_ptr<ParsedFile> owned_chunk(new ParsedFile);
    ParsedFile* chunk = owned_chunk.get();
    chunk->filename = file_->filename;
    chunk->text = file_->text;
    chunk->size = file_->size;
    chunk->depth = file_->depth;
    chunk->chunk_begin = starts[i];
    chunk->chunk_end = i + 1 < starts.size() ? starts[i + 1] : nullptr;
    // All of it is after the last definition.
    chunk->last_definition = 0;
    file_->items.push_back({ Item::kFile, 0, std::move(owned_chunk) });
    ParseContext* ctx = ctx_;
    BindingEnv* env = env_;
    bool frozen = frozen_;
    ctx_->tasks.Run([ctx, chunk, env, frozen] {
      ProcessFile(ctx, chunk, env, frozen);
      NotifyTaskWaiters();
    });
  }
  return true;
}

bool FileProcessor::ParsePool(std::string* err) {
  StringPiece name;
  if (!lexer_.ReadIdent(&name))
    return lexer_.Error("expected pool name", err);

  if (!lexer_.ExpectToken(Lex::NEWLINE, err))
    return false;

  // The duplicate check happens before parsing the pool's bindings.
  PendingPool pool;
  pool.name = name.AsString();
  pool.depth = -1;
  pool.add = false;
  pool.error_pos = lexer_.last_token_;
  file_->items.push_back({ Item::kPool, file_->pools.size(), nullptr });
  file_->pools.push_back(pool);

  int depth = -1;

  while (lexer_.PeekToken(Lex::INDENT)) {
    StringPiece key;
    RawString value;
    bool ok = ParseLet(&key, &value, err);
    FlushNewlineEscapes();
    if (!ok)
      return false;

    if (key == "depth") {
      std::string depth_string = Evaluate(value, env_);
      const char* begin = depth_string.data();
      const char* end = begin + depth_string.size();
      auto result = std::from_chars(begin, end, depth);
      if (result.ec != std::errc() || result.ptr != end || depth < 0) {
        return lexer_.Error("invalid pool depth", err);
      }
    } else {
      return lexer_.Error("unexpected variable '" + key.AsString() + "'",
                          err);
    }
  }

  if (depth < 0)
    return lexer_.Error("expected 'depth =' line", err);

  file_->pools.back().depth = depth;
  file_->pools.back().add = true;
  return true;
}

bool FileProcessor::ParseRule(std::string* err) {
  StringPiece name;
  if (!lexer_.ReadIdent(&name))
    return lexer_.Error("expected rule name", err);

  if (!lexer_.ExpectToken(Lex::NEWLINE, err))
    return false;

  if (env_->LookupRuleCurrentScope(name) != NULL)
    return lexer_.Error("duplicate rule '" + name.AsString() + "'", err);

  auto rule = std::unique_ptr<Rule>(new Rule(name.AsString()));

  while (lexer_.PeekToken(Lex::INDENT)) {
    StringPiece key;
    RawString value;
    bool ok = ParseLet(&key, &value, err);
    FlushNewlineEscapes();
    if (!ok)
      return false;

    if (Rule::IsReservedBinding(key)) {
      EvalString eval;
      ToEvalString(value, &eval);
      rule->AddBinding(key.AsString(), eval);
    } else {
      // Die on other keyvals for now; revisit if we want to add a
      // scope here.
      return lexer_.Error("unexpected variable '" + key.AsString() + "'",
                          err);
    }
  }

  // These bindings always exist (if only empty).  Note that this matters
  // when looking up variables: an empty rule binding hides a variable of
  // the same name in the edge's scope.
  auto binding = [&rule](const char* key) {
    if (!rule->GetBinding(key))
      rule->AddBinding(key, EvalString());
    return rule->GetBinding(key);
  };
  if (binding("rspfile")->empty() != binding("rspfile_content")->empty()) {
    return lexer_.Error("rspfile and rspfile_content need to be "
                        "both specified", err);
  }

  if (binding("command")->empty())
    return lexer_.Error("expected 'command =' line", err);

  env_->AddRule(std::move(rule));
  return true;
}

bool FileProcessor::ParseLet(StringPiece* key, RawString* value,
                             std::string* err) {
  if (!lexer_.ReadIdent(key))
    return lexer_.Error("expected variable name", err);
  if (!lexer_.ExpectToken(Lex::EQUALS, err))
    return false;
  if (!lexer_.ReadVarValue(value, err))
    return false;
  return true;
}

bool FileProcessor::ParseDefault(std::string* err) {
  RawString raw;
  bool ok = lexer_.ReadPath(&raw, err);
  FlushNewlineEscapes();
  if (!ok)
    return false;
  if (raw.empty())
    return lexer_.Error("expected target name", err);

  PendingDefault pending;
  auto flush = [&]() {
    if (pending.paths.empty())
      return;
    file_->items.push_back({ Item::kDefault, file_->defaults.size(), nullptr });
    file_->defaults.push_back(std::move(pending));
    pending.paths.clear();
  };
  do {
    // Defaults only look up nodes.
    PathRef path = EvaluatePath(raw, env_, /*with_node=*/false);
    pending.paths.emplace_back(path, lexer_.last_token_);
    // An empty path is an error, reported when merging.
    if (path.len == 0) {
      flush();
      return false;
    }

    ok = lexer_.ReadPath(&raw, err);
    if (!lexer_.newline_escapes_.empty()) {
      flush();
      FlushNewlineEscapes();
    }
    if (!ok) {
      flush();
      return false;
    }
  } while (!raw.empty());
  flush();

  return lexer_.ExpectToken(Lex::NEWLINE, err);
}

/// An Env for evaluating an edge's "pool" and "dyndep" bindings while
/// parsing, like EdgeEnv would, but without an Edge: it records whether
/// $in or $out were used, and whether there was a cycle, in which case the
/// evaluation must be redone when merging.
struct ParseEdgeEnv : public Env {
  ParseEdgeEnv(const Rule* rule, BindingEnv* env) : rule_(rule), env_(env) {}

  std::string LookupVariable(StringPiece var) override {
    std::string result;
    AppendVariable(var, &result);
    return result;
  }

  void AppendVariable(StringPiece var, std::string* out) override {
    if (var == "in" || var == "in_newline" || var == "out") {
      uses_in_out_ = true;
      return;
    }
    if (recursive_ &&
        std::find(lookups_.begin(), lookups_.end(), var) != lookups_.end()) {
      cycle_ = true;
      return;
    }
    const EvalString* eval = rule_->GetBinding(var);
    bool record_varname = recursive_ && eval;
    if (record_varname)
      lookups_.push_back(var);
    recursive_ = true;
    env_->AppendWithFallback(var, eval, this, out);
    if (record_varname)
      lookups_.pop_back();
  }

  bool uses_in_out_ = false;
  bool cycle_ = false;

 private:
  const Rule* rule_;
  BindingEnv* env_;
  std::vector<StringPiece> lookups_;
  bool recursive_ = false;
};

bool FileProcessor::ParseEdge(std::string* err) {
  ins_.clear();
  outs_.clear();
  validations_.clear();

  // Lexer errors are reported after the $^ uses before them.
  auto read_path = [&](RawString* path) {
    bool ok = lexer_.ReadPath(path, err);
    FlushNewlineEscapes();
    return ok;
  };

  {
    RawString out;
    if (!read_path(&out))
      return false;
    while (!out.empty()) {
      outs_.push_back(out);

      if (!read_path(&out))
        return false;
    }
  }

  // Add all implicit outs, counting how many as we go.
  int implicit_outs = 0;
  if (lexer_.PeekToken(Lex::PIPE)) {
    for (;;) {
      RawString out;
      if (!read_path(&out))
        return false;
      if (out.empty())
        break;
      outs_.push_back(out);
      ++implicit_outs;
    }
  }

  if (outs_.empty())
    return lexer_.Error("expected path", err);

  if (!lexer_.ExpectToken(Lex::COLON, err))
    return false;

  StringPiece rule_name;
  if (!lexer_.ReadIdent(&rule_name))
    return lexer_.Error("expected build command name", err);

  const Rule* rule = env_->LookupRule(rule_name);
  if (!rule)
    return lexer_.Error("unknown build rule '" + rule_name.AsString() + "'",
                        err);

  for (;;) {
    // XXX should we require one path here?
    RawString in;
    if (!read_path(&in))
      return false;
    if (in.empty())
      break;
    ins_.push_back(in);
  }

  // Add all implicit deps, counting how many as we go.
  int implicit = 0;
  if (lexer_.PeekToken(Lex::PIPE)) {
    for (;;) {
      RawString in;
      if (!read_path(&in))
        return false;
      if (in.empty())
        break;
      ins_.push_back(in);
      ++implicit;
    }
  }

  // Add all order-only deps, counting how many as we go.
  int order_only = 0;
  if (lexer_.PeekToken(Lex::PIPE2)) {
    for (;;) {
      RawString in;
      if (!read_path(&in))
        return false;
      if (in.empty())
        break;
      ins_.push_back(in);
      ++order_only;
    }
  }

  // Add all validations, counting how many as we go.
  if (lexer_.PeekToken(Lex::PIPEAT)) {
    for (;;) {
      RawString validation;
      if (!read_path(&validation))
        return false;
      if (validation.empty())
        break;
      validations_.push_back(validation);
    }
  }

  if (!lexer_.ExpectToken(Lex::NEWLINE, err))
    return false;

  // Bindings on edges are rare, so allocate per-edge envs only when needed.
  bool has_indent_token = lexer_.PeekToken(Lex::INDENT);
  BindingEnv* env = has_indent_token ? new BindingEnv(env_) : env_;
  while (has_indent_token) {
    StringPiece key;
    RawString val;
    bool ok = ParseLet(&key, &val, err);
    FlushNewlineEscapes();
    if (!ok)
      return false;

    // The manifest's text is kept as long as the State.
    if (val.literal)
      env->AddBindingView(key, StringPiece(val.begin, val.end - val.begin));
    else
      env->AddBindingView(key, env->Keep(Evaluate(val, env_)));
    has_indent_token = lexer_.PeekToken(Lex::INDENT);
  }

  PendingEdge edge;
  edge.rule = rule;
  edge.env = env;
  edge.error_pos = lexer_.last_token_;

  // The pool is looked up before the edge has inputs and outputs.
  {
    ParseEdgeEnv pool_env(rule, env);
    pool_env.AppendVariable("pool", &edge.pool);
    edge.pool_deferred = pool_env.cycle_;
  }

  edge.paths_begin = file_->paths.size();
  edge.outs = static_cast<int>(outs_.size());
  edge.implicit_outs = implicit_outs;
  edge.ins = static_cast<int>(ins_.size());
  edge.implicit_ins = implicit;
  edge.order_only_ins = order_only;
  edge.validations = static_cast<int>(validations_.size());
  for (const RawString& raw : outs_)
    file_->paths.push_back(EvaluatePath(raw, env, true));
  for (const RawString& raw : ins_)
    file_->paths.push_back(EvaluatePath(raw, env, true));
  for (const RawString& raw : validations_)
    file_->paths.push_back(EvaluatePath(raw, env, true));

  // Build the edge, unless a path is empty: then there will be an error
  // when adding it.
  bool empty_path = false;
  for (size_t i = edge.paths_begin; i < file_->paths.size(); ++i)
    empty_path |= !file_->paths[i].node;
  if (!empty_path) {
    Edge* e = State::CreateEdge(rule, env);
    const PathRef* path = &file_->paths[edge.paths_begin];
    e->outputs_.reserve(edge.outs);
    for (int i = 0; i < edge.outs; ++i)
      e->outputs_.push_back(path++->node);
    e->implicit_outs_ = edge.implicit_outs;
    e->inputs_.reserve(edge.ins);
    for (int i = 0; i < edge.ins; ++i)
      e->inputs_.push_back(path++->node);
    e->implicit_deps_ = edge.implicit_ins;
    e->order_only_deps_ = edge.order_only_ins;
    e->validations_.reserve(edge.validations);
    for (int i = 0; i < edge.validations; ++i)
      e->validations_.push_back(path++->node);
    edge.edge = e;
  }

  // The dyndep binding is evaluated after the edge got its inputs and
  // outputs; if it refers to them, evaluate it when merging.
  {
    ParseEdgeEnv dyndep_env(rule, env);
    dyndep_env.AppendVariable("dyndep", &edge.dyndep);
    edge.dyndep_deferred = dyndep_env.uses_in_out_ || dyndep_env.cycle_;
    if (!edge.dyndep_deferred && !edge.dyndep.empty())
      CanonicalizePath(&edge.dyndep, &edge.dyndep_slash_bits);
  }

  file_->items.push_back({ Item::kEdge, file_->edges.size(), nullptr });
  file_->edges.push_back(std::move(edge));
  return true;
}

bool FileProcessor::ParseFileInclude(bool new_scope, std::string* err) {
  RawString raw;
  bool ok = lexer_.ReadPath(&raw, err);
  FlushNewlineEscapes();
  if (!ok)
    return false;

  std::unique_ptr<ParsedFile> owned_child(new ParsedFile);
  ParsedFile* child = owned_child.get();
  child->filename = Evaluate(raw, env_);
  child->depth = file_->depth + 1;
  file_->items.push_back({ Item::kFile, 0, std::move(owned_child) });

  BindingEnv* child_env = new_scope ? new BindingEnv(env_) : env_;
  // Errors loading the file are reported at the end of its path.
  const char* pos = lexer_.last_token_;
  if (new_scope && ctx_->parallel && FrozenAt(pos)) {
    ParseContext* ctx = ctx_;
    ParsedFile* parent = file_;
    ctx_->tasks.Run([ctx, parent, pos, child, child_env] {
      LoadAndProcessFile(ctx, parent, pos, child, child_env, true);
      NotifyTaskWaiters();
    });
    return lexer_.ExpectToken(Lex::NEWLINE, err);
  }

  // Process the file right away.  Its parent scopes may change after this
  // statement, so its subninjas must be processed right away as well.
  if (!LoadAndProcessFile(ctx_, file_, pos, child, child_env, false))
    return false;
  return lexer_.ExpectToken(Lex::NEWLINE, err);
}

}  // anonymous namespace

/// Applies the items of parsed files to the State, in order.
struct ManifestParser::Merger {
  Merger(ManifestParser* parser) : parser_(parser), state_(parser->state_) {}

  bool Merge(ParsedFile* file, std::string* err);

 private:
  bool MergeEdge(ParsedFile* file, const PendingEdge& pending,
                 std::string* err);
  /// The part of MergeEdge() after the nodes of the edge are connected.
  bool FinishEdge(ParsedFile* file, const PendingEdge& pending, Edge* edge,
                  std::string* err);

  bool Error(ParsedFile* file, const char* pos, const std::string& message,
             std::string* err) {
    *err = FormatError(file->filename, file->text, pos, message);
    return false;
  }

  /// The state of the $^ checks of the Lexer of each depth.
  struct DepthState {
    int major = 0;
    int minor = 0;
    bool newline_checked = false;
  };
  DepthState& depth_state(int depth) {
    if (static_cast<size_t>(depth) >= depth_states_.size())
      depth_states_.resize(depth + 1);
    return depth_states_[depth];
  }

  ManifestParser* parser_;
  State* state_;
  std::vector<DepthState> depth_states_;
};

bool ManifestParser::Merger::Merge(ParsedFile* file, std::string* err) {
  for (Item& item : file->items) {
    switch (item.kind) {
    case Item::kEdge:
      if (!MergeEdge(file, file->edges[item.index], err))
        return false;
      break;
    case Item::kPool: {
      const PendingPool& pool = file->pools[item.index];
      if (state_->LookupPool(pool.name) != NULL)
        return Error(file, pool.error_pos,
                     "duplicate pool '" + pool.name + "'", err);
      if (pool.add)
        state_->AddPool(new Pool(pool.name, pool.depth));
      break;
    }
    case Item::kDefault:
      for (const auto& path : file->defaults[item.index].paths) {
        if (path.first.len == 0)
          return Error(file, path.second, "empty path", err);
        // Nodes that are only provisional have not been referenced yet in a
        // serial parse.
        Node* node = state_->LookupNode(path.first.piece());
        if (!node || node->slash_bits_provisional())
          return Error(file, path.second,
                       "unknown target '" + path.first.piece().AsString() +
                           "'",
                       err);
        state_->defaults_.push_back(node);
      }
      break;
    case Item::kFile: {
      ParsedFile* child = item.file.get();
      RunTasksUntil(
          [child] { return child->done.load(std::memory_order_acquire); });
      if (!Merge(child, err))
        return false;
      // The child's contents may be referred to by error messages only.
      break;
    }
    case Item::kError:
      *err = file->strings[item.index];
      return false;
    case Item::kVersion: {
      DepthState& depth = depth_state(file->depth);
      CheckNinjaVersion(file->strings[item.index], &depth.major,
                        &depth.minor);
      break;
    }
    case Item::kNewlineEscape: {
      DepthState& depth = depth_state(file->depth);
      if (!depth.newline_checked) {
        if ((depth.major < kMinNewlineEscapeVersionMajor) ||
            (depth.major == kMinNewlineEscapeVersionMajor &&
             depth.minor < kMinNewlineEscapeVersionMinor)) {
          *err = file->strings[item.index];
          return false;
        }
        depth.newline_checked = true;
      }
      break;
    }
    }
  }
  return true;
}

bool ManifestParser::Merger::MergeEdge(ParsedFile* file,
                                       const PendingEdge& pending,
                                       std::string* err) {
  Edge* edge = pending.edge;
  if (edge) {
    state_->AppendEdge(edge);
  } else {
    edge = state_->AddEdge(pending.rule);
    edge->env_ = pending.env;
  }

  // The pool is looked up before the edge has inputs and outputs.
  std::string pool_name;
  if (pending.pool_deferred) {
    Edge pool_edge;
    pool_edge.rule_ = pending.rule;
    pool_edge.env_ = pending.env;
    pool_name = pool_edge.GetBinding("pool");
  } else {
    pool_name = pending.pool;
  }
  if (!pool_name.empty()) {
    Pool* pool = state_->LookupPool(pool_name);
    if (pool == NULL)
      return Error(file, pending.error_pos,
                   "unknown pool name '" + pool_name + "'", err);
    edge->pool_ = pool;
  }

  const PathRef* path = &file->paths[pending.paths_begin];
  if (pending.edge) {
    // The edge has its nodes already: connect them.
    for (Node* node : edge->outputs_) {
      if (!state_->ConnectOutput(edge, node, path++->slash_bits, err))
        return Error(file, pending.error_pos, std::string(*err), err);
    }
    for (Node* node : edge->inputs_)
      state_->ConnectInput(edge, node, path++->slash_bits);
    for (Node* node : edge->validations_)
      state_->ConnectValidation(edge, node, path++->slash_bits);
    return FinishEdge(file, pending, edge, err);
  }

  edge->outputs_.reserve(pending.outs);
  for (int i = 0; i < pending.outs; ++i, ++path) {
    if (path->len == 0)
      return Error(file, pending.error_pos, "empty path", err);
    if (!state_->AddOut(edge, path->node, path->slash_bits, err))
      return Error(file, pending.error_pos, std::string(*err), err);
  }

  if (edge->outputs_.empty()) {
    // All outputs of the edge are already created by other edges. Don't add
    // this edge.  Do this check before input nodes are connected to the edge.
    // (Edges are never deleted.)
    state_->edges_.pop_back();
    return true;
  }
  edge->implicit_outs_ = pending.implicit_outs;

  edge->inputs_.reserve(pending.ins);
  for (int i = 0; i < pending.ins; ++i, ++path) {
    if (path->len == 0)
      return Error(file, pending.error_pos, "empty path", err);
    state_->AddIn(edge, path->node, path->slash_bits);
  }
  edge->implicit_deps_ = pending.implicit_ins;
  edge->order_only_deps_ = pending.order_only_ins;

  edge->validations_.reserve(pending.validations);
  for (int i = 0; i < pending.validations; ++i, ++path) {
    if (path->len == 0)
      return Error(file, pending.error_pos, "empty path", err);
    state_->AddValidation(edge, path->node, path->slash_bits);
  }
  return FinishEdge(file, pending, edge, err);
}

bool ManifestParser::Merger::FinishEdge(ParsedFile* file,
                                        const PendingEdge& pending,
                                        Edge* edge, std::string* err) {
  if (parser_->options_.phony_cycle_action_ == kPhonyCycleActionWarn &&
      edge->maybe_phonycycle_diagnostic()) {
    // CMake 2.8.12.x and 3.0.x incorrectly write phony build statements
    // that reference themselves.  Ninja used to tolerate these in the
    // build graph but that has since been fixed.  Filter them out to
    // support users of those old CMake versions.
    Node* out = edge->outputs_[0];
    vector<Node*>::iterator new_end =
        remove(edge->inputs_.begin(), edge->inputs_.end(), out);
    if (new_end != edge->inputs_.end()) {
      out->RemoveOutEdge(edge);
      edge->inputs_.erase(new_end, edge->inputs_.end());
      if (!parser_->quiet_) {
        Warning("phony target '%s' names itself as an input; "
                "ignoring [-w phonycycle=warn]",
                out->path().c_str());
      }
    }
  }

  // Lookup, validate, and save any dyndep binding.  It will be used later
  // to load generated dependency information dynamically, but it must
  // be one of our manifest-specified inputs.
  std::string dyndep;
  uint64_t slash_bits = pending.dyndep_slash_bits;
  if (pending.dyndep_deferred) {
    dyndep = edge->GetUnescapedDyndep();
    if (!dyndep.empty())
      CanonicalizePath(&dyndep, &slash_bits);
  } else {
    dyndep = pending.dyndep;
  }
  if (!dyndep.empty()) {
    edge->dyndep_ = state_->GetNode(dyndep, slash_bits);
    edge->dyndep_->set_dyndep_pending(true);
    vector<Node*>::iterator dgi =
      std::find(edge->inputs_.begin(), edge->inputs_.end(), edge->dyndep_);
    if (dgi == edge->inputs_.end()) {
      return Error(file, pending.error_pos,
                   "dyndep '" + dyndep + "' is not an input", err);
    }
    assert(!edge->dyndep_->generated_by_dep_loader());
  }

  return true;
}

ManifestParser::ManifestParser(State* state, FileReader* file_reader,
                               ManifestParserOptions options)
    : Parser(state, file_reader),
      options_(options), quiet_(false) {
  env_ = &state->bindings_;
}

std::string ManifestParser::LookupVariable(const std::string& varname) {
  return env_->LookupVariable(varname);
}

bool ManifestParser::Load(const string& filename, string* err) {
  METRIC_RECORD(".ninja parse");
  std::unique_ptr<ParsedFile> root(new ParsedFile);
  root->filename = filename;
  string read_err;
  if (file_reader_->LoadFile(filename, kCharScanPadding + 1,
                             root->contents.get(),
                             &read_err) != FileReader::Okay) {
    *err = "loading '" + filename + "': " + read_err;
    return false;
  }
  return ParseRoot(root.release(), err);
}

bool ManifestParser::Parse(const string& filename, const string& input,
                           string* err) {
  string contents = input;
  return ParseContents(filename, &contents, err);
}

bool ManifestParser::ParseContents(const string& filename, string* contents,
                                   string* err) {
  std::unique_ptr<ParsedFile> root(new ParsedFile);
  root->filename = filename;
  FileContents* c = root->contents.get();
  c->storage.swap(*contents);
  c->size = c->storage.size();
  c->storage.append(kCharScanPadding + 1, '\0');
  c->data = c->storage.data();
  return ParseRoot(root.release(), err);
}

bool ManifestParser::ParseRoot(void* root_file, string* err) {
  ParseContext ctx;
  ctx.state = state_;
  ctx.file_reader = file_reader_;
  ctx.parallel = file_reader_ && file_reader_->IsReadFileThreadSafe() &&
                 TaskGroup::Concurrency() > 1;

  std::unique_ptr<ParsedFile> root(static_cast<ParsedFile*>(root_file));
  ProcessFile(&ctx, root.get(), env_, true);

  Merger merger(this);
  bool ok = merger.Merge(root.get(), err);
  // On errors, there may still be files being processed.
  ctx.tasks.Wait();
  // Bindings point into the files' contents.
  std::vector<ParsedFile*> files(1, root.get());
  while (!files.empty()) {
    ParsedFile* file = files.back();
    files.pop_back();
    if (file->contents->data)
      state_->manifest_contents_.push_back(std::move(file->contents));
    for (Item& item : file->items)
      if (item.file)
        files.push_back(item.file.get());
  }
  // Freeing all that takes a while.
  if (ctx.parallel) {
    ParsedFile* released = root.release();
    RunDetached([released] { delete released; });
  }
  return ok;
}
