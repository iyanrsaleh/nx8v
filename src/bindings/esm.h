#pragma once
// nx8v/src/bindings/esm.h
// ES Module support by source transformation to CommonJS.
//
// Supported syntax:
//   import def from 'm';                 -> const def = __nx8vInterop(require('m'));
//   import * as ns from 'm';             -> const ns = require('m');
//   import { a, b as c } from 'm';       -> const { a, b: c } = require('m');
//   import def, { a } from 'm';          -> default + named
//   import 'm';                          -> require('m');
//   export default <expr>;               -> exports.default = <expr>;
//   export const/let/var x = ...;        -> keep decl, then exports.x = x;
//   export function f(){} / class C{}    -> keep decl, then exports.f = f;
//   export { a, b as c };                -> exports.c = b;
//   export { a } from 'm';               -> re-export
//   export * from 'm';                   -> copy all keys
//
// The transform is textual but handles multi-line import/export lists, nested
// strings are ignored by only scanning statement starts at line boundaries.

#include <cctype>
#include <string>
#include <vector>

namespace nx8v {
namespace bindings {

inline std::string EsmTrim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  size_t e = s.find_last_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  return s.substr(b, e - b + 1);
}

inline bool EsmStartsWith(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Find the end of a block declaration (function/class) starting at `pos`.
// Returns the index just past the matching closing brace (+ optional ';').
inline size_t EsmBlockEnd(const std::string& src, size_t pos) {
  bool in_s = false, in_d = false, in_t = false, escape = false;
  int depth = 0;
  bool seen_open = false;
  for (size_t i = pos; i < src.size(); ++i) {
    char c = src[i];
    if (escape) { escape = false; continue; }
    if (c == '\\' && (in_s || in_d || in_t)) { escape = true; continue; }
    if (!in_d && !in_t && c == '\'') { in_s = !in_s; continue; }
    if (!in_s && !in_t && c == '"') { in_d = !in_d; continue; }
    if (!in_s && !in_d && c == '`') { in_t = !in_t; continue; }
    if (in_s || in_d || in_t) continue;

    if (c == '{') { ++depth; seen_open = true; }
    else if (c == '}') {
      --depth;
      if (seen_open && depth == 0) {
        size_t j = i + 1;
        while (j < src.size() && (src[j] == ' ' || src[j] == '\t' ||
                                  src[j] == '\r' || src[j] == '\n'))
          ++j;
        if (j < src.size() && src[j] == ';') ++j;
        return j;
      }
    }
  }
  return src.size();
}

// Find the end of a statement starting at `pos`, respecting quotes.
// Returns the index just past the terminating ';' (or the line end).
inline size_t EsmStatementEnd(const std::string& src, size_t pos) {
  bool in_s = false, in_d = false, in_t = false, escape = false;
  int depth = 0;
  for (size_t i = pos; i < src.size(); ++i) {
    char c = src[i];
    if (escape) { escape = false; continue; }
    if (c == '\\' && (in_s || in_d || in_t)) { escape = true; continue; }
    if (!in_d && !in_t && c == '\'') { in_s = !in_s; continue; }
    if (!in_s && !in_t && c == '"') { in_d = !in_d; continue; }
    if (!in_s && !in_d && c == '`') { in_t = !in_t; continue; }
    if (in_s || in_d || in_t) continue;

    if (c == '{' || c == '(' || c == '[') ++depth;
    else if (c == '}' || c == ')' || c == ']') { if (depth > 0) --depth; }
    else if (c == ';' && depth == 0) return i + 1;
  }
  return src.size();
}

// Extract the specifier from `from 'x'` at the end of a statement.
inline bool EsmExtractFrom(const std::string& stmt, std::string& spec,
                           std::string& before) {
  size_t from_pos = stmt.rfind("from");
  if (from_pos == std::string::npos) return false;

  size_t q = stmt.find_first_of("'\"", from_pos);
  if (q == std::string::npos) return false;
  char quote = stmt[q];
  size_t q2 = stmt.find(quote, q + 1);
  if (q2 == std::string::npos) return false;

  spec = stmt.substr(q + 1, q2 - q - 1);
  before = EsmTrim(stmt.substr(0, from_pos));
  return true;
}

// Convert `{ a, b as c }` into `{ a, b: c }`.
inline std::string EsmNamedToDestructure(const std::string& list) {
  std::string inner = EsmTrim(list);
  if (inner.size() >= 2 && inner.front() == '{' && inner.back() == '}')
    inner = inner.substr(1, inner.size() - 2);

  std::string out, cur;
  std::vector<std::string> parts;
  for (char c : inner) {
    if (c == ',') { parts.push_back(cur); cur.clear(); }
    else cur += c;
  }
  if (!EsmTrim(cur).empty()) parts.push_back(cur);

  for (auto& p : parts) {
    std::string t = EsmTrim(p);
    if (t.empty()) continue;
    // "b as c" -> "b: c"
    size_t as_pos = t.find(" as ");
    if (as_pos != std::string::npos) {
      std::string orig = EsmTrim(t.substr(0, as_pos));
      std::string alias = EsmTrim(t.substr(as_pos + 4));
      if (!out.empty()) out += ", ";
      out += orig + ": " + alias;
    } else {
      if (!out.empty()) out += ", ";
      out += t;
    }
  }
  return out;
}

// Convert `{ a, b as c }` into export assignments.
inline std::string EsmNamedToExports(const std::string& list,
                                     const std::string& from_expr) {
  std::string inner = EsmTrim(list);
  if (inner.size() >= 2 && inner.front() == '{' && inner.back() == '}')
    inner = inner.substr(1, inner.size() - 2);

  std::string out, cur;
  std::vector<std::string> parts;
  for (char c : inner) {
    if (c == ',') { parts.push_back(cur); cur.clear(); }
    else cur += c;
  }
  if (!EsmTrim(cur).empty()) parts.push_back(cur);

  for (auto& p : parts) {
    std::string t = EsmTrim(p);
    if (t.empty()) continue;
    size_t as_pos = t.find(" as ");
    std::string local = (as_pos == std::string::npos) ? t : EsmTrim(t.substr(0, as_pos));
    std::string exported = (as_pos == std::string::npos) ? t : EsmTrim(t.substr(as_pos + 4));
    out += "exports." + exported + " = " + (from_expr.empty() ? local : from_expr + "." + local) + ";\n";
  }
  return out;
}

// Does this source look like an ES module?
inline bool EsmLooksLikeModule(const std::string& src) {
  size_t pos = 0;
  while (pos < src.size()) {
    size_t nl = src.find('\n', pos);
    std::string line = EsmTrim(src.substr(pos, nl == std::string::npos
                                                 ? std::string::npos
                                                 : nl - pos));
    if (EsmStartsWith(line, "import ") || EsmStartsWith(line, "import{") ||
        EsmStartsWith(line, "export ") || EsmStartsWith(line, "export{") ||
        EsmStartsWith(line, "export default"))
      return true;
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  return false;
}

// Transform ES module source into CommonJS.
inline std::string EsmTransform(const std::string& src) {
  std::string out;
  out.reserve(src.size() + 256);
  out += "\"use strict\";\n";
  out += "Object.defineProperty(exports, '__esModule', { value: true });\n";

  size_t pos = 0;
  int counter = 0;

  while (pos < src.size()) {
    // Preserve leading whitespace of the statement.
    size_t line_start = pos;
    while (line_start < src.size() &&
           (src[line_start] == ' ' || src[line_start] == '\t'))
      ++line_start;

    std::string rest = src.substr(line_start);

    // ---- import ... ----
    if (EsmStartsWith(rest, "import ")) {
      size_t end = EsmStatementEnd(src, line_start);
      std::string stmt = src.substr(line_start, end - line_start);
      std::string spec, before;
      bool has_from = EsmExtractFrom(stmt, spec, before);

      if (!has_from) {
        // side-effect import: import 'mod';
        size_t q1 = stmt.find_first_of("'\"");
        size_t q2 = (q1 == std::string::npos) ? std::string::npos
                                              : stmt.find(stmt[q1], q1 + 1);
        if (q1 != std::string::npos && q2 != std::string::npos) {
          out += "require('" + stmt.substr(q1 + 1, q2 - q1 - 1) + "');\n";
          pos = end;
          continue;
        }
      } else {
        // `before` looks like: "default", "* as ns", "{ a, b }", "def, { a }"
        std::string clause = EsmTrim(before.substr(6));  // drop "import"
        std::string tmp = "__nx8v_m" + std::to_string(counter++);

        out += "const " + tmp + " = require('" + spec + "');\n";

        if (EsmStartsWith(clause, "*")) {
          // import * as ns from 'm'
          std::string alias = EsmTrim(clause.substr(clause.find("as") + 2));
          out += "const " + alias + " = " + tmp + ";\n";
        } else if (!clause.empty() && clause[0] == '{') {
          out += "const { " + EsmNamedToDestructure(clause) + " } = " + tmp + ";\n";
        } else {
          // default [, { named }]
          size_t brace = clause.find('{');
          std::string def = (brace == std::string::npos) ? clause
                                                         : EsmTrim(clause.substr(0, brace));
          if (!def.empty() && def.back() == ',') def.pop_back();
          def = EsmTrim(def);
          if (!def.empty())
            out += "const " + def + " = (" + tmp + " && " + tmp +
                   ".__esModule) ? " + tmp + ".default : " + tmp + ";\n";
          if (brace != std::string::npos) {
            std::string named = EsmTrim(clause.substr(brace));
            out += "const { " + EsmNamedToDestructure(named) + " } = " + tmp + ";\n";
          }
        }
        pos = end;
        continue;
      }
    }

    // ---- export default ----
    if (EsmStartsWith(rest, "export default")) {
      size_t stmt_start = line_start + 14;  // after "export default"
      size_t probe = stmt_start;
      while (probe < src.size() && (src[probe] == ' ' || src[probe] == '\t')) ++probe;
      bool is_block = EsmStartsWith(src.substr(probe, 8), "function") ||
                      EsmStartsWith(src.substr(probe, 5), "class") ||
                      EsmStartsWith(src.substr(probe, 5), "async");
      size_t end = is_block ? EsmBlockEnd(src, probe)
                            : EsmStatementEnd(src, stmt_start);
      std::string expr = EsmTrim(src.substr(stmt_start, end - stmt_start));
      if (!expr.empty() && expr.back() == ';') expr.pop_back();
      out += "exports.default = " + expr + ";\n";
      pos = end;
      continue;
    }

    // ---- export * from 'm' ----
    if (EsmStartsWith(rest, "export *")) {
      size_t end = EsmStatementEnd(src, line_start);
      std::string stmt = src.substr(line_start, end - line_start);
      std::string spec, before;
      if (EsmExtractFrom(stmt, spec, before)) {
        std::string tmp = "__nx8v_m" + std::to_string(counter++);
        out += "const " + tmp + " = require('" + spec + "');\n";
        out += "for (const k in " + tmp + ") if (k !== 'default' && k !== '__esModule') exports[k] = " + tmp + "[k];\n";
      }
      pos = end;
      continue;
    }

    // ---- export { ... } [from 'm'] ----
    if (EsmStartsWith(rest, "export {")) {
      size_t end = EsmStatementEnd(src, line_start);
      std::string stmt = src.substr(line_start, end - line_start);
      size_t open = stmt.find('{');
      size_t close = stmt.find('}', open);
      if (open != std::string::npos && close != std::string::npos) {
        std::string list = stmt.substr(open, close - open + 1);
        std::string spec, before;
        if (EsmExtractFrom(stmt, spec, before)) {
          std::string tmp = "__nx8v_m" + std::to_string(counter++);
          out += "const " + tmp + " = require('" + spec + "');\n";
          out += EsmNamedToExports(list, tmp);
        } else {
          out += EsmNamedToExports(list, "");
        }
      }
      pos = end;
      continue;
    }

    // ---- export const/let/var/function/class ----
    if (EsmStartsWith(rest, "export const") || EsmStartsWith(rest, "export let") ||
        EsmStartsWith(rest, "export var") || EsmStartsWith(rest, "export function") ||
        EsmStartsWith(rest, "export class") || EsmStartsWith(rest, "export async function")) {
      // Emit the declaration without the `export ` keyword, then export names.
      std::string decl = rest.substr(7);
      bool is_block = EsmStartsWith(rest, "export function") ||
                      EsmStartsWith(rest, "export class") ||
                      EsmStartsWith(rest, "export async function");
      size_t end = is_block ? EsmBlockEnd(src, line_start)
                            : EsmStatementEnd(src, line_start);
      std::string stmt = src.substr(line_start, end - line_start);
      std::string decl_stmt = stmt.substr(7);

      out += decl_stmt;
      if (!decl_stmt.empty() && decl_stmt.back() != '\n') out += "\n";

      // Collect declared names.
      std::vector<std::string> names;
      std::string d = EsmTrim(decl_stmt);
      if (EsmStartsWith(d, "const") || EsmStartsWith(d, "let") || EsmStartsWith(d, "var")) {
        size_t sp = d.find(' ');
        std::string rest_decl = d.substr(sp + 1);
        // split top-level commas
        int depth = 0;
        bool in_s = false, in_d = false;
        std::string cur;
        for (char c : rest_decl) {
          if (c == '\'' && !in_d) in_s = !in_s;
          else if (c == '"' && !in_s) in_d = !in_d;
          if (!in_s && !in_d) {
            if (c == '{' || c == '(' || c == '[') ++depth;
            else if (c == '}' || c == ')' || c == ']') --depth;
            else if (c == ',' && depth == 0) { names.push_back(cur); cur.clear(); continue; }
          }
          cur += c;
        }
        names.push_back(cur);
      } else if (EsmStartsWith(d, "function") || EsmStartsWith(d, "async function")) {
        size_t p = d.find('(');
        if (p != std::string::npos) {
          std::string head = EsmTrim(d.substr(0, p));
          size_t sp = head.find_last_of(' ');
          names.push_back(head.substr(sp + 1));
        }
      } else if (EsmStartsWith(d, "class")) {
        size_t sp = d.find(' ');
        std::string head = EsmTrim(d.substr(sp + 1));
        size_t b = head.find_first_of(" {");
        names.push_back(b == std::string::npos ? head : head.substr(0, b));
      }

      for (auto& n : names) {
        std::string name = EsmTrim(n);
        size_t eq = name.find('=');
        if (eq != std::string::npos) name = EsmTrim(name.substr(0, eq));
        size_t colon = name.find(':');
        if (colon != std::string::npos) name = EsmTrim(name.substr(0, colon));
        if (!name.empty() && std::isalpha((unsigned char)name[0]))
          out += "exports." + name + " = " + name + ";\n";
      }
      pos = end;
      continue;
    }

    // Ordinary source: copy through.
    out += src[pos];
    ++pos;
  }

  return out;
}

}} // nx8v::bindings
