#pragma once
// nx8v/src/runtime.h
// Shared runtime state for NX8V.
//
// Event loop is driven by libuv (same foundation as Node.js).
// Timers, and future async I/O, are libuv handles.

#include <v8.h>
#include <uv.h>

#include <string>
#include <map>
#include <vector>
#include <cstdio>

namespace nx8v {

// ---------------------------------------------------------------------------
// Timer wrapper: owns a uv_timer_t handle plus the JS callback.
// Heap-allocated because libuv keeps a raw pointer to the handle.
// ---------------------------------------------------------------------------
struct Timer {
  uv_timer_t               handle;
  v8::Global<v8::Function> callback;
  bool                     repeat = false;
  int                      id     = 0;
};

// ---------------------------------------------------------------------------
// Runtime: global state shared across bindings.
// ---------------------------------------------------------------------------
struct Runtime {
  v8::Isolate*            isolate = nullptr;
  v8::Global<v8::Context> context;
  uv_loop_t*              loop = nullptr;

  // original argc/argv from main()
  int    argc = 0;
  char** argv = nullptr;

  // Directory of the entry script (used by path.resolve / require).
  std::string script_dir;

  // Active timers keyed by id (for clearTimeout/clearInterval).
  std::map<int, Timer*> timers;
  int next_timer_id = 1;

  // uv_check handle that pumps V8 microtasks each loop iteration.
  uv_check_t* microtask_check = nullptr;

  // Module cache: resolved_path -> exports value.
  std::map<std::string, v8::Global<v8::Value>> module_cache;

  // process.nextTick queue. Drained from a uv_prepare handle.
  struct NextTickTask {
    v8::Global<v8::Function> callback;
    std::vector<v8::Global<v8::Value>> args;
  };
  std::vector<NextTickTask*> next_ticks;
  uv_prepare_t* next_tick_prepare = nullptr;

  // console.* appends here when a project.json is found.
  std::string log_path;
};

// Global runtime instance (one per process).
extern Runtime* g_runtime;

// ---------------------------------------------------------------------------
// RAII helper: enter the isolate + context for a libuv completion callback.
//
// libuv invokes completion callbacks on the loop thread (the same thread that
// owns the isolate), so no v8::Locker is required — but we still need a
// HandleScope and a Context::Scope.
//
// Usage:
//     static void OnSomething(uv_fs_t* req) {
//       nx8v::EnterIsolate scope(nx8v::g_runtime->isolate);
//       // ... V8 API calls ...
//     }
// ---------------------------------------------------------------------------
struct EnterIsolate {
  v8::Isolate*            isolate;
  v8::HandleScope         handle_scope;
  v8::Local<v8::Context>  context;
  v8::Context::Scope      context_scope;

  explicit EnterIsolate(v8::Isolate* iso)
      : isolate(iso),
        handle_scope(iso),
        context(g_runtime->context.Get(iso)),
        context_scope(context) {}
};

// ---------------------------------------------------------------------------
// Invoke a JS callback with (err, result). Reports uncaught exceptions.
// Must be called with the isolate + context already entered.
// ---------------------------------------------------------------------------
inline void CallJsCallback(v8::Isolate* isolate,
                           v8::Local<v8::Function> cb,
                           v8::Local<v8::Value> err,
                           v8::Local<v8::Value> result) {
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::TryCatch tc(isolate);
  v8::Local<v8::Value> argv[2] = { err, result };
  v8::Local<v8::Value> ignored;
  if (!cb->Call(ctx, ctx->Global(), 2, argv).ToLocal(&ignored) ||
      tc.HasCaught()) {
    v8::String::Utf8Value ex(isolate, tc.Exception());
    std::fprintf(stderr, "Uncaught (callback): %s\n", *ex ? *ex : "?");
  }
}

} // namespace nx8v
