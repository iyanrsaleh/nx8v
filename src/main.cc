// NX8V - Custom JavaScript Runtime
// Entry point: wires together V8, libuv event loop, and all bindings.
//
// Usage:
//   nx8v                  → REPL mode
//   nx8v init             → project wizard (project.json + server.js + public/)
//   nx8v <script.js>      → Execute file

#include <libplatform/libplatform.h>
#include <v8.h>
#include <uv.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "runtime.h"
#include "utils.h"
#include "bindings/console.h"
#include "bindings/process.h"
#include "bindings/timers.h"
#include "bindings/loop.h"
#include "bindings/module.h"
#include "bindings/http.h"
#include "bindings/repl.h"
#include "bindings/init.h"
#include "bindings/dev.h"
#include "bindings/list.h"
#include "bindings/package.h"
#include "bindings/window.h"
#include "bindings/test.h"
#include "bindings/manifest.h"
#include "bindings/logs.h"
#include "bindings/install.h"
#include "bindings/querystring.h"
namespace fs = std::filesystem;

// Global runtime instance (referenced by all bindings via extern)
nx8v::Runtime* nx8v::g_runtime = nullptr;

// ---------------------------------------------------------------------------
// Build the global object template
// ---------------------------------------------------------------------------
static v8::Local<v8::ObjectTemplate> BuildGlobal(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  auto global = v8::ObjectTemplate::New(isolate);

  nx8v::bindings::RegisterTimers(isolate, global);

  return hs.Escape(global);
}

// ---------------------------------------------------------------------------
// Set properties that need runtime info (argc/argv, script path).
// ---------------------------------------------------------------------------
static void PopulateGlobal(v8::Isolate* isolate,
                           v8::Local<v8::Context> ctx,
                           int argc, char** argv,
                           const std::string& script_dir) {
  v8::HandleScope hs(isolate);
  auto global = ctx->Global();

  // console
  global->Set(ctx,
    v8::String::NewFromUtf8Literal(isolate, "console"),
    nx8v::bindings::MakeConsoleObject(isolate)).Check();

  // process
  global->Set(ctx,
    v8::String::NewFromUtf8Literal(isolate, "process"),
    nx8v::bindings::MakeProcessObject(isolate, argc, argv)).Check();

  // require
  global->Set(ctx,
    v8::String::NewFromUtf8Literal(isolate, "require"),
    nx8v::bindings::MakeRequire(isolate, script_dir)).Check();

  // fetch (WHATWG-ish, Promise based)
  global->Set(ctx,
    v8::String::NewFromUtf8Literal(isolate, "fetch"),
    v8::Function::New(ctx, nx8v::bindings::Nx8vFetch).ToLocalChecked()).Check();

  // test runner globals
  nx8v::bindings::RegisterTestGlobals(isolate, global);

  // assert (test runner)
  global->Set(ctx,
    v8::String::NewFromUtf8Literal(isolate, "assert"),
    nx8v::bindings::MakeAssertObject(isolate)).Check();

  // Buffer (Node-style)
  global->Set(ctx,
    v8::String::NewFromUtf8Literal(isolate, "Buffer"),
    nx8v::bindings::MakeBufferModule(isolate)).Check();

  global->Set(ctx,
    v8::String::NewFromUtf8Literal(isolate, "URLSearchParams"),
    nx8v::bindings::MakeURLSearchParams(isolate)).Check();

  // __filename / __dirname (script mode only)
  if (argc >= 2) {
    std::string abs_path = fs::absolute(fs::path(argv[1])).string();
    std::string dir      = fs::path(abs_path).parent_path().string();
    global->Set(ctx,
      v8::String::NewFromUtf8Literal(isolate, "__filename"),
      v8::String::NewFromUtf8(isolate, abs_path.c_str(),
                              v8::NewStringType::kNormal).ToLocalChecked()).Check();
    global->Set(ctx,
      v8::String::NewFromUtf8Literal(isolate, "__dirname"),
      v8::String::NewFromUtf8(isolate, dir.c_str(),
                              v8::NewStringType::kNormal).ToLocalChecked()).Check();
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  bool test_mode = false;
  bool window_mode = false;
  int  window_w = 1100, window_h = 760;
  bool cli_w = false, cli_h = false, cli_show = false, cli_max = false;
  bool cli_min_w = false, cli_min_h = false, cli_pos = false, cli_center = false;
  bool cli_frame = false, cli_resize = false, cli_single = false;
  std::string window_title = "nx8v";
  std::string window_url;
  int window_listen_port = 0;
  nx8v::bindings::Nx8vWindowOpts window_opts;

  bool packed = false;
#ifdef _WIN32
  {
    int installed = nx8v::bindings::Nx8vRunInstaller(argc, argv);
    if (installed >= 0) return installed;
  }
  packed = nx8v::bindings::Nx8vPreparePackedApp();
#endif

  // A packaged app is either packed inside the exe, or the runtime exe
  // sitting next to project.json. Double-clicking it opens the desktop window.
  if (argc < 2) {
#ifdef _WIN32
    if (packed) {
      window_mode = true;
    } else {
    wchar_t mod[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, mod, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      fs::path exe_dir = fs::path(mod).parent_path();
      std::error_code ec;
      if (fs::exists(exe_dir / "project.json", ec)) {
        SetCurrentDirectoryW(exe_dir.wstring().c_str());
        window_mode = true;
      }
    }
    }
#endif
  }

  // -- CLI subcommands (no V8 needed) --
  if (argc >= 2) {
    std::string arg1 = argv[1];
    if (arg1 == "init") return nx8v::bindings::RunInit();
    if (arg1 == "i" || arg1 == "install") return nx8v::bindings::RunInstall(argc, argv);
    if (arg1 == "u" || arg1 == "update") return nx8v::bindings::RunUpdate(argc, argv);
    if (arg1 == "p" || arg1 == "publish") return nx8v::bindings::RunPublish(argc, argv);
    if (arg1 == "d" || arg1 == "delete") return nx8v::bindings::RunRemove(argc, argv);
    if (arg1 == "r" || arg1 == "repo") return nx8v::bindings::RunRepoRemove(argc, argv);
    if (arg1 == "stop") return nx8v::bindings::RunStop(argc, argv);
    if (arg1 == "list") return nx8v::bindings::RunList(argc, argv);
    if (arg1 == "logs") return nx8v::bindings::RunLogs(argc, argv);
    if (arg1 == "package") return nx8v::bindings::RunPackage(argc, argv);
    if (arg1 == "test") test_mode = true;
    if (arg1 == "window") {
      window_mode = true;
      // Collect --width/--height/--title/--url and drop them from argv,
      // leaving the script path (if any) at argv[1].
      std::vector<char*> kept;
      kept.push_back(argv[0]);
      for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--width=", 0) == 0)  { window_w = std::atoi(a.c_str() + 8); cli_w = true; continue; }
        if (a.rfind("--height=", 0) == 0) { window_h = std::atoi(a.c_str() + 9); cli_h = true; continue; }
        if (a == "--maximized") { window_opts.show = 1; cli_show = true; continue; }
        if (a == "--minimized") { window_opts.show = 2; cli_show = true; continue; }
        if (a.rfind("--state=", 0) == 0) {
          window_opts.show = nx8v::bindings::Nx8vWindowShowFromName(a.substr(8));
          cli_show = true;
          continue;
        }
        if (a == "--no-maximize") { window_opts.maximizable = false; cli_max = true; continue; }
        if (a.rfind("--min-width=", 0) == 0)  { window_opts.min_w = std::atoi(a.c_str() + 12); cli_min_w = true; continue; }
        if (a.rfind("--min-height=", 0) == 0) { window_opts.min_h = std::atoi(a.c_str() + 13); cli_min_h = true; continue; }
        if (a.rfind("--x=", 0) == 0) { window_opts.has_pos = true; window_opts.x = std::atoi(a.c_str() + 4); cli_pos = true; continue; }
        if (a.rfind("--y=", 0) == 0) { window_opts.has_pos = true; window_opts.y = std::atoi(a.c_str() + 4); cli_pos = true; continue; }
        if (a == "--center") { window_opts.center = true; cli_center = true; cli_pos = true; continue; }
        if (a == "--frameless") { window_opts.frameless = true; cli_frame = true; continue; }
        if (a == "--no-resize") { window_opts.resizable = false; cli_resize = true; continue; }
        if (a == "--multi-instance") { window_opts.single_instance = false; cli_single = true; continue; }
        if (a.rfind("--icon=", 0) == 0) { window_opts.icon = a.substr(7); continue; }
        if (a.rfind("--title=", 0) == 0)  { window_title = a.substr(8); continue; }
        if (a.rfind("--url=", 0) == 0)    { window_url = a.substr(6); continue; }
        kept.push_back(argv[i]);
      }
      for (size_t i = 0; i < kept.size(); ++i) argv[i] = kept[i];
      argc = (int)kept.size();
    }
    if (arg1 == "run") {
      if (argc < 3) {
        std::puts("Penggunaan: nx8v run <script.js>");
        return 1;
      }
      // Shift so argv[1] is the script, then fall through to normal execution.
      for (int i = 2; i < argc; ++i) argv[i - 1] = argv[i];
      --argc;
    }
    if (arg1 == "dev") return nx8v::bindings::RunDev(argc, argv);
    if (arg1 == "start") {
      int rc = nx8v::bindings::RunStart(argc, argv);
      if (rc >= 0) return rc;          // handled (error/help)
      // rc == -1 → fall through, argv[1] now points to the project script
    }
    if (arg1 == "--version" || arg1 == "-v") {
      std::puts("nx8v/0.1.0");
      return 0;
    }
    if (arg1 == "--help" || arg1 == "-h") {
      std::puts(
        "nx8v - JavaScript runtime berbasis V8 + libuv\n"
        "\n"
        "Penggunaan:\n"
        "  nx8v                  REPL interaktif\n"
        "  nx8v init             wizard pembuatan proyek\n"
        "  nx8v i <nama>@<versi> pasang modul dari repository di project.json\n"
        "  nx8v u <nama>@<versi> ganti versi modul yang sudah terpasang\n"
        "  nx8v p <nama>@<versi> terbitkan modules/<nama> ke repository\n"
        "  nx8v d <nama>         hapus modul dari dependencies\n"
        "  nx8v r <nama>         hapus rilis, tag, dan folder modul dari repository\n"
        "  nx8v dev  [file] [--port=N]   mode development + auto-restart\n"
        "  nx8v start [file] [--port=N]  jalankan proyek (project.json)\n"
        "  nx8v stop [--port=N]  hentikan server proyek ini\n"
        "  nx8v list             daftar server nx8v yang berjalan\n"
        "  nx8v logs [--lines=N] ekor file nx8v.log proyek ini\n"
        "  nx8v test [path]      jalankan test (*.test.js)\n"
        "  nx8v package          buat folder distribusi (dist/)\n"
        "  nx8v window [file]    jendela desktop (WebView2) untuk proyek\n"
        "  nx8v run <script.js>  jalankan script (eksplisit)\n"
        "  nx8v <script.js>      eksekusi file script\n"
        "  nx8v --version        tampilkan versi\n"
        "  nx8v --help           tampilkan bantuan ini");
      return 0;
    }
  }

#ifdef _WIN32
  if (window_mode) {
    std::error_code ec;
    fs::path manifest = fs::current_path(ec) / "project.json";
    if (fs::exists(manifest, ec)) {
      std::string json = nx8v::bindings::ReadManifestFile(manifest);
      nx8v::bindings::Nx8vApplyDesktopWindow(
          json, window_w, window_h, window_opts,
          cli_w, cli_h, cli_show, cli_max, cli_min_w, cli_min_h, cli_pos,
          cli_center, cli_frame, cli_resize, cli_single);
    }
    if (window_opts.single_instance) {
      fs::path dir = fs::absolute(fs::current_path(ec), ec).lexically_normal();
      if (!nx8v::bindings::Nx8vTakeWindowInstance(dir.wstring())) {
        std::puts("nx8v window: jendela sudah terbuka");
        return 0;
      }
    }
  }
#endif

  // -- libuv event loop --
  uv_loop_t loop;
  uv_loop_init(&loop);

  // -- V8 initialisation --
  v8::V8::InitializeICUDefaultLocation(argv[0]);
  v8::V8::InitializeExternalStartupData(argv[0]);

  std::unique_ptr<v8::Platform> platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();

  // -- Isolate --
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  v8::Isolate* isolate = v8::Isolate::New(params);

  // -- Runtime --
  nx8v::Runtime runtime_inst;
  runtime_inst.isolate = isolate;
  runtime_inst.argc    = argc;
  runtime_inst.argv    = argv;
  runtime_inst.loop    = &loop;
  nx8v::g_runtime      = &runtime_inst;

  int exit_code = 0;

  {
    v8::Isolate::Scope iso_scope(isolate);
    v8::HandleScope    handle_scope(isolate);

    auto global_tpl = BuildGlobal(isolate);
    auto ctx        = v8::Context::New(isolate, nullptr, global_tpl);
    v8::Context::Scope ctx_scope(ctx);

    runtime_inst.context.Reset(isolate, ctx);

    // Determine mode and working directory
    bool repl_mode = (argc < 2) && !test_mode && !window_mode;

    // nx8v window with no script: use project.json "main".
    if (window_mode && argc < 2) {
      std::error_code ec;
      fs::path manifest = fs::current_path(ec) / "project.json";
      if (fs::exists(manifest, ec)) {
        std::string json = nx8v::bindings::ReadManifestFile(manifest);
        std::string main_file;
        if (nx8v::bindings::JsonFindString(json, "main", main_file) &&
            !main_file.empty()) {
          static std::string win_script = main_file;
          static char win_buf[1024];
          std::snprintf(win_buf, sizeof(win_buf), "%s", win_script.c_str());
          argv[1] = win_buf;
          argc = 2;
        }
      }
    }
    std::string script_dir;
    if (!repl_mode) {
      std::error_code ec;
      auto abs   = fs::absolute(fs::path(argv[1]), ec);
      script_dir = ec ? std::string(".") : abs.parent_path().string();
    } else {
      std::error_code ec;
      script_dir = fs::current_path(ec).string();
    }

    if (!test_mode) {
      std::string script_for_env = (argc >= 2) ? argv[1] : "";
      std::string log_path;
      if (window_mode) {
        int want = 0;
        std::error_code wec;
        fs::path manifest = fs::current_path(wec) / "project.json";
        if (fs::exists(manifest, wec)) {
          nx8v::bindings::JsonFindNumber(
              nx8v::bindings::ReadManifestFile(manifest), "port", want);
        }
        int safe = nx8v::bindings::Nx8vSafeListenPort(want);
        if (want > 0 && safe != want) {
          std::printf("port %d diblokir WebView2 (ERR_UNSAFE_PORT). Jendela memakai %d.\n",
                      want, safe);
        }
        window_listen_port = safe;
        std::string ps = std::to_string(safe);
#ifdef _WIN32
        _putenv_s("NX8V_PORT", ps.c_str());
        // Window watches files itself. NX8V_DEV must not inject a second poller.
        _putenv_s("NX8V_WINDOW", "1");
        _putenv_s("NX8V_DEV", "");
        SetEnvironmentVariableA("NX8V_WINDOW", "1");
        SetEnvironmentVariableA("NX8V_DEV", nullptr);
#else
        setenv("NX8V_PORT", ps.c_str(), 1);
        unsetenv("NX8V_DEV");
#endif
      }
      nx8v::bindings::ApplyProjectEnv(script_for_env, log_path);
      runtime_inst.log_path = log_path;
    }

    PopulateGlobal(isolate, ctx, argc, argv, script_dir);
    runtime_inst.script_dir = script_dir;

    // Install the V8 microtask pump on the libuv loop
    nx8v::bindings::SetupMicrotaskPump(isolate, &loop);

    if (test_mode) {
      exit_code = nx8v::bindings::RunTest(isolate, argc, argv);
    } else if (repl_mode) {
      nx8v::bindings::RunRepl(isolate);
    } else {
      const char* path   = argv[1];
      std::string source = nx8v::ReadFile(path);

      // Entry script may be an ES module (.mjs or import/export at top level).
      std::string path_str(path);
      bool esm_entry =
          (path_str.size() >= 4 &&
           path_str.compare(path_str.size() - 4, 4, ".mjs") == 0) ||
          nx8v::bindings::EsmLooksLikeModule(source);
      bool esm_ok = true;
      if (esm_entry && !source.empty()) {
        esm_ok = nx8v::bindings::ExecuteModuleEntry(isolate, source, path_str);
      }
      if (source.empty()) {
        exit_code = 1;
      } else if (!esm_ok) {
        exit_code = 1;
      } else if (!esm_entry && !nx8v::ExecuteScript(isolate, source, path)) {
        exit_code = 1;
      } else if (window_mode) {
        // Resolve the URL to open: explicit --url, or the project port.
        std::string url = window_url;
        if (url.empty()) {
          int port = window_listen_port;
          std::error_code ec;
          fs::path manifest = fs::current_path(ec) / "project.json";
          if (fs::exists(manifest, ec)) {
            std::string json = nx8v::bindings::ReadManifestFile(manifest);
            if (port <= 0) nx8v::bindings::JsonFindNumber(json, "port", port);
            nx8v::bindings::Nx8vApplyDesktopWindow(
                json, window_w, window_h, window_opts,
                cli_w, cli_h, cli_show, cli_max, cli_min_w, cli_min_h, cli_pos,
                cli_center, cli_frame, cli_resize, cli_single);
          }
          if (port <= 0) port = 4000;
          port = nx8v::bindings::Nx8vSafeListenPort(port);
          url = "http://127.0.0.1:" + std::to_string(port) + "/?nx8v=2";
        }
        std::printf("nx8v window: membuka %s\n", url.c_str());
        exit_code = nx8v::bindings::RunWindow(url, window_title, window_w,
                                             window_h, &loop, window_opts);
      } else {
        // Hand control to libuv: runs timers + pumps microtasks
        nx8v::bindings::RunEventLoop(isolate, &loop);
      }
    }

    {
      v8::Local<v8::Value> proc_v;
      if (ctx->Global()
              ->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "process"))
              .ToLocal(&proc_v) &&
          proc_v->IsObject()) {
        v8::Local<v8::Value> ec_v;
        if (proc_v.As<v8::Object>()
                ->Get(ctx, v8::String::NewFromUtf8Literal(isolate, "exitCode"))
                .ToLocal(&ec_v) &&
            ec_v->IsNumber()) {
          exit_code = ec_v->Int32Value(ctx).FromMaybe(exit_code);
        }
      }
    }

    runtime_inst.context.Reset();
    // Release every Global handle while the isolate is still alive.
    // Destroying a v8::Global after Isolate::Dispose() is undefined behaviour.
    runtime_inst.module_cache.clear();
  }

  // -- Teardown --
  nx8v::bindings::TeardownLoop(&loop);
  uv_loop_close(&loop);

  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  delete params.array_buffer_allocator;

  return exit_code;
}
