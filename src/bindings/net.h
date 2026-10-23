#pragma once
// nx8v/src/bindings/net.h
// Raw TCP, shaped like a small slice of Node's net module.
//
//   const net = require('net');
//   const server = net.createServer((socket) => {
//     socket.on('data', (chunk) => socket.write(chunk));
//   });
//   server.listen(9000, () => {});
//   const sock = net.connect(9000, '127.0.0.1', () => sock.write('hi'));

#include <v8.h>
#include <uv.h>

#include <cstring>
#include <string>

#include "../runtime.h"
#include "events.h"

namespace nx8v {
namespace bindings {

struct NetSocket {
  uv_tcp_t handle;
  uv_connect_t connect_req;
  uv_getaddrinfo_t dns_req;
  v8::Global<v8::Object> js;
  v8::Global<v8::Function> connect_cb;
  char scratch[65536];
  bool connected = false;
  bool closing = false;
  std::string host = "127.0.0.1";
  int port = 0;
};

struct NetServer {
  uv_tcp_t handle;
  v8::Global<v8::Object> js;
  v8::Global<v8::Function> connection_cb;
  v8::Global<v8::Function> listen_cb;
  bool closing = false;
};

static void NetEmit(v8::Isolate* isolate, v8::Local<v8::Object> obj,
                    const char* event, v8::Local<v8::Value> arg = v8::Local<v8::Value>()) {
  auto ctx = isolate->GetCurrentContext();
  auto emit_v = obj->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "emit"))
                    .ToLocalChecked();
  if (!emit_v->IsFunction()) return;
  v8::Local<v8::Value> name =
      v8::String::NewFromUtf8(isolate, event, v8::NewStringType::kNormal).ToLocalChecked();
  v8::TryCatch tc(isolate);
  v8::Local<v8::Value> ignored;
  if (arg.IsEmpty()) {
    v8::Local<v8::Value> argv[] = {name};
    (void)emit_v.As<v8::Function>()->Call(ctx, obj, 1, argv).ToLocal(&ignored);
  } else {
    v8::Local<v8::Value> argv[] = {name, arg};
    (void)emit_v.As<v8::Function>()->Call(ctx, obj, 2, argv).ToLocal(&ignored);
  }
}

static void NetFreeSocket(uv_handle_t* h) {
  auto* s = static_cast<NetSocket*>(h->data);
  s->js.Reset();
  s->connect_cb.Reset();
  delete s;
}

static void NetCloseSocket(NetSocket* s) {
  if (!s || s->closing) return;
  s->closing = true;
  uv_close(reinterpret_cast<uv_handle_t*>(&s->handle), NetFreeSocket);
}

static void NetAlloc(uv_handle_t* handle, size_t, uv_buf_t* buf) {
  auto* s = static_cast<NetSocket*>(handle->data);
  buf->base = s->scratch;
  buf->len = sizeof(s->scratch);
}

static void NetOnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t*) {
  auto* s = static_cast<NetSocket*>(stream->data);
  EnterIsolate scope(nx8v::g_runtime->isolate);
  v8::Isolate* isolate = scope.isolate;
  if (s->js.IsEmpty()) return;
  auto obj = s->js.Get(isolate);
  if (nread < 0) {
    NetEmit(isolate, obj, "end");
    NetCloseSocket(s);
    return;
  }
  if (nread == 0) return;
  NetEmit(isolate, obj, "data",
          v8::String::NewFromUtf8(isolate, s->scratch, v8::NewStringType::kNormal,
                                  (int)nread)
              .ToLocalChecked());
}

static v8::Local<v8::Object> NetMakeSocketObject(v8::Isolate* isolate, NetSocket* s) {
  auto ctx = isolate->GetCurrentContext();
  auto tmpl = v8::ObjectTemplate::New(isolate);
  tmpl->SetInternalFieldCount(1);
  auto obj = tmpl->NewInstance(ctx).ToLocalChecked();
  obj->SetAlignedPointerInInternalField(0, s, v8::kEmbedderDataTypeTagDefault);
  MakeEmitterMethods(isolate, obj);

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "write"),
           v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
             auto* sock = static_cast<NetSocket*>(
                 args.This()->GetAlignedPointerFromInternalField(
                     0, v8::kEmbedderDataTypeTagDefault));
             if (!sock || sock->closing || args.Length() < 1) return;
             v8::String::Utf8Value data(args.GetIsolate(), args[0]);
             if (!*data) return;
             auto* copy = new char[data.length()];
             std::memcpy(copy, *data, data.length());
             uv_buf_t buf = uv_buf_init(copy, (unsigned int)data.length());
             auto* req = new uv_write_t;
             req->data = copy;
             uv_write(req, reinterpret_cast<uv_stream_t*>(&sock->handle), &buf, 1,
                      [](uv_write_t* r, int) {
                        delete[] static_cast<char*>(r->data);
                        delete r;
                      });
           }).ToLocalChecked())
      .Check();

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "end"),
           v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
             auto* sock = static_cast<NetSocket*>(
                 args.This()->GetAlignedPointerFromInternalField(
                     0, v8::kEmbedderDataTypeTagDefault));
             if (!sock) return;
             if (args.Length() >= 1 && args[0]->IsString()) {
               v8::String::Utf8Value data(args.GetIsolate(), args[0]);
               if (*data && data.length() > 0) {
                 auto* copy = new char[data.length()];
                 std::memcpy(copy, *data, data.length());
                 uv_buf_t buf = uv_buf_init(copy, (unsigned int)data.length());
                 auto* req = new uv_write_t;
                 req->data = copy;
                 uv_write(req, reinterpret_cast<uv_stream_t*>(&sock->handle), &buf, 1,
                          [](uv_write_t* r, int) {
                            delete[] static_cast<char*>(r->data);
                            delete r;
                          });
               }
             }
             uv_shutdown(new uv_shutdown_t, reinterpret_cast<uv_stream_t*>(&sock->handle),
                         [](uv_shutdown_t* r, int) { delete r; });
           }).ToLocalChecked())
      .Check();

  s->js.Reset(isolate, obj);
  return obj;
}

static void NetOnConnection(uv_stream_t* server, int status) {
  auto* srv = static_cast<NetServer*>(server->data);
  if (status < 0 || srv->closing) return;
  auto* sock = new NetSocket();
  uv_tcp_init(nx8v::g_runtime->loop, &sock->handle);
  sock->handle.data = sock;
  if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&sock->handle)) != 0) {
    NetCloseSocket(sock);
    return;
  }
  sock->connected = true;
  EnterIsolate scope(nx8v::g_runtime->isolate);
  v8::Isolate* isolate = scope.isolate;
  auto obj = NetMakeSocketObject(isolate, sock);
  uv_read_start(reinterpret_cast<uv_stream_t*>(&sock->handle), NetAlloc, NetOnRead);
  if (!srv->connection_cb.IsEmpty()) {
    auto cb = srv->connection_cb.Get(isolate);
    v8::Local<v8::Value> argv[] = {obj};
    v8::TryCatch tc(isolate);
    v8::Local<v8::Value> ignored;
    (void)cb->Call(isolate->GetCurrentContext(),
                   srv->js.IsEmpty() ? isolate->GetCurrentContext()->Global()
                                     : srv->js.Get(isolate).As<v8::Value>(),
                   1, argv)
        .ToLocal(&ignored);
  }
}

static void NetFreeServer(uv_handle_t* h) {
  auto* s = static_cast<NetServer*>(h->data);
  s->js.Reset();
  s->connection_cb.Reset();
  s->listen_cb.Reset();
  delete s;
}

static void NetOnClientConnect(uv_connect_t* req, int status) {
  auto* sock = static_cast<NetSocket*>(req->data);
  EnterIsolate scope(nx8v::g_runtime->isolate);
  v8::Isolate* isolate = scope.isolate;
  if (sock->js.IsEmpty()) return;
  auto obj = sock->js.Get(isolate);
  if (status < 0) {
    NetEmit(isolate, obj, "error",
            v8::Exception::Error(
                v8::String::NewFromUtf8(isolate, uv_strerror(status),
                                        v8::NewStringType::kNormal)
                    .ToLocalChecked()));
    NetCloseSocket(sock);
    return;
  }
  sock->connected = true;
  uv_read_start(reinterpret_cast<uv_stream_t*>(&sock->handle), NetAlloc, NetOnRead);
  NetEmit(isolate, obj, "connect");
  if (!sock->connect_cb.IsEmpty()) {
    auto cb = sock->connect_cb.Get(isolate);
    v8::TryCatch tc(isolate);
    v8::Local<v8::Value> ignored;
    (void)cb->Call(isolate->GetCurrentContext(), obj, 0, nullptr).ToLocal(&ignored);
  }
}

static void NetOnClientResolved(uv_getaddrinfo_t* dns, int status, struct addrinfo* res) {
  auto* sock = static_cast<NetSocket*>(dns->data);
  if (status < 0 || !res) {
    EnterIsolate scope(nx8v::g_runtime->isolate);
    if (!sock->js.IsEmpty()) {
      NetEmit(scope.isolate, sock->js.Get(scope.isolate), "error",
              v8::Exception::Error(v8::String::NewFromUtf8Literal(
                  scope.isolate, "DNS lookup failed")));
    }
    if (res) uv_freeaddrinfo(res);
    NetCloseSocket(sock);
    return;
  }
  sock->connect_req.data = sock;
  int r = uv_tcp_connect(&sock->connect_req, &sock->handle, res->ai_addr,
                         NetOnClientConnect);
  uv_freeaddrinfo(res);
  if (r < 0) NetCloseSocket(sock);
}

static void NetConnect(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  if (nx8v::g_runtime && !nx8v::g_runtime->allow_net) {
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(isolate, "network access is not allowed")));
    return;
  }
  v8::HandleScope hs(isolate);
  auto ctx = isolate->GetCurrentContext();
  if (args.Length() < 1 || !args[0]->IsNumber()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "net.connect(port[, host][, cb])")));
    return;
  }
  auto* sock = new NetSocket();
  sock->port = args[0]->Int32Value(ctx).FromMaybe(0);
  int cb_index = 1;
  if (args.Length() >= 2 && args[1]->IsString()) {
    v8::String::Utf8Value h(isolate, args[1]);
    sock->host = *h ? *h : "127.0.0.1";
    cb_index = 2;
  }
  uv_tcp_init(nx8v::g_runtime->loop, &sock->handle);
  sock->handle.data = sock;
  auto obj = NetMakeSocketObject(isolate, sock);
  if (args.Length() > cb_index && args[cb_index]->IsFunction())
    sock->connect_cb.Reset(isolate, args[cb_index].As<v8::Function>());

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  std::string port = std::to_string(sock->port);
  sock->dns_req.data = sock;
  int r = uv_getaddrinfo(nx8v::g_runtime->loop, &sock->dns_req, NetOnClientResolved,
                         sock->host.c_str(), port.c_str(), &hints);
  if (r < 0) NetCloseSocket(sock);
  args.GetReturnValue().Set(obj);
}

static void NetCreateServer(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  if (nx8v::g_runtime && !nx8v::g_runtime->allow_net) {
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(isolate, "network access is not allowed")));
    return;
  }
  v8::HandleScope hs(isolate);
  auto ctx = isolate->GetCurrentContext();
  auto* srv = new NetServer();
  uv_tcp_init(nx8v::g_runtime->loop, &srv->handle);
  srv->handle.data = srv;
  if (args.Length() >= 1 && args[0]->IsFunction())
    srv->connection_cb.Reset(isolate, args[0].As<v8::Function>());

  auto obj = v8::Object::New(isolate);
  MakeEmitterMethods(isolate, obj);
  srv->js.Reset(isolate, obj);

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "listen"),
           v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value>& a) {
             v8::Isolate* iso = a.GetIsolate();
             auto c = iso->GetCurrentContext();
             auto* srv = static_cast<NetServer*>(
                 a.This()
                     ->GetAlignedPointerFromInternalField(
                         0, v8::kEmbedderDataTypeTagDefault));
             if (!srv) return;
             int port = a.Length() >= 1 ? a[0]->Int32Value(c).FromMaybe(0) : 0;
             if (a.Length() >= 2 && a[1]->IsFunction())
               srv->listen_cb.Reset(iso, a[1].As<v8::Function>());
             struct sockaddr_in addr;
             uv_ip4_addr("0.0.0.0", port, &addr);
             int r = uv_tcp_bind(&srv->handle, reinterpret_cast<sockaddr*>(&addr), 0);
             if (r < 0) {
               iso->ThrowException(v8::Exception::Error(
                   v8::String::NewFromUtf8(iso, uv_strerror(r),
                                           v8::NewStringType::kNormal)
                       .ToLocalChecked()));
               return;
             }
             r = uv_listen(reinterpret_cast<uv_stream_t*>(&srv->handle), 128,
                           NetOnConnection);
             if (r < 0) {
               iso->ThrowException(v8::Exception::Error(
                   v8::String::NewFromUtf8(iso, uv_strerror(r),
                                           v8::NewStringType::kNormal)
                       .ToLocalChecked()));
               return;
             }
             if (!srv->listen_cb.IsEmpty()) {
               auto cb = srv->listen_cb.Get(iso);
               v8::TryCatch tc(iso);
               v8::Local<v8::Value> ignored;
               (void)cb->Call(c, a.This(), 0, nullptr).ToLocal(&ignored);
             }
           }).ToLocalChecked())
      .Check();

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "close"),
           v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value>& a) {
             auto* srv = static_cast<NetServer*>(
                 a.This()->GetAlignedPointerFromInternalField(
                     0, v8::kEmbedderDataTypeTagDefault));
             if (!srv || srv->closing) return;
             srv->closing = true;
             uv_close(reinterpret_cast<uv_handle_t*>(&srv->handle), NetFreeServer);
           }).ToLocalChecked())
      .Check();

  // Internal field lives on a template instance; plain objects have none.
  // Keep the server pointer on the handle and look it up from listen via
  // a side table keyed by the JS object identity is heavier, so stash it
  // in an external on a private symbol through the aligned field of a
  // dedicated template object used as `this` for listen/close.
  auto tmpl = v8::ObjectTemplate::New(isolate);
  tmpl->SetInternalFieldCount(1);
  auto self = tmpl->NewInstance(ctx).ToLocalChecked();
  self->SetAlignedPointerInInternalField(0, srv, v8::kEmbedderDataTypeTagDefault);
  MakeEmitterMethods(isolate, self);
  // Re-bind listen/close onto the object that actually has the field.
  auto listen = obj->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "listen"))
                    .ToLocalChecked();
  auto close = obj->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "close"))
                   .ToLocalChecked();
  self->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "listen"), listen).Check();
  self->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "close"), close).Check();
  srv->js.Reset(isolate, self);
  args.GetReturnValue().Set(self);
}

inline v8::Local<v8::Object> MakeNetModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  auto ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "createServer"),
           v8::Function::New(ctx, NetCreateServer).ToLocalChecked())
      .Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "connect"),
           v8::Function::New(ctx, NetConnect).ToLocalChecked())
      .Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "createConnection"),
           v8::Function::New(ctx, NetConnect).ToLocalChecked())
      .Check();
  return hs.Escape(obj);
}

}}  // nx8v::bindings
