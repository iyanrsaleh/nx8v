#pragma once
// nx8v/src/bindings/timers.h
// Non-blocking setTimeout / clearTimeout / setInterval / clearInterval
// backed by libuv uv_timer_t handles.
//
// Each JS call to setTimeout allocates a Timer wrapper holding a uv_timer_t.
// libuv keeps the event loop alive while any timer is active, and uv_run()
// returns only after every handle has been closed — no manual sleep loop.

#include <v8.h>
#include <uv.h>
#include <cstdio>
#include <algorithm>

#include "../runtime.h"

namespace nx8v {
namespace bindings {

// ---------------------------------------------------------------------------
// Close callback: frees the Timer wrapper once libuv is done with the handle.
// ---------------------------------------------------------------------------
static void OnTimerClose(uv_handle_t* handle) {
  delete static_cast<nx8v::Timer*>(handle->data);
}

// ---------------------------------------------------------------------------
// Fires when a timer expires.
// ---------------------------------------------------------------------------
static void OnTimerFire(uv_timer_t* handle) {
  auto* t = static_cast<nx8v::Timer*>(handle->data);
  v8::Isolate* isolate = nx8v::g_runtime->isolate;

  v8::HandleScope     hs(isolate);
  v8::Local<v8::Context> ctx = nx8v::g_runtime->context.Get(isolate);
  v8::Context::Scope  cs(ctx);
  v8::TryCatch        tc(isolate);

  v8::Local<v8::Function> cb = t->callback.Get(isolate);
  if (!cb.IsEmpty()) {
    v8::Local<v8::Value> ignored;
    if (!cb->Call(ctx, ctx->Global(), 0, nullptr).ToLocal(&ignored)) {
      // exception captured by tc below
    }
  }

  if (tc.HasCaught()) {
    v8::String::Utf8Value ex(isolate, tc.Exception());
    std::fprintf(stderr, "Uncaught (timer): %s\n", *ex ? *ex : "?");
  }

  if (!t->repeat) {
    // One-shot: remove from registry and close the handle.
    nx8v::g_runtime->timers.erase(t->id);
    uv_timer_stop(handle);
    uv_close(reinterpret_cast<uv_handle_t*>(handle), OnTimerClose);
  }
}

// ---------------------------------------------------------------------------
// Internal helper: create and start a timer.
// ---------------------------------------------------------------------------
static int _StartTimer(v8::Isolate* isolate,
                       v8::Local<v8::Function> fn,
                       int delay,
                       bool repeat) {
  auto* t   = new nx8v::Timer();
  t->id     = nx8v::g_runtime->next_timer_id++;
  t->repeat = repeat;
  t->callback.Reset(isolate, fn);

  uv_timer_init(nx8v::g_runtime->loop, &t->handle);
  t->handle.data = t;

  nx8v::g_runtime->timers[t->id] = t;

  // repeat == 0 for one-shot; repeat == delay for intervals.
  uv_timer_start(&t->handle, OnTimerFire,
                 static_cast<uint64_t>(delay),
                 repeat ? static_cast<uint64_t>(delay) : 0);
  return t->id;
}

// ---------------------------------------------------------------------------
// JS: setTimeout(fn, delayMs)
// ---------------------------------------------------------------------------
static void SetTimeout(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);

  if (args.Length() < 1 || !args[0]->IsFunction()) {
    isolate->ThrowException(v8::Exception::TypeError(
      v8::String::NewFromUtf8Literal(isolate,
        "setTimeout: first argument must be a function")));
    return;
  }

  int delay = 0;
  if (args.Length() >= 2 && args[1]->IsNumber()) {
    delay = static_cast<int>(args[1].As<v8::Number>()->Value());
    delay = std::max(0, delay);
  }

  int id = _StartTimer(isolate, args[0].As<v8::Function>(), delay,
                       /*repeat=*/false);
  args.GetReturnValue().Set(v8::Integer::New(isolate, id));
}

// ---------------------------------------------------------------------------
// JS: setInterval(fn, delayMs)
// ---------------------------------------------------------------------------
static void SetInterval(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);

  if (args.Length() < 1 || !args[0]->IsFunction()) {
    isolate->ThrowException(v8::Exception::TypeError(
      v8::String::NewFromUtf8Literal(isolate,
        "setInterval: first argument must be a function")));
    return;
  }

  int delay = 0;
  if (args.Length() >= 2 && args[1]->IsNumber()) {
    delay = static_cast<int>(args[1].As<v8::Number>()->Value());
    delay = std::max(1, delay);   // interval must be > 0
  }

  int id = _StartTimer(isolate, args[0].As<v8::Function>(), delay,
                       /*repeat=*/true);
  args.GetReturnValue().Set(v8::Integer::New(isolate, id));
}

// ---------------------------------------------------------------------------
// JS: clearTimeout(id) / clearInterval(id)  (same implementation)
// ---------------------------------------------------------------------------
static void ClearTimer(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);

  if (args.Length() < 1 || !args[0]->IsInt32()) return;
  int id = args[0].As<v8::Int32>()->Value();

  auto it = nx8v::g_runtime->timers.find(id);
  if (it == nx8v::g_runtime->timers.end()) return;

  nx8v::Timer* t = it->second;
  nx8v::g_runtime->timers.erase(it);

  uv_timer_stop(&t->handle);
  t->callback.Reset();  // release the V8 persistent handle early
  uv_close(reinterpret_cast<uv_handle_t*>(&t->handle), OnTimerClose);
}

// ---------------------------------------------------------------------------
// Register all timer globals.
// ---------------------------------------------------------------------------
inline void RegisterTimers(v8::Isolate* isolate,
                           v8::Local<v8::ObjectTemplate>& global) {
  global->Set(isolate, "setTimeout",
    v8::FunctionTemplate::New(isolate, SetTimeout));
  global->Set(isolate, "setInterval",
    v8::FunctionTemplate::New(isolate, SetInterval));
  global->Set(isolate, "clearTimeout",
    v8::FunctionTemplate::New(isolate, ClearTimer));
  global->Set(isolate, "clearInterval",
    v8::FunctionTemplate::New(isolate, ClearTimer));
}

}} // nx8v::bindings
