#pragma once
// nx8v/src/bindings/streams.h
// Minimal Node-style streams built on EventEmitter:
//
//   const { Readable, Writable, PassThrough } = require('stream');
//
//   const r = new Readable();
//   r.on('data', (chunk) => ...);
//   r.on('end', () => ...);
//   r.push('hello'); r.push(null);      // null = EOF
//
//   const w = new Writable();
//   w.on('finish', () => ...);
//   w.write('x'); w.end();
//
//   src.pipe(dest);                     // forwards data and closes dest
//
// fs.createReadStream(path[, opts])     -> Readable (async, chunked)
// fs.createWriteStream(path[, opts])    -> Writable (async append/create)

#include <v8.h>
#include <uv.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../runtime.h"
#include "events.h"

namespace nx8v {
namespace bindings {

// ---------------------------------------------------------------------------
// State shared by all stream flavours
// ---------------------------------------------------------------------------
struct StreamState {
  bool                     readable = true;
  bool                     writable = true;
  bool                     ended    = false;   // EOF reached
  bool                     finished = false;   // writable side closed
  bool                     paused   = false;
  v8::Global<v8::Object>   self;
  v8::Global<v8::Object>   pipe_dest;          // set by pipe()
  int                      chunk_size = 64 * 1024;

  // For fs-backed streams
  uv_file                  fd = -1;
  std::string              path;
  char                     buf[65536];
  uv_fs_t                  req;
};

#ifndef NX8V_CALL_IGNORE_DEFINED
#define NX8V_CALL_IGNORE_DEFINED
// Call a JS function, discarding the result.
static inline void Nx8vCallIgnore(v8::Local<v8::Function> fn,
                                  v8::Local<v8::Context> ctx,
                                  v8::Local<v8::Value> recv,
                                  int argc,
                                  v8::Local<v8::Value>* argv) {
  v8::Local<v8::Value> ignored;
  (void)fn->Call(ctx, recv, argc, argv).ToLocal(&ignored);
}
#endif

static StreamState* StreamFrom(v8::Local<v8::Object> obj) {
  return reinterpret_cast<StreamState*>(
      obj->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault));
}

static void StreamEmit(v8::Isolate* isolate, v8::Local<v8::Object> obj,
                       const char* event, v8::Local<v8::Value> arg =
                                                   v8::Local<v8::Value>()) {
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
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

// Call dest.write(chunk) on the pipe target.
static void StreamPipeWrite(v8::Isolate* isolate, StreamState* st,
                            v8::Local<v8::Value> chunk) {
  if (st->pipe_dest.IsEmpty()) return;
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::Local<v8::Object> dest = st->pipe_dest.Get(isolate);

  auto write_v = dest->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "write"))
                     .ToLocalChecked();
  if (write_v->IsFunction()) {
    v8::Local<v8::Value> argv[] = { chunk };
    v8::Local<v8::Value> ignored;
    (void)write_v.As<v8::Function>()->Call(ctx, dest, 1, argv).ToLocal(&ignored);
  }
}

static void StreamPipeEnd(v8::Isolate* isolate, StreamState* st) {
  if (st->pipe_dest.IsEmpty()) return;
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::Local<v8::Object> dest = st->pipe_dest.Get(isolate);

  auto end_v = dest->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "end"))
                   .ToLocalChecked();
  if (end_v->IsFunction()) {
    v8::Local<v8::Value> ignored;
    (void)end_v.As<v8::Function>()->Call(ctx, dest, 0, nullptr).ToLocal(&ignored);
  }
}

// ---------------------------------------------------------------------------
// Readable: push(chunk) / push(null)
// ---------------------------------------------------------------------------
static void StreamPush(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  auto* st = StreamFrom(args.This());
  if (!st || st->ended) return;

  if (args.Length() < 1 || args[0]->IsNullOrUndefined()) {
    st->ended = true;
    StreamEmit(isolate, args.This(), "end");
    StreamPipeEnd(isolate, st);
    StreamEmit(isolate, args.This(), "close");
    return;
  }

  StreamEmit(isolate, args.This(), "data", args[0]);
  StreamPipeWrite(isolate, st, args[0]);
  args.GetReturnValue().Set(true);
}

// ---------------------------------------------------------------------------
// Writable: write(chunk) / end([chunk])
// ---------------------------------------------------------------------------
static void StreamWrite(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  auto* st = StreamFrom(args.This());
  if (!st || st->finished) { args.GetReturnValue().Set(false); return; }
  if (args.Length() < 1) { args.GetReturnValue().Set(true); return; }

  StreamEmit(isolate, args.This(), "data", args[0]);
  StreamPipeWrite(isolate, st, args[0]);
  args.GetReturnValue().Set(true);
}

static void StreamEnd(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  auto* st = StreamFrom(args.This());
  if (!st || st->finished) return;

  if (args.Length() >= 1 && !args[0]->IsNullOrUndefined()) {
    StreamEmit(isolate, args.This(), "data", args[0]);
    StreamPipeWrite(isolate, st, args[0]);
  }
  st->finished = true;
  st->ended = true;

  StreamEmit(isolate, args.This(), "finish");
  StreamEmit(isolate, args.This(), "end");
  StreamEmit(isolate, args.This(), "close");
  StreamPipeEnd(isolate, st);
}

// ---------------------------------------------------------------------------
// pipe(dest) / pause() / resume()
// ---------------------------------------------------------------------------
static void StreamPipe(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  auto* st = StreamFrom(args.This());
  if (!st || args.Length() < 1 || !args[0]->IsObject()) return;

  st->pipe_dest.Reset(isolate, args[0].As<v8::Object>());
  args.GetReturnValue().Set(args[0]);
}

static void StreamPause(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::HandleScope hs(args.GetIsolate());
  auto* st = StreamFrom(args.This());
  if (st) st->paused = true;
  args.GetReturnValue().Set(args.This());
}

static void StreamResume(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::HandleScope hs(args.GetIsolate());
  auto* st = StreamFrom(args.This());
  if (st) st->paused = false;
  args.GetReturnValue().Set(args.This());
}

// ---------------------------------------------------------------------------
// Object factory
// ---------------------------------------------------------------------------
enum class StreamKind { Readable, Writable, Duplex };

static v8::Local<v8::Object> MakeStreamObject(v8::Isolate* isolate,
                                              StreamKind kind) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto* st = new StreamState();
  st->readable = (kind != StreamKind::Writable);
  st->writable = (kind != StreamKind::Readable);

  auto tmpl = v8::ObjectTemplate::New(isolate);
  tmpl->SetInternalFieldCount(1);
  auto obj = tmpl->NewInstance(ctx).ToLocalChecked();
  obj->SetAlignedPointerInInternalField(0, st, v8::kEmbedderDataTypeTagDefault);
  st->self.Reset(isolate, obj);

  MakeEmitterMethods(isolate, obj);

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    obj->Set(ctx, v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal,
                                          (int)std::strlen(name)).ToLocalChecked(),
             v8::Function::New(ctx, cb).ToLocalChecked()).Check();
  };
  if (st->readable) {
    def("push", StreamPush);
    def("pause", StreamPause);
    def("resume", StreamResume);
    def("pipe", StreamPipe);
  }
  if (st->writable) {
    def("write", StreamWrite);
    def("end", StreamEnd);
  }

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "readable"),
           v8::Boolean::New(isolate, st->readable)).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "writable"),
           v8::Boolean::New(isolate, st->writable)).Check();

  return hs.Escape(obj);
}

// ---------------------------------------------------------------------------
// fs.createReadStream / createWriteStream
// ---------------------------------------------------------------------------
static void StreamReadNext(StreamState* st);

static void StreamReadDone(uv_fs_t* req) {
  auto* st = static_cast<StreamState*>(req->data);
  ssize_t n = req->result;
  uv_fs_req_cleanup(req);

  EnterIsolate scope(nx8v::g_runtime->isolate);
  v8::Isolate* isolate = scope.isolate;
  v8::HandleScope hs(isolate);
  v8::Local<v8::Object> self = st->self.Get(isolate);

  if (n < 0) {
    uv_fs_close(nx8v::g_runtime->loop, &st->req, st->fd, nullptr);
    st->fd = -1;
    StreamEmit(isolate, self, "error",
               v8::Exception::Error(v8::String::NewFromUtf8(
                   isolate, uv_strerror((int)n), v8::NewStringType::kNormal)
                                        .ToLocalChecked()));
    return;
  }
  if (n == 0) {
    uv_fs_close(nx8v::g_runtime->loop, &st->req, st->fd, nullptr);
    st->fd = -1;
    st->ended = true;
    StreamEmit(isolate, self, "end");
    StreamPipeEnd(isolate, st);
    StreamEmit(isolate, self, "close");
    return;
  }

  auto chunk = v8::String::NewFromUtf8(isolate, st->buf, v8::NewStringType::kNormal,
                                       (int)n).ToLocalChecked();
  StreamEmit(isolate, self, "data", chunk);
  StreamPipeWrite(isolate, st, chunk);

  if (st->paused) return;
  StreamReadNext(st);
}

static void StreamReadNext(StreamState* st) {
  st->req.data = st;
  uv_buf_t iov = uv_buf_init(st->buf, (unsigned)sizeof(st->buf));
  uv_fs_read(nx8v::g_runtime->loop, &st->req, st->fd, &iov, 1, -1, StreamReadDone);
}

static void StreamReadOpen(uv_fs_t* req) {
  auto* st = static_cast<StreamState*>(req->data);
  int fd = (int)req->result;
  uv_fs_req_cleanup(req);

  EnterIsolate scope(nx8v::g_runtime->isolate);
  v8::Isolate* isolate = scope.isolate;
  v8::HandleScope hs(isolate);

  if (fd < 0) {
    v8::Local<v8::Object> self = st->self.Get(isolate);
    StreamEmit(isolate, self, "error",
               v8::Exception::Error(v8::String::NewFromUtf8(
                   isolate, uv_strerror(fd), v8::NewStringType::kNormal)
                                        .ToLocalChecked()));
    return;
  }
  st->fd = fd;
  StreamReadNext(st);
}

// fs.createReadStream(path[, { chunkSize }])
static void CreateReadStream(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);

  auto stream = MakeStreamObject(isolate, StreamKind::Readable);
  auto* st = StreamFrom(stream);

  if (args.Length() >= 1) {
    v8::String::Utf8Value p(isolate, args[0]);
    st->path = *p ? *p : "";
  }

  st->req.data = st;
  uv_fs_open(nx8v::g_runtime->loop, &st->req, st->path.c_str(), O_RDONLY, 0,
             StreamReadOpen);

  args.GetReturnValue().Set(stream);
}
static void CreateWriteStream(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto stream = MakeStreamObject(isolate, StreamKind::Writable);
  auto* st = StreamFrom(stream);

  if (args.Length() >= 1) {
    v8::String::Utf8Value p(isolate, args[0]);
    st->path = *p ? *p : "";
  }

  // Intercept write/end so data goes to disk.
  auto write_fn = v8::Function::New(ctx, [](
      const v8::FunctionCallbackInfo<v8::Value>& a) {
    v8::Isolate* iso = a.GetIsolate();
    v8::HandleScope h(iso);
    auto* state = StreamFrom(a.This());
    if (!state || state->finished || a.Length() < 1) {
      a.GetReturnValue().Set(false);
      return;
    }

    std::string payload;
    if (a[0]->IsString()) {
      v8::String::Utf8Value s(iso, a[0]);
      payload.assign(*s ? *s : "", s.length());
    } else {
      size_t len = 0;
      uint8_t* data = nullptr;
      if (a[0]->IsUint8Array()) {
        auto arr = a[0].As<v8::Uint8Array>();
        auto ab = arr->Buffer();
        auto bs = ab->GetBackingStore();
        data = static_cast<uint8_t*>(bs->Data()) + arr->ByteOffset();
        len = arr->ByteLength();
      }
      if (data) payload.assign((char*)data, len);
    }

    uv_fs_t open_req;
    if (uv_fs_open(nx8v::g_runtime->loop, &open_req, state->path.c_str(),
                   O_WRONLY | O_CREAT | O_APPEND, 0644, nullptr) >= 0) {
      uv_file fd = (uv_file)open_req.result;
      uv_fs_req_cleanup(&open_req);

      uv_buf_t buf = uv_buf_init(const_cast<char*>(payload.data()),
                                 (unsigned)payload.size());
      uv_fs_t wr;
      uv_fs_write(nx8v::g_runtime->loop, &wr, fd, &buf, 1, -1, nullptr);
      uv_fs_req_cleanup(&wr);
      uv_fs_close(nx8v::g_runtime->loop, &wr, fd, nullptr);
      uv_fs_req_cleanup(&wr);
    } else {
      uv_fs_req_cleanup(&open_req);
    }
    a.GetReturnValue().Set(true);
  }).ToLocalChecked();

  stream->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "write"), write_fn).Check();

  auto end_fn = v8::Function::New(ctx, [](
      const v8::FunctionCallbackInfo<v8::Value>& a) {
    v8::Isolate* iso = a.GetIsolate();
    v8::HandleScope h(iso);
    auto* state = StreamFrom(a.This());
    if (!state || state->finished) return;
    state->finished = true;
    state->ended = true;
    StreamEmit(iso, a.This(), "finish");
    StreamEmit(iso, a.This(), "close");
  }).ToLocalChecked();
  stream->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "end"), end_fn).Check();

  args.GetReturnValue().Set(stream);
}

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------
inline v8::Local<v8::Object> MakeStreamModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);

  auto make_ctor = [&](const char* name, StreamKind kind) {
    auto data = v8::Integer::New(isolate, (int)kind).As<v8::Value>();
    // FunctionTemplate makes the result constructible with `new`.
    auto tmpl = v8::FunctionTemplate::New(isolate, [](
        const v8::FunctionCallbackInfo<v8::Value>& a) {
      v8::Isolate* iso = a.GetIsolate();
      v8::HandleScope h(iso);
      int k = 0;
      auto d = a.DataV2().As<v8::Value>();
      if (d->IsNumber()) k = d.As<v8::Int32>()->Value();
      // Returning an object from a constructor replaces `this`.
      a.GetReturnValue().Set(MakeStreamObject(iso, (StreamKind)k));
    }, data);
    auto ctor = tmpl->GetFunction(ctx).ToLocalChecked();
    obj->Set(ctx, v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal,
                                          (int)std::strlen(name)).ToLocalChecked(),
             ctor).Check();
  };

  make_ctor("Readable", StreamKind::Readable);
  make_ctor("Writable", StreamKind::Writable);
  make_ctor("Duplex", StreamKind::Duplex);
  make_ctor("PassThrough", StreamKind::Duplex);
  make_ctor("Stream", StreamKind::Duplex);

  return hs.Escape(obj);
}

}} // nx8v::bindings
