#pragma once
// nx8v/src/bindings/console.h
// Exposes console.log / console.error / console.warn / console.dir
//
// The console object is built as a concrete JS object with v8::Function::New
// callbacks (the same mechanism the other bindings use) rather than an
// ObjectTemplate/FunctionTemplate pair.
//
// Output goes through C stdio with an explicit flush after every call so that
// it is not lost when the runtime exits.

#include <v8.h>
#include <cstdio>
#include <string>

#include "../runtime.h"

namespace nx8v {
namespace bindings {

static void _ConsoleAppendLog(const std::string& line) {
  if (!nx8v::g_runtime || nx8v::g_runtime->log_path.empty()) return;
  FILE* f = std::fopen(nx8v::g_runtime->log_path.c_str(), "ab");
  if (!f) return;
  std::fwrite(line.data(), 1, line.size(), f);
  std::fputc('\n', f);
  std::fclose(f);
}

static void _ConsoleWrite(const v8::FunctionCallbackInfo<v8::Value>& args,
                          FILE* out) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  std::string line;
  for (int i = 0; i < args.Length(); ++i) {
    if (i > 0) {
      std::fputc(' ', out);
      line += ' ';
    }
    v8::String::Utf8Value s(isolate, args[i]);
    const char* text = *s ? *s : "(null)";
    std::fputs(text, out);
    line += text;
  }
  std::fputc('\n', out);
  std::fflush(out);
  _ConsoleAppendLog(line);
}

static void ConsoleLog(const v8::FunctionCallbackInfo<v8::Value>& args) {
  _ConsoleWrite(args, stdout);
}
static void ConsoleError(const v8::FunctionCallbackInfo<v8::Value>& args) {
  _ConsoleWrite(args, stderr);
}
static void ConsoleDir(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 1) {
    std::fputs("undefined\n", stdout);
    std::fflush(stdout);
    return;
  }
  v8::Local<v8::Value> json;
  if (v8::JSON::Stringify(ctx, args[0]).ToLocal(&json)) {
    v8::String::Utf8Value s(isolate, json);
    std::fprintf(stdout, "%s\n", *s ? *s : "null");
  }
  std::fflush(stdout);
}

// Build the concrete `console` object.
inline v8::Local<v8::Object> MakeConsoleObject(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    obj->Set(ctx,
      v8::String::NewFromUtf8(isolate, name,
                              v8::NewStringType::kNormal).ToLocalChecked(),
      v8::Function::New(ctx, cb).ToLocalChecked()).Check();
  };

  def("log",   ConsoleLog);
  def("error", ConsoleError);
  def("warn",  ConsoleError);
  def("dir",   ConsoleDir);

  return hs.Escape(obj);
}

}} // nx8v::bindings
