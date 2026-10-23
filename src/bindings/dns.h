#pragma once
// nx8v/src/bindings/dns.h
// require('dns')
//   dns.lookup(host, cb)            -> cb(err, address, family)
//   dns.resolve(host, cb)           -> cb(err, addresses[])
//   dns.resolve4(host, cb)          -> same, IPv4 only

#include <v8.h>
#include <uv.h>

#include <cstring>
#include <string>
#include <vector>

#include "../runtime.h"

namespace nx8v {
namespace bindings {

struct DnsReq {
  uv_getaddrinfo_t req;
  v8::Global<v8::Function> callback;
  bool v4_only = false;
  bool lookup = false;  // first address only
};

static void DnsOnResolved(uv_getaddrinfo_t* req, int status, struct addrinfo* res) {
  auto* st = static_cast<DnsReq*>(req->data);
  EnterIsolate scope(nx8v::g_runtime->isolate);
  v8::Isolate* isolate = scope.isolate;
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::Local<v8::Function> cb = st->callback.Get(isolate);

  if (status < 0 || !res) {
    std::string msg = std::string("DNS lookup failed: ") + uv_strerror(status);
    v8::Local<v8::Value> argv[] = {
        v8::Exception::Error(v8::String::NewFromUtf8(isolate, msg.c_str(),
                                                     v8::NewStringType::kNormal)
                                 .ToLocalChecked()),
        v8::Undefined(isolate)};
    v8::TryCatch tc(isolate);
    v8::Local<v8::Value> ignored;
    (void)cb->Call(ctx, ctx->Global(), 2, argv).ToLocal(&ignored);
  } else if (st->lookup) {
    char host[NI_MAXHOST];
    host[0] = 0;
    int family = 0;
    for (auto* ai = res; ai; ai = ai->ai_next) {
      if (st->v4_only && ai->ai_family != AF_INET) continue;
      if (uv_ip_name(ai->ai_addr, host, sizeof(host)) == 0) {
        family = ai->ai_family == AF_INET6 ? 6 : 4;
        break;
      }
    }
    v8::Local<v8::Value> argv[] = {
        v8::Null(isolate),
        v8::String::NewFromUtf8(isolate, host, v8::NewStringType::kNormal)
            .ToLocalChecked(),
        v8::Integer::New(isolate, family)};
    v8::TryCatch tc(isolate);
    v8::Local<v8::Value> ignored;
    (void)cb->Call(ctx, ctx->Global(), 3, argv).ToLocal(&ignored);
  } else {
    auto arr = v8::Array::New(isolate);
    uint32_t n = 0;
    for (auto* ai = res; ai; ai = ai->ai_next) {
      if (st->v4_only && ai->ai_family != AF_INET) continue;
      char host[NI_MAXHOST];
      if (uv_ip_name(ai->ai_addr, host, sizeof(host)) != 0) continue;
      arr->Set(ctx, n++,
               v8::String::NewFromUtf8(isolate, host, v8::NewStringType::kNormal)
                   .ToLocalChecked())
          .Check();
    }
    v8::Local<v8::Value> argv[] = {v8::Null(isolate), arr};
    v8::TryCatch tc(isolate);
    v8::Local<v8::Value> ignored;
    (void)cb->Call(ctx, ctx->Global(), 2, argv).ToLocal(&ignored);
  }

  if (res) uv_freeaddrinfo(res);
  st->callback.Reset();
  delete st;
}

static void DnsStart(const v8::FunctionCallbackInfo<v8::Value>& args,
                     bool lookup, bool v4_only) {
  v8::Isolate* isolate = args.GetIsolate();
  if (nx8v::g_runtime && !nx8v::g_runtime->allow_net) {
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(isolate, "network access is not allowed")));
    return;
  }
  v8::HandleScope hs(isolate);
  if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsFunction()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "dns: (host, callback) required")));
    return;
  }
  v8::String::Utf8Value host(isolate, args[0]);
  auto* st = new DnsReq();
  st->lookup = lookup;
  st->v4_only = v4_only;
  st->callback.Reset(isolate, args[1].As<v8::Function>());
  st->req.data = st;

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = v4_only ? AF_INET : AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  int r = uv_getaddrinfo(nx8v::g_runtime->loop, &st->req, DnsOnResolved,
                         *host ? *host : "", nullptr, &hints);
  if (r < 0) {
    st->callback.Reset();
    delete st;
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8(isolate, uv_strerror(r), v8::NewStringType::kNormal)
            .ToLocalChecked()));
  }
}

inline v8::Local<v8::Object> MakeDnsModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  auto ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);
  auto def = [&](const char* name, v8::FunctionCallback cb) {
    obj->Set(ctx,
             v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal)
                 .ToLocalChecked(),
             v8::Function::New(ctx, cb).ToLocalChecked())
        .Check();
  };
  def("lookup", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    DnsStart(a, true, false);
  });
  def("resolve", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    DnsStart(a, false, false);
  });
  def("resolve4", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    DnsStart(a, false, true);
  });
  return hs.Escape(obj);
}

}}  // nx8v::bindings
