#pragma once
// nx8v/src/bindings/events.h
// Minimal EventEmitter compatible with the Node.js shape:
//   emitter.on(event, listener)      -> this
//   emitter.once(event, listener)    -> this
//   emitter.off / removeListener     -> this
//   emitter.emit(event, ...args)     -> bool (true if a listener ran)
//   emitter.removeAllListeners([event]) -> this
//   emitter.listenerCount(event)     -> number
//   emitter.listeners(event)         -> array
//   emitter.eventNames()             -> array
//
// Listeners and the emitter use plain V8 objects; per-emitter state lives in
// a Private property so user objects cannot collide with it.

#include <v8.h>
#include <string>
#include <vector>

namespace nx8v {
namespace bindings {

// Private key slots stored on the emitter object.
inline v8::Local<v8::Private> EmitterEventsKey(v8::Isolate* isolate) {
  return v8::Private::ForApi(
      isolate,
      v8::String::NewFromUtf8Literal(isolate, "nx8v::EventEmitter::events"));
}
inline v8::Local<v8::Private> EmitterOnceKey(v8::Isolate* isolate) {
  return v8::Private::ForApi(
      isolate,
      v8::String::NewFromUtf8Literal(isolate, "nx8v::EventEmitter::once"));
}

inline v8::Local<v8::Array> GetEventList(v8::Isolate* isolate,
                                         v8::Local<v8::Context> ctx,
                                         v8::Local<v8::Object> self,
                                         const char* event) {
  auto store = self->GetPrivate(ctx, EmitterEventsKey(isolate))
                   .ToLocalChecked().As<v8::Object>();
  return store->Get(ctx,
      v8::String::NewFromUtf8(isolate, event, v8::NewStringType::kNormal)
          .ToLocalChecked()).ToLocalChecked().As<v8::Array>();
}

inline v8::Local<v8::Array> EnsureEventList(v8::Isolate* isolate,
                                            v8::Local<v8::Context> ctx,
                                            v8::Local<v8::Object> self,
                                            const char* event) {
  auto store = self->GetPrivate(ctx, EmitterEventsKey(isolate))
                   .ToLocalChecked().As<v8::Object>();
  v8::Local<v8::String> key =
      v8::String::NewFromUtf8(isolate, event, v8::NewStringType::kNormal)
          .ToLocalChecked();
  v8::Local<v8::Value> existing = store->Get(ctx, key).ToLocalChecked();
  if (existing->IsArray()) return existing.As<v8::Array>();
  auto arr = v8::Array::New(isolate);
  store->Set(ctx, key, arr).Check();
  return arr;
}

inline void AddListener(v8::Isolate* isolate,
                        v8::Local<v8::Context> ctx,
                        v8::Local<v8::Object> self,
                        const char* event,
                        v8::Local<v8::Value> listener,
                        bool once) {
  auto arr = EnsureEventList(isolate, ctx, self, event);
  arr->Set(ctx, arr->Length(), listener).Check();
  if (once) {
    auto once_store = self->GetPrivate(ctx, EmitterOnceKey(isolate))
                          .ToLocalChecked().As<v8::Object>();
    v8::Local<v8::Value> prev =
        once_store->Get(ctx, arr).ToLocalChecked();
    int n = prev->IsNumber() ? (int)prev.As<v8::Number>()->Value() : 0;
    once_store->Set(ctx, arr, v8::Integer::New(isolate, n + 1)).Check();
  }
}

inline void RemoveListener(v8::Isolate* isolate,
                           v8::Local<v8::Context> ctx,
                           v8::Local<v8::Object> self,
                           const char* event,
                           v8::Local<v8::Value> listener) {
  auto arr = GetEventList(isolate, ctx, self, event);
  if (arr.IsEmpty()) return;
  int len = (int)arr->Length();
  for (int i = 0; i < len; ++i) {
    v8::Local<v8::Value> item = arr->Get(ctx, i).ToLocalChecked();
    if (item->StrictEquals(listener)) {
      for (int j = i; j < len - 1; ++j)
        arr->Set(ctx, j, arr->Get(ctx, j + 1).ToLocalChecked()).Check();
      arr->Set(ctx, len - 1, v8::Undefined(isolate)).Check();
      arr->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "length"),
               v8::Integer::New(isolate, len - 1)).Check();
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Install the EventEmitter methods onto an object.
// ---------------------------------------------------------------------------
inline v8::Local<v8::Object> MakeEmitterMethods(v8::Isolate* isolate,
                                                v8::Local<v8::Object> obj) {
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  // initialize the private stores
  obj->SetPrivate(ctx, EmitterEventsKey(isolate), v8::Object::New(isolate)).Check();
  obj->SetPrivate(ctx, EmitterOnceKey(isolate),   v8::Object::New(isolate)).Check();

  auto on = v8::Function::New(
      ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* iso = args.GetIsolate();
        v8::HandleScope hs(iso);
        auto ctx = iso->GetCurrentContext();
        auto self = args.This();
        if (args.Length() < 2 || !args[0]->IsString()) {
          iso->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
              iso, "event name and listener required")));
          args.GetReturnValue().Set(v8::Undefined(iso));
          return;
        }
        v8::String::Utf8Value ev(iso, args[0]);
        AddListener(iso, ctx, self, *ev ? *ev : "", args[1], false);
        args.GetReturnValue().Set(self);
      }).ToLocalChecked();

  auto once = v8::Function::New(
      ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* iso = args.GetIsolate();
        v8::HandleScope hs(iso);
        auto ctx = iso->GetCurrentContext();
        auto self = args.This();
        if (args.Length() < 2 || !args[0]->IsString()) {
          iso->ThrowException(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
              iso, "event name and listener required")));
          args.GetReturnValue().Set(v8::Undefined(iso));
          return;
        }
        v8::String::Utf8Value ev(iso, args[0]);
        AddListener(iso, ctx, self, *ev ? *ev : "", args[1], true);
        args.GetReturnValue().Set(self);
      }).ToLocalChecked();

  auto emit = v8::Function::New(
      ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* iso = args.GetIsolate();
        v8::HandleScope hs(iso);
        auto ctx = iso->GetCurrentContext();
        auto self = args.This();
        if (args.Length() < 1 || !args[0]->IsString()) {
          args.GetReturnValue().Set(false);
          return;
        }
        v8::String::Utf8Value ev(iso, args[0]);
        std::string event = *ev ? *ev : "";

        auto arr = GetEventList(iso, ctx, self, event.c_str());
        if (arr.IsEmpty() || arr->Length() == 0) {
          // Node's special-case: emitting 'error' with no listener throws.
          if (event == "error") {
            if (args.Length() > 1)
              iso->ThrowException(args[1]);
            else
              iso->ThrowException(
                  v8::Exception::Error(v8::String::NewFromUtf8Literal(
                      iso, "Unhandled 'error' event")));
            return;
          }
          args.GetReturnValue().Set(false);
          return;
        }

        // Snapshot listeners + once flags before invoking (listeners may mutate).
        std::vector<v8::Global<v8::Value>> list;
        std::vector<bool> once_flags;
        int n = (int)arr->Length();
        for (int i = 0; i < n; ++i)
          list.emplace_back(iso, arr->Get(ctx, i).ToLocalChecked());

        // once map: arrays with a positive count mean "first listener is once"
        auto once_store = self->GetPrivate(ctx, EmitterOnceKey(iso))
                              .ToLocalChecked().As<v8::Object>();
        v8::Local<v8::Value> once_val = once_store->Get(ctx, arr).ToLocalChecked();
        int once_count = once_val->IsNumber() ? (int)once_val.As<v8::Number>()->Value() : 0;

        // number of args passed to listeners (skip event name)
        int argc = args.Length() - 1;
        std::vector<v8::Local<v8::Value>> argv;
        argv.reserve(argc);
        for (int i = 0; i < argc; ++i) argv.push_back(args[i + 1]);

        int ran = 0;
        for (size_t i = 0; i < list.size(); ++i) {
          v8::Local<v8::Value> fn = list[i].Get(iso);
          if (!fn->IsFunction()) continue;
          v8::TryCatch tc(iso);
          v8::Local<v8::Value> ignored;
          bool ok = fn.As<v8::Function>()
                        ->Call(ctx, self, argc, argv.empty() ? nullptr : argv.data())
                        .ToLocal(&ignored);
          (void)ok;
          if (tc.HasCaught()) {
            v8::String::Utf8Value ex(iso, tc.Exception());
            std::fprintf(stderr, "Uncaught (event '%s'): %s\n", event.c_str(),
                         *ex ? *ex : "?");
          }
          ran++;
        }

        // Remove once-listeners (they were the first once_count entries).
        if (once_count > 0) {
          auto fresh = EnsureEventList(iso, ctx, self, event.c_str());
          int remaining = 0;
          for (int i = 0; i < (int)fresh->Length(); ++i) {
            v8::Local<v8::Value> item = fresh->Get(ctx, i).ToLocalChecked();
            bool was_once = false;
            for (int k = 0; k < once_count; ++k)
              if (list[k].Get(iso)->StrictEquals(item)) { was_once = true; break; }
            if (!was_once)
              fresh->Set(ctx, remaining++, item).Check();
          }
          fresh->Set(ctx, v8::String::NewFromUtf8Literal(iso, "length"),
                     v8::Integer::New(iso, remaining)).Check();
          once_store->Set(ctx, fresh, v8::Integer::New(iso, 0)).Check();
        }

        args.GetReturnValue().Set(ran > 0);
      }).ToLocalChecked();

  auto remove_listener = v8::Function::New(
      ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* iso = args.GetIsolate();
        v8::HandleScope hs(iso);
        auto ctx = iso->GetCurrentContext();
        auto self = args.This();
        if (args.Length() >= 2 && args[0]->IsString()) {
          v8::String::Utf8Value ev(iso, args[0]);
          RemoveListener(iso, ctx, self, *ev ? *ev : "", args[1]);
        }
        args.GetReturnValue().Set(self);
      }).ToLocalChecked();

  auto remove_all = v8::Function::New(
      ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* iso = args.GetIsolate();
        v8::HandleScope hs(iso);
        auto ctx = iso->GetCurrentContext();
        auto self = args.This();
        auto store = self->GetPrivate(ctx, EmitterEventsKey(iso))
                         .ToLocalChecked().As<v8::Object>();
        if (args.Length() >= 1 && args[0]->IsString()) {
          v8::String::Utf8Value ev(iso, args[0]);
          store->Set(ctx,
              v8::String::NewFromUtf8(iso, *ev ? *ev : "",
                                      v8::NewStringType::kNormal).ToLocalChecked(),
              v8::Array::New(iso)).Check();
        } else {
          auto names = store->GetPropertyNames(ctx).ToLocalChecked();
          for (int i = 0; i < (int)names->Length(); ++i) {
            auto k = names->Get(ctx, i).ToLocalChecked();
            store->Set(ctx, k, v8::Array::New(iso)).Check();
          }
        }
        args.GetReturnValue().Set(self);
      }).ToLocalChecked();

  auto listener_count = v8::Function::New(
      ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* iso = args.GetIsolate();
        v8::HandleScope hs(iso);
        auto ctx = iso->GetCurrentContext();
        auto self = args.This();
        if (args.Length() < 1 || !args[0]->IsString()) {
          args.GetReturnValue().Set(0);
          return;
        }
        v8::String::Utf8Value ev(iso, args[0]);
        auto arr = GetEventList(iso, ctx, self, *ev ? *ev : "");
        args.GetReturnValue().Set(arr.IsEmpty() ? 0 : (int)arr->Length());
      }).ToLocalChecked();

  auto listeners_fn = v8::Function::New(
      ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* iso = args.GetIsolate();
        v8::HandleScope hs(iso);
        auto ctx = iso->GetCurrentContext();
        auto self = args.This();
        if (args.Length() < 1 || !args[0]->IsString()) {
          args.GetReturnValue().Set(v8::Array::New(iso));
          return;
        }
        v8::String::Utf8Value ev(iso, args[0]);
        auto arr = GetEventList(iso, ctx, self, *ev ? *ev : "");
        if (arr.IsEmpty()) { args.GetReturnValue().Set(v8::Array::New(iso)); return; }
        args.GetReturnValue().Set(arr);
      }).ToLocalChecked();

  auto event_names = v8::Function::New(
      ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate* iso = args.GetIsolate();
        v8::HandleScope hs(iso);
        auto ctx = iso->GetCurrentContext();
        auto self = args.This();
        auto store = self->GetPrivate(ctx, EmitterEventsKey(iso))
                         .ToLocalChecked().As<v8::Object>();
        auto names = store->GetPropertyNames(ctx).ToLocalChecked();
        auto out   = v8::Array::New(iso);
        int w = 0;
        for (int i = 0; i < (int)names->Length(); ++i) {
          auto k = names->Get(ctx, i).ToLocalChecked();
          auto v = store->Get(ctx, k).ToLocalChecked();
          if (v->IsArray() && v.As<v8::Array>()->Length() > 0)
            out->Set(ctx, w++, k).Check();
        }
        args.GetReturnValue().Set(out);
      }).ToLocalChecked();

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "on"), on).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "addListener"), on).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "once"), once).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "emit"), emit).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "off"), remove_listener).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "removeListener"), remove_listener).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "removeAllListeners"), remove_all).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "listenerCount"), listener_count).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "listeners"), listeners_fn).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "eventNames"), event_names).Check();

  return obj;
}

} // namespace bindings
} // namespace nx8v
