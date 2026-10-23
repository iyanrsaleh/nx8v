#pragma once
// nx8v/src/bindings/http.h
// Minimal Node.js-compatible HTTP server built on libuv TCP.
//
// Exposed surface (subset of Node's http module):
//   http.createServer([requestListener]) -> Server
//     Server extends EventEmitter.
//     Server.listen(port[, host][, cb])   -> this
//     Server.close([cb])                 -> this
//     Server.on('request', fn)           -> fn(req, res)
//     Server.on('listening', cb)
//     Server.on('close', cb)
//     Server.on('connection', cb)        -> cb(socket)
//     Server.on('error', cb)
//
//   http.STATUS_CODES -> object
//   http.Server        -> Server constructor (for instanceof checks)
//
// Request object (EventEmitter-ish, simplified):
//   req.method, req.url, req.httpVersion
//   req.headers  -> object
//   req.on('data', fn)   / req.on('end', fn)
//
// Response object:
//   res.statusCode (default 200)
//   res.setHeader(name, value)
//   res.getHeader(name)
//   res.write(chunk)
//   res.end([chunk][, cb])   -> sends response + closes connection
//   res.writeHead(statusCode[, statusMessage][, headers])

#include <v8.h>
#include <uv.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../runtime.h"
#include "../utils.h"
#include "events.h"
#include "partials.h"
#include "crypto.h"
#include "tls.h"
#include "buffer.h"
#include "static.h"
#include "manifest.h"

namespace nx8v {
namespace bindings {

// Embedder data tag used for our native-pointer internal fields.
#define NX8V_FIELD_TAG v8::kEmbedderDataTypeTagDefault

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void OnConnection(uv_stream_t* server, int status);

// Per-server state.
struct HttpServer {
  uv_tcp_t                 handle;
  v8::Global<v8::Object>   js_server;     // the JS Server object
  std::atomic<int>         conn_count{0};
  bool                     closing   = false;
  v8::Global<v8::Function> request_listener;  // default request listener (if any)
  v8::Isolate*             isolate   = nullptr;
  // TLS material for https.createServer. tls_state is owned here.
  bool                     tls = false;
  std::string              tls_cert;
  std::string              tls_key;
  std::string              tls_pfx;
  std::string              tls_pass;
  void*                    tls_state = nullptr;
};

// Per-connection state.
struct HttpConn {
  uv_tcp_t                 handle;
  uv_write_t               write_req;
  uv_shutdown_t            shutdown_req;
  std::shared_ptr<uv_buf_t> write_buf;  // keep buffer alive during write
  std::string              read_buf;
  bool                     headers_sent = false;
  bool                     keep_alive   = false;  // HTTP/1.0 closes by default
  // JS-side objects (kept alive until connection closes)
  v8::Global<v8::Object>   req_obj;
  v8::Global<v8::Object>   res_obj;
  v8::Global<v8::Object>   socket_obj;
  // pending chunks accumulated on req 'data'
  std::string              body;
  bool                     request_emitted = false;
  std::string              request_url;
  // Long-poll for `nx8v dev`: the page waits on one request until a file changes.
  bool                     dev_waiting = false;
  bool                     dev_replying = false;

  // WebSocket state (set after a successful upgrade handshake).
  bool                     ws_mode = false;
  v8::Global<v8::Object>   ws_obj;
  // Built-in page terminal (/__nx8v_terminal). Not a JS websocket object.
  void*                    term = nullptr;
  bool                     term_wait = false;
  std::string              term_keys;

  // Owning server (back-pointer; handle.data points to this conn).
  HttpServer*              server = nullptr;

  // TLS + HTTP/2. sec is owned here and freed when the connection is deleted.
  bool                     use_tls = false;
  bool                     tls_ready = false;
  bool                     alpn_h2 = false;
  bool                     h2 = false;
  bool                     h2_replied = false;
  bool                     h2_settings_sent = false;
  int                      h2_stream = 0;
  std::string              h2_in;
  std::string              h2_block;
  std::string              h2_body;
  std::string              h2_method;
  std::string              h2_path;
  std::string              h2_authority;
  std::map<std::string, std::string> h2_headers;
  void*                    sec = nullptr;
  ~HttpConn();
};

static void Nx8vSecFree(void* sec);
static void Nx8vH2Forget(HttpConn* conn);
inline HttpConn::~HttpConn() {
  Nx8vH2Forget(this);
  Nx8vSecFree(sec);
  sec = nullptr;
}

static void Nx8vH2Reply(HttpConn* conn, int status,
                        const std::map<std::string, std::string>& headers,
                        const std::string& body);
static std::string Nx8vTlsSeal(HttpConn* conn, const std::string& plain);
static bool Nx8vTlsPrepare(HttpServer* srv, std::string* err);
static void Nx8vTlsFree(HttpServer* srv);
static bool Nx8vTlsConsume(HttpConn* conn, const char* data, size_t n);
static void Nx8vH2Feed(HttpConn* conn, const std::string& chunk);
static bool Nx8vTakeH2(HttpConn* conn);
static void HttpWriteRaw(HttpConn* conn, const std::string& data,
                         bool shutdown_after = false);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static HttpServer* HttpServerFromObject(v8::Local<v8::Object> obj) {
  auto ext = obj->GetAlignedPointerFromInternalField(0, NX8V_FIELD_TAG);
  return reinterpret_cast<HttpServer*>(ext);
}

static std::string HttpTrim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  size_t e = s.find_last_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  return s.substr(b, e - b + 1);
}

#ifndef NX8V_CALL_IGNORE_DEFINED
#define NX8V_CALL_IGNORE_DEFINED
// Call a JS function, discarding the result but satisfying warn_unused_result.
static inline void Nx8vCallIgnore(v8::Local<v8::Function> fn,
                                  v8::Local<v8::Context> ctx,
                                  v8::Local<v8::Value> recv,
                                  int argc,
                                  v8::Local<v8::Value>* argv) {
  v8::Local<v8::Value> ignored;
  (void)fn->Call(ctx, recv, argc, argv).ToLocal(&ignored);
}
#endif

// Parse a single raw HTTP request into (method, url, version, headers, body).
static bool HttpParseRequest(const std::string& raw,
                             std::string& method, std::string& url,
                             std::string& version,
                             std::map<std::string, std::string>& headers,
                             std::string& body) {
  size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos) header_end = raw.find("\n\n");
  if (header_end == std::string::npos) return false;

  std::string header_block = raw.substr(0, header_end);
  body = raw.substr(header_end + (raw[header_end] == '\r' ? 4 : 2));

  // Split header block into lines.
  std::vector<std::string> lines;
  size_t pos = 0;
  while (true) {
    size_t nl = header_block.find('\n', pos);
    std::string line = header_block.substr(pos, nl == std::string::npos ? nl : nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (nl == std::string::npos) { if (!line.empty()) lines.push_back(line); break; }
    pos = nl + 1;
    if (!line.empty()) lines.push_back(line);
  }
  if (lines.empty()) return false;

  // Request line: METHOD URL VERSION
  size_t sp1 = lines[0].find(' ');
  size_t sp2 = lines[0].find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
  method  = lines[0].substr(0, sp1);
  url     = lines[0].substr(sp1 + 1, sp2 - sp1 - 1);
  version = lines[0].substr(sp2 + 1);

  for (size_t i = 1; i < lines.size(); ++i) {
    size_t colon = lines[i].find(':');
    if (colon == std::string::npos) continue;
    std::string key   = HttpTrim(lines[i].substr(0, colon));
    std::string value = HttpTrim(lines[i].substr(colon + 1));
    if (!key.empty()) headers[key] = value;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Response object
// ---------------------------------------------------------------------------
struct ResState {
  HttpConn*                conn = nullptr;
  int                      status_code = 200;
  std::string              status_message = "OK";
  std::map<std::string, std::string> headers;
};

static const char* HttpReasonPhrase(int code) {
  switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default:  return "";
  }
}

static void HttpApplyStatus(ResState* st, int code) {
  st->status_code = code;
  const char* phrase = HttpReasonPhrase(code);
  st->status_message = (phrase && phrase[0]) ? phrase : (code >= 400 ? "Error" : "OK");
}

static void ResWriteHead(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto ext = args.This()->GetAlignedPointerFromInternalField(0, NX8V_FIELD_TAG);
  auto* st = reinterpret_cast<ResState*>(ext);

  if (args.Length() < 1) return;
  HttpApplyStatus(st, args[0]->Int32Value(ctx).FromMaybe(200));

  int header_arg = 1;
  if (args.Length() >= 2 && args[1]->IsString()) {
    v8::String::Utf8Value msg(isolate, args[1]);
    st->status_message = *msg ? *msg : "OK";
    header_arg = 2;
  }
  if (args.Length() > header_arg && args[header_arg]->IsObject()) {
    auto obj = args[header_arg].As<v8::Object>();
    auto names = obj->GetPropertyNames(ctx).ToLocalChecked();
    for (int i = 0; i < (int)names->Length(); ++i) {
      auto k = names->Get(ctx, i).ToLocalChecked();
      auto v = obj->Get(ctx, k).ToLocalChecked();
      v8::String::Utf8Value ks(isolate, k);
      v8::String::Utf8Value vs(isolate, v);
      st->headers[*ks ? std::string(*ks) : ""] = *vs ? std::string(*vs) : "";
    }
  }
}

static void ResSetHeader(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  if (args.Length() < 2) return;
  auto ext = args.This()->GetAlignedPointerFromInternalField(0, NX8V_FIELD_TAG);
  auto* st = reinterpret_cast<ResState*>(ext);
  v8::String::Utf8Value name(isolate, args[0]);
  v8::String::Utf8Value value(isolate, args[1]);
  st->headers[*name ? std::string(*name) : ""] = *value ? std::string(*value) : "";
}

static void ResGetHeader(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  if (args.Length() < 1) { args.GetReturnValue().Set(v8::Undefined(isolate)); return; }
  auto ext = args.This()->GetAlignedPointerFromInternalField(0, NX8V_FIELD_TAG);
  auto* st = reinterpret_cast<ResState*>(ext);
  v8::String::Utf8Value name(isolate, args[0]);
  auto it = st->headers.find(*name ? std::string(*name) : "");
  if (it == st->headers.end()) {
    args.GetReturnValue().Set(v8::Undefined(isolate));
  } else {
    args.GetReturnValue().Set(
      v8::String::NewFromUtf8(isolate, it->second.c_str(),
                              v8::NewStringType::kNormal).ToLocalChecked());
  }
}

// Copy a response chunk without stopping at the first 0x00. A Buffer keeps
// every byte; a string is UTF-8 with its full length.
static std::string HttpChunkBytes(v8::Isolate* isolate, v8::Local<v8::Value> v,
                                  bool* ok) {
  if (ok) *ok = false;
  if (v.IsEmpty() || v->IsNullOrUndefined() || v->IsFunction()) return "";
  if (v->IsUint8Array()) {
    size_t n = 0;
    uint8_t* p = BufBytes(isolate, v, &n);
    if (ok) *ok = true;
    if (!p || n == 0) return "";
    return std::string(reinterpret_cast<const char*>(p), n);
  }
  if (v->IsString()) {
    if (ok) *ok = true;
    v8::String::Utf8Value s(isolate, v);
    if (!*s) return "";
    return std::string(*s, static_cast<size_t>(s.length()));
  }
  return "";
}

static void ResWrite(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  auto ext = args.This()->GetAlignedPointerFromInternalField(0, NX8V_FIELD_TAG);
  auto* st = reinterpret_cast<ResState*>(ext);
  if (args.Length() < 1) return;
  bool got = false;
  std::string body = HttpChunkBytes(isolate, args[0], &got);
  if (got && !body.empty()) {
    // Buffer writes; flushed on end() (no streaming chunked encoding).
    st->conn->body.append(body);
  }
}

static std::string Nx8vTerminalJs();

static void Nx8vCommitResponse(HttpConn* conn, ResState* st, std::string body) {
  if (st->headers.find("Content-Type") == st->headers.end()) {
    st->headers["Content-Type"] = "text/html; charset=utf-8";
  }
  bool html = false;
  for (auto& kv : st->headers) {
    std::string k = kv.first, v = kv.second;
    for (auto& c : k) c = (char)std::tolower((unsigned char)c);
    if (k != "content-type") continue;
    for (auto& c : v) c = (char)std::tolower((unsigned char)c);
    html = v.find("text/html") != std::string::npos;
    break;
  }
  if (html && body.find('<') == std::string::npos) html = false;
  if (!html && (body.find("<html") != std::string::npos ||
                body.find("<HTML") != std::string::npos ||
                body.find("<!doctype") != std::string::npos ||
                body.find("<!DOCTYPE") != std::string::npos))
    html = true;
  if (html) body = Nx8vExpandPartials(body, conn->request_url);
  if (html && st->status_code == 200) {
    auto tag = body.find("<nx8v-terminal");
    if (tag != std::string::npos &&
        body.find("customElements.get('nx8v-terminal')") == std::string::npos) {
      body.insert(tag, "<script>" + Nx8vTerminalJs() + "</script>");
    }
  }
  // nx8v dev: the page waits on one request and reloads when a file changes.
  const char* nx8v_window = std::getenv("NX8V_WINDOW");
  bool window_mode = nx8v_window && std::string(nx8v_window) == "1";
  const char* nx8v_dev = std::getenv("NX8V_DEV");
  bool dev_mode = nx8v_dev && std::string(nx8v_dev) == "1" && !window_mode;
  if (html && st->status_code == 200 && dev_mode &&
      body.find("__nx8v_dev") == std::string::npos) {
      static const char* kScript =
          "<script>(function(){var g=null;function t(){"
          "fetch('/__nx8v_dev'+(g===null?'':'?since='+encodeURIComponent(g)),{cache:'no-store'})"
          ".then(function(r){return r.json()}).then(function(j){"
          "var n=String(j.gen);if(g===null){g=n;t();return}"
          "if(n!==g)location.reload()})"
          ".catch(function(){setTimeout(t,1000)})}t()})()</script>";
      auto at = body.rfind("</body>");
      if (at == std::string::npos) at = body.rfind("</BODY>");
      if (at == std::string::npos) at = body.rfind("</html>");
      if (at == std::string::npos) body += kScript;
      else body.insert(at, kScript);
  }
  st->headers["Content-Length"] = std::to_string(body.size());
  if (conn->h2) {
    conn->headers_sent = true;
    Nx8vH2Reply(conn, st->status_code, st->headers, body);
    return;
  }
  st->headers["Connection"] = conn->keep_alive ? "keep-alive" : "close";

  std::string head = "HTTP/1.1 " + std::to_string(st->status_code) + " " +
                     st->status_message + "\r\n";
  for (auto& kv : st->headers) {
    head += kv.first + ": " + kv.second + "\r\n";
  }
  head += "\r\n";

  std::string full = head + body;
  conn->headers_sent = true;
  if (conn->tls_ready) {
    HttpWriteRaw(conn, full, !conn->keep_alive);
    return;
  }

  auto* buf = new char[full.size()];
  memcpy(buf, full.data(), full.size());
  conn->write_buf = std::make_shared<uv_buf_t>();
  conn->write_buf->base = buf;
  conn->write_buf->len  = (unsigned int)full.size();

  conn->write_req.data = conn;
  uv_write(&conn->write_req, (uv_stream_t*)&conn->handle,
           conn->write_buf.get(), 1,
           [](uv_write_t* req, int status) {
             auto* c = static_cast<HttpConn*>(req->data);
             (void)status;
             auto* b = (char*)c->write_buf->base;
             delete[] b;
             c->write_buf.reset();
             if (!c->keep_alive) {
               // Graceful close: half-close the socket (FIN) and let libuv
               // drain before destroying the handle. Calling uv_close()
               // straight after uv_write() can send an RST on Windows,
               // which makes browsers retry the request endlessly.
               c->shutdown_req.data = c;
               uv_shutdown(&c->shutdown_req, (uv_stream_t*)&c->handle,
                           [](uv_shutdown_t* sreq, int) {
                             auto* c2 = static_cast<HttpConn*>(sreq->data);
                             uv_close((uv_handle_t*)&c2->handle,
                                      [](uv_handle_t* h) {
                                        auto* c3 = reinterpret_cast<HttpConn*>(h->data);
                                        delete c3;
                                      });
                           });
             }
           });
}

static void ResEnd(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  auto ext = args.This()->GetAlignedPointerFromInternalField(0, NX8V_FIELD_TAG);
  auto* st = reinterpret_cast<ResState*>(ext);
  HttpConn* conn = st->conn;

  std::string body;
  bool got = false;
  if (args.Length() >= 1 && !args[0]->IsFunction())
    body = HttpChunkBytes(isolate, args[0], &got);
  if (!got) body = conn->body;  // accumulated writes
  Nx8vCommitResponse(conn, st, std::move(body));
}

// Build a fresh response object bound to a connection.
static v8::Local<v8::Object> MakeResponse(v8::Isolate* isolate,
                                          HttpConn* conn) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto tmpl = v8::ObjectTemplate::New(isolate);
  tmpl->SetInternalFieldCount(1);
  auto obj = tmpl->NewInstance(ctx).ToLocalChecked();

  auto* st = new ResState();
  st->conn = conn;
  obj->SetAlignedPointerInInternalField(0, st, NX8V_FIELD_TAG);

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    obj->Set(ctx,
      v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal).ToLocalChecked(),
      v8::Function::New(ctx, cb).ToLocalChecked()).Check();
  };
  def("writeHead",  ResWriteHead);
  def("setHeader",  ResSetHeader);
  def("getHeader",  ResGetHeader);
  def("write",      ResWrite);
  def("end",        ResEnd);

  // statusCode property
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "statusCode"),
           v8::Integer::New(isolate, 200)).Check();
  auto status_setter = v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    auto* st = reinterpret_cast<ResState*>(
        a.This()->GetAlignedPointerFromInternalField(0, NX8V_FIELD_TAG));
    HttpApplyStatus(st, a[0]->Int32Value(a.GetIsolate()->GetCurrentContext()).FromMaybe(200));
  }).ToLocalChecked();
  auto status_getter = v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    auto* st = reinterpret_cast<ResState*>(
        a.This()->GetAlignedPointerFromInternalField(0, NX8V_FIELD_TAG));
    a.GetReturnValue().Set(st->status_code);
  }).ToLocalChecked();
  obj->SetAccessorProperty(v8::String::NewFromUtf8Literal(isolate, "statusCode"),
                           status_getter, status_setter);

  return hs.Escape(obj);
}

// Build a request object bound to a connection.
static v8::Local<v8::Object> MakeRequest(v8::Isolate* isolate,
                                         HttpConn* conn,
                                         const std::string& method,
                                         const std::string& url,
                                         const std::string& version,
                                         const std::map<std::string, std::string>& headers) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "method"),
    v8::String::NewFromUtf8(isolate, method.c_str(),
                            v8::NewStringType::kNormal).ToLocalChecked()).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "url"),
    v8::String::NewFromUtf8(isolate, url.c_str(),
                            v8::NewStringType::kNormal).ToLocalChecked()).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "httpVersion"),
    v8::String::NewFromUtf8(isolate, version.c_str(),
                            v8::NewStringType::kNormal).ToLocalChecked()).Check();

  auto headers_obj = v8::Object::New(isolate);
  for (auto& kv : headers) {
    headers_obj->Set(ctx,
      v8::String::NewFromUtf8(isolate, kv.first.c_str(),
                              v8::NewStringType::kNormal).ToLocalChecked(),
      v8::String::NewFromUtf8(isolate, kv.second.c_str(),
                              v8::NewStringType::kNormal).ToLocalChecked()).Check();
  }
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "headers"),
           headers_obj).Check();

  // EventEmitter methods for 'data'/'end'.
  MakeEmitterMethods(isolate, obj);

  return hs.Escape(obj);
}

// ---------------------------------------------------------------------------
// WebSocket (RFC 6455) — server side, minimal but functional
//   server.on('websocket', (ws) => {
//     ws.on('message', (msg) => ws.send('echo: ' + msg));
//     ws.on('close', () => {});
//     ws.send(text); ws.close();
//   });
// ---------------------------------------------------------------------------
static std::string HttpLower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// Case-insensitive header lookup.
static bool HttpHeader(const std::map<std::string, std::string>& headers,
                       const std::string& name, std::string& out) {
  std::string want = HttpLower(name);
  for (const auto& kv : headers) {
    if (HttpLower(kv.first) == want) { out = kv.second; return true; }
  }
  return false;
}

static std::string WsAcceptKey(const std::string& client_key) {
  std::string src = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  Sha1Ctx sha;
  Sha1Update(sha, reinterpret_cast<const uint8_t*>(src.data()), src.size());
  auto digest = Sha1Final(sha);
  return BufBase64Encode(digest.data(), digest.size());
}

struct WsWriteReq {
  uv_write_t req;
  char*      data = nullptr;
  HttpConn*  conn = nullptr;
  bool       shutdown_after = false;
};

static void WsWriteDone(uv_write_t* req, int) {
  auto* w = reinterpret_cast<WsWriteReq*>(req);
  HttpConn* c = w->conn;
  bool shut = w->shutdown_after;
  delete[] w->data;
  delete w;
  if (!shut || !c) return;
  c->shutdown_req.data = c;
  uv_shutdown(&c->shutdown_req, (uv_stream_t*)&c->handle,
              [](uv_shutdown_t* sreq, int) {
                auto* c2 = static_cast<HttpConn*>(sreq->data);
                uv_close((uv_handle_t*)&c2->handle, [](uv_handle_t* h) {
                  delete reinterpret_cast<HttpConn*>(h->data);
                });
              });
}

// Queue one frame (server frames are never masked).
static void WsSendFrame(HttpConn* conn, uint8_t opcode, const std::string& payload) {
  std::string frame;
  frame.reserve(payload.size() + 10);
  frame += (char)(0x80 | opcode);           // FIN + opcode
  size_t len = payload.size();
  if (len < 126) {
    frame += (char)len;
  } else if (len <= 0xFFFF) {
    frame += (char)126;
    frame += (char)((len >> 8) & 0xFF);
    frame += (char)(len & 0xFF);
  } else {
    frame += (char)127;
    for (int i = 7; i >= 0; --i) frame += (char)((len >> (8 * i)) & 0xFF);
  }
  frame += payload;
  HttpWriteRaw(conn, frame, false);
}

// ws.send(text)
static void WsSend(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  auto* conn = reinterpret_cast<HttpConn*>(
      args.This()->GetAlignedPointerFromInternalField(0, NX8V_FIELD_TAG));
  if (!conn || args.Length() < 1) return;

  std::string payload;
  if (args[0]->IsString()) {
    v8::String::Utf8Value s(isolate, args[0]);
    payload.assign(*s ? *s : "", s.length());
  } else {
    size_t len = 0;
    uint8_t* data = BufBytes(isolate, args[0], &len);
    if (data) {
      WsSendFrame(conn, 0x2, std::string(reinterpret_cast<char*>(data), len));
      return;
    }
  }
  WsSendFrame(conn, 0x1, payload);
}

// ws.close()
static void WsClose(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* conn = reinterpret_cast<HttpConn*>(
      args.This()->GetAlignedPointerFromInternalField(0, NX8V_FIELD_TAG));
  if (!conn) return;
  WsSendFrame(conn, 0x8, "");
  uv_close((uv_handle_t*)&conn->handle, [](uv_handle_t* h) {
    auto* c = reinterpret_cast<HttpConn*>(h->data);
    delete c;
  });
}

// Build the JS ws object bound to a connection.
static v8::Local<v8::Object> MakeWsObject(v8::Isolate* isolate, HttpConn* conn) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto tmpl = v8::ObjectTemplate::New(isolate);
  tmpl->SetInternalFieldCount(1);
  auto obj = tmpl->NewInstance(ctx).ToLocalChecked();
  obj->SetAlignedPointerInInternalField(0, conn, NX8V_FIELD_TAG);
  MakeEmitterMethods(isolate, obj);

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    obj->Set(ctx, v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal,
                                          (int)std::strlen(name)).ToLocalChecked(),
             v8::Function::New(ctx, cb).ToLocalChecked()).Check();
  };
  def("send", WsSend);
  def("close", WsClose);

  return hs.Escape(obj);
}

// Emit an event on the ws object with one string argument.
static void WsEmit(v8::Isolate* isolate, v8::Local<v8::Context> ctx,
                   v8::Local<v8::Object> ws, const char* event,
                   const std::string& payload) {
  auto emit_v = ws->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "emit"))
                    .ToLocalChecked();
  if (!emit_v->IsFunction()) return;
  v8::Local<v8::Value> args[] = {
    v8::String::NewFromUtf8(isolate, event, v8::NewStringType::kNormal,
                            (int)std::strlen(event)).ToLocalChecked(),
    v8::String::NewFromUtf8(isolate, payload.data(), v8::NewStringType::kNormal,
                            (int)payload.size()).ToLocalChecked() };
  Nx8vCallIgnore(emit_v.As<v8::Function>(), ctx, ws, 2, args);
}

#include "terminal.h"

// Parse and dispatch buffered client frames.
static void WsProcessFrames(HttpConn* conn) {
  if (conn->term || conn->term_wait) {
    Nx8vTermConsume(conn);
    return;
  }
  v8::Isolate* isolate = nx8v::g_runtime->isolate;
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::Local<v8::Object> ws = conn->ws_obj.Get(isolate);

  for (;;) {
    const std::string& buf = conn->read_buf;
    if (buf.size() < 2) return;

    uint8_t b0 = (uint8_t)buf[0];
    uint8_t b1 = (uint8_t)buf[1];
    uint8_t opcode = b0 & 0x0F;
    bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7F;
    size_t offset = 2;

    if (len == 126) {
      if (buf.size() < 4) return;
      len = ((uint64_t)(uint8_t)buf[2] << 8) | (uint8_t)buf[3];
      offset = 4;
    } else if (len == 127) {
      if (buf.size() < 10) return;
      len = 0;
      for (int i = 0; i < 8; ++i)
        len = (len << 8) | (uint8_t)buf[2 + i];
      offset = 10;
    }

    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
      if (buf.size() < offset + 4) return;
      for (int i = 0; i < 4; ++i) mask[i] = (uint8_t)buf[offset + i];
      offset += 4;
    }
    if (buf.size() < offset + len) return;  // wait for the rest

    std::string payload = buf.substr(offset, (size_t)len);
    if (masked) {
      for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = (char)((uint8_t)payload[i] ^ mask[i % 4]);
    }
    conn->read_buf.erase(0, offset + (size_t)len);

    if (opcode == 0x8) {          // close
      if (conn->term) Nx8vTermClose(conn);
      WsSendFrame(conn, 0x8, "");
      WsEmit(isolate, ctx, ws, "close", "");
      uv_close((uv_handle_t*)&conn->handle, [](uv_handle_t* h) {
        delete reinterpret_cast<HttpConn*>(h->data);
      });
      return;
    }
    if (opcode == 0x9) {          // ping -> pong
      WsSendFrame(conn, 0xA, payload);
      continue;
    }
    if (opcode == 0xA) continue;  // pong: ignore

    // text (0x1) / binary (0x2) / continuation (0x0)
    if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
      WsEmit(isolate, ctx, ws, "message", payload);
    }
  }
}

// Write raw bytes on the connection (used for the 101 handshake).
static void HttpWriteRaw(HttpConn* conn, const std::string& data, bool shutdown_after) {
  std::string out = conn->tls_ready ? Nx8vTlsSeal(conn, data) : data;
  if (out.empty() && !data.empty()) return;
  auto* w = new WsWriteReq();
  w->conn = conn;
  w->shutdown_after = shutdown_after;
  w->data = new char[out.size()];
  std::memcpy(w->data, out.data(), out.size());
  uv_buf_t buf = uv_buf_init(w->data, (unsigned int)out.size());
  uv_write(&w->req, (uv_stream_t*)&conn->handle, &buf, 1, WsWriteDone);
}

// ---------------------------------------------------------------------------
// Connection handling
// ---------------------------------------------------------------------------
static void OnConnection(uv_stream_t* server, int status);

static std::string Nx8vDevGen() {
  const char* path = std::getenv("NX8V_DEV_RELOAD");
  if (!path || !path[0]) return "0";
  std::ifstream in(path);
  std::string gen;
  std::getline(in, gen);
  if (gen.empty()) return "0";
  return gen;
}

struct Nx8vDevHold {
  uv_fs_event_t watch{};
  bool watching = false;
  std::string filename;
  std::vector<HttpConn*> waiters;
};
static Nx8vDevHold g_dev_hold;

static void Nx8vDevForget(HttpConn* conn) {
  auto& w = g_dev_hold.waiters;
  w.erase(std::remove(w.begin(), w.end(), conn), w.end());
}

static std::string Nx8vDevBody(const std::string& gen) {
  std::string json = "{\"gen\":\"";
  for (unsigned char c : gen) {
    if (c == '"' || c == '\\') json += '\\';
    if (c >= 32) json += (char)c;
  }
  json += "\"}";
  return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
         std::to_string(json.size()) +
         "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + json;
}

static void Nx8vSendAndClose(HttpConn* conn, const std::string& full);

static void Nx8vDevReleaseWaiters() {
  std::string gen = Nx8vDevGen();
  std::vector<HttpConn*> ready;
  ready.swap(g_dev_hold.waiters);
  for (HttpConn* c : ready) {
    if (!c || c->dev_replying) continue;
    c->dev_waiting = false;
    c->dev_replying = true;
    uv_read_stop((uv_stream_t*)&c->handle);
    Nx8vSendAndClose(c, Nx8vDevBody(gen));
  }
}

static void Nx8vDevOnReloadFile(uv_fs_event_t*, const char* filename, int, int status) {
  if (status != 0 || !filename) return;
  std::string got = filename, want = g_dev_hold.filename;
  for (auto& c : got) c = (char)std::tolower((unsigned char)c);
  for (auto& c : want) c = (char)std::tolower((unsigned char)c);
  if (got != want) return;
  Nx8vDevReleaseWaiters();
}

static void Nx8vDevArmWatch(uv_loop_t* loop) {
  if (g_dev_hold.watching) return;
  const char* path = std::getenv("NX8V_DEV_RELOAD");
  if (!path || !path[0]) return;
  fs::path file(path);
  g_dev_hold.filename = file.filename().string();
  if (uv_fs_event_init(loop, &g_dev_hold.watch) != 0) return;
  if (uv_fs_event_start(&g_dev_hold.watch, Nx8vDevOnReloadFile,
                        file.parent_path().string().c_str(), 0) != 0) {
    uv_close(reinterpret_cast<uv_handle_t*>(&g_dev_hold.watch), nullptr);
    return;
  }
  g_dev_hold.watching = true;
}

static void Nx8vSendAndClose(HttpConn* conn, const std::string& full) {
  if (conn->tls_ready) {
    conn->keep_alive = false;
    HttpWriteRaw(conn, full, true);
    return;
  }
  auto* buf = new char[full.size()];
  std::memcpy(buf, full.data(), full.size());
  conn->write_buf = std::make_shared<uv_buf_t>();
  conn->write_buf->base = buf;
  conn->write_buf->len = (unsigned int)full.size();
  conn->keep_alive = false;
  conn->write_req.data = conn;
  uv_write(&conn->write_req, (uv_stream_t*)&conn->handle,
           conn->write_buf.get(), 1,
           [](uv_write_t* req, int) {
             auto* c = static_cast<HttpConn*>(req->data);
             delete[] (char*)c->write_buf->base;
             c->write_buf.reset();
             c->shutdown_req.data = c;
             uv_shutdown(&c->shutdown_req, (uv_stream_t*)&c->handle,
                         [](uv_shutdown_t* sreq, int) {
                           auto* c2 = static_cast<HttpConn*>(sreq->data);
                           uv_close((uv_handle_t*)&c2->handle,
                                    [](uv_handle_t* h) {
                                      delete reinterpret_cast<HttpConn*>(h->data);
                                    });
                         });
           });
}

static bool Nx8vReadUnder(const fs::path& root, const std::string& rel,
                           std::string* body, fs::path* file) {
  fs::path full = (root / fs::path(rel)).lexically_normal();
  fs::path root_n = root.lexically_normal();
  if (!Nx8vInsideRoot(full, root_n)) return false;
  std::error_code ec;
  if (!fs::is_regular_file(full, ec)) return false;
  std::ifstream in(full, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  std::streamoff n = in.tellg();
  if (n < 0) return false;
  in.seekg(0, std::ios::beg);
  body->assign((size_t)n, '\0');
  if (n > 0) in.read(&(*body)[0], n);
  if (!in && !in.eof()) return false;
  *file = full;
  return true;
}

static bool Nx8vTryRels(const fs::path& root, const std::vector<std::string>& rels,
                        std::string* body, fs::path* file) {
  for (const auto& rel : rels) {
    if (Nx8vReadUnder(root, rel, body, file)) return true;
  }
  return false;
}

// "dependencies": { "cotoh": "1.0.0" } -> modules/cotoh/index.js
// The key is the module name. The value is its version.
static bool Nx8vDependencyDir(const fs::path& base, const std::string& name,
                              fs::path* dir) {
  std::string obj;
  std::string version;
  std::string json = ReadManifestFile(base / "project.json");
  if (!JsonFindObjectBody(json, "dependencies", obj)) return false;
  if (!JsonFindString(obj, name, version) || version.empty()) return false;
  if (name.empty() || name.find("..") != std::string::npos ||
      name.find('/') != std::string::npos ||
      name.find('\\') != std::string::npos ||
      name.find(':') != std::string::npos)
    return false;
  *dir = (base / "modules" / name).lexically_normal();
  std::error_code ec;
  return fs::is_directory(*dir, ec);
}

static bool Nx8vHasExt(const std::string& seg) {
  auto dot = seg.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= seg.size()) return false;
  for (size_t i = dot + 1; i < seg.size(); ++i) {
    if (!std::isalnum((unsigned char)seg[i])) return false;
  }
  return true;
}

static void Nx8vSendPublic(HttpConn* conn, const std::string& method,
                           const std::string& url, const fs::path& file,
                           std::string body) {
  ResState st;
  st.conn = conn;
  st.status_code = 200;
  st.status_message = "OK";
  st.headers["Content-Type"] = StaticMime(file.extension().string());
  st.headers["Cache-Control"] = "no-store";
  std::printf("%s %s\n", method.c_str(), url.c_str());
  Nx8vCommitResponse(conn, &st, std::move(body));
}

// Pages, CSS, JS, images, and fonts under public/ are served here.
// /js, /css, /fonts, and /images are read from public/assets/.
// A name in project.json dependencies, such as /cotoh, is modules/<name>/index.js.
// /cotoh/index.js is the same file. Names that are not listed are not served.
// server.js only sees requests that are not one of those files.
static bool Nx8vTryServePublic(HttpConn* conn, const std::string& method,
                               const std::string& url) {
  if (method != "GET" && method != "HEAD") return false;
  std::string path = url;
  auto cut = path.find_first_of("?#");
  if (cut != std::string::npos) path.resize(cut);
  if (path.empty()) path = "/";

  std::vector<std::string> segs;
  std::string cur;
  for (char c : path) {
    if (c == '/' || c == '\\') {
      if (!cur.empty()) segs.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) segs.push_back(cur);
  for (const auto& s : segs) {
    if (s == "." || s == ".." || s.find(':') != std::string::npos) return false;
  }

  fs::path base = ".";
  if (nx8v::g_runtime && !nx8v::g_runtime->script_dir.empty())
    base = nx8v::g_runtime->script_dir;
  fs::path pub = base / "public";
  std::error_code ec;
  if (!fs::is_directory(pub, ec)) return false;

  std::string body;
  fs::path file;
  auto send = [&](const std::string& rest, const std::string& page) {
    if (!rest.empty()) {
      std::printf("  route: %s -> %s.html | data: %s\n",
                  path.c_str(), page.c_str(), rest.c_str());
    }
    Nx8vSendPublic(conn, method, url, file, std::move(body));
  };

  if (path == "/favicon.ico") {
    if (Nx8vTryRels(pub, {"favicon.ico", "assets/favicon.ico",
                          "assets/images/32x32.png", "images/32x32.png"},
                    &body, &file)) {
      Nx8vSendPublic(conn, method, url, file, std::move(body));
      return true;
    }
    return false;
  }

  // <script type="module" src="cotoh"> asks for /cotoh.
  if (segs.size() == 1 && !Nx8vHasExt(segs[0])) {
    fs::path named;
    if (Nx8vDependencyDir(base, segs[0], &named) &&
        Nx8vReadUnder(named, "index.js", &body, &file)) {
      Nx8vSendPublic(conn, method, url, file, std::move(body));
      return true;
    }
  }

  if (!segs.empty() && Nx8vHasExt(segs.back())) {
    std::string rel;
    for (size_t i = 0; i < segs.size(); ++i) {
      if (i) rel += '/';
      rel += segs[i];
    }
    // /js/dll.js, /css/style.css, /images/32x32.png, /fonts/...
    // live under public/assets/.
    bool asset_prefix = segs[0] == "js" || segs[0] == "css" ||
                        segs[0] == "fonts" || segs[0] == "images";
    if (asset_prefix) {
      if (Nx8vTryRels(pub, {std::string("assets/") + rel, rel}, &body, &file)) {
        Nx8vSendPublic(conn, method, url, file, std::move(body));
        return true;
      }
      return false;
    }
    // /cotoh/index.js is the same dependency as src="cotoh".
    fs::path mod_dir;
    Nx8vDependencyDir(base, segs[0], &mod_dir);
    if (!mod_dir.empty() && fs::is_directory(mod_dir, ec)) {
      std::string rest;
      for (size_t i = 1; i < segs.size(); ++i) {
        if (!rest.empty()) rest += '/';
        rest += segs[i];
      }
      if (!rest.empty() && Nx8vReadUnder(mod_dir, rest, &body, &file)) {
        Nx8vSendPublic(conn, method, url, file, std::move(body));
        return true;
      }
    }
    if (Nx8vReadUnder(pub, rel, &body, &file)) {
      Nx8vSendPublic(conn, method, url, file, std::move(body));
      return true;
    }
    return false;
  }

  for (size_t i = segs.size(); i >= 1; --i) {
    std::string base_rel;
    for (size_t k = 0; k < i; ++k) {
      if (k) base_rel += '/';
      base_rel += segs[k];
    }
    std::string rest;
    for (size_t k = i; k < segs.size(); ++k) {
      if (!rest.empty()) rest += '/';
      rest += segs[k];
    }
    if (Nx8vTryRels(pub, {base_rel + ".html", base_rel + "/index.html"}, &body, &file)) {
      send(rest, base_rel);
      return true;
    }
  }
  if (segs.empty() && Nx8vReadUnder(pub, "index.html", &body, &file)) {
    Nx8vSendPublic(conn, method, url, file, std::move(body));
    return true;
  }
  return false;
}

static void EmitRequest(HttpConn* conn) {
  v8::Isolate* isolate = nx8v::g_runtime->isolate;
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  // Parse the buffered data.
  std::string method, url, version, body;
  std::map<std::string, std::string> headers;
  if (!HttpParseRequest(conn->read_buf, method, url, version, headers, body)) {
    // Malformed; ignore for now.
    return;
  }
  conn->request_url = url;

  // Live reload probe. `nx8v window` answers once so a cached page stops polling.
  {
    std::string path = url;
    auto q = path.find('?');
    if (q != std::string::npos) path.resize(q);
    if (path == "/__nx8v_dev") {
      conn->read_buf.clear();
      const char* win = std::getenv("NX8V_WINDOW");
      if (win && std::string(win) == "1") {
        Nx8vSendAndClose(conn, Nx8vDevBody("0"));
        return;
      }
      if (!(std::getenv("NX8V_DEV") && std::string(std::getenv("NX8V_DEV")) == "1")) {
        Nx8vSendAndClose(conn, Nx8vDevBody("0"));
        return;
      }
      // Answer immediately the first time. After that the browser waits on
      // this one request; a response is sent only when a watched file changes.
      Nx8vDevArmWatch(conn->handle.loop);
      std::string since;
      auto qpos = url.find("since=");
      if (qpos != std::string::npos) {
        since = url.substr(qpos + 6);
        auto amp = since.find('&');
        if (amp != std::string::npos) since.resize(amp);
      }
      std::string gen = Nx8vDevGen();
      if (since.empty() || since != gen) {
        Nx8vSendAndClose(conn, Nx8vDevBody(gen));
        return;
      }
      conn->dev_waiting = true;
      g_dev_hold.waiters.push_back(conn);
      if (Nx8vDevGen() != since) Nx8vDevReleaseWaiters();
      return;
    }
  }
  // Consume the buffered request. Without this, every later read event would
  // re-process the same bytes and re-run the handler (duplicate requests).
  size_t header_end = conn->read_buf.find("\r\n\r\n");
  size_t sep_len = 4;
  if (header_end == std::string::npos) {
    header_end = conn->read_buf.find("\n\n");
    sep_len = 2;
  }
  std::string leftover;
  if (header_end != std::string::npos && header_end + sep_len <= conn->read_buf.size())
    leftover = conn->read_buf.substr(header_end + sep_len);
  conn->read_buf.clear();

  // ---- WebSocket upgrade -------------------------------------------------
  std::string upgrade_hdr;
  if (HttpHeader(headers, "Upgrade", upgrade_hdr) &&
      HttpLower(upgrade_hdr).find("websocket") != std::string::npos) {
    std::string key;
    HttpHeader(headers, "Sec-WebSocket-Key", key);
    if (key.empty()) {
      HttpWriteRaw(conn, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
      return;
    }

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + WsAcceptKey(key) + "\r\n\r\n";
    conn->keep_alive = true;   // don't close after "response"
    conn->ws_mode    = true;
    HttpWriteRaw(conn, response);

    std::string wspath = url;
    auto wq = wspath.find('?');
    if (wq != std::string::npos) wspath.resize(wq);
    if (wspath == "/__nx8v_terminal") {
      // Shell starts after the page sends its real column count.
      conn->term_wait = true;
      if (!leftover.empty()) {
        conn->read_buf = leftover;
        Nx8vTermConsume(conn);
      }
      return;
    }

    auto ws = MakeWsObject(isolate, conn);
    conn->ws_obj.Reset(isolate, ws);

    auto* srv = conn->server;
    v8::Local<v8::Object> js_server = srv->js_server.Get(isolate);
    auto emit_v = js_server->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "emit"))
                      .ToLocalChecked();
    if (emit_v->IsFunction()) {
      v8::Local<v8::Value> ws_args[] = {
        v8::String::NewFromUtf8Literal(isolate, "websocket"), ws };
      Nx8vCallIgnore(emit_v.As<v8::Function>(), ctx, js_server, 2, ws_args);
    }

    if (!leftover.empty()) {
      conn->read_buf = leftover;
      WsProcessFrames(conn);
    }
    return;
  }

  // This server closes the connection after each response (no keep-alive):
  // simpler and avoids half-implemented pipelining. Browsers will open a new
  // connection for the next request.
  conn->keep_alive = false;

  {
    std::string path = url;
    auto q = path.find('?');
    if (q != std::string::npos) path.resize(q);
    if ((method == "GET" || method == "HEAD") && path == "/terminal") {
      ResState st;
      st.conn = conn;
      st.status_code = 200;
      st.status_message = "OK";
      st.headers["Content-Type"] = "text/javascript; charset=utf-8";
      st.headers["Cache-Control"] = "no-store";
      std::printf("%s /terminal\n", method.c_str());
      Nx8vCommitResponse(conn, &st, method == "HEAD" ? std::string() : Nx8vTerminalJs());
      return;
    }
  }

  if (Nx8vTryServePublic(conn, method, url)) return;

  auto req  = MakeRequest(isolate, conn, method, url, version, headers);
  auto res  = MakeResponse(isolate, conn);

  conn->req_obj.Reset(isolate, req);
  conn->res_obj.Reset(isolate, res);

  // Pull server from the connection's stored server pointer.
  auto* srv = conn->server;

  v8::Local<v8::Value> argv[] = { req, res };

  // Use the JS server's emit('request', req, res).
  v8::Local<v8::Object> js_server = srv->js_server.Get(isolate);
  v8::Local<v8::Value> emit =
      js_server->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "emit"))
          .ToLocalChecked();
  if (emit->IsFunction()) {
    v8::TryCatch tc(isolate);
    v8::Local<v8::Value> req_args[] = {
      v8::String::NewFromUtf8Literal(isolate, "request"), req, res };
    Nx8vCallIgnore(emit.As<v8::Function>(), ctx, js_server, 3, req_args);
    if (tc.HasCaught()) {
      v8::String::Utf8Value ex(isolate, tc.Exception());
      std::fprintf(stderr, "Uncaught (request handler): %s\n", *ex ? *ex : "?");
    }
  }

  // Also invoke default request listener if present (http.createServer(fn)).
  if (!srv->request_listener.IsEmpty()) {
    v8::Local<v8::Function> fn = srv->request_listener.Get(isolate);
    v8::TryCatch tc(isolate);
    Nx8vCallIgnore(fn, ctx, js_server, 2, argv);
    if (tc.HasCaught()) {
      v8::String::Utf8Value ex(isolate, tc.Exception());
      std::fprintf(stderr, "Uncaught (request listener): %s\n", *ex ? *ex : "?");
    }
  }

  // If there's a body, emit 'data' then 'end'.
  if (!body.empty()) {
    v8::Local<v8::Value> data_args[] = {
      v8::String::NewFromUtf8Literal(isolate, "data"),
      v8::String::NewFromUtf8(isolate, body.c_str(),
                              v8::NewStringType::kNormal).ToLocalChecked() };
    v8::Local<v8::Value> e =
        req->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "emit"))
            .ToLocalChecked();
    if (e->IsFunction()) {
      Nx8vCallIgnore(e.As<v8::Function>(), ctx, req, 2, data_args);
    }
  }
  v8::Local<v8::Value> end_args[] = {
    v8::String::NewFromUtf8Literal(isolate, "end") };
  v8::Local<v8::Value> e2 =
      req->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "emit"))
          .ToLocalChecked();
  if (e2->IsFunction()) {
    Nx8vCallIgnore(e2.As<v8::Function>(), ctx, req, 1, end_args);
  }
}

static void OnConnAlloc(uv_handle_t*, size_t suggested, uv_buf_t* buf) {
  // Reuse a single 64KB scratch buffer per connection.
  (void)suggested;
  static thread_local char scratch[65536];
  buf->base = scratch;
  buf->len  = sizeof(scratch);
}

static void OnConnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  auto* conn = reinterpret_cast<HttpConn*>(stream->data);
  if (nread < 0) {
    if (conn->term) Nx8vTermClose(conn);
    if (conn->dev_waiting) {
      conn->dev_waiting = false;
      Nx8vDevForget(conn);
    }
    if (!conn->dev_replying && !uv_is_closing((uv_handle_t*)&conn->handle)) {
      uv_close((uv_handle_t*)&conn->handle,
               [](uv_handle_t* h) { delete reinterpret_cast<HttpConn*>(h->data); });
    }
    return;
  }
  if (nread == 0 || conn->dev_waiting || conn->dev_replying) return;

  if (conn->use_tls) {
    if (!Nx8vTlsConsume(conn, buf->base, (size_t)nread)) return;
  } else {
    conn->read_buf.append(buf->base, (size_t)nread);
  }

  if (Nx8vTakeH2(conn)) return;

  // WebSocket connections: every byte after the handshake is a frame.
  if (conn->ws_mode) {
    EnterIsolate scope(nx8v::g_runtime->isolate);
    WsProcessFrames(conn);
    return;
  }

  // Try to parse a complete request (headers + blank line present).
  if (conn->read_buf.find("\r\n\r\n") != std::string::npos ||
      conn->read_buf.find("\n\n") != std::string::npos) {
    EnterIsolate scope(nx8v::g_runtime->isolate);
    EmitRequest(conn);
  }
}

static void OnConnection(uv_stream_t* server, int status) {
  if (status != 0) return;
  auto* srv = reinterpret_cast<HttpServer*>(server->data);

  auto* conn = new HttpConn();
  uv_tcp_init(nx8v::g_runtime->loop, &conn->handle);
  conn->handle.data = conn;  // stream->data points to the HttpConn
  conn->server = srv;
  conn->use_tls = srv->tls;

  if (uv_accept(server, (uv_stream_t*)&conn->handle) == 0) {
    srv->conn_count.fetch_add(1, std::memory_order_relaxed);
    uv_read_start((uv_stream_t*)&conn->handle, OnConnAlloc, OnConnRead);
  } else {
    uv_close((uv_handle_t*)&conn->handle,
             [](uv_handle_t* h) { delete reinterpret_cast<HttpConn*>(h->data); });
  }
}

// ---------------------------------------------------------------------------
// Server object (the JS-facing http.Server)
// ---------------------------------------------------------------------------
static void ServerListen(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto* srv = HttpServerFromObject(args.This());

  if (srv->tls && !srv->tls_state) {
    std::string err;
    if (!Nx8vTlsPrepare(srv, &err)) {
      isolate->ThrowException(v8::Exception::Error(
          v8::String::NewFromUtf8(isolate, err.c_str(),
                                  v8::NewStringType::kNormal).ToLocalChecked()));
      return;
    }
  }

  int port = 0;
  std::string host = "0.0.0.0";
  v8::Local<v8::Function> cb;

  if (args.Length() >= 1 && args[0]->IsNumber()) port = args[0]->Int32Value(ctx).FromMaybe(0);
  if (args.Length() >= 2 && args[1]->IsString()) {
    v8::String::Utf8Value h(isolate, args[1]);
    host = *h ? *h : "0.0.0.0";
  }
  if (args.Length() >= 2 && args[1]->IsFunction())
    cb = args[1].As<v8::Function>();
  else if (args.Length() >= 3 && args[2]->IsFunction())
    cb = args[2].As<v8::Function>();

  struct sockaddr_in addr;
  int r = uv_ip4_addr(host == "0.0.0.0" ? "0.0.0.0" : host.c_str(), port, &addr);
  if (r != 0) {
    isolate->ThrowException(v8::Exception::Error(
      v8::String::NewFromUtf8Literal(isolate, "invalid listen address")));
    return;
  }
  r = uv_tcp_bind(&srv->handle, (const sockaddr*)&addr, 0);
#ifdef _WIN32
  // A window relaunch starts while the previous process still holds the port.
  if (r == UV_EADDRINUSE && std::getenv("NX8V_WINDOW_RELAUNCH")) {
    for (int i = 0; i < 40 && r == UV_EADDRINUSE; ++i) {
      Sleep(50);
      r = uv_tcp_bind(&srv->handle, (const sockaddr*)&addr, 0);
    }
  }
#endif
  if (r != 0) {
    isolate->ThrowException(v8::Exception::Error(
      v8::String::NewFromUtf8(isolate, uv_strerror(r),
                              v8::NewStringType::kNormal).ToLocalChecked()));
    return;
  }

  r = uv_listen((uv_stream_t*)&srv->handle, 128, OnConnection);
  if (r != 0) {
    isolate->ThrowException(v8::Exception::Error(
      v8::String::NewFromUtf8(isolate, uv_strerror(r),
                              v8::NewStringType::kNormal).ToLocalChecked()));
    return;
  }

  // Emit 'listening'.
  v8::Local<v8::Value> emit =
      args.This()->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "emit"))
          .ToLocalChecked();
  if (emit->IsFunction()) {
    v8::Local<v8::Value> listening_args[] = {
      v8::String::NewFromUtf8Literal(isolate, "listening") };
    Nx8vCallIgnore(emit.As<v8::Function>(), ctx, args.This(), 1, listening_args);
  }
  if (!cb.IsEmpty()) {
    Nx8vCallIgnore(cb, ctx, args.This(), 0, nullptr);
  }

  args.GetReturnValue().Set(args.This());
}

static void ServerClose(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);

  auto* srv = HttpServerFromObject(args.This());
  srv->closing = true;

  uv_close((uv_handle_t*)&srv->handle,
           [](uv_handle_t* h) {
             auto* s = reinterpret_cast<HttpServer*>(h->data);
             EnterIsolate scope(nx8v::g_runtime->isolate);
             v8::Isolate* iso = scope.isolate;
             v8::Local<v8::Context> ctx = iso->GetCurrentContext();
             v8::Local<v8::Object> js = s->js_server.Get(iso);
             v8::Local<v8::Value> emit =
                 js->Get(ctx, v8::String::NewFromUtf8Literal(iso, "emit"))
                     .ToLocalChecked();
              if (emit->IsFunction()) {
                v8::Local<v8::Value> close_args[] = {
                  v8::String::NewFromUtf8Literal(iso, "close") };
                Nx8vCallIgnore(emit.As<v8::Function>(), ctx, js, 1, close_args);
              }
             Nx8vTlsFree(s);
             delete s;
           });

  args.GetReturnValue().Set(args.This());
}

// Build the JS Server object (EventEmitter + listen/close).
static v8::Local<v8::Object> MakeServer(v8::Isolate* isolate,
                                        v8::Local<v8::Function> request_listener);

// Create an http server object without a default request listener.
// Used by the router (which registers its own 'request' listener).
inline v8::Local<v8::Object> MakeHttpServerObject(v8::Isolate* isolate) {
  return MakeServer(isolate, v8::Local<v8::Function>());
}

static v8::Local<v8::Object> MakeServer(v8::Isolate* isolate,
                                        v8::Local<v8::Function> request_listener) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto tmpl = v8::ObjectTemplate::New(isolate);
  tmpl->SetInternalFieldCount(1);
  auto obj = tmpl->NewInstance(ctx).ToLocalChecked();

  // EventEmitter methods.
  MakeEmitterMethods(isolate, obj);

  // Server C++ state.
  auto* srv = new HttpServer();
  srv->isolate   = isolate;
  srv->js_server.Reset(isolate, obj);
  if (!request_listener.IsEmpty())
    srv->request_listener.Reset(isolate, request_listener);

  // uv_tcp handle lives in the C++ struct; init it.
  uv_tcp_init(nx8v::g_runtime->loop, &srv->handle);
  srv->handle.data = srv;

  obj->SetAlignedPointerInInternalField(0, srv, NX8V_FIELD_TAG);

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    obj->Set(ctx,
      v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal).ToLocalChecked(),
      v8::Function::New(ctx, cb).ToLocalChecked()).Check();
  };
  def("listen", ServerListen);
  def("close",  ServerClose);

  // Default 'request' listener via EventEmitter 'on' semantics is already
  // possible through server.on('request', ...). Store default here too.
  if (!request_listener.IsEmpty()) {
    // No-op: default listener captured above.
  }

  return hs.Escape(obj);
}

// ---------------------------------------------------------------------------
// http.createServer([requestListener])
// ---------------------------------------------------------------------------
static void HttpCreateServer(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Function> listener;
  if (args.Length() >= 1 && args[0]->IsFunction())
    listener = args[0].As<v8::Function>();
  args.GetReturnValue().Set(MakeServer(isolate, listener));
}

// ===========================================================================
// HTTP CLIENT
//   http.get(url[, cb])              -> ClientRequest
//   http.request(url|options[, cb])  -> ClientRequest
//   fetch(url[, options])            -> Promise<Response>
//
// The client sends "Connection: close" and reads until EOF, which keeps the
// implementation small and robust (no chunked-encoding state machine).
// ===========================================================================

struct ClientReq {
  uv_tcp_t                 handle;
  uv_connect_t             connect_req;
  uv_write_t               write_req;
  uv_getaddrinfo_t         dns_req;

  std::string              host;
  int                      port = 80;
  std::string              method = "GET";
  std::string              path = "/";
  std::map<std::string, std::string> headers;
  std::string              body;

  std::string              read_buf;
  char*                    write_data = nullptr;
  bool                     finished = false;
  bool                     is_fetch = false;
  bool                     use_tls  = false;
  bool                     reject_unauthorized = true;
  HttpsJob*                https_job = nullptr;

  v8::Global<v8::Function> callback;   // (res)
  v8::Global<v8::Promise::Resolver> resolver;  // fetch only
  v8::Global<v8::Object>   req_obj;    // JS-side ClientRequest
  v8::Global<v8::Object>   res_obj;    // JS-side response emitter
};

// --- tiny URL splitter (http://host:port/path?query) -----------------------
struct UrlParts {
  std::string scheme;
  std::string host;
  int         port = 80;
  std::string path = "/";
};

static bool Nx8vParseUrl(const std::string& url, UrlParts& out) {
  std::string rest = url;
  auto scheme_end = rest.find("://");
  if (scheme_end != std::string::npos) {
    out.scheme = rest.substr(0, scheme_end);
    for (auto& c : out.scheme) c = (char)std::tolower((unsigned char)c);
    rest = rest.substr(scheme_end + 3);
  } else {
    out.scheme = "http";
  }
  if (out.scheme != "http" && out.scheme != "https") return false;

  auto slash = rest.find('/');
  std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  out.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
  if (out.path.empty()) out.path = "/";

  const int default_port = (out.scheme == "https") ? 443 : 80;
  auto colon = authority.rfind(':');
  if (colon != std::string::npos && authority.find(']') == std::string::npos) {
    out.host = authority.substr(0, colon);
    int p = std::atoi(authority.c_str() + colon + 1);
    out.port = p > 0 ? p : default_port;
  } else {
    out.host = authority;
    out.port = default_port;
  }
  return !out.host.empty();
}

// --- response helpers ------------------------------------------------------
static v8::Local<v8::Promise> Nx8vResolvedPromise(v8::Isolate* isolate,
                                                 v8::Local<v8::Context> ctx,
                                                 v8::Local<v8::Value> value) {
  auto resolver = v8::Promise::Resolver::New(ctx).ToLocalChecked();
  (void)resolver->Resolve(ctx, value).FromMaybe(false);
  return resolver->GetPromise();
}

static void Nx8vFreeClientReq(uv_handle_t* h) {
  auto* req = reinterpret_cast<ClientReq*>(h->data);
  delete req;
}

// Close + free the client request.
static void ClientReqDispose(ClientReq* req) {
  if (req->write_data) { delete[] req->write_data; req->write_data = nullptr; }
  if (req->use_tls) {
    delete req;   // the WinHTTP job is owned by the after-work callback
    return;
  }
  uv_close(reinterpret_cast<uv_handle_t*>(&req->handle), Nx8vFreeClientReq);
}

// Emit 'data' + 'end' on an emitter object.
static void EmitDataAndEnd(v8::Isolate* isolate, v8::Local<v8::Context> ctx,
                           v8::Local<v8::Object> obj, const std::string& body) {
  auto emit_v = obj->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "emit"))
                    .ToLocalChecked();
  if (!emit_v->IsFunction()) return;
  auto emit = emit_v.As<v8::Function>();

  if (!body.empty()) {
    v8::Local<v8::Value> args[] = {
      v8::String::NewFromUtf8Literal(isolate, "data"),
      v8::String::NewFromUtf8(isolate, body.c_str(), v8::NewStringType::kNormal,
                              (int)body.size()).ToLocalChecked() };
    Nx8vCallIgnore(emit, ctx, obj, 2, args);
  }
  v8::Local<v8::Value> end_args[] = {
    v8::String::NewFromUtf8Literal(isolate, "end") };
  Nx8vCallIgnore(emit, ctx, obj, 1, end_args);
}

// Build the fetch Response object: { ok, status, statusText, headers, text(), json() }
static v8::Local<v8::Object> MakeFetchResponse(v8::Isolate* isolate,
                                               v8::Local<v8::Context> ctx,
                                               int status,
                                               const std::string& status_text,
                                               std::map<std::string, std::string> headers,
                                               const std::string& body) {
  v8::EscapableHandleScope hs(isolate);
  auto obj = v8::Object::New(isolate);

  auto set = [&](const char* name, v8::Local<v8::Value> v) {
    obj->Set(ctx,
            v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal,
                                (int)std::strlen(name)).ToLocalChecked(), v).Check();
  };

  set("ok", v8::Boolean::New(isolate, status >= 200 && status < 300));
  set("status", v8::Integer::New(isolate, status));
  set("statusText",
      v8::String::NewFromUtf8(isolate, status_text.c_str(),
                              v8::NewStringType::kNormal).ToLocalChecked());

  auto hdr = v8::Object::New(isolate);
  for (auto& kv : headers) {
    std::string lower;
    for (char c : kv.first) lower += (char)std::tolower((unsigned char)c);
    hdr->Set(ctx, v8::String::NewFromUtf8(isolate, lower.c_str(),
                                          v8::NewStringType::kNormal).ToLocalChecked(),
             v8::String::NewFromUtf8(isolate, kv.second.c_str(),
                                     v8::NewStringType::kNormal).ToLocalChecked()).Check();
  }
  set("headers", hdr);

  // text() -> Promise<string>
  auto text_fn = v8::Function::New(ctx, [](
      const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    v8::Local<v8::Context> c = iso->GetCurrentContext();
    auto self = args.This();
    auto body_v = self->Get(c, v8::String::NewFromUtf8Literal(iso, "__body"))
                      .ToLocalChecked();
    args.GetReturnValue().Set(Nx8vResolvedPromise(iso, c, body_v));
  }).ToLocalChecked();
  set("text", text_fn);

  // json() -> Promise<object>
  auto json_fn = v8::Function::New(ctx, [](
      const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    v8::Local<v8::Context> c = iso->GetCurrentContext();
    auto self = args.This();
    auto body_v = self->Get(c, v8::String::NewFromUtf8Literal(iso, "__body"))
                      .ToLocalChecked();
    v8::Local<v8::Value> parsed;
    if (v8::JSON::Parse(c, body_v.As<v8::String>()).ToLocal(&parsed)) {
      args.GetReturnValue().Set(Nx8vResolvedPromise(iso, c, parsed));
    } else {
      auto rej = v8::Promise::Resolver::New(c).ToLocalChecked();
      (void)rej->Reject(c, v8::Exception::Error(
          v8::String::NewFromUtf8Literal(iso, "invalid JSON"))).FromMaybe(false);
      args.GetReturnValue().Set(rej->GetPromise());
    }
  }).ToLocalChecked();
  set("json", json_fn);

  // Non-enumerable-ish body storage used by text()/json().
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "__body"),
           v8::String::NewFromUtf8(isolate, body.c_str(),
                                   v8::NewStringType::kNormal,
                                   (int)body.size()).ToLocalChecked()).Check();

  return hs.Escape(obj);
}

// Parse "HTTP/1.1 200 OK\r\nheaders\r\n\r\nbody"
static bool Nx8vParseResponse(const std::string& raw, int& status,
                              std::string& status_text,
                              std::map<std::string, std::string>& headers,
                              std::string& body) {
  size_t header_end = raw.find("\r\n\r\n");
  size_t sep_len = 4;
  if (header_end == std::string::npos) {
    header_end = raw.find("\n\n");
    sep_len = 2;
  }
  if (header_end == std::string::npos) return false;

  std::string block = raw.substr(0, header_end);
  body = raw.substr(header_end + sep_len);

  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos <= block.size()) {
    size_t nl = block.find('\n', pos);
    std::string line = block.substr(pos, nl == std::string::npos ? std::string::npos
                                                                 : nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) lines.push_back(line);
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  if (lines.empty()) return false;

  // Status line: HTTP/1.1 200 OK
  size_t sp1 = lines[0].find(' ');
  if (sp1 == std::string::npos) return false;
  size_t sp2 = lines[0].find(' ', sp1 + 1);
  status = std::atoi(lines[0].c_str() + sp1 + 1);
  status_text = (sp2 == std::string::npos) ? "" : lines[0].substr(sp2 + 1);

  for (size_t i = 1; i < lines.size(); ++i) {
    size_t colon = lines[i].find(':');
    if (colon == std::string::npos) continue;
    headers[HttpTrim(lines[i].substr(0, colon))] = HttpTrim(lines[i].substr(colon + 1));
  }

  // --- Transfer-Encoding: chunked ---
  // Each chunk is "<hex-size>\r\n<data>\r\n", terminated by "0\r\n\r\n".
  bool chunked = false;
  for (auto& kv : headers) {
    std::string k = kv.first;
    for (auto& c : k) c = (char)std::tolower((unsigned char)c);
    if (k != "transfer-encoding") continue;
    std::string v = kv.second;
    for (auto& c : v) c = (char)std::tolower((unsigned char)c);
    if (v.find("chunked") != std::string::npos) chunked = true;
  }

  if (chunked) {
    std::string decoded_body;
    size_t cpos = 0;
    for (;;) {
      size_t nl = body.find("\r\n", cpos);
      if (nl == std::string::npos) break;

      std::string size_line = body.substr(cpos, nl - cpos);
      auto semi = size_line.find(';');           // chunk extensions
      if (semi != std::string::npos) size_line = size_line.substr(0, semi);

      unsigned long long chunk_len = 0;
      bool valid = !size_line.empty();
      for (char c : size_line) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else { valid = false; break; }
        chunk_len = chunk_len * 16 + (unsigned long long)v;
      }
      if (!valid) break;
      if (chunk_len == 0) break;                 // last chunk

      size_t data_start = nl + 2;
      if (data_start + chunk_len > body.size()) {
        decoded_body.append(body, data_start, body.size() - data_start);
        break;
      }
      decoded_body.append(body, data_start, (size_t)chunk_len);
      cpos = data_start + (size_t)chunk_len + 2;  // skip trailing CRLF
      if (cpos > body.size()) break;
    }
    body = decoded_body;
    return true;
  }

  // Content-Length wins if the body arrived truncated (rare with EOF reads).
  auto cl = headers.find("Content-Length");
  if (cl != headers.end()) {
    size_t want = (size_t)std::atoi(cl->second.c_str());
    if (body.size() > want) body.resize(want);
  }
  return true;
}

// --- client callbacks ------------------------------------------------------
static void ClientOnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);

static void ClientFinish(ClientReq* req) {
  req->finished = true;
  if (!req->use_tls) uv_read_stop((uv_stream_t*)&req->handle);

  EnterIsolate scope(nx8v::g_runtime->isolate);
  v8::Isolate* isolate = scope.isolate;
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  int status = 0;
  std::string status_text;
  std::map<std::string, std::string> headers;
  std::string body;
  bool ok = Nx8vParseResponse(req->read_buf, status, status_text, headers, body);

  if (req->is_fetch) {
    if (!req->resolver.IsEmpty()) {
      auto resolver = req->resolver.Get(isolate);
      if (ok) {
        auto response = MakeFetchResponse(isolate, ctx, status, status_text,
                                          headers, body);
        (void)resolver->Resolve(ctx, response).FromMaybe(false);
      } else {
        (void)resolver->Reject(ctx, v8::Exception::Error(
            v8::String::NewFromUtf8Literal(isolate, "malformed HTTP response")))
            .FromMaybe(false);
      }
    }
  } else {
    // Build a response EventEmitter: statusCode / statusMessage / headers.
    auto res = MakeEmitterMethods(isolate, v8::Object::New(isolate));
    if (ok) {
      res->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "statusCode"),
               v8::Integer::New(isolate, status)).Check();
      res->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "statusMessage"),
               v8::String::NewFromUtf8(isolate, status_text.c_str(),
                                       v8::NewStringType::kNormal).ToLocalChecked()).Check();
      auto hdr = v8::Object::New(isolate);
      for (auto& kv : headers) {
        hdr->Set(ctx, v8::String::NewFromUtf8(isolate, kv.first.c_str(),
                                              v8::NewStringType::kNormal).ToLocalChecked(),
                 v8::String::NewFromUtf8(isolate, kv.second.c_str(),
                                         v8::NewStringType::kNormal).ToLocalChecked()).Check();
      }
      res->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "headers"), hdr).Check();
      res->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "httpVersion"),
               v8::String::NewFromUtf8Literal(isolate, "1.1")).Check();
    }
    req->res_obj.Reset(isolate, res);

    if (!req->callback.IsEmpty()) {
      auto cb = req->callback.Get(isolate);
      v8::Local<v8::Value> argv[] = { res };
      v8::TryCatch tc(isolate);
      Nx8vCallIgnore(cb, ctx, v8::Undefined(isolate), 1, argv);
      if (tc.HasCaught()) {
        v8::String::Utf8Value ex(isolate, tc.Exception());
        std::fprintf(stderr, "Uncaught (http.get callback): %s\n", *ex ? *ex : "?");
      }
    }

    if (ok) EmitDataAndEnd(isolate, ctx, res, body);
  }

  req->callback.Reset();
  req->resolver.Reset();
  req->res_obj.Reset();
  ClientReqDispose(req);
}

static void ClientOnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  auto* req = reinterpret_cast<ClientReq*>(stream->data);
  if (nread > 0) {
    req->read_buf.append(buf->base, (size_t)nread);
    return;
  }
  if (nread == 0) return;
  // EOF or error: the server closed the connection (we asked for it).
  if (!req->finished) ClientFinish(req);
}

static void ClientAlloc(uv_handle_t*, size_t, uv_buf_t* buf) {
  static thread_local char scratch[65536];
  buf->base = scratch;
  buf->len  = sizeof(scratch);
}

static void ClientOnWrite(uv_write_t* wreq, int status) {
  auto* req = reinterpret_cast<ClientReq*>(wreq->data);
  if (req->write_data) { delete[] req->write_data; req->write_data = nullptr; }
  if (status < 0) {
    if (!req->finished) ClientFinish(req);
    return;
  }
  uv_read_start((uv_stream_t*)&req->handle, ClientAlloc, ClientOnRead);
}

// Build the request head+body. Shared by the plain and TLS transports.
static std::string ClientBuildRequest(ClientReq* req) {
  std::string head = req->method + " " + req->path + " HTTP/1.1\r\n";
  bool has_host = false;
  for (auto& kv : req->headers) {
    std::string lower;
    for (char c : kv.first) lower += (char)std::tolower((unsigned char)c);
    if (lower == "host") has_host = true;
    if (lower == "connection") continue;
    head += kv.first + ": " + kv.second + "\r\n";
  }
  if (!has_host) {
    head += "Host: " + req->host;
    const int default_port = req->use_tls ? 443 : 80;
    if (req->port != default_port) head += ":" + std::to_string(req->port);
    head += "\r\n";
  }
  if (!req->body.empty()) {
    head += "Content-Length: " + std::to_string(req->body.size()) + "\r\n";
  }
  head += "Connection: close\r\n\r\n";
  head += req->body;
  return head;
}

// ---- HTTPS via WinHTTP (runs on a worker thread) ---------------------------
static void ClientHttpsFail(ClientReq* req, const std::string& msg) {
  req->finished = true;
  EnterIsolate scope(nx8v::g_runtime->isolate);
  v8::Isolate* isolate = scope.isolate;
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  if (req->is_fetch) {
    if (!req->resolver.IsEmpty()) {
      auto resolver = req->resolver.Get(isolate);
      (void)resolver->Reject(ctx, v8::Exception::Error(
          v8::String::NewFromUtf8(isolate, msg.c_str(),
                                  v8::NewStringType::kNormal).ToLocalChecked()))
          .FromMaybe(false);
    }
  } else if (!req->req_obj.IsEmpty()) {
    auto obj = req->req_obj.Get(isolate);
    auto emit_v = obj->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "emit"))
                      .ToLocalChecked();
    if (emit_v->IsFunction()) {
      v8::Local<v8::Value> argv[] = {
        v8::String::NewFromUtf8Literal(isolate, "error"),
        v8::Exception::Error(v8::String::NewFromUtf8(
            isolate, msg.c_str(), v8::NewStringType::kNormal).ToLocalChecked()) };
      Nx8vCallIgnore(emit_v.As<v8::Function>(), ctx, obj, 2, argv);
    }
  }
}

static void ClientHttpsWork(uv_work_t* w) {
  HttpsRun(static_cast<HttpsJob*>(w->data));
}

static void ClientHttpsDone(uv_work_t* w, int status) {
  auto* job = static_cast<HttpsJob*>(w->data);
  auto* req = static_cast<ClientReq*>(job->owner);

  std::string raw = job->raw_response;
  std::string err = job->error;
  delete job;
  req->https_job = nullptr;

  if (status < 0 && err.empty()) err = "HTTPS worker cancelled";

  if (!err.empty()) {
    ClientHttpsFail(req, err);
    ClientReqDispose(req);
    return;
  }

  // Hand the synthesized response to the shared parser.
  req->read_buf = raw;
  ClientFinish(req);
}


static void ClientOnConnect(uv_connect_t* creq, int status) {
  auto* req = reinterpret_cast<ClientReq*>(creq->data);
  if (status < 0) {
    req->finished = true;
    EnterIsolate scope(nx8v::g_runtime->isolate);
    v8::Isolate* isolate = scope.isolate;
    v8::HandleScope hs(isolate);
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    std::string msg = std::string("connect failed: ") + uv_strerror(status);

    if (req->is_fetch) {
      if (!req->resolver.IsEmpty()) {
        auto resolver = req->resolver.Get(isolate);
        (void)resolver->Reject(ctx, v8::Exception::Error(
            v8::String::NewFromUtf8(isolate, msg.c_str(),
                                    v8::NewStringType::kNormal).ToLocalChecked()))
            .FromMaybe(false);
      }
    } else if (!req->req_obj.IsEmpty()) {
      auto obj = req->req_obj.Get(isolate);
      auto emit_v = obj->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "emit"))
                        .ToLocalChecked();
      if (emit_v->IsFunction()) {
        v8::Local<v8::Value> argv[] = {
          v8::String::NewFromUtf8Literal(isolate, "error"),
          v8::Exception::Error(v8::String::NewFromUtf8(
              isolate, msg.c_str(), v8::NewStringType::kNormal).ToLocalChecked()) };
        Nx8vCallIgnore(emit_v.As<v8::Function>(), ctx, obj, 2, argv);
      }
    }
    req->callback.Reset();
    req->resolver.Reset();
    ClientReqDispose(req);
    return;
  }

  // Build and send the request.
  std::string head = ClientBuildRequest(req);

  req->write_data = new char[head.size()];
  std::memcpy(req->write_data, head.data(), head.size());
  uv_buf_t wbuf = uv_buf_init(req->write_data, (unsigned int)head.size());
  req->write_req.data = req;
  int r = uv_write(&req->write_req, (uv_stream_t*)&req->handle, &wbuf, 1, ClientOnWrite);
  if (r < 0) {
    if (req->write_data) { delete[] req->write_data; req->write_data = nullptr; }
    if (!req->finished) ClientFinish(req);
  }
}

static void ClientOnResolved(uv_getaddrinfo_t* dns, int status,
                             struct addrinfo* res) {
  auto* req = reinterpret_cast<ClientReq*>(dns->data);
  if (status < 0 || !res) {
    req->finished = true;
    EnterIsolate scope(nx8v::g_runtime->isolate);
    v8::Isolate* isolate = scope.isolate;
    v8::HandleScope hs(isolate);
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    std::string msg = std::string("DNS lookup failed: ") + uv_strerror(status);

    if (req->is_fetch) {
      if (!req->resolver.IsEmpty()) {
        auto resolver = req->resolver.Get(isolate);
        (void)resolver->Reject(ctx, v8::Exception::Error(
            v8::String::NewFromUtf8(isolate, msg.c_str(),
                                    v8::NewStringType::kNormal).ToLocalChecked()))
            .FromMaybe(false);
      }
    } else if (!req->req_obj.IsEmpty()) {
      auto obj = req->req_obj.Get(isolate);
      auto emit_v = obj->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "emit"))
                        .ToLocalChecked();
      if (emit_v->IsFunction()) {
        v8::Local<v8::Value> argv[] = {
          v8::String::NewFromUtf8Literal(isolate, "error"),
          v8::Exception::Error(v8::String::NewFromUtf8(
              isolate, msg.c_str(), v8::NewStringType::kNormal).ToLocalChecked()) };
        Nx8vCallIgnore(emit_v.As<v8::Function>(), ctx, obj, 2, argv);
      }
    }
    req->callback.Reset();
    req->resolver.Reset();
    ClientReqDispose(req);
    return;
  }

  req->connect_req.data = req;
  int r = uv_tcp_connect(&req->connect_req, &req->handle, res->ai_addr, ClientOnConnect);
  uv_freeaddrinfo(res);
  if (r < 0) {
    req->finished = true;
    ClientReqDispose(req);
  }
}

// Create the libuv machinery and start connecting.
static void ClientStart(ClientReq* req) {
  // ---- HTTPS (WinHTTP on a worker thread) ----
  if (req->use_tls) {
    auto* job = new HttpsJob();
    job->host = req->host;
    job->port = req->port;
    job->path = req->path;
    job->method = req->method;
    job->headers = req->headers;
    job->body = req->body;
    job->reject_unauthorized = req->reject_unauthorized;
    job->owner = req;
    req->https_job = job;

    job->work.data = job;
    int r = uv_queue_work(nx8v::g_runtime->loop, &job->work,
                          ClientHttpsWork, ClientHttpsDone);
    if (r < 0) {
      req->https_job = nullptr;
      delete job;
      ClientHttpsFail(req, std::string("cannot queue HTTPS request: ") + uv_strerror(r));
      ClientReqDispose(req);
    }
    return;
  }

  // ---- plain HTTP ----
  uv_tcp_init(nx8v::g_runtime->loop, &req->handle);
  req->handle.data = req;

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  std::string port_str = std::to_string(req->port);
  req->dns_req.data = req;
  int r = uv_getaddrinfo(nx8v::g_runtime->loop, &req->dns_req, ClientOnResolved,
                         req->host.c_str(), port_str.c_str(), &hints);
  if (r < 0) {
    req->finished = true;
    ClientReqDispose(req);
  }
}
// --- JS entry points -------------------------------------------------------
// Collects options from either a URL string or an options object.
static bool ClientParseArgs(v8::Isolate* isolate, v8::Local<v8::Context> ctx,
                            v8::Local<v8::Value> first, ClientReq* req) {
  if (first->IsString()) {
    v8::String::Utf8Value u(isolate, first);
    UrlParts p;
    if (!Nx8vParseUrl(*u ? *u : "", p)) return false;
    req->host = p.host;
    req->port = p.port;
    req->path = p.path;
    req->use_tls = (p.scheme == "https");
    return true;
  }
  if (!first->IsObject()) return false;

  auto obj = first.As<v8::Object>();
  auto get_str = [&](const char* key, std::string& out) -> bool {
    auto v = obj->Get(ctx, v8::String::NewFromUtf8(isolate, key, v8::NewStringType::kNormal, (int)std::strlen(key)).ToLocalChecked()).ToLocalChecked();
    if (v->IsString()) {
      v8::String::Utf8Value s(isolate, v);
      out = *s ? *s : "";
      return true;
    }
    return false;
  };

  std::string url;
  if (get_str("href", url) || get_str("url", url)) {
    UrlParts p;
    if (Nx8vParseUrl(url, p)) {
      req->host = p.host;
      req->port = p.port;
      req->path = p.path;
      req->use_tls = (p.scheme == "https");
    }
  }
  get_str("hostname", req->host);
  get_str("host", req->host);
  get_str("path", req->path);
  get_str("method", req->method);

  auto rej = obj->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "rejectUnauthorized"))
                 .ToLocalChecked();
  if (rej->IsBoolean()) req->reject_unauthorized = rej->BooleanValue(isolate);

  auto pv = obj->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "port")).ToLocalChecked();
  if (pv->IsNumber()) req->port = pv->Int32Value(ctx).FromMaybe(80);
  if (req->port <= 0) req->port = 80;

  auto hv = obj->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "headers")).ToLocalChecked();
  if (hv->IsObject()) {
    auto hobj = hv.As<v8::Object>();
    auto names = hobj->GetPropertyNames(ctx).ToLocalChecked();
    for (int i = 0; i < (int)names->Length(); ++i) {
      auto k = names->Get(ctx, i).ToLocalChecked();
      auto v = hobj->Get(ctx, k).ToLocalChecked();
      v8::String::Utf8Value ks(isolate, k);
      v8::String::Utf8Value vs(isolate, v);
      req->headers[*ks ? *ks : ""] = *vs ? *vs : "";
    }
  }
  return !req->host.empty();
}

static void ClientBegin(const v8::FunctionCallbackInfo<v8::Value>& args,
                        bool force_get) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  if (args.Length() < 1) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "url or options required")));
    return;
  }

  auto* req = new ClientReq();
  if (!ClientParseArgs(isolate, ctx, args[0], req)) {
    delete req;
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "invalid url/options")));
    return;
  }
  if (force_get) req->method = "GET";

  // Optional body / callback in the second argument.
  for (int i = 1; i < args.Length(); ++i) {
    if (args[i]->IsFunction()) {
      req->callback.Reset(isolate, args[i].As<v8::Function>());
    } else if (args[i]->IsString() && req->body.empty()) {
      v8::String::Utf8Value b(isolate, args[i]);
      req->body = *b ? *b : "";
    }
  }

  // ClientRequest object: emitter with end()/write() and abort().
  auto req_obj = MakeEmitterMethods(isolate, v8::Object::New(isolate));
  auto set = [&](const char* name, v8::Local<v8::Value> v) {
    req_obj->Set(ctx, v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal, (int)std::strlen(name)).ToLocalChecked(), v).Check();
  };
  set("method", v8::String::NewFromUtf8(isolate, req->method.c_str(),
                                        v8::NewStringType::kNormal).ToLocalChecked());
  set("path", v8::String::NewFromUtf8(isolate, req->path.c_str(),
                                      v8::NewStringType::kNormal).ToLocalChecked());
  set("host", v8::String::NewFromUtf8(isolate, req->host.c_str(),
                                      v8::NewStringType::kNormal).ToLocalChecked());

  req->req_obj.Reset(isolate, req_obj);
  args.GetReturnValue().Set(req_obj);

  ClientStart(req);
}

static void HttpClientGet(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ClientBegin(args, true);
}

static void HttpClientRequest(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ClientBegin(args, false);
}

// --- fetch(url[, options]) -> Promise<Response> -----------------------------
static void Nx8vFetch(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto resolver = v8::Promise::Resolver::New(ctx).ToLocalChecked();
  args.GetReturnValue().Set(resolver->GetPromise());

  if (args.Length() < 1) {
    (void)resolver->Reject(ctx, v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "fetch requires a URL"))).FromMaybe(false);
    return;
  }

  auto* req = new ClientReq();
  req->is_fetch = true;
  if (!ClientParseArgs(isolate, ctx, args[0], req)) {
    delete req;
    (void)resolver->Reject(ctx, v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "invalid URL"))).FromMaybe(false);
    return;
  }

  // options: { method, headers, body }
  if (args.Length() >= 2 && args[1]->IsObject()) {
    auto opts = args[1].As<v8::Object>();
    auto get = [&](const char* k) {
      return opts->Get(ctx, v8::String::NewFromUtf8(isolate, k, v8::NewStringType::kNormal, (int)std::strlen(k)).ToLocalChecked()).ToLocalChecked();
    };
    auto mv = get("method");
    if (mv->IsString()) {
      v8::String::Utf8Value s(isolate, mv);
      req->method = *s ? *s : "GET";
      for (auto& c : req->method) c = (char)std::toupper((unsigned char)c);
    }
    auto bv = get("body");
    if (bv->IsString()) {
      v8::String::Utf8Value s(isolate, bv);
      req->body = *s ? *s : "";
    }
    auto rej = get("rejectUnauthorized");
    if (rej->IsBoolean()) req->reject_unauthorized = rej->BooleanValue(isolate);
    auto hv = get("headers");
    if (hv->IsObject()) {
      auto hobj = hv.As<v8::Object>();
      auto names = hobj->GetPropertyNames(ctx).ToLocalChecked();
      for (int i = 0; i < (int)names->Length(); ++i) {
        auto k = names->Get(ctx, i).ToLocalChecked();
        auto v = hobj->Get(ctx, k).ToLocalChecked();
        v8::String::Utf8Value ks(isolate, k);
        v8::String::Utf8Value vs(isolate, v);
        req->headers[*ks ? *ks : ""] = *vs ? *vs : "";
      }
    }
  }

  req->resolver.Reset(isolate, resolver);
  ClientStart(req);
}

// ---------------------------------------------------------------------------
// Build the http module object
// ---------------------------------------------------------------------------
inline v8::Local<v8::Object> MakeHttpModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);

  // createServer
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "createServer"),
           v8::Function::New(ctx, HttpCreateServer).ToLocalChecked()).Check();

  // request / get
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "request"),
           v8::Function::New(ctx, HttpClientRequest).ToLocalChecked()).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "get"),
           v8::Function::New(ctx, HttpClientGet).ToLocalChecked()).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "Agent"),
           v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value>& a) {
             a.GetReturnValue().Set(v8::Object::New(a.GetIsolate()));
           }).ToLocalChecked()).Check();

  // STATUS_CODES
  auto codes = v8::Object::New(isolate);
  codes->Set(ctx, v8::Integer::New(isolate, 200),
    v8::String::NewFromUtf8Literal(isolate, "OK")).Check();
  codes->Set(ctx, v8::Integer::New(isolate, 201),
    v8::String::NewFromUtf8Literal(isolate, "Created")).Check();
  codes->Set(ctx, v8::Integer::New(isolate, 204),
    v8::String::NewFromUtf8Literal(isolate, "No Content")).Check();
  codes->Set(ctx, v8::Integer::New(isolate, 301),
    v8::String::NewFromUtf8Literal(isolate, "Moved Permanently")).Check();
  codes->Set(ctx, v8::Integer::New(isolate, 302),
    v8::String::NewFromUtf8Literal(isolate, "Found")).Check();
  codes->Set(ctx, v8::Integer::New(isolate, 400),
    v8::String::NewFromUtf8Literal(isolate, "Bad Request")).Check();
  codes->Set(ctx, v8::Integer::New(isolate, 403),
    v8::String::NewFromUtf8Literal(isolate, "Forbidden")).Check();
  codes->Set(ctx, v8::Integer::New(isolate, 404),
    v8::String::NewFromUtf8Literal(isolate, "Not Found")).Check();
  codes->Set(ctx, v8::Integer::New(isolate, 500),
    v8::String::NewFromUtf8Literal(isolate, "Internal Server Error")).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "STATUS_CODES"), codes).Check();

  return hs.Escape(obj);
}

static std::string Nx8vOptBytes(v8::Isolate* isolate, v8::Local<v8::Value> v) {
  if (v.IsEmpty() || v->IsUndefined() || v->IsNull()) return "";
  if (v->IsString()) {
    v8::String::Utf8Value s(isolate, v);
    return *s ? std::string(*s, s.length()) : "";
  }
  bool ok = false;
  return HttpChunkBytes(isolate, v, &ok);
}

static void HttpsCreateServer(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 1 || !args[0]->IsObject() || args[0]->IsFunction()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(
            isolate, "https.createServer membutuhkan { pfx, passphrase } atau { cert, key }")));
    return;
  }
  auto opts = args[0].As<v8::Object>();
  v8::Local<v8::Function> listener;
  if (args.Length() >= 2 && args[1]->IsFunction()) listener = args[1].As<v8::Function>();
  auto get = [&](const char* k) {
    return opts->Get(ctx, v8::String::NewFromUtf8(isolate, k, v8::NewStringType::kNormal,
                                                   (int)std::strlen(k)).ToLocalChecked())
        .ToLocalChecked();
  };
  auto obj = MakeServer(isolate, listener);
  auto* srv = HttpServerFromObject(obj);
  srv->tls = true;
  srv->tls_pfx = Nx8vOptBytes(isolate, get("pfx"));
  srv->tls_pass = Nx8vOptBytes(isolate, get("passphrase"));
  srv->tls_cert = Nx8vOptBytes(isolate, get("cert"));
  srv->tls_key = Nx8vOptBytes(isolate, get("key"));
  if (srv->tls_pfx.empty() && (srv->tls_cert.empty() || srv->tls_key.empty())) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(
            isolate, "https.createServer membutuhkan pfx atau cert dan key")));
    return;
  }
  args.GetReturnValue().Set(obj);
}

inline v8::Local<v8::Object> MakeHttpsModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto obj = MakeHttpModule(isolate);
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "createServer"),
           v8::Function::New(ctx, HttpsCreateServer).ToLocalChecked()).Check();
  return hs.Escape(obj);
}

}} // nx8v::bindings

#include "http2tls.h"
