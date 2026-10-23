#pragma once
// nx8v/src/bindings/path.h
// Node.js-compatible `path` module (win32 flavour on Windows, posix elsewhere).
//
//   path.join(...parts)
//   path.resolve(...parts)
//   path.normalize(p)
//   path.dirname(p) / basename(p[, ext]) / extname(p)
//   path.isAbsolute(p)
//   path.relative(from, to)
//   path.parse(p) -> { root, dir, base, ext, name }
//   path.format({ root, dir, base, ext, name })
//   path.sep / path.delimiter / path.toNamespacedPath(p)

#include <v8.h>

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace nx8v {
namespace bindings {

#ifdef _WIN32
static constexpr char kPathSep = '\\';
static constexpr char kPathDelim = ';';
#else
static constexpr char kPathSep = '/';
static constexpr char kPathDelim = ':';
#endif

// Node's win32 flavour always reports backslashes; normalise output.
inline std::string PathToNative(std::string p) {
#ifdef _WIN32
  for (auto& c : p) if (c == '/') c = '\\';
#endif
  return p;
}

inline bool PathIsSep(char c) {
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

inline bool PathIsAbsolute(const std::string& p) {
  if (p.empty()) return false;
#ifdef _WIN32
  if (p.size() >= 2 && PathIsSep(p[0]) && PathIsSep(p[1])) return true;  // UNC
  if (p.size() >= 3 && std::isalpha((unsigned char)p[0]) && p[1] == ':' &&
      PathIsSep(p[2]))
    return true;
  return false;
#else
  return p[0] == '/';
#endif
}

// Root of a path: "C:\" / "\" / "/" / "" — empty when relative.
inline std::string PathRoot(const std::string& p) {
  if (p.empty()) return "";
#ifdef _WIN32
  if (p.size() >= 2 && PathIsSep(p[0]) && PathIsSep(p[1])) {
    // UNC: \\server\share
    size_t i = 2;
    int seen = 0;
    while (i < p.size()) {
      if (PathIsSep(p[i])) {
        if (++seen == 2) return p.substr(0, i + 1);
      }
      ++i;
    }
    return p;
  }
  if (p.size() >= 2 && std::isalpha((unsigned char)p[0]) && p[1] == ':') {
    if (p.size() >= 3 && PathIsSep(p[2])) return PathToNative(p.substr(0, 3));
    return p.substr(0, 2);
  }
  if (PathIsSep(p[0])) return PathToNative(p.substr(0, 1));
  return "";
#else
  if (p[0] == '/') return "/";
  return "";
#endif
}

inline std::string PathNormalize(const std::string& input) {
  if (input.empty()) return ".";

  std::string root = PathRoot(input);
  size_t i = root.size();

  std::vector<std::string> parts;
  std::string cur;
  for (; i <= input.size(); ++i) {
    if (i == input.size() || PathIsSep(input[i])) {
      if (cur == "..") {
        if (!parts.empty() && parts.back() != "..") parts.pop_back();
        else if (root.empty()) parts.push_back("..");
      } else if (!cur.empty() && cur != ".") {
        parts.push_back(cur);
      }
      cur.clear();
    } else {
      cur += input[i];
    }
  }

  std::string out = root;
  for (size_t k = 0; k < parts.size(); ++k) {
    if (!out.empty() && !PathIsSep(out.back())) out += kPathSep;
    else if (out.empty() && k > 0) out += kPathSep;
    out += parts[k];
  }
  if (out.empty()) return root.empty() ? "." : root;
  return PathToNative(out);
}

inline std::string PathJoin(const std::vector<std::string>& parts) {
  std::string joined;
  bool any = false;
  for (const auto& p : parts) {
    if (p.empty()) continue;
    any = true;
    if (!joined.empty() && !PathIsSep(joined.back()) && !PathIsSep(p.front()))
      joined += kPathSep;
    else if (!joined.empty() && PathIsSep(joined.back()) && PathIsSep(p.front()))
      joined.pop_back();
    joined += p;
  }
  if (!any) return ".";
  return PathNormalize(joined);
}

inline std::string PathDirname(const std::string& p) {
  if (p.empty()) return ".";
  std::string root = PathRoot(p);
  // Strip trailing separators (but keep the root itself).
  size_t end = p.size();
  while (end > root.size() && PathIsSep(p[end - 1])) --end;
  if (end <= root.size()) return root.empty() ? "." : root;

  size_t cut = end;
  while (cut > root.size() && !PathIsSep(p[cut - 1])) --cut;
  if (cut <= root.size()) return root.empty() ? "." : root;
  while (cut > root.size() && PathIsSep(p[cut - 1])) --cut;
  if (cut == 0) return ".";
  return PathToNative(p.substr(0, cut));
}

inline std::string PathBasename(const std::string& p, const std::string& ext) {
  if (p.empty()) return "";
  std::string root = PathRoot(p);
  size_t end = p.size();
  while (end > root.size() && PathIsSep(p[end - 1])) --end;
  if (end <= root.size()) return "";
  size_t start = end;
  while (start > 0 && !PathIsSep(p[start - 1])) --start;
  std::string base = p.substr(start, end - start);

  if (!ext.empty() && base.size() > ext.size() &&
      base.compare(base.size() - ext.size(), ext.size(), ext) == 0) {
    base.resize(base.size() - ext.size());
  }
  return base;
}

inline std::string PathExtname(const std::string& p) {
  std::string base = PathBasename(p, "");
  if (base.empty()) return "";
  size_t dot = base.find_last_of('.');
  if (dot == std::string::npos || dot == 0) return "";
  return base.substr(dot);
}

// Split an absolute-or-relative path into root + segments.
inline std::vector<std::string> PathSegments(const std::string& p,
                                             std::string& root) {
  root = PathRoot(p);
  std::vector<std::string> segs;
  std::string cur;
  for (size_t i = root.size(); i <= p.size(); ++i) {
    if (i == p.size() || PathIsSep(p[i])) {
      if (!cur.empty() && cur != ".") segs.push_back(cur);
      cur.clear();
    } else {
      cur += p[i];
    }
  }
  return segs;
}

inline std::string PathResolve(const std::vector<std::string>& parts,
                               const std::string& cwd) {
  std::string resolved;
  bool found_absolute = false;

  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    if (it->empty()) continue;
    resolved = *it + (resolved.empty() ? "" : std::string(1, kPathSep) + resolved);
    if (PathIsAbsolute(resolved)) {
      found_absolute = true;
      break;
    }
  }
  if (!found_absolute) {
    if (!resolved.empty())
      resolved = cwd + kPathSep + resolved;
    else
      resolved = cwd;
  }
  return PathNormalize(resolved);
}

inline std::string PathRelative(const std::string& from, const std::string& to) {
  std::string r1, r2;
  auto a = PathSegments(PathNormalize(from), r1);
  auto b = PathSegments(PathNormalize(to), r2);

  if (r1 != r2) return PathNormalize(to);  // different roots → absolute

  size_t common = 0;
  while (common < a.size() && common < b.size() && a[common] == b[common])
    ++common;

  std::string out;
  for (size_t i = common; i < a.size(); ++i) {
    if (!out.empty()) out += kPathSep;
    out += "..";
  }
  for (size_t i = common; i < b.size(); ++i) {
    if (!out.empty()) out += kPathSep;
    out += b[i];
  }
  return out.empty() ? "" : out;
}

// ---------------------------------------------------------------------------
// JS-facing
// ---------------------------------------------------------------------------
inline std::string PathArg(v8::Isolate* isolate, v8::Local<v8::Value> v) {
  v8::String::Utf8Value s(isolate, v);
  return *s ? std::string(*s, s.length()) : std::string();
}

inline void PathSet(v8::Local<v8::Context> ctx, v8::Local<v8::Object> obj,
                    const char* name, const std::string& value,
                    v8::Isolate* isolate) {
  obj->Set(ctx,
           v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal,
                                   (int)std::strlen(name)).ToLocalChecked(),
           v8::String::NewFromUtf8(isolate, value.c_str(),
                                   v8::NewStringType::kNormal,
                                   (int)value.size()).ToLocalChecked()).Check();
}

inline v8::Local<v8::Object> MakePathModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    obj->Set(ctx,
             v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal,
                                     (int)std::strlen(name)).ToLocalChecked(),
             v8::Function::New(ctx, cb).ToLocalChecked()).Check();
  };

  def("join", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    std::vector<std::string> parts;
    for (int i = 0; i < args.Length(); ++i) parts.push_back(PathArg(iso, args[i]));
    std::string out = PathJoin(parts);
    args.GetReturnValue().Set(
        v8::String::NewFromUtf8(iso, out.c_str(), v8::NewStringType::kNormal,
                                (int)out.size()).ToLocalChecked());
  });

  def("normalize", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    std::string out = PathNormalize(args.Length() >= 1 ? PathArg(iso, args[0]) : "");
    args.GetReturnValue().Set(
        v8::String::NewFromUtf8(iso, out.c_str(), v8::NewStringType::kNormal,
                                (int)out.size()).ToLocalChecked());
  });

  def("resolve", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    std::vector<std::string> parts;
    for (int i = 0; i < args.Length(); ++i) parts.push_back(PathArg(iso, args[i]));

    std::string cwd;
    std::error_code ec;
    cwd = std::filesystem::current_path(ec).string();
    if (cwd.empty() && nx8v::g_runtime) cwd = nx8v::g_runtime->script_dir;
    std::string out = PathResolve(parts, cwd);
    args.GetReturnValue().Set(
        v8::String::NewFromUtf8(iso, out.c_str(), v8::NewStringType::kNormal,
                                (int)out.size()).ToLocalChecked());
  });

  def("dirname", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    std::string out = PathDirname(args.Length() >= 1 ? PathArg(iso, args[0]) : ".");
    args.GetReturnValue().Set(
        v8::String::NewFromUtf8(iso, out.c_str(), v8::NewStringType::kNormal,
                                (int)out.size()).ToLocalChecked());
  });

  def("basename", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    std::string ext;
    if (args.Length() >= 2) ext = PathArg(iso, args[1]);
    std::string out = PathBasename(args.Length() >= 1 ? PathArg(iso, args[0]) : "", ext);
    args.GetReturnValue().Set(
        v8::String::NewFromUtf8(iso, out.c_str(), v8::NewStringType::kNormal,
                                (int)out.size()).ToLocalChecked());
  });

  def("extname", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    std::string out = PathExtname(args.Length() >= 1 ? PathArg(iso, args[0]) : "");
    args.GetReturnValue().Set(
        v8::String::NewFromUtf8(iso, out.c_str(), v8::NewStringType::kNormal,
                                (int)out.size()).ToLocalChecked());
  });

  def("isAbsolute", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    bool abs = args.Length() >= 1 && PathIsAbsolute(PathArg(iso, args[0]));
    args.GetReturnValue().Set(v8::Boolean::New(iso, abs));
  });

  def("relative", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    std::string from = args.Length() >= 1 ? PathArg(iso, args[0]) : "";
    std::string to   = args.Length() >= 2 ? PathArg(iso, args[1]) : "";
    std::string out = PathRelative(from, to);
    args.GetReturnValue().Set(
        v8::String::NewFromUtf8(iso, out.c_str(), v8::NewStringType::kNormal,
                                (int)out.size()).ToLocalChecked());
  });

  def("parse", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    v8::Local<v8::Context> c = iso->GetCurrentContext();

    std::string p = args.Length() >= 1 ? PathArg(iso, args[0]) : "";
    std::string root = PathRoot(p);
    std::string dir  = PathDirname(p);
    std::string base = PathBasename(p, "");
    std::string ext  = PathExtname(p);
    std::string name = (ext.empty() || base.size() <= ext.size())
                           ? base
                           : base.substr(0, base.size() - ext.size());

    auto o = v8::Object::New(iso);
    PathSet(c, o, "root", root, iso);
    PathSet(c, o, "dir", dir == "." && root.empty() ? "" : dir, iso);
    PathSet(c, o, "base", base, iso);
    PathSet(c, o, "ext", ext, iso);
    PathSet(c, o, "name", name, iso);
    args.GetReturnValue().Set(o);
  });

  def("format", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    v8::Local<v8::Context> c = iso->GetCurrentContext();

    std::string out;
    if (args.Length() >= 1 && args[0]->IsObject()) {
      auto o = args[0].As<v8::Object>();
      auto get = [&](const char* k) -> std::string {
        auto v = o->Get(c, v8::String::NewFromUtf8(iso, k, v8::NewStringType::kNormal,
                                                   (int)std::strlen(k)).ToLocalChecked())
                     .ToLocalChecked();
        return v->IsString() ? PathArg(iso, v) : std::string();
      };
      std::string dir  = get("dir");
      std::string root = get("root");
      std::string base = get("base");
      if (base.empty()) {
        std::string name = get("name");
        std::string ext  = get("ext");
        base = name + ext;
      }
      std::string prefix = dir.empty() ? root : dir;
      if (!prefix.empty() && !base.empty() && !PathIsSep(prefix.back()))
        out = prefix + kPathSep + base;
      else
        out = prefix + base;
    }
    args.GetReturnValue().Set(
        v8::String::NewFromUtf8(iso, PathToNative(out).c_str(),
                                v8::NewStringType::kNormal,
                                (int)out.size()).ToLocalChecked());
  });

  def("toNamespacedPath", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    std::string p = args.Length() >= 1 ? PathArg(iso, args[0]) : "";
    args.GetReturnValue().Set(
        v8::String::NewFromUtf8(iso, p.c_str(), v8::NewStringType::kNormal,
                                (int)p.size()).ToLocalChecked());
  });

  PathSet(ctx, obj, "sep", std::string(1, kPathSep), isolate);
  PathSet(ctx, obj, "delimiter", std::string(1, kPathDelim), isolate);

  // path.posix / path.win32 are not provided; path itself matches the host.
  return hs.Escape(obj);
}

}} // nx8v::bindings
