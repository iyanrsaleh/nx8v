#pragma once
// nx8v/src/bindings/url.h
// URL parsing without external dependencies.
//
//   require('url').parse(str)        -> legacy object (protocol, host, port, ...)
//   require('url').format(obj)       -> string
//   require('url').resolve(from, to) -> string
//   require('url').URL               -> constructor (WHATWG-ish, minimal)
//
// The WHATWG URL implementation is intentionally small: it handles the common
// hierarchy  scheme://[user:pass@]host[:port]/path?query#hash  and several
// non-special schemes. It is not a full spec-compliant parser.

#include <v8.h>
#include <string>
#include <cctype>
#include <algorithm>

namespace nx8v {
namespace bindings {

namespace urldetail {

struct Parts {
  std::string scheme;
  std::string username;
  std::string password;
  std::string host;
  std::string port;
  std::string path;
  std::string query;   // without leading '?'
  std::string hash;    // without leading '#'
  bool        has_authority = false;
};

inline std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

inline bool IsSpecialScheme(const std::string& s) {
  return s == "http" || s == "https" || s == "ws" || s == "wss" ||
         s == "ftp" || s == "file";
}

inline std::string DefaultPort(const std::string& scheme) {
  if (scheme == "http"  || scheme == "ws")  return "80";
  if (scheme == "https" || scheme == "wss") return "443";
  if (scheme == "ftp")  return "21";
  return "";
}

// Split a "user:pass@host:port" authority component.
inline void SplitAuthority(const std::string& authority, Parts& p) {
  std::string auth = authority;
  auto at = auth.rfind('@');
  if (at != std::string::npos) {
    std::string userinfo = auth.substr(0, at);
    auth = auth.substr(at + 1);
    auto colon = userinfo.find(':');
    if (colon != std::string::npos) {
      p.username = userinfo.substr(0, colon);
      p.password = userinfo.substr(colon + 1);
    } else {
      p.username = userinfo;
    }
  }
  // IPv6 literal
  if (!auth.empty() && auth[0] == '[') {
    auto close = auth.find(']');
    if (close != std::string::npos) {
      p.host = auth.substr(0, close + 1);
      if (close + 1 < auth.size() && auth[close + 1] == ':')
        p.port = auth.substr(close + 2);
      return;
    }
  }
  auto colon = auth.rfind(':');
  if (colon != std::string::npos) {
    p.host = auth.substr(0, colon);
    p.port = auth.substr(colon + 1);
  } else {
    p.host = auth;
  }
}

inline Parts Parse(const std::string& input) {
  Parts p;
  std::string s = input;

  // scheme:
  auto colon = s.find(':');
  if (colon != std::string::npos) {
    std::string scheme = s.substr(0, colon);
    bool valid = !scheme.empty() &&
                 std::isalpha((unsigned char)scheme[0]);
    for (char c : scheme)
      if (!std::isalnum((unsigned char)c) && c != '+' && c != '-' && c != '.')
        valid = false;
    if (valid) {
      p.scheme = ToLower(scheme);
      s = s.substr(colon + 1);
    }
  }

  if (s.rfind("//", 0) == 0) {
    p.has_authority = true;
    s = s.substr(2);
    // authority ends at the first '/', '?' or '#'
    size_t end = s.find_first_of("/?#");
    std::string authority = (end == std::string::npos) ? s : s.substr(0, end);
    s = (end == std::string::npos) ? "" : s.substr(end);
    SplitAuthority(authority, p);
  }

  // fragment
  auto hash = s.find('#');
  if (hash != std::string::npos) {
    p.hash = s.substr(hash + 1);
    s = s.substr(0, hash);
  }
  // query
  auto q = s.find('?');
  if (q != std::string::npos) {
    p.query = s.substr(q + 1);
    s = s.substr(0, q);
  }
  p.path = s;
  if (p.path.empty() && p.has_authority) p.path = "/";
  return p;
}

inline std::string Format(const Parts& p) {
  std::string out;
  if (!p.scheme.empty()) out += p.scheme + ":";
  if (p.has_authority) {
    out += "//";
    if (!p.username.empty() || !p.password.empty()) {
      out += p.username;
      if (!p.password.empty()) out += ":" + p.password;
      out += "@";
    }
    out += p.host;
    if (!p.port.empty() && p.port != DefaultPort(p.scheme)) out += ":" + p.port;
  }
  out += p.path;
  if (!p.query.empty()) out += "?" + p.query;
  if (!p.hash.empty())  out += "#" + p.hash;
  return out;
}

inline v8::Local<v8::String> V8Str(v8::Isolate* iso, const std::string& s) {
  return v8::String::NewFromUtf8(iso, s.c_str(), v8::NewStringType::kNormal,
                                 (int)s.size()).ToLocalChecked();
}

inline v8::Local<v8::Object> PartsToObject(v8::Isolate* iso,
                                           v8::Local<v8::Context> ctx,
                                           const Parts& p,
                                           bool legacy,
                                           bool parse_query_string = false) {
  auto o = v8::Object::New(iso);
  auto set = [&](const char* k, const std::string& v) {
    o->Set(ctx, V8Str(iso, k), V8Str(iso, v)).Check();
  };

  std::string host_with_port = p.host;
  if (!p.port.empty() && p.port != DefaultPort(p.scheme))
    host_with_port += ":" + p.port;

  set("protocol", p.scheme.empty() ? "" : p.scheme + ":");
  set("username", p.username);
  set("password", p.password);
  set("hostname", p.host);
  set("host",     p.has_authority ? host_with_port : "");
  set("port",     p.port);
  set("pathname", p.path);
  set("search",   p.query.empty() ? "" : "?" + p.query);
  set("hash",     p.hash.empty()  ? "" : "#" + p.hash);
  set("href",     Format(p));

  if (legacy) {
    set("path", (p.path.empty() ? "" : p.path) +
                (p.query.empty() ? "" : "?" + p.query));
    set("auth", p.username.empty() && p.password.empty()
                    ? ""
                    : p.username + ":" + p.password);
    set("origin", p.has_authority
                      ? p.scheme + "://" + host_with_port
                      : "null");
    // query: string by default, or an object when parseQueryString is true
    if (parse_query_string) {
      auto q = v8::Object::New(iso);
      std::string qs = p.query;
      size_t start = 0;
      while (start <= qs.size()) {
        size_t amp = qs.find('&', start);
        std::string pair = qs.substr(start, amp == std::string::npos
                                              ? std::string::npos
                                              : amp - start);
        if (!pair.empty()) {
          auto eq = pair.find('=');
          std::string key = (eq == std::string::npos) ? pair
                                                      : pair.substr(0, eq);
          std::string val = (eq == std::string::npos) ? ""
                                                      : pair.substr(eq + 1);
          q->Set(ctx, V8Str(iso, key), V8Str(iso, val)).Check();
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
      }
      o->Set(ctx, V8Str(iso, "query"), q).Check();
    } else {
      set("query", p.query);
    }
  }
  return o;
}

} // namespace urldetail

// ---------------------------------------------------------------------------
// require('url')
// ---------------------------------------------------------------------------
inline v8::Local<v8::Object> MakeUrlModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);

  auto parse_fn = v8::Function::New(
      ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* iso = args.GetIsolate();
        v8::HandleScope h(iso);
        auto c = iso->GetCurrentContext();
        bool legacy = false;
        bool parse_qs = false;
        if (args.Length() >= 2) {
          if (args[1]->IsBoolean()) {
            legacy = args[1].As<v8::Boolean>()->Value();
            parse_qs = legacy;
          } else if (args[1]->IsObject()) {
            legacy = true;  // legacy url.parse with an options object
            auto opts = args[1].As<v8::Object>();
            auto v = opts->Get(c, v8::String::NewFromUtf8Literal(iso, "parseQueryString"))
                         .ToLocalChecked();
            parse_qs = v->IsBoolean() && v.As<v8::Boolean>()->Value();
          }
        }
        std::string input;
        if (args.Length() >= 1 && args[0]->IsString()) {
          v8::String::Utf8Value s(iso, args[0]);
          input = *s ? *s : "";
        }
        auto parts = urldetail::Parse(input);
        args.GetReturnValue().Set(
            urldetail::PartsToObject(iso, c, parts, legacy, parse_qs));
      }).ToLocalChecked();

  auto format_fn = v8::Function::New(
      ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* iso = args.GetIsolate();
        v8::HandleScope h(iso);
        auto c = iso->GetCurrentContext();
        if (args.Length() < 1 || !args[0]->IsObject()) {
          args.GetReturnValue().SetEmptyString();
          return;
        }
        auto o = args[0].As<v8::Object>();
        auto get = [&](const char* k) -> std::string {
          auto v = o->Get(c, urldetail::V8Str(iso, k))
                       .ToLocalChecked();
          if (!v->IsString()) return "";
          v8::String::Utf8Value s(iso, v);
          return *s ? *s : "";
        };
        urldetail::Parts p;
        std::string protocol = get("protocol");
        if (protocol.size() > 1 && protocol.back() == ':') protocol.pop_back();
        p.scheme        = protocol;
        p.host          = get("hostname");
        p.port          = get("port");
        p.path          = get("pathname");
        std::string search = get("search");
        if (!search.empty() && search[0] == '?') search = search.substr(1);
        p.query         = search;
        std::string hash = get("hash");
        if (!hash.empty() && hash[0] == '#') hash = hash.substr(1);
        p.hash          = hash;
        p.has_authority = !p.host.empty() || !p.scheme.empty();
        // legacy "host" may carry an embedded port
        if (p.port.empty()) {
          std::string host = get("host");
          auto colon = host.rfind(':');
          if (colon != std::string::npos) {
            p.host = host.substr(0, colon);
            p.port = host.substr(colon + 1);
          }
        }
        std::string full = get("href");
        args.GetReturnValue().Set(
            urldetail::V8Str(iso, full.empty() ? urldetail::Format(p) : full));
      }).ToLocalChecked();

  auto resolve_fn = v8::Function::New(
      ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* iso = args.GetIsolate();
        v8::HandleScope h(iso);
        std::string from = args.Length() >= 1 ? std::string(*v8::String::Utf8Value(iso, args[0])) : "";
        std::string to   = args.Length() >= 2 ? std::string(*v8::String::Utf8Value(iso, args[1])) : "";
        auto base = urldetail::Parse(from);
        auto ref  = urldetail::Parse(to);

        urldetail::Parts out;
        if (!ref.scheme.empty()) {
          out = ref;
        } else {
          out = base;
          out.query = ref.query;
          out.hash  = ref.hash;
          if (ref.has_authority) {
            out.has_authority = true;
            out.username = ref.username;
            out.password = ref.password;
            out.host     = ref.host;
            out.port     = ref.port;
            out.path     = ref.path;
          } else if (!ref.path.empty()) {
            if (ref.path[0] == '/') {
              out.path = ref.path;
            } else {
              std::string dir = base.path;
              auto slash = dir.rfind('/');
              dir = (slash == std::string::npos) ? "/" : dir.substr(0, slash + 1);
              out.path = dir + ref.path;
            }
          } else {
            if (!ref.query.empty()) out.path = base.path;
            out.query = ref.query.empty() ? base.query : ref.query;
          }
        }
        args.GetReturnValue().Set(urldetail::V8Str(iso, urldetail::Format(out)));
      }).ToLocalChecked();

  auto url_ctor = v8::Function::New(
      ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* iso = args.GetIsolate();
        v8::HandleScope h(iso);
        auto c = iso->GetCurrentContext();
        std::string input;
        if (args.Length() >= 1 && args[0]->IsString()) {
          v8::String::Utf8Value s(iso, args[0]);
          input = *s ? *s : "";
        }
        auto parts = urldetail::Parse(input);
        if (args.IsConstructCall()) {
          auto self = args.This();
          auto data = urldetail::PartsToObject(iso, c, parts, false);
          auto props = data->GetPropertyNames(c).ToLocalChecked();
          for (int i = 0; i < (int)props->Length(); ++i) {
            auto k = props->Get(c, i).ToLocalChecked();
            self->Set(c, k, data->Get(c, k).ToLocalChecked()).Check();
          }
          args.GetReturnValue().Set(self);
        } else {
          args.GetReturnValue().Set(urldetail::PartsToObject(iso, c, parts, false));
        }
      }).ToLocalChecked();

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "parse"),   parse_fn).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "format"),  format_fn).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "resolve"), resolve_fn).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "URL"),     url_ctor).Check();

  return hs.Escape(obj);
}

} // namespace bindings
} // namespace nx8v
