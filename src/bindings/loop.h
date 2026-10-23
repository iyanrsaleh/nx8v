#pragma once
// nx8v/src/bindings/loop.h
// Event loop helpers built on libuv.
//
// Responsibilities:
//   1. Pump V8 microtasks (Promise .then callbacks) on every loop iteration
//      via a uv_check_t handle that is *unref'd* so it never keeps the loop
//      alive on its own.
//   2. Run the loop until no active handles remain (uv_run UV_RUN_DEFAULT).
//   3. Tear the loop down cleanly.

#include <v8.h>
#include <uv.h>
#include <cstdio>
#include <vector>

#include "../runtime.h"

namespace nx8v {
namespace bindings {

// ---------------------------------------------------------------------------
// process.nextTick: run before the poll phase so callbacks are not stuck
// behind a blocking I/O wait.
// ---------------------------------------------------------------------------
inline void DrainNextTicks() {
  if (!nx8v::g_runtime || !nx8v::g_runtime->isolate) return;
  v8::Isolate* isolate = nx8v::g_runtime->isolate;
  int guard = 0;
  while (!nx8v::g_runtime->next_ticks.empty() && guard++ < 10000) {
    auto* t = nx8v::g_runtime->next_ticks.front();
    nx8v::g_runtime->next_ticks.erase(nx8v::g_runtime->next_ticks.begin());
    {
      EnterIsolate scope(isolate);
      v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
      v8::Local<v8::Function> cb = t->callback.Get(isolate);
      std::vector<v8::Local<v8::Value>> argv;
      argv.reserve(t->args.size());
      for (auto& g : t->args) argv.push_back(g.Get(isolate));
      v8::TryCatch tc(isolate);
      v8::Local<v8::Value> ignored;
      if (!cb.IsEmpty()) {
        (void)cb->Call(ctx, ctx->Global(), (int)argv.size(),
                       argv.empty() ? nullptr : argv.data())
            .ToLocal(&ignored);
      }
      if (tc.HasCaught()) {
        v8::String::Utf8Value ex(isolate, tc.Exception());
        std::fprintf(stderr, "Uncaught (nextTick): %s\n", *ex ? *ex : "?");
      }
    }
    t->callback.Reset();
    for (auto& g : t->args) g.Reset();
    delete t;
  }
  if (nx8v::g_runtime->next_tick_prepare) {
    auto* h = reinterpret_cast<uv_handle_t*>(nx8v::g_runtime->next_tick_prepare);
    if (nx8v::g_runtime->next_ticks.empty()) uv_unref(h);
    else uv_ref(h);
  }
}

static void OnNextTickPrepare(uv_prepare_t*) { DrainNextTicks(); }

static void OnPrepareClose(uv_handle_t* handle) {
  delete reinterpret_cast<uv_prepare_t*>(handle);
}

// ---------------------------------------------------------------------------
// Microtask pump: called once per libuv iteration (check phase).
// ---------------------------------------------------------------------------
static void OnMicrotaskCheck(uv_check_t* handle) {
  auto* isolate = static_cast<v8::Isolate*>(handle->data);
  isolate->PerformMicrotaskCheckpoint();
}

// ---------------------------------------------------------------------------
// Close callback used when tearing down the microtask pump handle.
// ---------------------------------------------------------------------------
static void OnCheckClose(uv_handle_t* handle) {
  delete reinterpret_cast<uv_check_t*>(handle);
}

// ---------------------------------------------------------------------------
// Install the microtask pump on the loop.
// ---------------------------------------------------------------------------
inline void SetupMicrotaskPump(v8::Isolate* isolate, uv_loop_t* loop) {
  auto* check = new uv_check_t;
  uv_check_init(loop, check);
  check->data = isolate;
  uv_check_start(check, OnMicrotaskCheck);

  // Unref so the pump alone does not keep the process alive.
  uv_unref(reinterpret_cast<uv_handle_t*>(check));

  nx8v::g_runtime->microtask_check = check;

  auto* prep = new uv_prepare_t;
  uv_prepare_init(loop, prep);
  uv_prepare_start(prep, OnNextTickPrepare);
  uv_unref(reinterpret_cast<uv_handle_t*>(prep));
  nx8v::g_runtime->next_tick_prepare = prep;
}

// ---------------------------------------------------------------------------
// Run the event loop until all handles are closed.
// ---------------------------------------------------------------------------
inline void RunEventLoop(v8::Isolate* isolate, uv_loop_t* loop) {
  // nextTick and microtasks produced by the top-level script.
  DrainNextTicks();
  isolate->PerformMicrotaskCheckpoint();

  uv_run(loop, UV_RUN_DEFAULT);

  DrainNextTicks();
  isolate->PerformMicrotaskCheckpoint();
}

// ---------------------------------------------------------------------------
// Tear down the microtask pump and close the loop.
// Must be called after RunEventLoop().
// ---------------------------------------------------------------------------
// Close every handle so a desktop window can end the process. The HTTP
// server would otherwise keep uv_run() blocked after the window is gone.
static void OnReleaseHandle(uv_handle_t* handle) {
  if (!nx8v::g_runtime) return;
  if (handle == reinterpret_cast<uv_handle_t*>(nx8v::g_runtime->microtask_check)) {
    delete nx8v::g_runtime->microtask_check;
    nx8v::g_runtime->microtask_check = nullptr;
  } else if (handle ==
             reinterpret_cast<uv_handle_t*>(nx8v::g_runtime->next_tick_prepare)) {
    delete nx8v::g_runtime->next_tick_prepare;
    nx8v::g_runtime->next_tick_prepare = nullptr;
  }
}

inline void ReleaseLoopHandles(uv_loop_t* loop) {
  uv_walk(loop, [](uv_handle_t* h, void*) {
    if (!uv_is_closing(h)) uv_close(h, OnReleaseHandle);
  }, nullptr);
  uv_run(loop, UV_RUN_DEFAULT);
}

inline void TeardownLoop(uv_loop_t* loop) {
  bool pending = false;
  if (nx8v::g_runtime->microtask_check) {
    uv_close(reinterpret_cast<uv_handle_t*>(nx8v::g_runtime->microtask_check),
             OnCheckClose);
    nx8v::g_runtime->microtask_check = nullptr;
    pending = true;
  }
  if (nx8v::g_runtime->next_tick_prepare) {
    uv_prepare_stop(nx8v::g_runtime->next_tick_prepare);
    uv_close(reinterpret_cast<uv_handle_t*>(nx8v::g_runtime->next_tick_prepare),
             OnPrepareClose);
    nx8v::g_runtime->next_tick_prepare = nullptr;
    pending = true;
  }
  if (pending) uv_run(loop, UV_RUN_DEFAULT);
}

}} // nx8v::bindings
