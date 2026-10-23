#pragma once
// nx8v/src/bindings/dev.h
// `nx8v dev` — development supervisor with file watching.
//
// Spawns `nx8v <script>` as a child process, watches the project tree with a
// libuv fs_event, and restarts the child when watched files change.
//
//   nx8v dev            → use project.json (main + nx8v.port)
//   nx8v dev file.js    → watch + run a specific script
//   nx8v dev --port=N   → override port
//
// HTML/CSS/SVG saves reload the open browser. JS/JSON saves restart the server.

#include <uv.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "manifest.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace nx8v {
namespace bindings {

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
struct DevConfig {
  std::string script = "server.js";
  std::string dir;             // project root to watch
  int port = 0;
  // Extensions that trigger a restart.
  static constexpr const char* kWatchExts[] = {
    ".js", ".mjs", ".json", ".html", ".css", ".svg", nullptr
  };
};

static bool HasExt(const std::string& path, const char* const* exts) {
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string ext = path.substr(dot);
  for (const char* e = *exts; e; e = *++exts) {
    if (ext == e) return true;
  }
  return false;
}

static bool IsCodeFile(const std::string& path) {
  static constexpr const char* kExts[] = { ".js", ".mjs", ".json", nullptr };
  return HasExt(path, kExts);
}

static bool ShouldRestart(const std::string& path) {
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string ext = path.substr(dot);
  for (const char* e : DevConfig::kWatchExts) {
    if (!e) break;
    if (ext == e) return true;
  }
  return false;
}

static bool IsSkippedDir(const fs::path& p) {
  std::string n = p.filename().string();
  return n == "node_modules" || n == ".git" || n == "build" ||
         n == ".nx8v" || n == "dist";
}

// ---------------------------------------------------------------------------
// DevSupervisor: owns the loop, the child process, and the watchers.
// ---------------------------------------------------------------------------
struct DevSupervisor {
  uv_loop_t        loop{};
  uv_process_t     child{};
  bool             child_running = false;
  bool             restart_pending = false;
  bool             shutting_down = false;
  std::string      exe;             // path to nx8v.exe itself
  std::string      script;          // absolute path to entry script
  std::string      dir;             // project root (watched)
  int              port = 0;
  int              debounce_ms = 120;
  int              reload_gen = 0;
  bool             code_change = false;
  std::string      reload_file;

  // Watchers: one uv_fs_event per directory (recursive manual walk).
  std::vector<uv_fs_event_t*> watchers;

  // Debounce timer so an editor's burst of writes causes one restart.
  uv_timer_t        debounce{};
  // Kill/restart sequencing timer.
  uv_timer_t        restart_timer{};

  void PrintTag(const char* tag, const std::string& msg) {
    std::printf("[%s] %s\n", tag, msg.c_str());
    std::fflush(stdout);
  }
};

static void BumpDevReload(DevSupervisor* sup) {
  sup->reload_gen++;
  std::ofstream out(sup->reload_file, std::ios::trunc);
  out << sup->reload_gen;
}

// ---------------------------------------------------------------------------
// Child process lifecycle
// ---------------------------------------------------------------------------
static void StartChild(struct DevSupervisor* sup);

static void OnChildExit(uv_process_t* proc, int64_t exit_status, int term_sig) {
  auto* sup = static_cast<DevSupervisor*>(proc->data);
  sup->child_running = false;
  uv_close(reinterpret_cast<uv_handle_t*>(proc), nullptr);
  if (sup->shutting_down) return;

  if (sup->restart_pending) {
    sup->restart_pending = false;
    // Restart after a short delay.
    uv_timer_start(&sup->restart_timer, [](uv_timer_t* t) {
      auto* s = static_cast<DevSupervisor*>(t->data);
      s->PrintTag("dev", "restarting " + s->script);
      StartChild(s);
    }, 150, 0);
  } else if (exit_status != 0) {
    // Child crashed — keep supervisor alive so the user can edit & fix.
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "server exited with code %lld%s",
                  (long long)exit_status,
                  term_sig ? " (killed)" : "");
    sup->PrintTag("warn", buf);
    sup->PrintTag("dev", "edit files to restart, or Ctrl+C to quit");
  } else {
    sup->PrintTag("dev", "server exited cleanly");
    sup->PrintTag("dev", "edit files to restart, or Ctrl+C to quit");
  }
}

static void StartChild(DevSupervisor* sup) {
  BumpDevReload(sup);
  uv_process_options_t opts{};
  std::vector<char*> cargv;
  cargv.push_back(const_cast<char*>(sup->exe.c_str()));
  cargv.push_back(const_cast<char*>(sup->script.c_str()));
  cargv.push_back(nullptr);  // uv_spawn requires a NULL-terminated args array
  opts.file = sup->exe.c_str();
  opts.args = cargv.data();
  opts.exit_cb = OnChildExit;

  // Inherit stdin/stdout/stderr so the child's console output is visible
  // in the supervisor's terminal (otherwise uv_spawn discards it).
  static uv_stdio_container_t stdio[3];
  stdio[0].flags = UV_INHERIT_FD; stdio[0].data.fd = 0;
  stdio[1].flags = UV_INHERIT_FD; stdio[1].data.fd = 1;
  stdio[2].flags = UV_INHERIT_FD; stdio[2].data.fd = 2;
  opts.stdio_count = 3;
  opts.stdio = stdio;
  // opts.env = nullptr (default) → child inherits our environment,
  // including NX8V_PORT set via SetEnvironmentVariableA above.

  int r = uv_spawn(&sup->loop, &sup->child, &opts);
  if (r != 0) {
    sup->PrintTag("error", std::string("spawn failed: ") + uv_strerror(r));
    sup->shutting_down = true;
    return;
  }
  sup->child.data = sup;
  sup->child_running = true;
  sup->PrintTag("dev", "server started (pid " +
                std::to_string((long)sup->child.pid) + ")");
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// File watching
// ---------------------------------------------------------------------------
static void OnWatchClose(uv_handle_t* h) {
  delete reinterpret_cast<uv_fs_event_t*>(h);
}

static void OnFileEvent(uv_fs_event_t* handle, const char* filename,
                        int events, int status) {
  if (status != 0 || !filename) return;
  auto* sup = static_cast<DevSupervisor*>(handle->data);

  // Resolve the full path of the changed entry.
  char dirname[1024];
  size_t dsize = sizeof(dirname);
  if (uv_fs_event_getpath(handle, dirname, &dsize) != 0) return;
  std::string full = std::string(dirname, dsize) + "\\" + filename;

  if (!ShouldRestart(full)) return;
  if (sup->shutting_down) return;

  // Don't restart if there is no child (already exiting).
  if (!sup->child_running) {
    sup->restart_pending = true;
    return;
  }

  if (IsCodeFile(full)) sup->code_change = true;
  sup->PrintTag("watch", "change detected: " + full);

  // Debounce: an editor's burst of writes becomes one reload.
  uv_timer_stop(&sup->debounce);
  uv_timer_start(&sup->debounce, [](uv_timer_t* t) {
    auto* s = static_cast<DevSupervisor*>(t->data);
    if (s->shutting_down) return;
    if (s->code_change && s->child_running) {
      s->code_change = false;
      s->restart_pending = true;
      s->PrintTag("dev", "stopping server for restart...");
      uv_process_kill(&s->child, SIGTERM);
      return;
    }
    s->code_change = false;
    // HTML/CSS: the page is read from disk on each request, so the browser
    // just needs to reload. The server process stays up.
    BumpDevReload(s);
    s->PrintTag("dev", "browser reload");
  }, sup->debounce_ms, 0);
}

static void WatchDir(DevSupervisor* sup, const fs::path& p) {
  auto* w = new uv_fs_event_t;
  w->data = sup;
  if (uv_fs_event_init(&sup->loop, w) != 0) {
    delete w;
    return;
  }
  int r = uv_fs_event_start(w, OnFileEvent, p.string().c_str(), 0);
  if (r == 0) {
    sup->watchers.push_back(w);
    return;
  }
  uv_close(reinterpret_cast<uv_handle_t*>(w), OnWatchClose);
}

static void WatchTree(DevSupervisor* sup, const fs::path& dir) {
  WatchDir(sup, dir);
  std::error_code ec;
  fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;
  while (it != end) {
    if (ec) { it.increment(ec); continue; }
    const auto& p = it->path();
    if (it->is_directory(ec) && !IsSkippedDir(p)) WatchDir(sup, p);
    it.increment(ec);
  }
}

// ---------------------------------------------------------------------------
// Entry point: nx8v dev
// ---------------------------------------------------------------------------
inline int RunDev(int argc, char* argv[]) {
  DevSupervisor sup;
  sup.exe = []() {
#ifdef _WIN32
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::string(buf);
#else
    return std::string("/proc/self/exe");
#endif
  }();

  std::error_code ec;
  fs::path cwd = fs::current_path(ec);
  sup.dir = cwd.string();

  // Parse args: [dev] <script?> --port=N?
  bool script_from_cli = false;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--port=", 0) == 0) {
      long v = std::strtol(a.c_str() + 7, nullptr, 10);
      if (v > 0 && v <= 65535) sup.port = (int)v;
    } else if (a.rfind("--", 0) != 0) {
      sup.script = a;
      script_from_cli = true;
    }
  }

  // Default from project.json
  if (!script_from_cli) {
    sup.script = "server.js";
    fs::path manifest_path = cwd / "project.json";
    if (fs::exists(manifest_path, ec)) {
      std::string manifest = ReadManifestFile(manifest_path);
      std::string main_file;
      int mport = 0;
      if (JsonFindString(manifest, "main", main_file) && !main_file.empty())
        sup.script = main_file;
      JsonFindNumber(manifest, "port", mport);
      if (sup.port == 0) sup.port = mport;
    }
  }

  fs::path script_abs = cwd / sup.script;
  if (!fs::exists(script_abs, ec)) {
    std::printf("Error: '%s' not found in %s\n",
                sup.script.c_str(), sup.dir.c_str());
    return 1;
  }
  sup.script = script_abs.string();

  // ---- Setup loop & handles ----
  uv_loop_init(&sup.loop);

  sup.debounce.data = &sup;
  uv_timer_init(&sup.loop, &sup.debounce);
  sup.restart_timer.data = &sup;
  uv_timer_init(&sup.loop, &sup.restart_timer);

#ifdef _WIN32
  {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    sup.reload_file = std::string(tmp) + "nx8v-dev-reload-" +
                      std::to_string(GetCurrentProcessId());
  }
#else
  sup.reload_file = "/tmp/nx8v-dev-reload-" + std::to_string(getpid());
#endif
#ifdef _WIN32
  SetEnvironmentVariableA("NX8V_DEV", "1");
  SetEnvironmentVariableA("NX8V_DEV_RELOAD", sup.reload_file.c_str());
#else
  setenv("NX8V_DEV", "1", 1);
  setenv("NX8V_DEV_RELOAD", sup.reload_file.c_str(), 1);
#endif

  // Port via environment (server.js template reads NX8V_PORT)
  if (sup.port > 0) {
    std::string ps = std::to_string(sup.port);
#ifdef _WIN32
    SetEnvironmentVariableA("NX8V_PORT", ps.c_str());
#else
    setenv("NX8V_PORT", ps.c_str(), 1);
#endif
  }

  // ---- Watch the tree ----
  WatchTree(&sup, cwd);

  // ---- Start ----
  sup.PrintTag("dev", "watching " + sup.dir);
  sup.PrintTag("dev", "entry   " + sup.script);
  if (sup.port > 0)
    sup.PrintTag("dev", "port    " + std::to_string(sup.port));
  sup.PrintTag("dev", "Ctrl+C to quit");

  StartChild(&sup);
  if (sup.shutting_down) {
    // Spawn failed — no handles active, bail out.
    uv_loop_close(&sup.loop);
    return 1;
  }

  uv_run(&sup.loop, UV_RUN_DEFAULT);

  // ---- Teardown ----
  if (sup.child_running) uv_process_kill(&sup.child, SIGTERM);
  // Drain loop so SIGTERM delivery + exit_cb can run.
  uv_run(&sup.loop, UV_RUN_ONCE);

  for (auto* w : sup.watchers)
    uv_close(reinterpret_cast<uv_handle_t*>(w), OnWatchClose);
  uv_close(reinterpret_cast<uv_handle_t*>(&sup.debounce), nullptr);
  uv_close(reinterpret_cast<uv_handle_t*>(&sup.restart_timer), nullptr);
  uv_run(&sup.loop, UV_RUN_ONCE);
  uv_loop_close(&sup.loop);

  return 0;
}

}} // nx8v::bindings
