#pragma once
// nx8v/src/bindings/process.h
// Exposes the `process` global object:
//   process.argv       - array of args (argv[0]=executable, argv[1]=script, ...)
//   process.env        - object of environment variables
//   process.exit(code) - terminate with exit code
//   process.version    - NX8V version string
//   process.platform   - "win32" | "linux" | "darwin"

#include <v8.h>
#include <uv.h>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>

#include "../runtime.h"

namespace fs = std::filesystem;

#ifdef _WIN32
#  include <windows.h>    // GetEnvironmentStringsW
#else
#  include <unistd.h>
extern char** environ;
#endif

namespace nx8v {
namespace bindings {

#ifdef _WIN32
// Convert a UTF-16 string to UTF-8.
static std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                              nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                      out.data(), n, nullptr, nullptr);
  return out;
}
#endif

static void ProcessExit(const v8::FunctionCallbackInfo<v8::Value>& args) {
  int code = 0;
  if (args.Length() > 0 && args[0]->IsInt32()) {
    code = args[0].As<v8::Int32>()->Value();
  }
  std::exit(code);
}

static void ProcessCwd(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  std::error_code ec;
  fs::path cwd = fs::current_path(ec);
  std::string s = ec ? std::string(".") : cwd.string();
  args.GetReturnValue().Set(
      v8::String::NewFromUtf8(isolate, s.c_str(), v8::NewStringType::kNormal,
                              (int)s.size())
          .ToLocalChecked());
}

static void ProcessNextTick(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  if (args.Length() < 1 || !args[0]->IsFunction()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "nextTick requires a function")));
    return;
  }
  auto* t = new nx8v::Runtime::NextTickTask();
  t->callback.Reset(isolate, args[0].As<v8::Function>());
  for (int i = 1; i < args.Length(); ++i) {
    t->args.emplace_back();
    t->args.back().Reset(isolate, args[i]);
  }
  nx8v::g_runtime->next_ticks.push_back(t);
  if (nx8v::g_runtime->next_tick_prepare)
    uv_ref(reinterpret_cast<uv_handle_t*>(nx8v::g_runtime->next_tick_prepare));
}

inline v8::Local<v8::Object> MakeProcessObject(v8::Isolate* isolate,
                                                int argc, char** argv) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);

  // --- process.version ---
  obj->Set(ctx,
    v8::String::NewFromUtf8Literal(isolate, "version"),
    v8::String::NewFromUtf8Literal(isolate, "nx8v/0.2.2")).Check();
  // --- process.platform ---
#ifdef _WIN32
  const char* platform_str = "win32";
#elif defined(__APPLE__)
  const char* platform_str = "darwin";
#else
  const char* platform_str = "linux";
#endif
  obj->Set(ctx,
    v8::String::NewFromUtf8Literal(isolate, "platform"),
    v8::String::NewFromUtf8(isolate, platform_str,
                            v8::NewStringType::kNormal).ToLocalChecked())
    .Check();

  // --- process.argv ---
  auto argv_arr = v8::Array::New(isolate, argc);
  for (int i = 0; i < argc; ++i) {
    argv_arr->Set(ctx, i,
      v8::String::NewFromUtf8(isolate, argv[i],
                              v8::NewStringType::kNormal).ToLocalChecked())
      .Check();
  }
  obj->Set(ctx,
    v8::String::NewFromUtf8Literal(isolate, "argv"),
    argv_arr).Check();

  // --- process.env ---
  auto env_obj = v8::Object::New(isolate);
#ifdef _WIN32
  {
    // GetEnvironmentStrings() maps to the wide (W) variant because UNICODE is
    // defined, so read the block as UTF-16 and convert each entry to UTF-8.
    LPWCH envblk = GetEnvironmentStringsW();
    if (envblk) {
      for (LPWCH p = envblk; *p; p += wcslen(p) + 1) {
        std::wstring entry(p);
        auto eq = entry.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;
        std::string key = WideToUtf8(entry.substr(0, eq));
        std::string val = WideToUtf8(entry.substr(eq + 1));
        auto k = v8::String::NewFromUtf8(isolate, key.c_str(),
                                         v8::NewStringType::kNormal);
        auto v = v8::String::NewFromUtf8(isolate, val.c_str(),
                                         v8::NewStringType::kNormal);
        v8::Local<v8::String> kl, vl;
        if (k.ToLocal(&kl) && v.ToLocal(&vl))
          env_obj->Set(ctx, kl, vl).Check();
      }
      FreeEnvironmentStringsW(envblk);
    }
  }
#else
  for (char** ep = environ; ep && *ep; ++ep) {
    std::string entry(*ep);
    auto eq = entry.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    std::string key = entry.substr(0, eq);
    std::string val = entry.substr(eq + 1);
    auto k = v8::String::NewFromUtf8(isolate, key.c_str(), v8::NewStringType::kNormal);
    auto v = v8::String::NewFromUtf8(isolate, val.c_str(), v8::NewStringType::kNormal);
    v8::Local<v8::String> kl, vl;
    if (k.ToLocal(&kl) && v.ToLocal(&vl))
      env_obj->Set(ctx, kl, vl).Check();
  }
#endif
  obj->Set(ctx,
    v8::String::NewFromUtf8Literal(isolate, "env"),
    env_obj).Check();

  // --- process.exit / cwd / nextTick ---
  // exitCode is left unset so a later assignment is detectable.
  obj->Set(ctx,
    v8::String::NewFromUtf8Literal(isolate, "exit"),
    v8::Function::New(ctx, ProcessExit).ToLocalChecked()).Check();
  obj->Set(ctx,
    v8::String::NewFromUtf8Literal(isolate, "cwd"),
    v8::Function::New(ctx, ProcessCwd).ToLocalChecked()).Check();
  obj->Set(ctx,
    v8::String::NewFromUtf8Literal(isolate, "nextTick"),
    v8::Function::New(ctx, ProcessNextTick).ToLocalChecked()).Check();

  return hs.Escape(obj);
}

}} // nx8v::bindings
