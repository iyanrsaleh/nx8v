#pragma once
// nx8v/src/bindings/module.h
// CommonJS-style require() for local .js files.
//
// Supports:
//   require('./relative/path')   - relative to the requiring file
//   require('/absolute/path')    - absolute path
//   Built-in stubs: require('path'), require('os')
//
// Each module gets its own (module, exports, require, __filename, __dirname).
// Results are cached by resolved path.

#include <v8.h>
#include <string>
#include <cstdio>
#include <filesystem>

#include "../runtime.h"
#include "../utils.h"
#include "fs.h"
#include "os.h"
#include "url.h"
#include "events.h"
#include "http.h"
#include "buffer.h"
#include "crypto.h"
#include "path.h"
#include "ws.h"
#include "router.h"
#include "streams.h"
#include "esm.h"
#include "static.h"
#include "compress.h"
#include "querystring.h"
#include "dns.h"
#include "net.h"
#include "child_process.h"

namespace nx8v {
namespace bindings {

// Forward declaration
static v8::Local<v8::Value> LoadModule(v8::Isolate* isolate,
                                       const std::string& resolved_path);

// ---------------------------------------------------------------------------
// Built-in modules (stubs)
// ---------------------------------------------------------------------------
static v8::Local<v8::Value> LoadBuiltin(v8::Isolate* isolate,
                                        const std::string& name) {
  v8::EscapableHandleScope hs(isolate);

  if (name == "fs") {
    return hs.Escape(nx8v::bindings::MakeFsModule(isolate));
  }
  if (name == "os") {
    return hs.Escape(nx8v::bindings::MakeOsModule(isolate));
  }
  if (name == "url") {
    return hs.Escape(nx8v::bindings::MakeUrlModule(isolate));
  }
  if (name == "http") {
    return hs.Escape(nx8v::bindings::MakeHttpModule(isolate));
  }
  if (name == "https") {
    return hs.Escape(nx8v::bindings::MakeHttpModule(isolate));
  }
  if (name == "buffer") {
    return hs.Escape(nx8v::bindings::MakeBufferModule(isolate));
  }
  if (name == "crypto") {
    return hs.Escape(nx8v::bindings::MakeCryptoModule(isolate));
  }
  if (name == "ws") {
    return hs.Escape(nx8v::bindings::MakeWsModule(isolate));
  }
  if (name == "router") {
    return hs.Escape(nx8v::bindings::MakeRouter(isolate));
  }
  if (name == "stream") {
    return hs.Escape(nx8v::bindings::MakeStreamModule(isolate));
  }
  if (name == "static") {
    return hs.Escape(nx8v::bindings::MakeStaticModule(isolate));
  }
  if (name == "events") {
    // module.exports = EventEmitter (constructor) + .EventEmitter
    auto ctor = v8::Function::New(
        isolate->GetCurrentContext(),
        [](const v8::FunctionCallbackInfo<v8::Value>& args) {
          v8::Isolate* iso = args.GetIsolate();
          v8::HandleScope h(iso);
          v8::Local<v8::Object> self = args.IsConstructCall()
              ? args.This()
              : v8::Object::New(iso);
          MakeEmitterMethods(iso, self);
          args.GetReturnValue().Set(self);
        }).ToLocalChecked();
    ctor->Set(isolate->GetCurrentContext(),
              v8::String::NewFromUtf8Literal(isolate, "EventEmitter"), ctor).Check();
    return hs.Escape(ctor.As<v8::Value>());
  }

  if (name == "path") {
    return hs.Escape(nx8v::bindings::MakePathModule(isolate));
  }
  if (name == "zlib") {
    return hs.Escape(nx8v::bindings::MakeZlibModule(isolate));
  }
  if (name == "querystring") {
    return hs.Escape(nx8v::bindings::MakeQueryStringModule(isolate));
  }
  if (name == "dns") {
    return hs.Escape(nx8v::bindings::MakeDnsModule(isolate));
  }
  if (name == "net") {
    return hs.Escape(nx8v::bindings::MakeNetModule(isolate));
  }
  if (name == "child_process") {
    return hs.Escape(nx8v::bindings::MakeChildProcessModule(isolate));
  }

  return hs.Escape(v8::Object::New(isolate));
}

// ---------------------------------------------------------------------------
// Built-in module registry
// ---------------------------------------------------------------------------
static bool IsBuiltin(const std::string& name) {
  return name == "path" || name == "os" || name == "fs" ||
         name == "url"  || name == "events" || name == "http" ||
         name == "buffer" || name == "crypto" || name == "ws" ||
         name == "router" || name == "stream" || name == "static" ||
         name == "https" || name == "zlib" || name == "querystring" ||
         name == "dns" || name == "net" || name == "child_process";
}

// ---------------------------------------------------------------------------
// Resolve module path relative to the calling file
// ---------------------------------------------------------------------------
static std::string ResolvePath(const std::string& specifier,
                               const std::string& from_dir) {
  // Built-ins: no path resolution
  if (IsBuiltin(specifier)) return "";

  fs::path p(specifier);
  if (!p.is_absolute()) {
    p = fs::path(from_dir) / p;
  }

  // Try as-is, then with .js, then folder/index.js
  if (fs::exists(p) && !fs::is_directory(p))
    return fs::absolute(p).string();
  fs::path pjs = p;
  pjs += ".js";
  if (fs::exists(pjs) && !fs::is_directory(pjs))
    return fs::absolute(pjs).string();
  if (fs::is_directory(p)) {
    fs::path index = p / "index.js";
    if (fs::exists(index) && !fs::is_directory(index))
      return fs::absolute(index).string();
  }

  return "";
}

// ---------------------------------------------------------------------------
// Create a require() function bound to a specific __dirname
// ---------------------------------------------------------------------------
struct RequireData {
  std::string current_dir;
};

static void RequireCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);

  if (args.Length() < 1 || !args[0]->IsString()) {
    isolate->ThrowException(v8::Exception::TypeError(
      v8::String::NewFromUtf8Literal(isolate, "require: argument must be a string")));
    return;
  }

  v8::String::Utf8Value specifier(isolate, args[0]);
  std::string spec(*specifier ? *specifier : "");

  // Retrieve the RequireData stored in the function's external data
  auto ext = args.DataV2().As<v8::External>();
  auto* data = reinterpret_cast<RequireData*>(
      ext->Value(v8::kExternalPointerTypeTagDefault));

  // Built-ins (cached so repeated require() returns the same object)
  if (IsBuiltin(spec)) {
    std::string key = "\0builtin:" + spec;
    auto it = g_runtime->module_cache.find(key);
    if (it != g_runtime->module_cache.end()) {
      args.GetReturnValue().Set(it->second.Get(isolate));
      return;
    }
    v8::Local<v8::Value> builtin = LoadBuiltin(isolate, spec);
    g_runtime->module_cache[key].Reset(isolate, builtin);
    args.GetReturnValue().Set(builtin);
    return;
  }

  // Resolve path
  std::string resolved = ResolvePath(spec, data->current_dir);
  if (resolved.empty()) {
    std::string err = "Cannot find module '" + spec + "'";
    isolate->ThrowException(v8::Exception::Error(
      v8::String::NewFromUtf8(isolate, err.c_str(),
                              v8::NewStringType::kNormal).ToLocalChecked()));
    return;
  }

  // Cache hit
  auto cache_it = g_runtime->module_cache.find(resolved);
  if (cache_it != g_runtime->module_cache.end()) {
    args.GetReturnValue().Set(cache_it->second.Get(isolate));
    return;
  }

  // Load and execute
  v8::Local<v8::Value> exports = LoadModule(isolate, resolved);
  args.GetReturnValue().Set(exports);
}

// ---------------------------------------------------------------------------
// Load a .js file as a CommonJS module
// ---------------------------------------------------------------------------
static v8::Local<v8::Value> LoadModule(v8::Isolate* isolate,
                                       const std::string& resolved_path) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  std::string source = ReadFile(resolved_path.c_str());
  if (source.empty()) {
    std::string err = "Cannot read module '" + resolved_path + "'";
    isolate->ThrowException(v8::Exception::Error(
      v8::String::NewFromUtf8(isolate, err.c_str(),
                              v8::NewStringType::kNormal).ToLocalChecked()));
    return hs.Escape(v8::Undefined(isolate));
  }

  // ES module? (.mjs, or a .js file containing import/export statements)
  bool is_esm = false;
  if (resolved_path.size() >= 4 &&
      resolved_path.compare(resolved_path.size() - 4, 4, ".mjs") == 0) {
    is_esm = true;
  } else {
    is_esm = EsmLooksLikeModule(source);
  }
  if (is_esm) source = EsmTransform(source);

  // Wrap in CommonJS wrapper
  //   (function(module, exports, require, __filename, __dirname) { <source> })
  std::string wrapped =
    "(function(module, exports, require, __filename, __dirname) {\n" +
    source + "\n})";

  v8::Local<v8::String> src_str =
    v8::String::NewFromUtf8(isolate, wrapped.c_str(),
                            v8::NewStringType::kNormal,
                            (int)wrapped.size()).ToLocalChecked();

  v8::ScriptOrigin origin(
    v8::String::NewFromUtf8(isolate, resolved_path.c_str(),
                            v8::NewStringType::kNormal).ToLocalChecked());

  v8::TryCatch tc(isolate);
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(ctx, src_str, &origin).ToLocal(&script)) {
    ReportException(isolate, &tc);
    return hs.Escape(v8::Undefined(isolate));
  }

  v8::Local<v8::Value> wrapper_val;
  if (!script->Run(ctx).ToLocal(&wrapper_val) || !wrapper_val->IsFunction()) {
    ReportException(isolate, &tc);
    return hs.Escape(v8::Undefined(isolate));
  }

  auto wrapper_fn = wrapper_val.As<v8::Function>();

  // Build module object
  auto module_obj  = v8::Object::New(isolate);
  auto exports_obj = v8::Object::New(isolate);
  module_obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "exports"),
                  exports_obj).Check();

  // Build require() for this module
  std::string dir = fs::path(resolved_path).parent_path().string();
  auto* req_data  = new RequireData{ dir };
  auto  ext_data  = v8::External::New(isolate, req_data,
                                      v8::kExternalPointerTypeTagDefault);
  auto  req_fn    = v8::Function::New(ctx, RequireCallback, ext_data)
                      .ToLocalChecked();

  // __filename, __dirname
  auto filename_str = v8::String::NewFromUtf8(isolate, resolved_path.c_str(),
                                              v8::NewStringType::kNormal).ToLocalChecked();
  auto dirname_str  = v8::String::NewFromUtf8(isolate, dir.c_str(),
                                              v8::NewStringType::kNormal).ToLocalChecked();

  // Place in module cache before execution (handle circular requires)
  g_runtime->module_cache[resolved_path].Reset(isolate, exports_obj);

  // Call wrapper
  v8::Local<v8::Value> argv[] = {
    module_obj, exports_obj, req_fn, filename_str, dirname_str
  };
  if (!wrapper_fn->Call(ctx, ctx->Global(), 5, argv).IsEmpty()) {
    // module.exports may have been replaced
    v8::Local<v8::Value> new_exports =
      module_obj->Get(ctx,
        v8::String::NewFromUtf8Literal(isolate, "exports")).ToLocalChecked();
    g_runtime->module_cache[resolved_path].Reset(isolate, new_exports);
    return hs.Escape(new_exports);
  } else {
    ReportException(isolate, &tc);
    return hs.Escape(v8::Undefined(isolate));
  }
}

// ---------------------------------------------------------------------------
// Create the top-level require() function (bound to the script's directory)
// ---------------------------------------------------------------------------
inline v8::Local<v8::Function> MakeRequire(v8::Isolate* isolate,
                                           const std::string& script_dir) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto* data   = new RequireData{ script_dir };
  auto  ext    = v8::External::New(isolate, data,
                                   v8::kExternalPointerTypeTagDefault);
  auto  req_fn = v8::Function::New(ctx, RequireCallback, ext).ToLocalChecked();
  return hs.Escape(req_fn);
}

// ---------------------------------------------------------------------------
// Execute an ES module entry point (wrapped so `exports`/`require` exist).
// ---------------------------------------------------------------------------
inline bool ExecuteModuleEntry(v8::Isolate* isolate, const std::string& source,
                               const std::string& path) {
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  std::string transformed = EsmTransform(source);
  std::string wrapped =
      "(function(module, exports, require, __filename, __dirname) {\n" +
      transformed + "\n})";

  v8::Local<v8::String> src_str =
      v8::String::NewFromUtf8(isolate, wrapped.c_str(),
                              v8::NewStringType::kNormal,
                              (int)wrapped.size()).ToLocalChecked();
  v8::ScriptOrigin origin(v8::String::NewFromUtf8(
      isolate, path.c_str(), v8::NewStringType::kNormal).ToLocalChecked());

  v8::TryCatch tc(isolate);
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(ctx, src_str, &origin).ToLocal(&script)) {
    ReportException(isolate, &tc);
    return false;
  }

  v8::Local<v8::Value> fn_val;
  if (!script->Run(ctx).ToLocal(&fn_val) || !fn_val->IsFunction()) {
    ReportException(isolate, &tc);
    return false;
  }

  auto module_obj  = v8::Object::New(isolate);
  auto exports_obj = v8::Object::New(isolate);
  module_obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "exports"),
                  exports_obj).Check();

  std::string dir = fs::path(path).parent_path().string();
  auto* req_data = new RequireData{ dir };
  auto ext_data = v8::External::New(isolate, req_data,
                                    v8::kExternalPointerTypeTagDefault);
  auto req_fn = v8::Function::New(ctx, RequireCallback, ext_data).ToLocalChecked();

  v8::Local<v8::Value> argv[] = {
    module_obj, exports_obj, req_fn,
    v8::String::NewFromUtf8(isolate, path.c_str(),
                            v8::NewStringType::kNormal).ToLocalChecked(),
    v8::String::NewFromUtf8(isolate, dir.c_str(),
                            v8::NewStringType::kNormal).ToLocalChecked() };
  v8::Local<v8::Value> ignored;
  if (!fn_val.As<v8::Function>()->Call(ctx, ctx->Global(), 5, argv).ToLocal(&ignored)) {
    ReportException(isolate, &tc);
    return false;
  }
  return true;
}

}} // nx8v::bindings
