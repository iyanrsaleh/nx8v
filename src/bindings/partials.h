#pragma once
// HTML partials. Any tag may pull in another file:
//
//   <header partial="./header.html"></header>
//
// nx8v reads the file while sending the page and puts its contents inside
// the tag. The attribute is removed. Paths are relative to the page that
// contains the tag (./header.html sits next to index.html). A path that
// climbs out of the project's public folder is ignored.

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../runtime.h"

namespace fs = std::filesystem;

namespace nx8v {
namespace bindings {

static std::string Nx8vLowerCopy(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

static size_t Nx8vFindTagEnd(const std::string& html, size_t lt) {
  char quote = 0;
  for (size_t i = lt + 1; i < html.size(); ++i) {
    char c = html[i];
    if (quote) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      continue;
    }
    if (c == '>') return i;
  }
  return std::string::npos;
}

static std::string Nx8vTagName(const std::string& tag) {
  size_t i = 1;
  if (i < tag.size() && tag[i] == '/') ++i;
  while (i < tag.size() && std::isspace((unsigned char)tag[i])) ++i;
  size_t b = i;
  while (i < tag.size() &&
         (std::isalnum((unsigned char)tag[i]) || tag[i] == '-' || tag[i] == ':'))
    ++i;
  return Nx8vLowerCopy(tag.substr(b, i - b));
}

// Reads partial="..." / partial='...' from an opening tag.
static bool Nx8vPartialAttr(const std::string& tag, size_t& attr_b, size_t& attr_e,
                            std::string& value) {
  std::string low = Nx8vLowerCopy(tag);
  size_t pos = 0;
  while ((pos = low.find("partial", pos)) != std::string::npos) {
    if (pos > 0) {
      unsigned char prev = (unsigned char)low[pos - 1];
      if (std::isalnum(prev) || prev == '-' || prev == '_' || prev == ':') {
        pos += 7;
        continue;
      }
    }
    size_t after = pos + 7;
    while (after < tag.size() && std::isspace((unsigned char)tag[after])) ++after;
    if (after >= tag.size() || tag[after] != '=') {
      pos += 7;
      continue;
    }
    ++after;
    while (after < tag.size() && std::isspace((unsigned char)tag[after])) ++after;
    if (after >= tag.size()) return false;
    if (tag[after] == '"' || tag[after] == '\'') {
      char q = tag[after];
      size_t end = tag.find(q, after + 1);
      if (end == std::string::npos) return false;
      value = tag.substr(after + 1, end - after - 1);
      attr_b = pos;
      attr_e = end + 1;
      return true;
    }
    size_t end = after;
    while (end < tag.size() && !std::isspace((unsigned char)tag[end]) &&
           tag[end] != '>' && tag[end] != '/')
      ++end;
    value = tag.substr(after, end - after);
    attr_b = pos;
    attr_e = end;
    return !value.empty();
  }
  return false;
}

static fs::path Nx8vPublicRoot() {
  fs::path base = ".";
  if (nx8v::g_runtime && !nx8v::g_runtime->script_dir.empty())
    base = nx8v::g_runtime->script_dir;
  std::error_code ec;
  fs::path pub = base / "public";
  if (fs::is_directory(pub, ec)) return fs::absolute(pub, ec).lexically_normal();
  return fs::absolute(base, ec).lexically_normal();
}

static fs::path Nx8vPageDir(const std::string& url, const fs::path& root) {
  std::string path = url;
  auto cut = path.find_first_of("?#");
  if (cut != std::string::npos) path.resize(cut);
  if (path.empty() || path == "/") return root;
  while (!path.empty() && (path[0] == '/' || path[0] == '\\')) path.erase(0, 1);
  while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
  if (path.empty()) return root;

  std::vector<std::string> segments;
  std::string cur;
  for (char c : path) {
    if (c == '/' || c == '\\') {
      if (!cur.empty()) segments.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) segments.push_back(cur);
  for (const auto& s : segments) {
    if (s == ".." || s == ".") return root;
  }

  // /tex/hel/segmen1/dll uses public/tex/hel.html, same as the page router.
  std::error_code ec;
  for (size_t i = segments.size(); i >= 1; --i) {
    fs::path base = root;
    for (size_t k = 0; k < i; ++k) base /= segments[k];
    if (fs::is_regular_file(base, ec)) return base.parent_path();
    fs::path as_html = base;
    as_html += ".html";
    if (fs::is_regular_file(as_html, ec)) return as_html.parent_path();
    if (fs::is_regular_file(base / "index.html", ec)) return base;
  }
  return root;
}

static bool Nx8vInsideRoot(const fs::path& file, const fs::path& root) {
  fs::path rel = file.lexically_relative(root);
  if (rel.empty()) return false;
  std::string s = rel.generic_string();
  return s != ".." && s.rfind("../", 0) != 0 && s.find("/../") == std::string::npos;
}

static size_t Nx8vMatchingClose(const std::string& html, size_t from,
                                const std::string& tag) {
  int depth = 1;
  size_t i = from;
  while (i < html.size()) {
    size_t lt = html.find('<', i);
    if (lt == std::string::npos) return std::string::npos;
    if (html.compare(lt, 4, "<!--") == 0) {
      size_t end = html.find("-->", lt + 4);
      i = end == std::string::npos ? html.size() : end + 3;
      continue;
    }
    size_t gt = Nx8vFindTagEnd(html, lt);
    if (gt == std::string::npos) return std::string::npos;
    std::string name = Nx8vTagName(html.substr(lt, gt - lt + 1));
    bool closing = lt + 1 < html.size() && html[lt + 1] == '/';
    bool self = gt > lt && html[gt - 1] == '/';
    if (name == tag) {
      if (closing) {
        if (--depth == 0) return gt + 1;
      } else if (!self) {
        ++depth;
      }
    }
    i = gt + 1;
  }
  return std::string::npos;
}

static std::string Nx8vExpandPartials(const std::string& html,
                                      const fs::path& page_dir,
                                      const fs::path& root,
                                      int depth,
                                      std::vector<std::string>& stack);

static std::string Nx8vLoadPartial(const std::string& spec,
                                   const fs::path& page_dir,
                                   const fs::path& root,
                                   int depth,
                                   std::vector<std::string>& stack) {
  if (depth > 8) return "<!-- nx8v: partial terlalu dalam -->";
  if (spec.empty() || spec.find(':') != std::string::npos)
    return "<!-- nx8v: partial tidak valid -->";

  std::string rel = spec;
  while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel.erase(0, 1);
  fs::path base = rel.empty() ? root : (spec[0] == '/' || spec[0] == '\\' ? root : page_dir);
  std::error_code ec;
  fs::path file = fs::absolute(base / rel, ec).lexically_normal();
  if (ec || !Nx8vInsideRoot(file, root))
    return "<!-- nx8v: partial di luar folder public -->";

  std::string key = file.generic_string();
  for (const auto& seen : stack) {
    if (seen == key) return "<!-- nx8v: partial berulang " + spec + " -->";
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) return "<!-- nx8v: partial tidak ditemukan: " + spec + " -->";
  std::string data((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  if (data.size() >= 3 && (unsigned char)data[0] == 0xEF &&
      (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
    data.erase(0, 3);

  stack.push_back(key);
  std::string expanded = Nx8vExpandPartials(data, file.parent_path(), root, depth + 1, stack);
  stack.pop_back();
  return expanded;
}

static std::string Nx8vExpandPartials(const std::string& html,
                                      const fs::path& page_dir,
                                      const fs::path& root,
                                      int depth,
                                      std::vector<std::string>& stack) {
  if (html.find("partial") == std::string::npos) return html;
  std::string out = html;
  size_t pos = 0;
  while (pos < out.size()) {
    size_t lt = out.find('<', pos);
    if (lt == std::string::npos) break;
    if (out.compare(lt, 4, "<!--") == 0) {
      size_t end = out.find("-->", lt + 4);
      pos = end == std::string::npos ? out.size() : end + 3;
      continue;
    }
    if (lt + 1 < out.size() && (out[lt + 1] == '/' || out[lt + 1] == '!' || out[lt + 1] == '?')) {
      pos = lt + 1;
      continue;
    }
    size_t gt = Nx8vFindTagEnd(out, lt);
    if (gt == std::string::npos) break;
    std::string open = out.substr(lt, gt - lt + 1);
    std::string name = Nx8vTagName(open);
    if (name == "script" || name == "style") {
      std::string close = "</" + name;
      size_t end = Nx8vLowerCopy(out.substr(gt)).find(close);
      pos = end == std::string::npos ? out.size() : gt + end + close.size();
      continue;
    }
    size_t attr_b = 0, attr_e = 0;
    std::string spec;
    if (!Nx8vPartialAttr(open, attr_b, attr_e, spec)) {
      pos = gt + 1;
      continue;
    }
    while (attr_b > 0 && std::isspace((unsigned char)open[attr_b - 1])) --attr_b;
    open.erase(attr_b, attr_e - attr_b);
    if (!open.empty() && open.back() == '>') {
      // <header />  ->  <header>
      if (open.size() >= 2 && open[open.size() - 2] == '/')
        open.erase(open.size() - 2, 1);
    }

    size_t close_at = Nx8vMatchingClose(out, gt + 1, name);
    std::string inner = Nx8vLoadPartial(spec, page_dir, root, depth, stack);
    std::string built = open + inner;
    size_t erase_end = gt + 1;
    if (close_at != std::string::npos) {
      // Keep the closing tag that belonged to the host element.
      size_t close_lt = out.rfind('<', close_at - 1);
      built += out.substr(close_lt, close_at - close_lt);
      erase_end = close_at;
    } else {
      built += "</" + name + ">";
    }
    out.replace(lt, erase_end - lt, built);
    pos = lt + built.size();
  }
  return out;
}

static std::string Nx8vExpandPartials(const std::string& html, const std::string& url) {
  if (html.find("partial") == std::string::npos) return html;
  fs::path root = Nx8vPublicRoot();
  fs::path dir = Nx8vPageDir(url, root);
  std::vector<std::string> stack;
  return Nx8vExpandPartials(html, dir, root, 0, stack);
}

}  // namespace bindings
}  // namespace nx8v
