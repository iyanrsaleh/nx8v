#pragma once
// nx8v/src/bindings/ws.h
// WebSocket client (RFC 6455), exposed as:
//
//   const ws = require('ws');
//   const sock = ws.connect('ws://localhost:5300/', () => { ... });
//   sock.on('open',    () => sock.send('halo'));
//   sock.on('message', (msg) => console.log(msg));
//   sock.on('close',   () => {});
//   sock.on('error',   (err) => {});
//   sock.close();
//
// Client frames are masked, as the spec requires.

#include <v8.h>
#include <uv.h>

#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../runtime.h"
#include "events.h"
#include "crypto.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace nx8v {
namespace bindings {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct WsClientConn {
  uv_tcp_t            handle;
  uv_connect_t        connect_req;
  uv_getaddrinfo_t    dns_req;

  std::string         host;
  int                 port = 80;
  std::string         path = "/";
  std::string         key;          // Sec-WebSocket-Key we sent
  std::string         read_buf;
  bool                handshake_done = false;
  bool                closed = false;
  bool                tcp_inited = false;
  bool                use_tls = false;
  bool                reject_unauthorized = true;

#ifdef _WIN32
  HINTERNET           websocket = nullptr;
  HINTERNET           session = nullptr;
  HINTERNET           connect = nullptr;
  uv_async_t          async{};
  uv_thread_t         thread{};
  bool                async_inited = false;
  bool                thread_started = false;
  CRITICAL_SECTION    lock;
  bool                lock_ready = false;
  std::string         inbound;
  bool                got_open = false;
  bool                got_close = false;
  std::string         fail_reason;
#endif

  v8::Global<v8::Object> js_obj;
  v8::Isolate*           isolate = nullptr;
};

struct WsClientWrite {
  uv_write_t req;
  char*      data = nullptr;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string WsClientLower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

static std::string WsClientAccept(const std::string& key) {
  std::string src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  Sha1Ctx sha;
  Sha1Update(sha, reinterpret_cast<const uint8_t*>(src.data()), src.size());
  auto digest = Sha1Final(sha);
  return BufBase64Encode(digest.data(), digest.size());
}

static std::string WsClientRandomKey() {
  std::random_device rd;
  uint8_t bytes[16];
  for (int i = 0; i < 16; ++i) bytes[i] = (uint8_t)(rd() & 0xFF);
  return BufBase64Encode(bytes, sizeof(bytes));
}

static void WsClientEmit(WsClientConn* c, const char* event,
                         v8::Local<v8::Value> arg = v8::Local<v8::Value>()) {
  if (c->js_obj.IsEmpty()) return;
  v8::Isolate* isolate = c->isolate;
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::Local<v8::Object> obj = c->js_obj.Get(isolate);

  auto emit_v = obj->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "emit"))
                    .ToLocalChecked();
  if (!emit_v->IsFunction()) return;

  if (arg.IsEmpty()) {
    v8::Local<v8::Value> args[] = {
      v8::String::NewFromUtf8(isolate, event, v8::NewStringType::kNormal,
                              (int)std::strlen(event)).ToLocalChecked() };
    Nx8vCallIgnore(emit_v.As<v8::Function>(), ctx, obj, 1, args);
  } else {
    v8::Local<v8::Value> args[] = {
      v8::String::NewFromUtf8(isolate, event, v8::NewStringType::kNormal,
                              (int)std::strlen(event)).ToLocalChecked(),
      arg };
    Nx8vCallIgnore(emit_v.As<v8::Function>(), ctx, obj, 2, args);
  }
}

static void WsClientFreeHandle(uv_handle_t* h) {
  delete reinterpret_cast<WsClientConn*>(h->data);
}

static void WsClientDestroy(WsClientConn* c) {
  if (!c || c->closed) return;
  c->closed = true;
#ifdef _WIN32
  if (c->use_tls) {
    if (c->websocket) {
      WinHttpWebSocketClose(c->websocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                            nullptr, 0);
      WinHttpCloseHandle(c->websocket);
      c->websocket = nullptr;
    }
    return;
  }
#endif
  if (c->tcp_inited)
    uv_close((uv_handle_t*)&c->handle, WsClientFreeHandle);
  else
    delete c;
}

static void WsClientWriteDone(uv_write_t* req, int) {
  auto* w = reinterpret_cast<WsClientWrite*>(req);
  delete[] w->data;
  delete w;
}

static void WsClientWriteRaw(WsClientConn* c, const std::string& data) {
  auto* w = new WsClientWrite();
  w->data = new char[data.size()];
  std::memcpy(w->data, data.data(), data.size());
  uv_buf_t buf = uv_buf_init(w->data, (unsigned int)data.size());
  w->req.data = c;
  uv_write(&w->req, (uv_stream_t*)&c->handle, &buf, 1, WsClientWriteDone);
}

// Client frames must be masked.
static void WsClientSendFrame(WsClientConn* c, uint8_t opcode,
                              const std::string& payload) {
  std::string frame;
  frame += (char)(0x80 | opcode);

  size_t len = payload.size();
  if (len < 126) {
    frame += (char)(0x80 | (uint8_t)len);
  } else if (len <= 0xFFFF) {
    frame += (char)(0x80 | 126);
    frame += (char)((len >> 8) & 0xFF);
    frame += (char)(len & 0xFF);
  } else {
    frame += (char)(0x80 | 127);
    for (int i = 7; i >= 0; --i) frame += (char)((len >> (8 * i)) & 0xFF);
  }

  std::random_device rd;
  uint8_t mask[4];
  for (int i = 0; i < 4; ++i) mask[i] = (uint8_t)(rd() & 0xFF);
  for (int i = 0; i < 4; ++i) frame += (char)mask[i];

  for (size_t i = 0; i < payload.size(); ++i)
    frame += (char)((uint8_t)payload[i] ^ mask[i % 4]);

  WsClientWriteRaw(c, frame);
}

// ---------------------------------------------------------------------------
// Frame parsing
// ---------------------------------------------------------------------------
static void WsClientProcess(WsClientConn* c) {
  for (;;) {
    if (c->read_buf.size() < 2) return;

    uint8_t b0 = (uint8_t)c->read_buf[0];
    uint8_t b1 = (uint8_t)c->read_buf[1];
    uint8_t opcode = b0 & 0x0F;
    bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7F;
    size_t offset = 2;

    if (len == 126) {
      if (c->read_buf.size() < 4) return;
      len = ((uint64_t)(uint8_t)c->read_buf[2] << 8) | (uint8_t)c->read_buf[3];
      offset = 4;
    } else if (len == 127) {
      if (c->read_buf.size() < 10) return;
      len = 0;
      for (int i = 0; i < 8; ++i) len = (len << 8) | (uint8_t)c->read_buf[2 + i];
      offset = 10;
    }

    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
      if (c->read_buf.size() < offset + 4) return;
      for (int i = 0; i < 4; ++i) mask[i] = (uint8_t)c->read_buf[offset + i];
      offset += 4;
    }
    if (c->read_buf.size() < offset + len) return;

    std::string payload = c->read_buf.substr(offset, (size_t)len);
    if (masked) {
      for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = (char)((uint8_t)payload[i] ^ mask[i % 4]);
    }
    c->read_buf.erase(0, offset + (size_t)len);

    if (opcode == 0x8) {
      WsClientSendFrame(c, 0x8, "");
      WsClientEmit(c, "close");
      WsClientDestroy(c);
      return;
    }
    if (opcode == 0x9) { WsClientSendFrame(c, 0xA, payload); continue; }
    if (opcode == 0xA) continue;

    if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
      v8::Isolate* isolate = c->isolate;
      v8::HandleScope hs(isolate);
      WsClientEmit(c, "message",
                   v8::String::NewFromUtf8(isolate, payload.data(),
                                           v8::NewStringType::kNormal,
                                           (int)payload.size()).ToLocalChecked());
    }
  }
}

// ---------------------------------------------------------------------------
// IO callbacks
// ---------------------------------------------------------------------------
static void WsClientOnRead(uv_stream_t* stream, ssize_t nread,
                           const uv_buf_t* buf) {
  auto* c = reinterpret_cast<WsClientConn*>(stream->data);
  if (nread < 0) {
    EnterIsolate scope(nx8v::g_runtime->isolate);
    WsClientEmit(c, "close");
    WsClientDestroy(c);
    return;
  }
  if (nread == 0) return;

  EnterIsolate scope(nx8v::g_runtime->isolate);
  c->read_buf.append(buf->base, (size_t)nread);

  if (!c->handshake_done) {
    size_t end = c->read_buf.find("\r\n\r\n");
    if (end == std::string::npos) return;

    std::string head = c->read_buf.substr(0, end);
    c->read_buf.erase(0, end + 4);

    bool ok = head.find("101") != std::string::npos;
    if (ok) {
      // Verify Sec-WebSocket-Accept when present.
      std::string lower = WsClientLower(head);
      std::string want = WsClientLower(WsClientAccept(c->key));
      if (lower.find("sec-websocket-accept") != std::string::npos &&
          lower.find(want) == std::string::npos)
        ok = false;
    }
    if (!ok) {
      v8::Isolate* isolate = c->isolate;
      v8::HandleScope hs(isolate);
      WsClientEmit(c, "error", v8::Exception::Error(
          v8::String::NewFromUtf8Literal(isolate, "WebSocket handshake failed")));
      WsClientDestroy(c);
      return;
    }

    c->handshake_done = true;
    WsClientEmit(c, "open");
    if (!c->read_buf.empty()) WsClientProcess(c);
    return;
  }

  WsClientProcess(c);
}

static void WsClientAlloc(uv_handle_t*, size_t, uv_buf_t* buf) {
  static thread_local char scratch[65536];
  buf->base = scratch;
  buf->len  = sizeof(scratch);
}

static void WsClientOnConnect(uv_connect_t* req, int status) {
  auto* c = reinterpret_cast<WsClientConn*>(req->data);
  EnterIsolate scope(nx8v::g_runtime->isolate);

  if (status < 0) {
    v8::Isolate* isolate = c->isolate;
    v8::HandleScope hs(isolate);
    WsClientEmit(c, "error", v8::Exception::Error(
        v8::String::NewFromUtf8(isolate, uv_strerror(status),
                                v8::NewStringType::kNormal).ToLocalChecked()));
    WsClientDestroy(c);
    return;
  }

  std::string req_str =
      "GET " + c->path + " HTTP/1.1\r\n"
      "Host: " + c->host + ":" + std::to_string(c->port) + "\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: " + c->key + "\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  WsClientWriteRaw(c, req_str);

  uv_read_start((uv_stream_t*)&c->handle, WsClientAlloc, WsClientOnRead);
}

static void WsClientOnResolved(uv_getaddrinfo_t* dns, int status,
                               struct addrinfo* res) {
  auto* c = reinterpret_cast<WsClientConn*>(dns->data);
  EnterIsolate scope(nx8v::g_runtime->isolate);

  if (status < 0 || !res) {
    v8::Isolate* isolate = c->isolate;
    v8::HandleScope hs(isolate);
    WsClientEmit(c, "error", v8::Exception::Error(
        v8::String::NewFromUtf8Literal(isolate, "DNS lookup failed")));
    WsClientDestroy(c);
    return;
  }
  c->connect_req.data = c;
  int r = uv_tcp_connect(&c->connect_req, &c->handle, res->ai_addr,
                         WsClientOnConnect);
  uv_freeaddrinfo(res);
  if (r < 0) WsClientDestroy(c);
}

// ---------------------------------------------------------------------------
// JS API
// ---------------------------------------------------------------------------
static WsClientConn* WsClientFromObject(v8::Local<v8::Object> obj) {
  return reinterpret_cast<WsClientConn*>(
      obj->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
}

static void WsClientSend(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  auto* c = WsClientFromObject(args.This());
  if (!c || c->closed || !c->handshake_done || args.Length() < 1) return;

#ifdef _WIN32
  if (c->use_tls && c->websocket) {
    std::string payload;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
    if (args[0]->IsString()) {
      v8::String::Utf8Value s(isolate, args[0]);
      payload.assign(*s ? *s : "", (size_t)s.length());
    } else {
      size_t len = 0;
      uint8_t* data = BufBytes(isolate, args[0], &len);
      if (!data) return;
      payload.assign(reinterpret_cast<char*>(data), len);
      type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
    }
    WinHttpWebSocketSend(c->websocket, type,
                         payload.empty() ? nullptr : (PVOID)payload.data(),
                         (DWORD)payload.size());
    return;
  }
#endif

  if (args[0]->IsString()) {
    v8::String::Utf8Value s(isolate, args[0]);
    WsClientSendFrame(c, 0x1, std::string(*s ? *s : "", s.length()));
  } else {
    size_t len = 0;
    uint8_t* data = BufBytes(isolate, args[0], &len);
    if (data) WsClientSendFrame(c, 0x2, std::string((char*)data, len));
  }
}

static void WsClientCloseFn(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::HandleScope hs(args.GetIsolate());
  auto* c = WsClientFromObject(args.This());
  if (!c || c->closed) return;
  WsClientSendFrame(c, 0x8, "");
  WsClientDestroy(c);
}

#ifdef _WIN32
static std::wstring WsWiden(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring out((size_t)n, L'\0');
  if (n > 0)
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
  return out;
}

static void WsWssOnAsync(uv_async_t* handle) {
  auto* c = static_cast<WsClientConn*>(handle->data);
  if (!c) return;
  EnterIsolate scope(nx8v::g_runtime->isolate);
  v8::Isolate* isolate = scope.isolate;

  std::string inbound;
  std::string fail;
  bool open = false;
  bool dead = false;
  EnterCriticalSection(&c->lock);
  inbound.swap(c->inbound);
  fail.swap(c->fail_reason);
  open = c->got_open;
  dead = c->got_close;
  c->got_open = false;
  c->got_close = false;
  LeaveCriticalSection(&c->lock);

  if (open) {
    c->handshake_done = true;
    if (!c->js_obj.IsEmpty()) {
      auto obj = c->js_obj.Get(isolate);
      auto ctx = isolate->GetCurrentContext();
      obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "readyState"),
               v8::Integer::New(isolate, 1)).Check();
    }
    WsClientEmit(c, "open");
  }
  if (!inbound.empty()) {
    WsClientEmit(c, "message",
                 v8::String::NewFromUtf8(isolate, inbound.data(),
                                         v8::NewStringType::kNormal,
                                         (int)inbound.size())
                     .ToLocalChecked());
  }
  if (!fail.empty()) {
    WsClientEmit(c, "error", v8::Exception::Error(
        v8::String::NewFromUtf8(isolate, fail.c_str(), v8::NewStringType::kNormal)
            .ToLocalChecked()));
  }
  if (dead) {
    WsClientEmit(c, "close");
    if (c->async_inited && !uv_is_closing(reinterpret_cast<uv_handle_t*>(&c->async))) {
      uv_close(reinterpret_cast<uv_handle_t*>(&c->async), [](uv_handle_t* h) {
        auto* c = static_cast<WsClientConn*>(h->data);
        if (c->thread_started) uv_thread_join(&c->thread);
        if (c->session) WinHttpCloseHandle(c->session);
        if (c->connect) WinHttpCloseHandle(c->connect);
        if (c->lock_ready) DeleteCriticalSection(&c->lock);
        c->js_obj.Reset();
        delete c;
      });
    }
  }
}

static void WsWssSignal(WsClientConn* c) { uv_async_send(&c->async); }

static void WsWssThread(void* arg) {
  auto* c = static_cast<WsClientConn*>(arg);
  HINTERNET session = WinHttpOpen(L"nx8v/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET connect = nullptr;
  HINTERNET request = nullptr;
  HINTERNET ws = nullptr;
  std::string fail;

  do {
    if (!session) { fail = "WinHttpOpen failed"; break; }
    connect = WinHttpConnect(session, WsWiden(c->host).c_str(),
                             (INTERNET_PORT)c->port, 0);
    if (!connect) { fail = "WinHttpConnect failed"; break; }
    DWORD flags = WINHTTP_FLAG_SECURE;
    std::wstring path = WsWiden(c->path.empty() ? "/" : c->path);
    request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 flags);
    if (!request) { fail = "WinHttpOpenRequest failed"; break; }
    WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
    if (!c->reject_unauthorized) {
      DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                  SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
      WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));
    }
    BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 nullptr, 0, 0, 0);
    if (!ok || !WinHttpReceiveResponse(request, nullptr)) {
      fail = "WebSocket TLS handshake failed";
      break;
    }
    ws = WinHttpWebSocketCompleteUpgrade(request, 0);
    WinHttpCloseHandle(request);
    request = nullptr;
    if (!ws) { fail = "WebSocket upgrade failed"; break; }
  } while (false);

  c->session = session;
  c->connect = connect;
  c->websocket = ws;

  EnterCriticalSection(&c->lock);
  if (!fail.empty()) {
    c->fail_reason = fail;
    c->got_close = true;
  } else {
    c->got_open = true;
  }
  LeaveCriticalSection(&c->lock);
  WsWssSignal(c);
  if (!ws) {
    c->thread_started = true;
    return;
  }

  std::string acc;
  while (!c->closed) {
    BYTE buf[16384];
    DWORD read = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
    DWORD err = WinHttpWebSocketReceive(ws, buf, sizeof(buf), &read, &type);
    if (err != NO_ERROR) break;
    if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
    acc.append(reinterpret_cast<char*>(buf), read);
    bool end = type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
               type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
    if (end) {
      EnterCriticalSection(&c->lock);
      c->inbound += acc;
      LeaveCriticalSection(&c->lock);
      acc.clear();
      WsWssSignal(c);
    }
  }

  EnterCriticalSection(&c->lock);
  c->got_close = true;
  LeaveCriticalSection(&c->lock);
  if (c->websocket) {
    WinHttpCloseHandle(c->websocket);
    c->websocket = nullptr;
  }
  WsWssSignal(c);
}

static void WsClientStartWss(WsClientConn* c) {
  InitializeCriticalSection(&c->lock);
  c->lock_ready = true;
  uv_async_init(nx8v::g_runtime->loop, &c->async, WsWssOnAsync);
  c->async.data = c;
  c->async_inited = true;
  if (uv_thread_create(&c->thread, WsWssThread, c) == 0)
    c->thread_started = true;
}
#endif

// ws.connect(url[, cb]) -> WebSocket object
static void WsConnect(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  if (nx8v::g_runtime && !nx8v::g_runtime->allow_net) {
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(isolate, "network access is not allowed")));
    return;
  }
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  if (args.Length() < 1 || !args[0]->IsString()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "ws.connect(url) requires a URL")));
    return;
  }

  v8::String::Utf8Value url(isolate, args[0]);
  std::string s = *url ? *url : "";

  // ws://host[:port][/path]   or   wss://host[:port][/path]
  std::string rest = s;
  bool use_tls = false;
  auto scheme = rest.find("://");
  if (scheme != std::string::npos) {
    std::string proto = WsClientLower(rest.substr(0, scheme));
    if (proto == "wss") use_tls = true;
    else if (proto != "ws") {
      isolate->ThrowException(v8::Exception::Error(
          v8::String::NewFromUtf8Literal(isolate, "only ws:// and wss:// are supported")));
      return;
    }
    rest = rest.substr(scheme + 3);
  }

  std::string authority = rest, path = "/";
  auto slash = rest.find('/');
  if (slash != std::string::npos) {
    authority = rest.substr(0, slash);
    path = rest.substr(slash);
  }

  std::string host = authority;
  int port = use_tls ? 443 : 80;
  auto colon = authority.rfind(':');
  if (colon != std::string::npos) {
    host = authority.substr(0, colon);
    int p = std::atoi(authority.c_str() + colon + 1);
    if (p > 0) port = p;
  }

  auto* c = new WsClientConn();
  c->host = host;
  c->port = port;
  c->path = path;
  c->key  = WsClientRandomKey();
  c->isolate = isolate;
  c->use_tls = use_tls;

  if (!use_tls) {
    uv_tcp_init(nx8v::g_runtime->loop, &c->handle);
    c->handle.data = c;
    c->tcp_inited = true;
  }

  // JS object
  auto tmpl = v8::ObjectTemplate::New(isolate);
  tmpl->SetInternalFieldCount(1);
  auto obj = tmpl->NewInstance(ctx).ToLocalChecked();
  obj->SetAlignedPointerInInternalField(0, c, v8::kEmbedderDataTypeTagDefault);
  MakeEmitterMethods(isolate, obj);

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    obj->Set(ctx, v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal,
                                          (int)std::strlen(name)).ToLocalChecked(),
             v8::Function::New(ctx, cb).ToLocalChecked()).Check();
  };
  def("send", WsClientSend);
  def("close", WsClientCloseFn);

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "url"),
           v8::String::NewFromUtf8(isolate, s.c_str(),
                                   v8::NewStringType::kNormal).ToLocalChecked()).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "readyState"),
           v8::Integer::New(isolate, 0)).Check();

  c->js_obj.Reset(isolate, obj);

  // Optional options object and/or open callback.
  int cb_at = 1;
  if (args.Length() >= 2 && args[1]->IsObject() && !args[1]->IsFunction()) {
    auto opt = args[1].As<v8::Object>();
    auto rv = opt->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "rejectUnauthorized"))
                  .ToLocalChecked();
    if (rv->IsBoolean()) c->reject_unauthorized = rv->BooleanValue(isolate);
    cb_at = 2;
  }
  if (args.Length() > cb_at && args[cb_at]->IsFunction()) {
    auto cb = args[cb_at].As<v8::Function>();
    v8::Local<v8::Value> once_args[] = {
      v8::String::NewFromUtf8Literal(isolate, "open"), cb };
    auto once_v = obj->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "once"))
                      .ToLocalChecked();
    if (once_v->IsFunction())
      Nx8vCallIgnore(once_v.As<v8::Function>(), ctx, obj, 2, once_args);
  }

#ifdef _WIN32
  if (use_tls) {
    WsClientStartWss(c);
    args.GetReturnValue().Set(obj);
    return;
  }
#else
  if (use_tls) {
    uv_timer_t* timer = new uv_timer_t;
    timer->data = c;
    uv_timer_init(nx8v::g_runtime->loop, timer);
    uv_timer_start(timer, [](uv_timer_t* t) {
      auto* c = static_cast<WsClientConn*>(t->data);
      uv_timer_stop(t);
      EnterIsolate scope(nx8v::g_runtime->isolate);
      WsClientEmit(c, "error", v8::Exception::Error(v8::String::NewFromUtf8Literal(
          scope.isolate, "wss:// needs Windows or a build with OpenSSL")));
      WsClientEmit(c, "close");
      uv_close(reinterpret_cast<uv_handle_t*>(t), [](uv_handle_t* h) {
        auto* c = static_cast<WsClientConn*>(h->data);
        c->js_obj.Reset();
        delete c;
        delete reinterpret_cast<uv_timer_t*>(h);
      });
    }, 0, 0);
    args.GetReturnValue().Set(obj);
    return;
  }
#endif

  // Start async connect.
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  std::string port_str = std::to_string(port);
  c->dns_req.data = c;
  int r = uv_getaddrinfo(nx8v::g_runtime->loop, &c->dns_req, WsClientOnResolved,
                         host.c_str(), port_str.c_str(), &hints);
  if (r < 0) {
    WsClientDestroy(c);
  }

  args.GetReturnValue().Set(obj);
}

inline v8::Local<v8::Object> MakeWsModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "connect"),
           v8::Function::New(ctx, WsConnect).ToLocalChecked()).Check();
  return hs.Escape(obj);
}

}} // nx8v::bindings
