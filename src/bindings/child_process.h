#pragma once
// nx8v/src/bindings/child_process.h
// require('child_process')
//   child_process.exec(command, (err, stdout, stderr) => {})
//   child_process.spawn(file[, args]) -> { stdout, stderr, kill(), on('exit') }

#include <v8.h>
#include <uv.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../runtime.h"
#include "events.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace nx8v {
namespace bindings {

struct ChildProc {
  uv_process_t proc;
  uv_pipe_t out_pipe;
  uv_pipe_t err_pipe;
  v8::Global<v8::Object> js;
  v8::Global<v8::Object> stdout_obj;
  v8::Global<v8::Object> stderr_obj;
  v8::Global<v8::Function> exec_cb;
  std::string out_buf;
  std::string err_buf;
  std::string file;
  std::vector<std::string> args;
  std::vector<char*> arg_ptrs;
  bool exec_mode = false;
  bool exited = false;
  bool proc_closed = false;
  bool skip_js = false;
  bool finished = false;
  int exit_status = 0;
  int live_pipes = 0;
  char out_scratch[8192];
  char err_scratch[8192];
};

static void ChildEmit(v8::Isolate* isolate, v8::Local<v8::Object> obj,
                      const char* event, v8::Local<v8::Value> extra = v8::Local<v8::Value>()) {
  if (obj.IsEmpty()) return;
  auto ctx = isolate->GetCurrentContext();
  auto emit_v = obj->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "emit"))
                    .ToLocalChecked();
  if (!emit_v->IsFunction()) return;
  v8::Local<v8::Value> name =
      v8::String::NewFromUtf8(isolate, event, v8::NewStringType::kNormal).ToLocalChecked();
  v8::TryCatch tc(isolate);
  v8::Local<v8::Value> ignored;
  if (extra.IsEmpty()) {
    v8::Local<v8::Value> argv[] = {name};
    (void)emit_v.As<v8::Function>()->Call(ctx, obj, 1, argv).ToLocal(&ignored);
  } else {
    v8::Local<v8::Value> argv[] = {name, extra};
    (void)emit_v.As<v8::Function>()->Call(ctx, obj, 2, argv).ToLocal(&ignored);
  }
}

static void ChildMaybeFinish(ChildProc* c) {
  if (c->finished) return;
  if (!c->exited || !c->proc_closed || c->live_pipes > 0) return;
  c->finished = true;
  if (c->skip_js) {
    c->js.Reset();
    c->stdout_obj.Reset();
    c->stderr_obj.Reset();
    c->exec_cb.Reset();
    delete c;
    return;
  }
  EnterIsolate scope(nx8v::g_runtime->isolate);
  v8::Isolate* isolate = scope.isolate;
  auto ctx = isolate->GetCurrentContext();
  if (c->exec_mode && !c->exec_cb.IsEmpty()) {
    auto cb = c->exec_cb.Get(isolate);
    v8::Local<v8::Value> err = v8::Null(isolate);
    if (c->exit_status != 0) {
      std::string msg = "command exited with code " + std::to_string(c->exit_status);
      err = v8::Exception::Error(
          v8::String::NewFromUtf8(isolate, msg.c_str(), v8::NewStringType::kNormal)
              .ToLocalChecked());
    }
    v8::Local<v8::Value> argv[] = {
        err,
        v8::String::NewFromUtf8(isolate, c->out_buf.c_str(), v8::NewStringType::kNormal,
                                (int)c->out_buf.size())
            .ToLocalChecked(),
        v8::String::NewFromUtf8(isolate, c->err_buf.c_str(), v8::NewStringType::kNormal,
                                (int)c->err_buf.size())
            .ToLocalChecked()};
    v8::TryCatch tc(isolate);
    v8::Local<v8::Value> ignored;
    (void)cb->Call(ctx, ctx->Global(), 3, argv).ToLocal(&ignored);
    c->exec_cb.Reset();
  }
  if (!c->js.IsEmpty()) {
    ChildEmit(isolate, c->js.Get(isolate), "exit",
              v8::Integer::New(isolate, c->exit_status));
  }
  c->js.Reset();
  c->stdout_obj.Reset();
  c->stderr_obj.Reset();
  delete c;
}

static void ChildOnPipeClosed(uv_handle_t* h) {
  auto* c = static_cast<ChildProc*>(h->data);
  c->live_pipes--;
  ChildMaybeFinish(c);
}

static void ChildOnProcClosed(uv_handle_t* h) {
  auto* c = static_cast<ChildProc*>(h->data);
  c->proc_closed = true;
  ChildMaybeFinish(c);
}

static void ChildOnExit(uv_process_t* proc, int64_t status, int) {
  auto* c = static_cast<ChildProc*>(proc->data);
  c->exited = true;
  c->exit_status = (int)status;
  uv_close(reinterpret_cast<uv_handle_t*>(proc), ChildOnProcClosed);
}

static void ChildOnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  auto* c = static_cast<ChildProc*>(stream->data);
  bool is_err = stream == reinterpret_cast<uv_stream_t*>(&c->err_pipe);
  if (nread > 0) {
    std::string chunk(buf->base, (size_t)nread);
    if (is_err) c->err_buf += chunk;
    else c->out_buf += chunk;
    if (!c->exec_mode) {
      EnterIsolate scope(nx8v::g_runtime->isolate);
      v8::Isolate* isolate = scope.isolate;
      auto obj = (is_err ? c->stderr_obj : c->stdout_obj).Get(isolate);
      if (!obj.IsEmpty()) {
        ChildEmit(isolate, obj, "data",
                  v8::String::NewFromUtf8(isolate, chunk.c_str(),
                                          v8::NewStringType::kNormal,
                                          (int)chunk.size())
                      .ToLocalChecked());
      }
    }
    return;
  }
  if (nread < 0) {
    uv_read_stop(stream);
    uv_close(reinterpret_cast<uv_handle_t*>(stream), ChildOnPipeClosed);
  }
}

static void ChildAllocOut(uv_handle_t* handle, size_t, uv_buf_t* buf) {
  auto* c = static_cast<ChildProc*>(handle->data);
  buf->base = c->out_scratch;
  buf->len = sizeof(c->out_scratch);
}
static void ChildAllocErr(uv_handle_t* handle, size_t, uv_buf_t* buf) {
  auto* c = static_cast<ChildProc*>(handle->data);
  buf->base = c->err_scratch;
  buf->len = sizeof(c->err_scratch);
}

static std::string ResolveProgram(const std::string& file) {
  if (file.find('/') != std::string::npos || file.find('\\') != std::string::npos)
    return file;
#ifdef _WIN32
  char buf[MAX_PATH];
  DWORD n = SearchPathA(nullptr, file.c_str(), ".exe", MAX_PATH, buf, nullptr);
  if (n > 0 && n < MAX_PATH) return std::string(buf, n);
#endif
  return file;
}

static ChildProc* SpawnProcess(v8::Isolate* isolate, const std::string& file,
                               const std::vector<std::string>& args, bool exec_mode,
                               v8::Local<v8::Function> exec_cb, std::string* error) {
  auto* c = new ChildProc();
  c->exec_mode = exec_mode;
  c->file = ResolveProgram(file);
  c->args.push_back(c->file);
  for (auto& a : args) c->args.push_back(a);
  c->arg_ptrs.reserve(c->args.size() + 1);
  for (auto& s : c->args) c->arg_ptrs.push_back(const_cast<char*>(s.c_str()));
  c->arg_ptrs.push_back(nullptr);
  if (!exec_cb.IsEmpty()) c->exec_cb.Reset(isolate, exec_cb);

  uv_pipe_init(nx8v::g_runtime->loop, &c->out_pipe, 0);
  uv_pipe_init(nx8v::g_runtime->loop, &c->err_pipe, 0);
  c->out_pipe.data = c;
  c->err_pipe.data = c;
  c->proc.data = c;

  uv_stdio_container_t stdio[3];
  stdio[0].flags = UV_IGNORE;
  stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(&c->out_pipe);
  stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  stdio[2].data.stream = reinterpret_cast<uv_stream_t*>(&c->err_pipe);

  uv_process_options_t opt;
  std::memset(&opt, 0, sizeof(opt));
  opt.exit_cb = ChildOnExit;
  opt.file = c->file.c_str();
  opt.args = c->arg_ptrs.data();
  opt.stdio_count = 3;
  opt.stdio = stdio;
#ifdef _WIN32
  opt.flags = UV_PROCESS_WINDOWS_HIDE;
#endif

  int r = uv_spawn(nx8v::g_runtime->loop, &c->proc, &opt);
  if (r < 0) {
    if (error) *error = uv_strerror(r);
    c->skip_js = true;
    c->exited = true;
    c->proc_closed = true;
    c->live_pipes = 2;
    c->exec_cb.Reset();
    uv_close(reinterpret_cast<uv_handle_t*>(&c->out_pipe), ChildOnPipeClosed);
    uv_close(reinterpret_cast<uv_handle_t*>(&c->err_pipe), ChildOnPipeClosed);
    return nullptr;
  }
  c->live_pipes = 2;
  uv_read_start(reinterpret_cast<uv_stream_t*>(&c->out_pipe), ChildAllocOut, ChildOnRead);
  uv_read_start(reinterpret_cast<uv_stream_t*>(&c->err_pipe), ChildAllocErr, ChildOnRead);
  return c;
}

inline v8::Local<v8::Object> MakeChildProcessModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  auto ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "exec"),
           v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
             v8::Isolate* iso = args.GetIsolate();
             v8::HandleScope h(iso);
             if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsFunction()) {
               iso->ThrowException(v8::Exception::TypeError(
                   v8::String::NewFromUtf8Literal(iso, "exec(command, callback)")));
               return;
             }
             v8::String::Utf8Value cmd(iso, args[0]);
             std::string command = *cmd ? *cmd : "";
#ifdef _WIN32
             const char* shell = std::getenv("ComSpec");
             if (!shell) shell = "cmd.exe";
             std::vector<std::string> argv = {"/d", "/s", "/c", command};
#else
             const char* shell = "/bin/sh";
             std::vector<std::string> argv = {"-c", command};
#endif
             std::string err;
             auto* child = SpawnProcess(iso, shell, argv, true, args[1].As<v8::Function>(), &err);
             if (!child) {
               auto cb = args[1].As<v8::Function>();
               v8::Local<v8::Value> argv_js[] = {
                   v8::Exception::Error(v8::String::NewFromUtf8(iso, err.c_str(),
                                                               v8::NewStringType::kNormal)
                                            .ToLocalChecked()),
                   v8::String::NewFromUtf8Literal(iso, ""),
                   v8::String::NewFromUtf8Literal(iso, "")};
               v8::Local<v8::Value> ignored;
               (void)cb->Call(iso->GetCurrentContext(), iso->GetCurrentContext()->Global(),
                              3, argv_js)
                   .ToLocal(&ignored);
             }
           }).ToLocalChecked())
      .Check();

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "spawn"),
           v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
             v8::Isolate* iso = args.GetIsolate();
             v8::HandleScope h(iso);
             auto c = iso->GetCurrentContext();
             if (args.Length() < 1 || !args[0]->IsString()) {
               iso->ThrowException(v8::Exception::TypeError(
                   v8::String::NewFromUtf8Literal(iso, "spawn(file[, args])")));
               return;
             }
             v8::String::Utf8Value file(iso, args[0]);
             std::vector<std::string> argv;
             if (args.Length() >= 2 && args[1]->IsArray()) {
               auto arr = args[1].As<v8::Array>();
               for (uint32_t i = 0; i < arr->Length(); ++i) {
                 v8::String::Utf8Value s(iso, arr->Get(c, i).ToLocalChecked());
                 argv.push_back(*s ? std::string(*s, s.length()) : "");
               }
             }
             std::string err;
             auto* child = SpawnProcess(iso, *file ? *file : "", argv, false,
                                        v8::Local<v8::Function>(), &err);
             auto js = v8::Object::New(iso);
             MakeEmitterMethods(iso, js);
             auto stdout_obj = v8::Object::New(iso);
             auto stderr_obj = v8::Object::New(iso);
             MakeEmitterMethods(iso, stdout_obj);
             MakeEmitterMethods(iso, stderr_obj);
             js->Set(c, v8::String::NewFromUtf8Literal(iso, "stdout"), stdout_obj).Check();
             js->Set(c, v8::String::NewFromUtf8Literal(iso, "stderr"), stderr_obj).Check();
             if (!child) {
               js->Set(c, v8::String::NewFromUtf8Literal(iso, "error"),
                       v8::String::NewFromUtf8(iso, err.c_str(), v8::NewStringType::kNormal)
                           .ToLocalChecked())
                   .Check();
               args.GetReturnValue().Set(js);
               return;
             }
             child->js.Reset(iso, js);
             child->stdout_obj.Reset(iso, stdout_obj);
             child->stderr_obj.Reset(iso, stderr_obj);
             auto ext = v8::External::New(iso, child, v8::kExternalPointerTypeTagDefault);
             js->Set(c, v8::String::NewFromUtf8Literal(iso, "kill"),
                     v8::Function::New(c, [](const v8::FunctionCallbackInfo<v8::Value>& a) {
                       auto* child = static_cast<ChildProc*>(
                           a.DataV2().As<v8::External>()->Value(
                               v8::kExternalPointerTypeTagDefault));
                       if (child && !child->exited)
                         uv_process_kill(&child->proc, SIGTERM);
                     }, ext).ToLocalChecked())
                 .Check();
             js->Set(c, v8::String::NewFromUtf8Literal(iso, "pid"),
                     v8::Integer::New(iso, child->proc.pid))
                 .Check();
             args.GetReturnValue().Set(js);
           }).ToLocalChecked())
      .Check();

  return hs.Escape(obj);
}

}}  // nx8v::bindings
