#pragma once
// nx8v/src/bindings/fs.h
// The `fs` built-in module.
//
// Synchronous API  : std::filesystem (no event loop involvement)
// Asynchronous API : libuv thread pool (uv_fs_*), completion on the loop thread
//
// Exposed surface:
//   readFileSync(path[, encoding])  -> Buffer, or string when encoding is set
//   writeFileSync(path, data)
//   existsSync(path)                -> bool
//   mkdirSync(path)
//   unlinkSync(path)
//   readdirSync(path)               -> string[]
//   statSync(path)                  -> { size, mtimeMs, isFile(), isDirectory() }
//   readFile(path, cb)              -> cb(err, Buffer)
//   writeFile(path, data, cb)       -> cb(err)

#include <v8.h>
#include <uv.h>

#include <fcntl.h>
#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <sstream>
#include <filesystem>

#include "../runtime.h"
#include "streams.h"
#include "buffer.h"

namespace fs = std::filesystem;

namespace nx8v {
namespace bindings {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static v8::Local<v8::Value> MakeError(v8::Isolate* isolate,
                                      const std::string& message,
                                      const char* code = nullptr) {
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto err = v8::Exception::Error(
      v8::String::NewFromUtf8(isolate, message.c_str(),
                              v8::NewStringType::kNormal).ToLocalChecked())
      .As<v8::Object>();
  if (code) {
    err->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "code"),
             v8::String::NewFromUtf8(isolate, code,
                                     v8::NewStringType::kNormal).ToLocalChecked())
        .Check();
  }
  return err;
}

static std::string ToStdString(v8::Isolate* isolate, v8::Local<v8::Value> v) {
  if (v->IsUint8Array()) {
    size_t n = 0;
    uint8_t* p = BufBytes(isolate, v, &n);
    if (!p || n == 0) return "";
    return std::string(reinterpret_cast<const char*>(p), n);
  }
  v8::String::Utf8Value s(isolate, v);
  if (!*s) return "";
  return std::string(*s, static_cast<size_t>(s.length()));
}

// uv error number -> message
static std::string UvErrStr(int status) {
  return std::string(uv_strerror(status));
}

// ---------------------------------------------------------------------------
// Synchronous API
// ---------------------------------------------------------------------------

static void ReadFileSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  if (args.Length() < 1) {
    isolate->ThrowException(v8::Exception::TypeError(
      v8::String::NewFromUtf8Literal(isolate, "readFileSync: path required")));
    return;
  }
  std::string path = ToStdString(isolate, args[0]);

  std::ifstream f(path, std::ios::binary);
  if (!f) {
    isolate->ThrowException(MakeError(isolate,
      "ENOENT: no such file or directory, open '" + path + "'", "ENOENT"));
    return;
  }
  std::ostringstream buf;
  buf << f.rdbuf();
  std::string content = buf.str();

  if (args.Length() >= 2 && args[1]->IsString()) {
    args.GetReturnValue().Set(
      v8::String::NewFromUtf8(isolate, content.data(),
                              v8::NewStringType::kNormal,
                              (int)content.size()).ToLocalChecked());
    return;
  }
  args.GetReturnValue().Set(MakeBufferFromBytes(
      isolate, reinterpret_cast<const uint8_t*>(content.data()), content.size()));
}

static void WriteFileSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  if (args.Length() < 2) {
    isolate->ThrowException(v8::Exception::TypeError(
      v8::String::NewFromUtf8Literal(isolate, "writeFileSync: path and data required")));
    return;
  }
  std::string path = ToStdString(isolate, args[0]);
  std::string data = ToStdString(isolate, args[1]);

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    isolate->ThrowException(MakeError(isolate,
      "EACCES: cannot write file '" + path + "'", "EACCES"));
    return;
  }
  f.write(data.data(), (std::streamsize)data.size());
}

static void ExistsSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  if (args.Length() < 1) { args.GetReturnValue().Set(false); return; }
  std::string path = ToStdString(isolate, args[0]);
  std::error_code ec;
  args.GetReturnValue().Set(v8::Boolean::New(isolate, fs::exists(path, ec)));
}

static void MkdirSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  if (args.Length() < 1) return;
  std::string path = ToStdString(isolate, args[0]);
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) {
    isolate->ThrowException(MakeError(isolate,
      "ENOENT: cannot create directory '" + path + "': " + ec.message()));
  }
}

static void UnlinkSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  if (args.Length() < 1) return;
  std::string path = ToStdString(isolate, args[0]);
  std::error_code ec;
  fs::remove(path, ec);
  if (ec) {
    isolate->ThrowException(MakeError(isolate,
      "ENOENT: cannot unlink '" + path + "': " + ec.message()));
  }
}

static void ReaddirSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 1) {
    args.GetReturnValue().Set(v8::Array::New(isolate, 0));
    return;
  }
  std::string path = ToStdString(isolate, args[0]);

  std::error_code ec;
  std::vector<std::string> names;
  for (auto& entry : fs::directory_iterator(path, ec)) {
    names.push_back(entry.path().filename().string());
  }
  if (ec) {
    isolate->ThrowException(MakeError(isolate,
      "ENOENT: cannot read directory '" + path + "': " + ec.message()));
    return;
  }

  auto arr = v8::Array::New(isolate, (int)names.size());
  for (int i = 0; i < (int)names.size(); ++i) {
    arr->Set(ctx, i,
      v8::String::NewFromUtf8(isolate, names[i].c_str(),
                              v8::NewStringType::kNormal).ToLocalChecked()).Check();
  }
  args.GetReturnValue().Set(arr);
}

static void StatSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 1) return;
  std::string path = ToStdString(isolate, args[0]);

  std::error_code ec;
  auto st = fs::status(path, ec);
  if (ec || !fs::exists(st)) {
    isolate->ThrowException(MakeError(isolate,
      "ENOENT: no such file or directory, stat '" + path + "'", "ENOENT"));
    return;
  }

  auto obj = v8::Object::New(isolate);
  auto fsize = fs::file_size(path, ec);
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "size"),
           v8::Number::New(isolate, ec ? 0.0 : (double)fsize)).Check();

  // mtimeMs
  auto ftime = fs::last_write_time(path, ec);
  double mtime_ms = 0.0;
  if (!ec) {
    auto sctp = std::chrono::time_point_cast<std::chrono::milliseconds>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    mtime_ms = (double)sctp.time_since_epoch().count();
  }
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "mtimeMs"),
           v8::Number::New(isolate, mtime_ms)).Check();

  bool is_file = fs::is_regular_file(st);
  bool is_dir  = fs::is_directory(st);

  // Expose the predicates as functions; the computed boolean travels as the
  // function's data (a capturing lambda cannot convert to v8::FunctionCallback).
  auto bool_predicate = [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    a.GetReturnValue().Set(a.DataV2().As<v8::Boolean>());
  };
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "isFile"),
    v8::Function::New(ctx, bool_predicate,
                      v8::Boolean::New(isolate, is_file)).ToLocalChecked()).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "isDirectory"),
    v8::Function::New(ctx, bool_predicate,
                      v8::Boolean::New(isolate, is_dir)).ToLocalChecked()).Check();

  args.GetReturnValue().Set(obj);
}

// ---------------------------------------------------------------------------
// Asynchronous API (libuv thread pool)
// ---------------------------------------------------------------------------

struct AsyncReadState {
  uv_fs_t                  req;       // first: reused for each operation
  std::string              path;
  v8::Global<v8::Function> callback;
  uv_file                  fd = -1;
  std::string              data;
  char                     buf[65536];
  int                      error = 0;
};

static void AsyncReadChunk(AsyncReadState* st);

static void OnReadClose(uv_fs_t* req) {
  auto* st = static_cast<AsyncReadState*>(req->data);
  uv_fs_req_cleanup(req);

  EnterIsolate scope(nx8v::g_runtime->isolate);
  v8::Isolate* isolate = scope.isolate;
  v8::Local<v8::Function> cb = st->callback.Get(isolate);

  if (st->error != 0) {
    CallJsCallback(isolate, cb, MakeError(isolate, UvErrStr(st->error)),
                   v8::Undefined(isolate));
  } else {
    auto result = MakeBufferFromBytes(
        isolate, reinterpret_cast<const uint8_t*>(st->data.data()), st->data.size());
    CallJsCallback(isolate, cb, v8::Null(isolate), result);
  }

  st->callback.Reset();
  delete st;
}

static void OnRead(uv_fs_t* req) {
  auto* st = static_cast<AsyncReadState*>(req->data);
  ssize_t n = req->result;
  uv_fs_req_cleanup(req);

  if (n < 0) {
    st->error = (int)n;
    uv_fs_close(nx8v::g_runtime->loop, &st->req, st->fd, OnReadClose);
    return;
  }
  if (n == 0) {
    // EOF
    uv_fs_close(nx8v::g_runtime->loop, &st->req, st->fd, OnReadClose);
    return;
  }
  st->data.append(st->buf, (size_t)n);
  AsyncReadChunk(st);
}

static void AsyncReadChunk(AsyncReadState* st) {
  st->req.data = st;
  uv_buf_t iov = uv_buf_init(st->buf, (unsigned int)sizeof(st->buf));
  uv_fs_read(nx8v::g_runtime->loop, &st->req, st->fd, &iov, 1, -1, OnRead);
}

static void OnOpen(uv_fs_t* req) {
  auto* st = static_cast<AsyncReadState*>(req->data);
  if (req->result < 0) {
    st->error = (int)req->result;
    uv_fs_req_cleanup(req);

    EnterIsolate scope(nx8v::g_runtime->isolate);
    v8::Isolate* isolate = scope.isolate;
    v8::Local<v8::Function> cb = st->callback.Get(isolate);
    CallJsCallback(isolate, cb, MakeError(isolate, UvErrStr(st->error)),
                   v8::Undefined(isolate));
    st->callback.Reset();
    delete st;
    return;
  }
  st->fd = (uv_file)req->result;
  uv_fs_req_cleanup(req);
  AsyncReadChunk(st);
}

static void ReadFileAsync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);

  if (args.Length() < 2 || !args[1]->IsFunction()) {
    isolate->ThrowException(v8::Exception::TypeError(
      v8::String::NewFromUtf8Literal(isolate,
        "readFile: second argument must be a callback")));
    return;
  }

  auto* st = new AsyncReadState();
  st->path = ToStdString(isolate, args[0]);
  st->callback.Reset(isolate, args[1].As<v8::Function>());
  st->req.data = st;

  uv_fs_open(nx8v::g_runtime->loop, &st->req, st->path.c_str(),
             O_RDONLY, 0, OnOpen);
}

// --- async writeFile ---

struct AsyncWriteState {
  uv_fs_t                  req;
  std::string              path;
  std::string              data;
  v8::Global<v8::Function> callback;
  uv_file                  fd = -1;
  int                      error = 0;   // sticky error (uv_fs_close overwrites req->result)
};

static void OnWriteClose(uv_fs_t* req) {
  auto* st = static_cast<AsyncWriteState*>(req->data);
  uv_fs_req_cleanup(req);

  EnterIsolate scope(nx8v::g_runtime->isolate);
  v8::Isolate* isolate = scope.isolate;
  v8::Local<v8::Function> cb = st->callback.Get(isolate);

  if (st->error != 0) {
    CallJsCallback(isolate, cb, MakeError(isolate, UvErrStr(st->error)),
                   v8::Undefined(isolate));
  } else {
    CallJsCallback(isolate, cb, v8::Null(isolate), v8::Undefined(isolate));
  }
  st->callback.Reset();
  delete st;
}

static void OnWrite(uv_fs_t* req) {
  auto* st = static_cast<AsyncWriteState*>(req->data);
  ssize_t n = req->result;
  uv_fs_req_cleanup(req);

  if (n < 0) st->error = (int)n;

  st->req.data = st;
  uv_fs_close(nx8v::g_runtime->loop, &st->req, st->fd, OnWriteClose);
}

static void OnWriteOpen(uv_fs_t* req) {
  auto* st = static_cast<AsyncWriteState*>(req->data);
  ssize_t open_result = req->result;
  uv_fs_req_cleanup(req);

  if (open_result < 0) {
    st->error = (int)open_result;

    EnterIsolate scope(nx8v::g_runtime->isolate);
    v8::Isolate* isolate = scope.isolate;
    v8::Local<v8::Function> cb = st->callback.Get(isolate);
    CallJsCallback(isolate, cb, MakeError(isolate, UvErrStr(st->error)),
                   v8::Undefined(isolate));
    st->callback.Reset();
    delete st;
    return;
  }
  st->fd = (uv_file)open_result;

  st->req.data = st;
  uv_buf_t iov = uv_buf_init(const_cast<char*>(st->data.data()),
                             (unsigned int)st->data.size());
  uv_fs_write(nx8v::g_runtime->loop, &st->req, st->fd, &iov, 1, -1, OnWrite);
}

static void WriteFileAsync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);

  if (args.Length() < 3 || !args[2]->IsFunction()) {
    isolate->ThrowException(v8::Exception::TypeError(
      v8::String::NewFromUtf8Literal(isolate,
        "writeFile: third argument must be a callback")));
    return;
  }

  auto* st = new AsyncWriteState();
  st->path = ToStdString(isolate, args[0]);
  st->data = ToStdString(isolate, args[1]);
  st->callback.Reset(isolate, args[2].As<v8::Function>());
  st->req.data = st;

  uv_fs_open(nx8v::g_runtime->loop, &st->req, st->path.c_str(),
             O_WRONLY | O_CREAT | O_TRUNC, 0644, OnWriteOpen);
}

// ---------------------------------------------------------------------------
// rename / copyFile / appendFile / rm / watch
// ---------------------------------------------------------------------------

struct FsJob {
  uv_work_t work;
  v8::Global<v8::Function> callback;
  std::string a;
  std::string b;
  std::string data;
  int kind = 0;  // 1 rename, 2 copy, 3 append, 4 rm
  bool recursive = false;
  bool force = false;
  std::string error;
};

static std::string FsArgData(v8::Isolate* isolate, v8::Local<v8::Value> v) {
  if (v->IsUint8Array()) {
    size_t n = 0;
    uint8_t* p = BufBytes(isolate, v, &n);
    if (p) return std::string(reinterpret_cast<char*>(p), n);
    return {};
  }
  return ToStdString(isolate, v);
}

static void FsJobWork(uv_work_t* w) {
  auto* j = static_cast<FsJob*>(w->data);
  std::error_code ec;
  if (j->kind == 1) {
    fs::rename(j->a, j->b, ec);
  } else if (j->kind == 2) {
    fs::copy_file(j->a, j->b, fs::copy_options::overwrite_existing, ec);
  } else if (j->kind == 3) {
    std::ofstream f(j->a, std::ios::binary | std::ios::app);
    if (!f) {
      j->error = "EACCES: cannot append '" + j->a + "'";
      return;
    }
    f.write(j->data.data(), (std::streamsize)j->data.size());
    if (!f) j->error = "EIO: append failed '" + j->a + "'";
    return;
  } else if (j->kind == 4) {
    if (j->recursive) fs::remove_all(j->a, ec);
    else fs::remove(j->a, ec);
    if (ec && j->force && ec == std::errc::no_such_file_or_directory) ec.clear();
  }
  if (ec) j->error = ec.message();
}

static void FsJobDone(uv_work_t* w, int) {
  auto* j = static_cast<FsJob*>(w->data);
  EnterIsolate scope(nx8v::g_runtime->isolate);
  v8::Isolate* isolate = scope.isolate;
  v8::Local<v8::Function> cb = j->callback.Get(isolate);
  if (!j->error.empty()) {
    CallJsCallback(isolate, cb, MakeError(isolate, j->error), v8::Undefined(isolate));
  } else {
    CallJsCallback(isolate, cb, v8::Null(isolate), v8::Undefined(isolate));
  }
  j->callback.Reset();
  delete j;
}

static void FsQueue(v8::Isolate* isolate, v8::Local<v8::Function> cb, FsJob* j) {
  j->callback.Reset(isolate, cb);
  j->work.data = j;
  int r = uv_queue_work(nx8v::g_runtime->loop, &j->work, FsJobWork, FsJobDone);
  if (r < 0) {
    j->callback.Reset();
    delete j;
    isolate->ThrowException(MakeError(isolate, uv_strerror(r)));
  }
}

static void RenameSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  if (args.Length() < 2) return;
  std::error_code ec;
  fs::rename(ToStdString(isolate, args[0]), ToStdString(isolate, args[1]), ec);
  if (ec) isolate->ThrowException(MakeError(isolate, ec.message(), "EIO"));
}

static void RenameAsync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  if (args.Length() < 3 || !args[2]->IsFunction()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "rename(from, to, cb)")));
    return;
  }
  auto* j = new FsJob();
  j->kind = 1;
  j->a = ToStdString(isolate, args[0]);
  j->b = ToStdString(isolate, args[1]);
  FsQueue(isolate, args[2].As<v8::Function>(), j);
}

static void CopyFileSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  if (args.Length() < 2) return;
  std::error_code ec;
  fs::copy_file(ToStdString(isolate, args[0]), ToStdString(isolate, args[1]),
                fs::copy_options::overwrite_existing, ec);
  if (ec) isolate->ThrowException(MakeError(isolate, ec.message(), "EIO"));
}

static void CopyFileAsync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  if (args.Length() < 3 || !args[2]->IsFunction()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "copyFile(src, dest, cb)")));
    return;
  }
  auto* j = new FsJob();
  j->kind = 2;
  j->a = ToStdString(isolate, args[0]);
  j->b = ToStdString(isolate, args[1]);
  FsQueue(isolate, args[2].As<v8::Function>(), j);
}

static void AppendFileSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  if (args.Length() < 2) return;
  std::string path = ToStdString(isolate, args[0]);
  std::string data = FsArgData(isolate, args[1]);
  std::ofstream f(path, std::ios::binary | std::ios::app);
  if (!f) {
    isolate->ThrowException(MakeError(isolate, "EACCES: cannot append '" + path + "'", "EACCES"));
    return;
  }
  f.write(data.data(), (std::streamsize)data.size());
}

static void AppendFileAsync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  if (args.Length() < 3 || !args[2]->IsFunction()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "appendFile(path, data, cb)")));
    return;
  }
  auto* j = new FsJob();
  j->kind = 3;
  j->a = ToStdString(isolate, args[0]);
  j->data = FsArgData(isolate, args[1]);
  FsQueue(isolate, args[2].As<v8::Function>(), j);
}

static void RmReadOpts(v8::Isolate* isolate, v8::Local<v8::Value> v,
                       bool& recursive, bool& force) {
  recursive = false;
  force = false;
  if (!v->IsObject()) return;
  auto ctx = isolate->GetCurrentContext();
  auto o = v.As<v8::Object>();
  auto rec = o->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "recursive"))
                 .ToLocalChecked();
  auto fr = o->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "force"))
                .ToLocalChecked();
  if (rec->IsBoolean()) recursive = rec.As<v8::Boolean>()->Value();
  if (fr->IsBoolean()) force = fr.As<v8::Boolean>()->Value();
}

static void RmSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  if (args.Length() < 1) return;
  bool recursive = false, force = false;
  if (args.Length() >= 2) RmReadOpts(isolate, args[1], recursive, force);
  std::error_code ec;
  std::string path = ToStdString(isolate, args[0]);
  if (recursive) fs::remove_all(path, ec);
  else fs::remove(path, ec);
  if (ec && force && ec == std::errc::no_such_file_or_directory) ec.clear();
  if (ec) isolate->ThrowException(MakeError(isolate, ec.message(), "EIO"));
}

static void RmAsync(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  if (args.Length() < 2) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "rm(path, [opts,] cb)")));
    return;
  }
  v8::Local<v8::Function> cb;
  bool recursive = false, force = false;
  if (args[1]->IsFunction()) {
    cb = args[1].As<v8::Function>();
  } else if (args.Length() >= 3 && args[2]->IsFunction()) {
    RmReadOpts(isolate, args[1], recursive, force);
    cb = args[2].As<v8::Function>();
  } else {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "rm(path, [opts,] cb)")));
    return;
  }
  auto* j = new FsJob();
  j->kind = 4;
  j->a = ToStdString(isolate, args[0]);
  j->recursive = recursive;
  j->force = force;
  FsQueue(isolate, cb, j);
}

struct FsWatch {
  uv_fs_event_t handle;
  v8::Global<v8::Function> callback;
  bool closed = false;
};

static void OnFsWatch(uv_fs_event_t* handle, const char* filename, int events, int status) {
  auto* w = static_cast<FsWatch*>(handle->data);
  if (!w || w->closed || status < 0) return;
  EnterIsolate scope(nx8v::g_runtime->isolate);
  v8::Isolate* isolate = scope.isolate;
  auto ctx = isolate->GetCurrentContext();
  const char* kind = (events & UV_RENAME) ? "rename" : "change";
  std::string name = filename ? filename : "";
  v8::Local<v8::Value> argv[] = {
      v8::String::NewFromUtf8(isolate, kind, v8::NewStringType::kNormal).ToLocalChecked(),
      v8::String::NewFromUtf8(isolate, name.c_str(), v8::NewStringType::kNormal,
                              (int)name.size())
          .ToLocalChecked()};
  v8::TryCatch tc(isolate);
  v8::Local<v8::Value> ignored;
  auto cb = w->callback.Get(isolate);
  if (!cb.IsEmpty())
    (void)cb->Call(ctx, ctx->Global(), 2, argv).ToLocal(&ignored);
}

static void WatchClose(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* w = static_cast<FsWatch*>(
      args.DataV2().As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));
  if (!w || w->closed) return;
  w->closed = true;
  uv_fs_event_stop(&w->handle);
  uv_close(reinterpret_cast<uv_handle_t*>(&w->handle), [](uv_handle_t* h) {
    auto* w = static_cast<FsWatch*>(h->data);
    w->callback.Reset();
    delete w;
  });
}

static void Watch(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  auto ctx = isolate->GetCurrentContext();
  if (args.Length() < 2 || !args[1]->IsFunction()) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "watch(path, listener)")));
    return;
  }
  auto* w = new FsWatch();
  w->callback.Reset(isolate, args[1].As<v8::Function>());
  uv_fs_event_init(nx8v::g_runtime->loop, &w->handle);
  w->handle.data = w;
  std::string path = ToStdString(isolate, args[0]);
  int r = uv_fs_event_start(&w->handle, OnFsWatch, path.c_str(), 0);
  if (r < 0) {
    std::string msg = uv_strerror(r);
    w->closed = true;
    w->callback.Reset();
    uv_close(reinterpret_cast<uv_handle_t*>(&w->handle), [](uv_handle_t* h) {
      delete static_cast<FsWatch*>(h->data);
    });
    isolate->ThrowException(MakeError(isolate, msg));
    return;
  }
  auto ext = v8::External::New(isolate, w, v8::kExternalPointerTypeTagDefault);
  auto obj = v8::Object::New(isolate);
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "close"),
           v8::Function::New(ctx, WatchClose, ext).ToLocalChecked())
      .Check();
  args.GetReturnValue().Set(obj);
}

// ---------------------------------------------------------------------------
// Build the fs module object
// ---------------------------------------------------------------------------
inline v8::Local<v8::Object> MakeFsModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    obj->Set(ctx,
      v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal).ToLocalChecked(),
      v8::Function::New(ctx, cb).ToLocalChecked()).Check();
  };

  def("readFileSync",  ReadFileSync);
  def("writeFileSync", WriteFileSync);
  def("existsSync",    ExistsSync);
  def("mkdirSync",     MkdirSync);
  def("unlinkSync",    UnlinkSync);
  def("readdirSync",   ReaddirSync);
  def("statSync",      StatSync);
  def("readFile",      ReadFileAsync);
  def("writeFile",     WriteFileAsync);
  def("createReadStream",  CreateReadStream);
  def("createWriteStream", CreateWriteStream);
  def("renameSync",    RenameSync);
  def("rename",        RenameAsync);
  def("copyFileSync",  CopyFileSync);
  def("copyFile",      CopyFileAsync);
  def("appendFileSync", AppendFileSync);
  def("appendFile",    AppendFileAsync);
  def("rmSync",        RmSync);
  def("rm",            RmAsync);
  def("watch",         Watch);

  return hs.Escape(obj);
}

}} // nx8v::bindings
