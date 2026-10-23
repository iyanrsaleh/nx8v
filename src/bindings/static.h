#pragma once
// nx8v/src/bindings/static.h
// Production-ish static file handler.
//
//   const serve = require('static');
//   const handler = serve('public', { maxAge: 3600, index: 'index.html' });
//
//   http.createServer(handler).listen(3000);          // langsung
//   app.use(handler);                                  // atau lewat router
//
// Fitur:
//   - Content-Type berdasarkan ekstensi
//   - ETag (size + mtime) + If-None-Match -> 304
//   - Last-Modified + If-Modified-Since -> 304
//   - Range: bytes=... -> 206 Partial Content
//   - HEAD (tanpa body)
//   - Cache-Control: max-age (default 0 = no-cache)
//   - Direktori -> index.html
//   - Path traversal ("..") ditolak -> 403

#include <v8.h>

#include "buffer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "compress.h"

namespace fs = std::filesystem;

namespace nx8v {
namespace bindings {

// ---------------------------------------------------------------------------
// MIME types
// ---------------------------------------------------------------------------
inline std::string StaticMime(const std::string& ext_in) {
  std::string ext = ext_in;
  for (auto& c : ext) c = (char)std::tolower((unsigned char)c);

  static const std::map<std::string, const char*> kMap = {
    {".html", "text/html; charset=utf-8"},
    {".htm",  "text/html; charset=utf-8"},
    {".js",   "text/javascript; charset=utf-8"},
    {".mjs",  "text/javascript; charset=utf-8"},
    {".css",  "text/css; charset=utf-8"},
    {".json", "application/json; charset=utf-8"},
    {".txt",  "text/plain; charset=utf-8"},
    {".md",   "text/markdown; charset=utf-8"},
    {".xml",  "application/xml; charset=utf-8"},
    {".svg",  "image/svg+xml"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".webp", "image/webp"},
    {".ico",  "image/x-icon"},
    {".woff", "font/woff"},
    {".woff2","font/woff2"},
    {".ttf",  "font/ttf"},
    {".pdf",  "application/pdf"},
    {".wasm", "application/wasm"},
    {".mp4",  "video/mp4"},
    {".webm", "video/webm"},
    {".mp3",  "audio/mpeg"},
  };
  auto it = kMap.find(ext);
  return it == kMap.end() ? "application/octet-stream" : it->second;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
inline std::string StaticHttpDate(std::time_t t) {
  char buf[64];
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
  return buf;
}

// Weak-free ETag from size + mtime.
inline std::string StaticEtag(const fs::path& p, uintmax_t size,
                              std::time_t mtime) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "\"%llx-%llx\"",
                (unsigned long long)size, (unsigned long long)mtime);
  return buf;
}

inline std::string StaticLower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// Read a header value from the JS req.headers object.
inline std::string StaticGetHeader(v8::Isolate* isolate,
                                   v8::Local<v8::Context> ctx,
                                   v8::Local<v8::Object> headers,
                                   const std::string& name) {
  if (headers.IsEmpty()) return "";
  auto names = headers->GetPropertyNames(ctx).ToLocalChecked();
  std::string want = StaticLower(name);
  for (uint32_t i = 0; i < names->Length(); ++i) {
    auto k = names->Get(ctx, i).ToLocalChecked();
    v8::String::Utf8Value ks(isolate, k);
    if (!*ks) continue;
    if (StaticLower(std::string(*ks, ks.length())) != want) continue;
    auto v = headers->Get(ctx, k).ToLocalChecked();
    v8::String::Utf8Value vs(isolate, v);
    return *vs ? std::string(*vs, vs.length()) : std::string();
  }
  return "";
}

// Call res.writeHead(status, headersObject)
inline void StaticWriteHead(v8::Isolate* isolate, v8::Local<v8::Context> ctx,
                            v8::Local<v8::Object> res, int status,
                            const std::vector<std::pair<std::string, std::string>>& hdrs) {
  auto hdr_obj = v8::Object::New(isolate);
  for (auto& kv : hdrs) {
    hdr_obj->Set(ctx,
        v8::String::NewFromUtf8(isolate, kv.first.c_str(),
                                v8::NewStringType::kNormal).ToLocalChecked(),
        v8::String::NewFromUtf8(isolate, kv.second.c_str(),
                                v8::NewStringType::kNormal).ToLocalChecked()).Check();
  }

  auto wh = res->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "writeHead"))
                .ToLocalChecked();
  if (wh->IsFunction()) {
    v8::Local<v8::Value> argv[] = { v8::Integer::New(isolate, status), hdr_obj };
    v8::Local<v8::Value> ignored;
    (void)wh.As<v8::Function>()->Call(ctx, res, 2, argv).ToLocal(&ignored);
  }
}

inline void StaticEnd(v8::Isolate* isolate, v8::Local<v8::Context> ctx,
                      v8::Local<v8::Object> res, const std::string& body) {
  auto end_v = res->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "end"))
                   .ToLocalChecked();
  if (!end_v->IsFunction()) return;
  v8::Local<v8::Value> argv[] = {
    MakeBufferFromBytes(isolate, reinterpret_cast<const uint8_t*>(body.data()),
                        body.size()) };
  v8::Local<v8::Value> ignored;
  (void)end_v.As<v8::Function>()->Call(ctx, res, 1, argv).ToLocal(&ignored);
}

// ---------------------------------------------------------------------------
// Options carried by the handler function (as external data)
// ---------------------------------------------------------------------------
struct StaticOptions {
  std::string root;
  std::string index   = "index.html";
  int         max_age = 0;
  bool        etag    = true;
  bool        ranges  = true;
  bool        gzip    = true;
  bool        listing = false;
};

// ---------------------------------------------------------------------------
// The handler itself
// ---------------------------------------------------------------------------
inline void StaticHandler(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto* opts = reinterpret_cast<StaticOptions*>(
      args.DataV2().As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));
  if (!opts || args.Length() < 2) return;

  auto req = args[0].As<v8::Object>();
  auto res = args[1].As<v8::Object>();

  // method + url
  auto method_v = req->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "method"))
                      .ToLocalChecked();
  auto url_v = req->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "url"))
                   .ToLocalChecked();
  v8::String::Utf8Value method_s(isolate, method_v);
  v8::String::Utf8Value url_s(isolate, url_v);
  std::string method = *method_s ? std::string(*method_s, method_s.length()) : "GET";
  std::string url = *url_s ? std::string(*url_s, url_s.length()) : "/";

  if (method != "GET" && method != "HEAD") {
    StaticWriteHead(isolate, ctx, res, 405,
                    {{"Content-Type", "text/plain; charset=utf-8"},
                     {"Allow", "GET, HEAD"}});
    StaticEnd(isolate, ctx, res, "405 Method Not Allowed");
    return;
  }

  // strip query/hash, decode %XX minimally
  auto cut = url.find_first_of("?#");
  if (cut != std::string::npos) url = url.substr(0, cut);

  std::string decoded;
  decoded.reserve(url.size());
  for (size_t i = 0; i < url.size(); ++i) {
    if (url[i] == '%' && i + 2 < url.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      int h = hex(url[i + 1]), l = hex(url[i + 2]);
      if (h >= 0 && l >= 0) {
        decoded += (char)((h << 4) | l);
        i += 2;
        continue;
      }
    }
    decoded += url[i];
  }
  if (decoded.empty() || decoded[0] != '/') decoded = "/" + decoded;

  // Reject traversal: any ".." segment.
  {
    std::string seg;
    for (size_t i = 0; i <= decoded.size(); ++i) {
      if (i == decoded.size() || decoded[i] == '/') {
        if (seg == "..") {
          StaticWriteHead(isolate, ctx, res, 403,
                          {{"Content-Type", "text/plain; charset=utf-8"}});
          StaticEnd(isolate, ctx, res, "403 Forbidden");
          return;
        }
        seg.clear();
      } else {
        seg += decoded[i];
      }
    }
  }

  std::error_code ec;
  fs::path file = fs::path(opts->root) / fs::path(decoded.substr(1));
  file = file.lexically_normal();

  if (fs::is_directory(file, ec)) {
    file /= opts->index;
  }

  if (!fs::is_regular_file(file, ec)) {
    StaticWriteHead(isolate, ctx, res, 404,
                    {{"Content-Type", "text/plain; charset=utf-8"}});
    StaticEnd(isolate, ctx, res, "404 Not Found");
    return;
  }

  uintmax_t size = fs::file_size(file, ec);
  if (ec) size = 0;

  auto ftime = fs::last_write_time(file, ec);
  std::time_t mtime = 0;
  if (!ec) {
    // Convert file_time_type -> system_clock (C++17 portable-ish).
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    mtime = std::chrono::system_clock::to_time_t(sctp);
  }

  std::string etag = StaticEtag(file, size, mtime);
  std::string mime = StaticMime(file.extension().string());
  std::string cache = opts->max_age > 0
                          ? "public, max-age=" + std::to_string(opts->max_age)
                          : "no-cache";

  // headers object of the request
  auto headers_v = req->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "headers"))
                       .ToLocalChecked();
  v8::Local<v8::Object> req_headers;
  if (headers_v->IsObject()) req_headers = headers_v.As<v8::Object>();

  std::vector<std::pair<std::string, std::string>> base_hdrs = {
    {"Content-Type", mime},
    {"Cache-Control", cache},
    {"Last-Modified", StaticHttpDate(mtime)},
    {"Accept-Ranges", "bytes"},
  };
  if (opts->etag) base_hdrs.push_back({"ETag", etag});

  // ---- conditional requests ----
  std::string inm = StaticGetHeader(isolate, ctx, req_headers, "if-none-match");
  bool not_modified = false;
  if (opts->etag && !inm.empty()) {
    if (inm == "*" || inm.find(etag) != std::string::npos) not_modified = true;
  }
  if (!not_modified) {
    std::string ims = StaticGetHeader(isolate, ctx, req_headers, "if-modified-since");
    if (!ims.empty() && ims == StaticHttpDate(mtime)) not_modified = true;
  }
  if (not_modified) {
    StaticWriteHead(isolate, ctx, res, 304, base_hdrs);
    StaticEnd(isolate, ctx, res, "");
    return;
  }

  // ---- range requests ----
  uintmax_t start = 0, end = size > 0 ? size - 1 : 0;
  bool partial = false;
  std::string range = StaticGetHeader(isolate, ctx, req_headers, "range");
  if (opts->ranges && !range.empty() && size > 0 &&
      range.rfind("bytes=", 0) == 0) {
    std::string spec = range.substr(6);
    auto dash = spec.find('-');
    if (dash != std::string::npos) {
      std::string a = spec.substr(0, dash);
      std::string b = spec.substr(dash + 1);
      if (a.empty()) {
        // suffix: bytes=-500
        uintmax_t n = (uintmax_t)std::strtoull(b.c_str(), nullptr, 10);
        if (n > 0 && n <= size) {
          start = size - n;
          end = size - 1;
          partial = true;
        }
      } else {
        start = (uintmax_t)std::strtoull(a.c_str(), nullptr, 10);
        if (!b.empty()) {
          end = (uintmax_t)std::strtoull(b.c_str(), nullptr, 10);
          if (end >= size) end = size - 1;
        } else {
          end = size - 1;
        }
        if (start <= end && start < size) partial = true;
      }
    }
    if (!partial) {
      StaticWriteHead(isolate, ctx, res, 416,
                      {{"Content-Type", "text/plain; charset=utf-8"},
                       {"Content-Range", "bytes */" + std::to_string(size)}});
      StaticEnd(isolate, ctx, res, "416 Range Not Satisfiable");
      return;
    }
  }

  // ---- read the body ----
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    StaticWriteHead(isolate, ctx, res, 500,
                    {{"Content-Type", "text/plain; charset=utf-8"}});
    StaticEnd(isolate, ctx, res, "500 Internal Server Error");
    return;
  }

  std::string body;
  if (size > 0) {
    in.seekg((std::streamoff)start, std::ios::beg);
    size_t want = (size_t)(end - start + 1);
    body.resize(want);
    in.read(body.data(), (std::streamsize)want);
    body.resize((size_t)in.gcount());
  }

  auto hdrs = base_hdrs;
  std::string accept = StaticLower(StaticGetHeader(isolate, ctx, req_headers, "accept-encoding"));
  if (opts->gzip && !partial && method != "HEAD" &&
      accept.find("gzip") != std::string::npos && body.size() >= 64 &&
      body.size() <= 4u * 1024u * 1024u) {
    std::string gz;
    if (Nx8vGzip(body, gz) && gz.size() + 16 < body.size()) {
      body.swap(gz);
      hdrs.push_back({"Content-Encoding", "gzip"});
      hdrs.push_back({"Vary", "Accept-Encoding"});
    }
  }
  hdrs.push_back({"Content-Length", std::to_string(body.size())});
  if (partial) {
    hdrs.push_back({"Content-Range",
                    "bytes " + std::to_string(start) + "-" +
                    std::to_string(end) + "/" + std::to_string(size)});
  }

  StaticWriteHead(isolate, ctx, res, partial ? 206 : 200, hdrs);

  if (method == "HEAD") {
    StaticEnd(isolate, ctx, res, "");
    return;
  }
  StaticEnd(isolate, ctx, res, body);
}

// ---------------------------------------------------------------------------
// serve(root[, options]) -> handler(req, res)
// ---------------------------------------------------------------------------
inline void StaticServe(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto* opts = new StaticOptions();
  if (args.Length() >= 1 && args[0]->IsString()) {
    v8::String::Utf8Value r(isolate, args[0]);
    opts->root = *r ? std::string(*r, r.length()) : ".";
  } else {
    opts->root = ".";
  }

  if (args.Length() >= 2 && args[1]->IsObject()) {
    auto o = args[1].As<v8::Object>();
    auto get = [&](const char* k) {
      return o->Get(ctx, v8::String::NewFromUtf8(isolate, k, v8::NewStringType::kNormal,
                                                 (int)std::strlen(k)).ToLocalChecked())
          .ToLocalChecked();
    };
    auto idx = get("index");
    if (idx->IsString()) {
      v8::String::Utf8Value s(isolate, idx);
      if (*s) opts->index = std::string(*s, s.length());
    }
    auto ma = get("maxAge");
    if (ma->IsNumber()) opts->max_age = ma->Int32Value(ctx).FromMaybe(0);
    auto et = get("etag");
    if (et->IsBoolean()) opts->etag = et.As<v8::Boolean>()->Value();
    auto rg = get("ranges");
    if (rg->IsBoolean()) opts->ranges = rg.As<v8::Boolean>()->Value();
    auto gz = get("gzip");
    if (gz->IsBoolean()) opts->gzip = gz.As<v8::Boolean>()->Value();
  }

  auto ext = v8::External::New(isolate, opts, v8::kExternalPointerTypeTagDefault);
  auto fn = v8::Function::New(ctx, StaticHandler, ext).ToLocalChecked();
  args.GetReturnValue().Set(fn);
}

// module.exports = serve, with serve.serve also available:
//   const serve = require('static');
//   serve('public');
inline v8::Local<v8::Value> MakeStaticModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto fn = v8::Function::New(ctx, StaticServe).ToLocalChecked();
  fn->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "serve"), fn).Check();
  return hs.Escape(fn.As<v8::Value>());
}

}} // nx8v::bindings
