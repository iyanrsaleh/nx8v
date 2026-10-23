#pragma once
// nx8v/src/bindings/test.h
// Built-in test runner:
//
//   nx8v test                  # jalankan semua *.test.js / test/*.js
//   nx8v test tests/           # folder tertentu
//   nx8v test a.test.js        # file tertentu
//
// Di dalam file test:
//   test('nama tes', () => {
//     assert.equal(1 + 1, 2);
//     assert.strictEqual(typeof 'x', 'string');
//     assert.ok(true);
//     assert.throws(() => { throw new Error('x'); });
//     assert.deepEqual([1,2], [1,2]);
//   });

#include <v8.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "../runtime.h"
#include "../utils.h"

namespace fs = std::filesystem;

namespace nx8v {
namespace bindings {

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
struct TestCase {
  std::string              name;
  v8::Global<v8::Function> fn;
};

struct TestRegistry {
  std::vector<TestCase>    tests;
  std::string              current_file;
  int                      passed = 0;
  int                      failed = 0;
  std::vector<std::string> failures;
};

static TestRegistry* g_test_registry = nullptr;

// ---------------------------------------------------------------------------
// Assertions
// ---------------------------------------------------------------------------
static std::string TestStringify(v8::Isolate* isolate, v8::Local<v8::Context> ctx,
                                 v8::Local<v8::Value> v) {
  v8::Local<v8::Value> json;
  if (v8::JSON::Stringify(ctx, v).ToLocal(&json)) {
    v8::String::Utf8Value s(isolate, json);
    if (*s) return std::string(*s, s.length());
  }
  v8::String::Utf8Value s(isolate, v);
  return *s ? std::string(*s, s.length()) : std::string("<unprintable>");
}

static void AssertFail(v8::Isolate* isolate, const std::string& msg) {
  isolate->ThrowException(v8::Exception::Error(
      v8::String::NewFromUtf8(isolate, msg.c_str(), v8::NewStringType::kNormal,
                              (int)msg.size()).ToLocalChecked()));
}

static void TestAssertEqual(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 2) { AssertFail(isolate, "assert.equal(a, b) needs 2 args"); return; }

  auto a = args[0], b = args[1];
  bool same = a->StrictEquals(b);
  if (!same && a->IsNumber() && b->IsNumber()) {
    same = a.As<v8::Number>()->Value() == b.As<v8::Number>()->Value();
  }
  if (!same && a->IsString() && b->IsString()) {
    v8::String::Utf8Value sa(isolate, a), sb(isolate, b);
    same = sa.length() == sb.length() && std::string(*sa, sa.length()) == std::string(*sb, sb.length());
  }
  if (!same) {
    AssertFail(isolate, "expected " + TestStringify(isolate, ctx, b) +
                            " but got " + TestStringify(isolate, ctx, a));
  }
}

static void TestAssertDeepEqual(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 2) { AssertFail(isolate, "assert.deepEqual(a, b) needs 2 args"); return; }

  std::string ja = TestStringify(isolate, ctx, args[0]);
  std::string jb = TestStringify(isolate, ctx, args[1]);
  if (ja != jb) {
    AssertFail(isolate, "expected " + jb + " but got " + ja);
  }
}

static void TestAssertOk(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  bool truthy = args.Length() >= 1 && args[0]->BooleanValue(isolate);
  if (!truthy) AssertFail(isolate, "expected value to be truthy");
}

static void TestAssertNotOk(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  bool truthy = args.Length() >= 1 && args[0]->BooleanValue(isolate);
  if (truthy) AssertFail(isolate, "expected value to be falsy");
}

static void TestAssertThrows(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 1 || !args[0]->IsFunction()) {
    AssertFail(isolate, "assert.throws(fn) requires a function");
    return;
  }
  v8::TryCatch tc(isolate);
  v8::Local<v8::Value> ignored;
  (void)args[0].As<v8::Function>()
      ->Call(ctx, v8::Undefined(isolate), 0, nullptr)
      .ToLocal(&ignored);
  if (!tc.HasCaught()) {
    AssertFail(isolate, "expected function to throw, but it did not");
  } else {
    tc.Reset();
  }
}

static void TestAssertNotThrows(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 1 || !args[0]->IsFunction()) return;
  v8::TryCatch tc(isolate);
  v8::Local<v8::Value> ignored;
  (void)args[0].As<v8::Function>()
      ->Call(ctx, v8::Undefined(isolate), 0, nullptr)
      .ToLocal(&ignored);
  if (tc.HasCaught()) {
    v8::String::Utf8Value ex(isolate, tc.Exception());
    AssertFail(isolate, std::string("expected no throw, but got: ") +
                            (*ex ? *ex : "?"));
  }
}

// ---------------------------------------------------------------------------
// test(name, fn) / describe(name, fn)
// ---------------------------------------------------------------------------
static void TestRegister(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  if (!g_test_registry) return;
  if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsFunction()) {
    AssertFail(isolate, "test(name, fn) requires a name and a function");
    return;
  }
  v8::String::Utf8Value name(isolate, args[0]);
  TestCase tc;
  tc.name = *name ? std::string(*name, name.length()) : "";
  tc.fn.Reset(isolate, args[1].As<v8::Function>());
  g_test_registry->tests.push_back(std::move(tc));
}

// describe(name, fn): runs fn immediately (tests inside are collected).
static void TestDescribe(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 2 || !args[1]->IsFunction()) return;

  if (args.Length() >= 1 && args[0]->IsString() && g_test_registry) {
    v8::String::Utf8Value g(isolate, args[0]);
    g_test_registry->current_file = *g ? std::string(*g) : "";
  }
  v8::Local<v8::Value> ignored;
  (void)args[1].As<v8::Function>()
      ->Call(ctx, v8::Undefined(isolate), 0, nullptr)
      .ToLocal(&ignored);
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
inline void RegisterTestGlobals(v8::Isolate* isolate,
                                v8::Local<v8::Object> global) {
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  global->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "test"),
              v8::Function::New(ctx, TestRegister).ToLocalChecked()).Check();
  global->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "it"),
              v8::Function::New(ctx, TestRegister).ToLocalChecked()).Check();
  global->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "describe"),
              v8::Function::New(ctx, TestDescribe).ToLocalChecked()).Check();
}

inline v8::Local<v8::Object> MakeAssertObject(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    obj->Set(ctx, v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal,
                                          (int)std::strlen(name)).ToLocalChecked(),
             v8::Function::New(ctx, cb).ToLocalChecked()).Check();
  };
  def("equal", TestAssertEqual);
  def("strictEqual", TestAssertEqual);
  def("notEqual", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope hs(iso);
    if (args.Length() >= 2 && args[0]->StrictEquals(args[1]))
      AssertFail(iso, "expected values to differ");
  });
  def("deepEqual", TestAssertDeepEqual);
  def("deepStrictEqual", TestAssertDeepEqual);
  def("ok", TestAssertOk);
  def("truthy", TestAssertOk);
  def("notOk", TestAssertNotOk);
  def("throws", TestAssertThrows);
  def("doesNotThrow", TestAssertNotThrows);

  return hs.Escape(obj);
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------
inline bool TestIsTestFile(const fs::path& p) {
  std::string name = p.filename().string();
  if (name.size() >= 8 && name.compare(name.size() - 8, 8, ".test.js") == 0)
    return true;
  if (name.size() >= 7 && name.compare(name.size() - 7, 7, ".spec.js") == 0)
    return true;
  // Inside a "test" or "tests" directory: any .js file counts.
  for (auto part : p) {
    std::string s = part.string();
    if (s == "test" || s == "tests" || s == "__tests__") return true;
  }
  return false;
}

inline std::vector<fs::path> TestDiscover(const fs::path& root) {
  std::vector<fs::path> files;
  std::error_code ec;
  if (!fs::exists(root, ec)) return files;

  if (fs::is_regular_file(root, ec)) {
    files.push_back(root);
    return files;
  }

  fs::recursive_directory_iterator it(root,
      fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;
  while (it != end) {
    if (ec) { it.increment(ec); continue; }
    const auto& p = it->path();
    if (it->is_directory(ec)) {
      std::string d = p.filename().string();
      if (d == "node_modules" || d == ".git" || d == "build" || d == "deps") {
        it.disable_recursion_pending();
      }
    } else if (it->is_regular_file(ec) && TestIsTestFile(p)) {
      files.push_back(p);
    }
    it.increment(ec);
  }
  return files;
}

// ---------------------------------------------------------------------------
// Runner (called with an isolate + context already active)
// ---------------------------------------------------------------------------
inline int RunTestFiles(v8::Isolate* isolate, const std::vector<fs::path>& files) {
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  TestRegistry registry;
  g_test_registry = &registry;

  int total_pass = 0, total_fail = 0;
  std::vector<std::string> failures;

  for (const auto& file : files) {
    registry.tests.clear();
    registry.current_file = file.string();

    std::string source = ReadFile(file.string().c_str());
    if (source.empty()) {
      std::printf("  ! cannot read %s\n", file.string().c_str());
      ++total_fail;
      failures.push_back(file.string() + ": unreadable");
      continue;
    }

    std::printf("\n%s\n", file.string().c_str());

    // Load the file (CommonJS wrapper, same as require()).
    std::string wrapped =
        "(function(module, exports, require, __filename, __dirname) {\n" +
        source + "\n})";
    v8::Local<v8::String> src_str =
        v8::String::NewFromUtf8(isolate, wrapped.c_str(),
                                v8::NewStringType::kNormal,
                                (int)wrapped.size()).ToLocalChecked();
    v8::ScriptOrigin origin(v8::String::NewFromUtf8(
        isolate, file.string().c_str(), v8::NewStringType::kNormal).ToLocalChecked());

    v8::TryCatch tc(isolate);
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(ctx, src_str, &origin).ToLocal(&script)) {
      ReportException(isolate, &tc);
      ++total_fail;
      failures.push_back(file.string() + ": compile error");
      continue;
    }

    v8::Local<v8::Value> fn_val;
    if (!script->Run(ctx).ToLocal(&fn_val) || !fn_val->IsFunction()) {
      ReportException(isolate, &tc);
      ++total_fail;
      failures.push_back(file.string() + ": load error");
      continue;
    }

    auto module_obj  = v8::Object::New(isolate);
    auto exports_obj = v8::Object::New(isolate);
    module_obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "exports"),
                    exports_obj).Check();

    std::string dir = file.parent_path().string();
    auto require_fn = MakeRequire(isolate, dir);

    v8::Local<v8::Value> argv[] = {
      module_obj, exports_obj, require_fn,
      v8::String::NewFromUtf8(isolate, file.string().c_str(),
                              v8::NewStringType::kNormal).ToLocalChecked(),
      v8::String::NewFromUtf8(isolate, dir.c_str(),
                              v8::NewStringType::kNormal).ToLocalChecked() };
    v8::Local<v8::Value> ignored;
    if (!fn_val.As<v8::Function>()->Call(ctx, ctx->Global(), 5, argv).ToLocal(&ignored)) {
      ReportException(isolate, &tc);
      ++total_fail;
      failures.push_back(file.string() + ": runtime error while loading");
      continue;
    }

    // Run collected tests.
    for (auto& t : registry.tests) {
      v8::TryCatch ttc(isolate);
      v8::Local<v8::Value> r;
      (void)t.fn.Get(isolate)->Call(ctx, v8::Undefined(isolate), 0, nullptr).ToLocal(&r);

      if (ttc.HasCaught()) {
        v8::String::Utf8Value ex(isolate, ttc.Exception());
        std::printf("  \xE2\x9C\x97 %s\n      %s\n", t.name.c_str(),
                    *ex ? *ex : "?");
        ++total_fail;
        failures.push_back(file.filename().string() + " > " + t.name);
      } else {
        std::printf("  \xE2\x9C\x93 %s\n", t.name.c_str());
        ++total_pass;
      }
      t.fn.Reset();
    }
    registry.tests.clear();
  }

  g_test_registry = nullptr;

  std::printf("\n----------------------------------------\n");
  std::printf("%d passed, %d failed\n", total_pass, total_fail);
  if (!failures.empty()) {
    std::printf("\nFailed:\n");
    for (auto& f : failures) std::printf("  - %s\n", f.c_str());
  }
  return total_fail == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Entry point (needs an active isolate + context)
// ---------------------------------------------------------------------------
inline int RunTest(v8::Isolate* isolate, int argc, char* argv[]) {
  fs::path root = ".";
  if (argc >= 3) root = argv[2];

  auto files = TestDiscover(root);
  if (files.empty()) {
    std::printf("No test file found in '%s'\n", root.string().c_str());
    std::printf("Add a *.test.js file, or a test/ folder containing *.js\n");
    return 1;
  }

  std::printf("Running %d test file(s)\n", (int)files.size());
  return RunTestFiles(isolate, files);
}

}} // nx8v::bindings
