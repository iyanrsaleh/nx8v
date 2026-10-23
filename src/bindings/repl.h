#pragma once
// nx8v/src/bindings/repl.h
// Simple interactive REPL: read a line, compile+run, print result.
// Activated when nx8v is launched with no arguments.

#include <v8.h>
#include <iostream>
#include <string>
#include <cstdio>
#include "../utils.h"

namespace nx8v {
namespace bindings {

inline void RunRepl(v8::Isolate* isolate) {
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  std::cout << "NX8V JavaScript Runtime  (Ctrl+C or .exit to quit)\n";
  std::cout << "Version: nx8v/0.1.0\n\n";

  std::string line;
  for (;;) {
    // Prompt
    std::cout << "> " << std::flush;
    if (!std::getline(std::cin, line)) break;  // EOF (Ctrl+D / Ctrl+Z)

    // REPL commands
    if (line == ".exit" || line == ".quit") break;
    if (line == ".help") {
      std::cout << "  .exit  - exit REPL\n"
                << "  .help  - show this help\n"
                << "  .clear - clear module cache\n";
      continue;
    }
    if (line == ".clear") {
      g_runtime->module_cache.clear();
      std::cout << "Module cache cleared.\n";
      continue;
    }
    if (line.empty()) continue;

    // Compile and run
    v8::TryCatch tc(isolate);
    v8::Local<v8::String> src;
    if (!v8::String::NewFromUtf8(isolate, line.c_str(),
                                 v8::NewStringType::kNormal,
                                 (int)line.size()).ToLocal(&src)) continue;

    v8::ScriptOrigin origin(
      v8::String::NewFromUtf8Literal(isolate, "<repl>"));

    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(ctx, src, &origin).ToLocal(&script)) {
      ReportException(isolate, &tc);
      continue;
    }

    v8::Local<v8::Value> result;
    if (!script->Run(ctx).ToLocal(&result)) {
      ReportException(isolate, &tc);
      continue;
    }

    // Print non-undefined results
    if (!result->IsUndefined()) {
      v8::Local<v8::Value> json;
      if (v8::JSON::Stringify(ctx, result).ToLocal(&json)) {
        v8::String::Utf8Value json_str(isolate, json);
        std::cout << (*json_str ? *json_str : "null") << '\n';
      } else {
        v8::String::Utf8Value str(isolate, result);
        std::cout << (*str ? *str : "null") << '\n';
      }
    }

    // Pump microtasks (for Promise results shown in REPL)
    isolate->PerformMicrotaskCheckpoint();
  }

  std::cout << "\nBye.\n";
}

}} // nx8v::bindings
