#pragma once
// nx8v/src/bindings/router.h
// Minimal Express-style router on top of the http server.
//
//   const router = require('router');
//   const app = router();
//
//   app.get('/user/:id', (req, res) => res.json({ id: req.params.id }));
//   app.post('/api/login', (req, res) => { ... });
//   app.use((req, res, next) => { ... });       // middleware
//   app.use('/api', (req, res, next) => { ... }); // path-scoped middleware
//   app.notFound((req, res) => res.status(404).json({ error: 'not found' }));
//
//   app.listen(3000);
//
// Route params:   /user/:id          -> req.params.id
// Optional param: /file/:name?       -> req.params.name may be undefined
// Wildcard:       /static/*          -> req.params[0]
//
// Response helpers added by the router:
//   res.status(code) / res.json(obj) / res.send(str|Buffer) / res.redirect(url)

#include <v8.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../runtime.h"
#include "events.h"
#include "http.h"

namespace nx8v {
namespace bindings {

// ---------------------------------------------------------------------------
// Route definition
// ---------------------------------------------------------------------------
struct RouteSeg {
  std::string literal;   // empty when dynamic
  std::string param;     // ":id" -> "id"
  bool        dynamic = false;
  bool        optional = false;
  bool        wildcard = false;
};

struct Route {
  std::string              method;   // GET/POST/... or "*"
  std::vector<RouteSeg>    segs;
  v8::Global<v8::Function> handler;
  bool                     is_middleware = false;
  std::string              scope;    // for use('/api', fn)
};

struct RouterState {
  std::vector<Route>       routes;
  v8::Global<v8::Function> not_found;
  v8::Global<v8::Object>   js_app;
  v8::Global<v8::Object>   server;
  int                      port = 0;
};

// ---------------------------------------------------------------------------
// Path parsing
// ---------------------------------------------------------------------------
static std::vector<RouteSeg> ParseRoutePath(const std::string& path) {
  std::vector<RouteSeg> segs;
  size_t i = 0;
  while (i < path.size() && path[i] == '/') ++i;

  while (i <= path.size()) {
    size_t next = path.find('/', i);
    std::string part = (next == std::string::npos) ? path.substr(i)
                                                   : path.substr(i, next - i);
    if (!part.empty()) {
      RouteSeg s;
      if (part == "*") {
        s.dynamic = true;
        s.wildcard = true;
        s.param = "0";
      } else if (part[0] == ':') {
        s.dynamic = true;
        s.param = part.substr(1);
        if (!s.param.empty() && s.param.back() == '?') {
          s.optional = true;
          s.param.pop_back();
        }
      } else {
        s.literal = part;
      }
      segs.push_back(s);
    }
    if (next == std::string::npos) break;
    i = next + 1;
  }
  return segs;
}

static std::vector<std::string> SplitPath(const std::string& path) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < path.size() && path[i] == '/') ++i;
  while (i <= path.size()) {
    size_t next = path.find('/', i);
    std::string part = (next == std::string::npos) ? path.substr(i)
                                                   : path.substr(i, next - i);
    if (!part.empty()) out.push_back(part);
    if (next == std::string::npos) break;
    i = next + 1;
  }
  return out;
}

// Match route segments against URL parts. Fills params on success.
static bool MatchRoute(const std::vector<RouteSeg>& segs,
                       const std::vector<std::string>& parts,
                       std::map<std::string, std::string>& params) {
  size_t pi = 0;
  for (size_t si = 0; si < segs.size(); ++si) {
    const RouteSeg& s = segs[si];
    if (s.wildcard) {
      std::string rest;
      for (size_t k = pi; k < parts.size(); ++k) {
        if (!rest.empty()) rest += "/";
        rest += parts[k];
      }
      params[s.param] = rest;
      return true;
    }
    if (pi >= parts.size()) {
      if (s.optional) continue;
      return false;
    }
    if (s.dynamic) {
      params[s.param] = parts[pi++];
    } else if (s.literal == parts[pi]) {
      ++pi;
    } else {
      return false;
    }
  }
  return pi == parts.size();
}

// ---------------------------------------------------------------------------
// Response helpers: status/json/send/redirect
// ---------------------------------------------------------------------------
// Internal slots on the res object created by http.h: index 0 = ResState*.
// We add our own methods that forward to writeHead/end.
static void RouterResStatus(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  if (args.Length() >= 1 && args[0]->IsNumber()) {
    args.This()
        ->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "statusCode"), args[0])
        .Check();
  }
  args.GetReturnValue().Set(args.This());
}

static void RouterResSend(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  v8::Local<v8::Object> self = args.This();
  std::string body;
  if (args.Length() >= 1) {
    if (args[0]->IsString()) {
      v8::String::Utf8Value s(isolate, args[0]);
      body.assign(*s ? *s : "", s.length());
    } else {
      size_t len = 0;
      uint8_t* data = BufBytes(isolate, args[0], &len);
      if (data) body.assign((char*)data, len);
    }
  }

  auto get = [&](const char* name) {
    return self->Get(ctx, v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal, (int)std::strlen(name)).ToLocalChecked())
        .ToLocalChecked();
  };

  auto code_v = get("statusCode");
  int code = code_v->IsNumber() ? code_v->Int32Value(ctx).FromMaybe(200) : 200;

  // Default content type if the user did not set one.
  auto set_header_v = get("setHeader");
  if (set_header_v->IsFunction()) {
    auto get_header_v = get("getHeader");
    bool has_type = false;
    if (get_header_v->IsFunction()) {
      v8::Local<v8::Value> argv[] = {
        v8::String::NewFromUtf8Literal(isolate, "Content-Type") };
      v8::Local<v8::Value> got;
      if (get_header_v.As<v8::Function>()
              ->Call(ctx, self, 1, argv)
              .ToLocal(&got) &&
          got->IsString()) {
        v8::String::Utf8Value s(isolate, got);
        has_type = s.length() > 0;
      }
    }
    if (!has_type) {
      v8::Local<v8::Value> argv[] = {
        v8::String::NewFromUtf8Literal(isolate, "Content-Type"),
        v8::String::NewFromUtf8Literal(isolate, "text/plain; charset=utf-8") };
      v8::Local<v8::Value> ignored;
      (void)set_header_v.As<v8::Function>()
          ->Call(ctx, self, 2, argv)
          .ToLocal(&ignored);
    }
  }

  auto write_head_v = get("writeHead");
  if (write_head_v->IsFunction()) {
    v8::Local<v8::Value> argv[] = { v8::Integer::New(isolate, code) };
    v8::Local<v8::Value> ignored;
    (void)write_head_v.As<v8::Function>()->Call(ctx, self, 1, argv).ToLocal(&ignored);
  }

  auto end_v = get("end");
  if (end_v->IsFunction()) {
    v8::Local<v8::Value> argv[] = {
      v8::String::NewFromUtf8(isolate, body.data(), v8::NewStringType::kNormal,
                              (int)body.size()).ToLocalChecked() };
    v8::Local<v8::Value> ignored;
    (void)end_v.As<v8::Function>()->Call(ctx, self, 1, argv).ToLocal(&ignored);
  }
  args.GetReturnValue().Set(self);
}

static void RouterResJson(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::Local<v8::Object> self = args.This();

  v8::Local<v8::Value> json;
  if (args.Length() >= 1) {
    if (!v8::JSON::Stringify(ctx, args[0]).ToLocal(&json)) {
      json = v8::String::NewFromUtf8Literal(isolate, "null");
    }
  } else {
    json = v8::String::NewFromUtf8Literal(isolate, "null");
  }

  auto set_header_v =
      self->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "setHeader"))
          .ToLocalChecked();
  if (set_header_v->IsFunction()) {
    v8::Local<v8::Value> argv[] = {
      v8::String::NewFromUtf8Literal(isolate, "Content-Type"),
      v8::String::NewFromUtf8Literal(isolate, "application/json; charset=utf-8") };
    v8::Local<v8::Value> ignored;
    (void)set_header_v.As<v8::Function>()->Call(ctx, self, 2, argv).ToLocal(&ignored);
  }

  auto send_v = self->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "send"))
                    .ToLocalChecked();
  if (send_v->IsFunction()) {
    v8::Local<v8::Value> argv[] = { json };
    v8::Local<v8::Value> ignored;
    (void)send_v.As<v8::Function>()->Call(ctx, self, 1, argv).ToLocal(&ignored);
  }
  args.GetReturnValue().Set(self);
}

static void RouterResRedirect(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::Local<v8::Object> self = args.This();

  std::string loc;
  int code = 302;
  if (args.Length() >= 1 && args[0]->IsString()) {
    v8::String::Utf8Value s(isolate, args[0]);
    loc = *s ? *s : "";
  }
  if (args.Length() >= 2 && args[1]->IsNumber())
    code = args[1]->Int32Value(ctx).FromMaybe(302);

  auto set_header_v =
      self->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "setHeader"))
          .ToLocalChecked();
  if (set_header_v->IsFunction()) {
    v8::Local<v8::Value> argv[] = {
      v8::String::NewFromUtf8Literal(isolate, "Location"),
      v8::String::NewFromUtf8(isolate, loc.c_str(),
                              v8::NewStringType::kNormal).ToLocalChecked() };
    v8::Local<v8::Value> ignored;
    (void)set_header_v.As<v8::Function>()->Call(ctx, self, 2, argv).ToLocal(&ignored);
  }

  auto write_head_v =
      self->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "writeHead"))
          .ToLocalChecked();
  if (write_head_v->IsFunction()) {
    v8::Local<v8::Value> argv[] = { v8::Integer::New(isolate, code) };
    v8::Local<v8::Value> ignored;
    (void)write_head_v.As<v8::Function>()->Call(ctx, self, 1, argv).ToLocal(&ignored);
  }

  auto end_v = self->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "end"))
                   .ToLocalChecked();
  if (end_v->IsFunction()) {
    v8::Local<v8::Value> ignored;
    (void)end_v.As<v8::Function>()->Call(ctx, self, 0, nullptr).ToLocal(&ignored);
  }
  args.GetReturnValue().Set(self);
}

// Attach router helpers to a response object (idempotent).
static void RouterDecorateRes(v8::Isolate* isolate, v8::Local<v8::Object> res) {
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto def = [&](const char* name, v8::FunctionCallback cb) {
    res->Set(ctx, v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal,
                                          (int)std::strlen(name)).ToLocalChecked(),
             v8::Function::New(ctx, cb).ToLocalChecked()).Check();
  };
  def("status", RouterResStatus);
  def("send", RouterResSend);
  def("json", RouterResJson);
  def("redirect", RouterResRedirect);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
static RouterState* RouterFromObject(v8::Local<v8::Object> obj) {
  return reinterpret_cast<RouterState*>(
      obj->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
}

// Runs middleware + matching handler. Returns true when the response was sent.
static bool RouterDispatch(RouterState* st, v8::Isolate* isolate,
                           v8::Local<v8::Object> req, v8::Local<v8::Object> res,
                           const std::string& method, const std::string& path) {
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto parts = SplitPath(path);

  // --- middleware (in registration order) -------------------------------
  for (auto& r : st->routes) {
    if (!r.is_middleware) continue;
    std::vector<std::string> scoped = parts;
    if (!r.scope.empty()) {
      auto scope_parts = SplitPath(r.scope);
      if (scope_parts.size() > parts.size()) continue;
      bool ok = true;
      for (size_t i = 0; i < scope_parts.size(); ++i)
        if (scope_parts[i] != parts[i]) { ok = false; break; }
      if (!ok) continue;
      scoped.assign(parts.begin() + scope_parts.size(), parts.end());
    }
    (void)scoped;

    v8::Local<v8::Function> fn = r.handler.Get(isolate);
    v8::TryCatch tc(isolate);
    v8::Local<v8::Value> argv[] = { req, res };
    v8::Local<v8::Value> result;
    (void)fn->Call(ctx, v8::Undefined(isolate), 2, argv).ToLocal(&result);
    if (tc.HasCaught()) return true;   // handler already reported/threw
    // A middleware that returned false stops the chain (and owns the response).
    if (result->IsBoolean() && !result.As<v8::Boolean>()->Value()) return true;
  }

  // --- routes -----------------------------------------------------------
  for (auto& r : st->routes) {
    if (r.is_middleware) continue;
    if (r.method != "*" && r.method != method) continue;

    std::map<std::string, std::string> params;
    if (!MatchRoute(r.segs, parts, params)) continue;

    // req.params = {...}
    auto params_obj = v8::Object::New(isolate);
    for (auto& kv : params) {
      params_obj->Set(ctx,
        v8::String::NewFromUtf8(isolate, kv.first.c_str(),
                                v8::NewStringType::kNormal).ToLocalChecked(),
        v8::String::NewFromUtf8(isolate, kv.second.c_str(),
                                v8::NewStringType::kNormal).ToLocalChecked()).Check();
    }
    req->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "params"), params_obj).Check();
    req->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "path"), 
             v8::String::NewFromUtf8(isolate, path.c_str(),
                                     v8::NewStringType::kNormal).ToLocalChecked()).Check();

    RouterDecorateRes(isolate, res);

    v8::Local<v8::Function> fn = r.handler.Get(isolate);
    v8::TryCatch tc(isolate);
    v8::Local<v8::Value> argv[] = { req, res };
    v8::Local<v8::Value> ignored;
    (void)fn->Call(ctx, v8::Undefined(isolate), 2, argv).ToLocal(&ignored);
    if (tc.HasCaught()) {
      v8::String::Utf8Value ex(isolate, tc.Exception());
      std::fprintf(stderr, "Router handler error: %s\n", *ex ? *ex : "?");
      // Try to answer 500 if nothing was sent yet.
      auto end_v = res->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "end"))
                       .ToLocalChecked();
      if (end_v->IsFunction()) {
        v8::Local<v8::Value> err_argv[] = {
          v8::String::NewFromUtf8Literal(isolate, "500 Internal Server Error") };
        v8::Local<v8::Value> e2;
        (void)end_v.As<v8::Function>()->Call(ctx, res, 1, err_argv).ToLocal(&e2);
      }
    }
    return true;
  }

  return false;
}

// Called by the underlying http server for every request.
static void RouterOnRequest(RouterState* st, v8::Local<v8::Object> req,
                            v8::Local<v8::Object> res) {
  v8::Isolate* isolate = nx8v::g_runtime->isolate;
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto method_v = req->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "method"))
                      .ToLocalChecked();
  auto url_v = req->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "url"))
                   .ToLocalChecked();
  v8::String::Utf8Value method_s(isolate, method_v);
  v8::String::Utf8Value url_s(isolate, url_v);

  std::string method = *method_s ? *method_s : "GET";
  std::string url = *url_s ? *url_s : "/";
  auto q = url.find('?');
  if (q != std::string::npos) url = url.substr(0, q);

  RouterDecorateRes(isolate, res);

  if (RouterDispatch(st, isolate, req, res, method, url)) return;

  // No route matched -> notFound handler, else 404 JSON.
  if (!st->not_found.IsEmpty()) {
    v8::Local<v8::Function> fn = st->not_found.Get(isolate);
    v8::Local<v8::Value> argv[] = { req, res };
    v8::Local<v8::Value> ignored;
    (void)fn->Call(ctx, v8::Undefined(isolate), 2, argv).ToLocal(&ignored);
    return;
  }

  auto json_v = res->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "json"))
                    .ToLocalChecked();
  if (json_v->IsFunction()) {
    auto status_v = res->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "status"))
                        .ToLocalChecked();
    if (status_v->IsFunction()) {
      v8::Local<v8::Value> sargv[] = { v8::Integer::New(isolate, 404) };
      v8::Local<v8::Value> sres;
      (void)status_v.As<v8::Function>()->Call(ctx, res, 1, sargv).ToLocal(&sres);
    }
    auto err = v8::Object::New(isolate);
    err->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "error"),
             v8::String::NewFromUtf8Literal(isolate, "Not Found")).Check();
    err->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "path"),
             v8::String::NewFromUtf8(isolate, url.c_str(),
                                     v8::NewStringType::kNormal).ToLocalChecked()).Check();
    v8::Local<v8::Value> argv[] = { err };
    v8::Local<v8::Value> ignored;
    (void)json_v.As<v8::Function>()->Call(ctx, res, 1, argv).ToLocal(&ignored);
  }
}

// ---------------------------------------------------------------------------
// Registration helpers
// ---------------------------------------------------------------------------
static void RouterRegister(v8::Isolate* isolate, v8::Local<v8::Object> self,
                           const std::string& method, const std::string& path,
                           v8::Local<v8::Function> handler) {
  auto* st = RouterFromObject(self);
  if (!st) return;
  Route r;
  r.method = method;
  r.segs = ParseRoutePath(path);
  r.handler.Reset(isolate, handler);
  st->routes.push_back(std::move(r));
}

// app.get(path, handler) / app.post(...) / app.put / app.delete / app.all
static void RouterMethodHandler(const v8::FunctionCallbackInfo<v8::Value>& args,
                                const char* method) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);

  if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsFunction()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "route(path, handler) required")));
    return;
  }
  v8::String::Utf8Value p(isolate, args[0]);
  RouterRegister(isolate, args.This(), method, *p ? *p : "/",
                 args[1].As<v8::Function>());
  args.GetReturnValue().Set(args.This());
}

// app.use([path,] fn)
static void RouterUse(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);

  auto* st = RouterFromObject(args.This());
  if (!st) return;

  std::string scope;
  v8::Local<v8::Function> fn;
  for (int i = 0; i < args.Length(); ++i) {
    if (args[i]->IsString()) {
      v8::String::Utf8Value s(isolate, args[i]);
      scope = *s ? *s : "";
    } else if (args[i]->IsFunction()) {
      fn = args[i].As<v8::Function>();
    }
  }
  if (fn.IsEmpty()) return;

  Route r;
  r.is_middleware = true;
  r.scope = scope;
  r.method = "*";
  r.handler.Reset(isolate, fn);
  st->routes.push_back(std::move(r));
  args.GetReturnValue().Set(args.This());
}

// app.notFound(fn)
static void RouterNotFound(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  auto* st = RouterFromObject(args.This());
  if (!st || args.Length() < 1 || !args[0]->IsFunction()) return;
  st->not_found.Reset(isolate, args[0].As<v8::Function>());
  args.GetReturnValue().Set(args.This());
}

// app.listen(port[, cb])
static void RouterListen(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto* st = RouterFromObject(args.This());
  if (!st) return;

  int port = 0;
  if (args.Length() >= 1 && args[0]->IsNumber())
    port = args[0]->Int32Value(ctx).FromMaybe(0);
  if (port <= 0) port = 3000;

  // Build an http server whose 'request' listener dispatches into the router.
  auto server = nx8v::bindings::MakeHttpServerObject(isolate);
  st->server.Reset(isolate, server);

  auto on_v = server->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "on"))
                  .ToLocalChecked();
  if (on_v->IsFunction()) {
    auto req_handler = v8::Function::New(ctx, [](
        const v8::FunctionCallbackInfo<v8::Value>& a) {
      v8::Isolate* iso = a.GetIsolate();
      v8::HandleScope h(iso);
      auto* state = RouterFromObject(a.DataV2().As<v8::Object>());
      if (!state || a.Length() < 2) return;
      RouterOnRequest(state, a[0].As<v8::Object>(), a[1].As<v8::Object>());
    }, args.This()).ToLocalChecked();

    v8::Local<v8::Value> argv[] = {
      v8::String::NewFromUtf8Literal(isolate, "request"), req_handler };
    v8::Local<v8::Value> ignored;
    (void)on_v.As<v8::Function>()->Call(ctx, server, 2, argv).ToLocal(&ignored);
  }

  // server.listen(port, cb)
  auto listen_v = server->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "listen"))
                      .ToLocalChecked();
  if (listen_v->IsFunction()) {
    std::vector<v8::Local<v8::Value>> argv;
    argv.push_back(v8::Integer::New(isolate, port));
    if (args.Length() >= 2 && args[1]->IsFunction()) argv.push_back(args[1]);
    v8::Local<v8::Value> ignored;
    (void)listen_v.As<v8::Function>()
        ->Call(ctx, server, (int)argv.size(), argv.data())
        .ToLocal(&ignored);
  }

  args.GetReturnValue().Set(server);
}

// ---------------------------------------------------------------------------
// router() factory
// ---------------------------------------------------------------------------
inline v8::Local<v8::Object> CreateRouterApp(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto* st = new RouterState();

  auto tmpl = v8::ObjectTemplate::New(isolate);
  tmpl->SetInternalFieldCount(1);
  auto app = tmpl->NewInstance(ctx).ToLocalChecked();
  app->SetAlignedPointerInInternalField(0, st, v8::kEmbedderDataTypeTagDefault);
  st->js_app.Reset(isolate, app);

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    app->Set(ctx, v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal,
                                          (int)std::strlen(name)).ToLocalChecked(),
             v8::Function::New(ctx, cb).ToLocalChecked()).Check();
  };

  def("get", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    RouterMethodHandler(a, "GET");
  });
  def("post", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    RouterMethodHandler(a, "POST");
  });
  def("put", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    RouterMethodHandler(a, "PUT");
  });
  def("delete", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    RouterMethodHandler(a, "DELETE");
  });
  def("patch", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    RouterMethodHandler(a, "PATCH");
  });
  def("all", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    RouterMethodHandler(a, "*");
  });
  def("use", RouterUse);
  def("notFound", RouterNotFound);
  def("listen", RouterListen);

  return hs.Escape(app);
}

// module.exports = function router() { return app }
inline v8::Local<v8::Value> MakeRouter(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto factory = v8::Function::New(ctx, [](
      const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    args.GetReturnValue().Set(CreateRouterApp(iso));
  }).ToLocalChecked();

  return hs.Escape(factory.As<v8::Value>());
}

}} // nx8v::bindings
