#pragma once
// nx8v/src/utils.h
// File I/O and exception reporting helpers.

#include <v8.h>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>

namespace nx8v {

// Read entire file into string. Returns "" on failure.
inline std::string ReadFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { std::fprintf(stderr, "Error: cannot open '%s'\n", path); return {}; }
  std::ostringstream buf;
  buf << f.rdbuf();
  return buf.str();
}

// Print exception info from TryCatch to stderr.
inline void ReportException(v8::Isolate* isolate, v8::TryCatch* tc) {
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  v8::String::Utf8Value exc(isolate, tc->Exception());
  const char* exc_str = *exc ? *exc : "(unknown)";

  v8::Local<v8::Message> msg = tc->Message();
  if (msg.IsEmpty()) {
    std::fprintf(stderr, "Uncaught: %s\n", exc_str);
    return;
  }

  v8::String::Utf8Value fname(isolate, msg->GetScriptOrigin().ResourceName());
  int line = msg->GetLineNumber(ctx).FromMaybe(0);
  std::fprintf(stderr, "%s:%d: %s\n", *fname ? *fname : "?", line, exc_str);

  v8::Local<v8::String> srcline;
  if (msg->GetSourceLine(ctx).ToLocal(&srcline)) {
    v8::String::Utf8Value src(isolate, srcline);
    std::fprintf(stderr, "%s\n", *src);
    int col = msg->GetStartColumn(ctx).FromMaybe(0);
    for (int i = 0; i < col; ++i) std::fputc(' ', stderr);
    std::fprintf(stderr, "^\n");
  }

  v8::Local<v8::Value> st;
  if (tc->StackTrace(ctx).ToLocal(&st) && st->IsString()) {
    v8::String::Utf8Value stv(isolate, st);
    std::fprintf(stderr, "%s\n", *stv);
  }
}

// Compile + run JS source. Returns true on success.
inline bool ExecuteScript(v8::Isolate* isolate,
                          const std::string& source,
                          const char* filename) {
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::TryCatch tc(isolate);

  v8::Local<v8::String> src_str;
  if (!v8::String::NewFromUtf8(isolate, source.c_str(),
                               v8::NewStringType::kNormal,
                               (int)source.size()).ToLocal(&src_str)) {
    return false;
  }

  v8::ScriptOrigin origin(
    v8::String::NewFromUtf8(isolate, filename,
                            v8::NewStringType::kNormal).ToLocalChecked());

  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(ctx, src_str, &origin).ToLocal(&script)) {
    ReportException(isolate, &tc); return false;
  }
  v8::Local<v8::Value> result;
  if (!script->Run(ctx).ToLocal(&result)) {
    ReportException(isolate, &tc); return false;
  }
  return true;
}

} // namespace nx8v
