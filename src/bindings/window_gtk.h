#pragma once
// Linux desktop window: GTK 3 and WebKitGTK.
// The page sees the same window.nx8v commands as the Windows window.

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>

#include <uv.h>

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace nx8v {
namespace bindings {
namespace fs = std::filesystem;

struct Nx8vGtkWin {
  GtkWidget* window = nullptr;
  WebKitWebView* view = nullptr;
  std::string id;
  bool primary = false;
  bool devtools = false;
  bool alive = true;
};

static uv_loop_t* g_gtk_loop = nullptr;
static Nx8vGtkWin* g_gtk_primary = nullptr;
static std::vector<Nx8vGtkWin*> g_gtk_extra;
static int g_gtk_lock = -1;
static int g_gtk_next_id = 1;
static bool g_gtk_relaunching = false;

static std::string Nx8vGtkJsonStr(const std::string& j, const char* key) {
  std::string pat = std::string("\"") + key + "\":\"";
  auto p = j.find(pat);
  if (p == std::string::npos) return "";
  p += pat.size();
  std::string out;
  for (; p < j.size() && j[p] != '"'; ++p) {
    if (j[p] == '\\' && p + 1 < j.size()) {
      char n = j[++p];
      if (n == 'n') out.push_back('\n');
      else if (n == 'r') out.push_back('\r');
      else if (n == 't') out.push_back('\t');
      else out.push_back(n);
    } else {
      out.push_back(j[p]);
    }
  }
  return out;
}

static int Nx8vGtkJsonNum(const std::string& j, const char* key) {
  std::string pat = std::string("\"") + key + "\":";
  auto p = j.find(pat);
  if (p == std::string::npos) return 0;
  return std::atoi(j.c_str() + p + pat.size());
}

static bool Nx8vGtkJsonHas(const std::string& j, const char* key) {
  return j.find(std::string("\"") + key + "\":") != std::string::npos;
}

static std::string Nx8vGtkQuote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back((char)c);
        }
    }
  }
  out += "\"";
  return out;
}

static void Nx8vGtkReply(Nx8vGtkWin* st, int id, const std::string& value_json) {
  if (!st || !st->view || id <= 0) return;
  std::string js = "if(window.__nx8vReply)window.__nx8vReply(" +
                   std::to_string(id) + "," + value_json + ");";
  webkit_web_view_evaluate_javascript(st->view, js.c_str(), -1, nullptr, nullptr,
                                     nullptr, nullptr, nullptr);
}

static void Nx8vGtkReloadAll() {
  auto reload = [](Nx8vGtkWin* w) {
    if (w && w->view) webkit_web_view_reload(w->view);
  };
  reload(g_gtk_primary);
  for (Nx8vGtkWin* w : g_gtk_extra) reload(w);
  std::printf("[window] reloading the page\n");
  std::fflush(stdout);
}

static void Nx8vGtkRelaunch() {
  if (g_gtk_relaunching) return;
  g_gtk_relaunching = true;
  std::printf("[window] server changed, reopening the window\n");
  std::fflush(stdout);
  setenv("NX8V_WINDOW_RELAUNCH", "1", 1);
  if (g_gtk_lock >= 0) {
    close(g_gtk_lock);
    g_gtk_lock = -1;
  }
  if (!nx8v::g_runtime || nx8v::g_runtime->argc < 1 || !nx8v::g_runtime->argv) {
    std::fprintf(stderr, "[window] failed to reopen the window\n");
    g_gtk_relaunching = false;
    return;
  }
  char exe[4096];
  ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0) {
    std::fprintf(stderr, "[window] failed to reopen the window\n");
    g_gtk_relaunching = false;
    return;
  }
  exe[n] = 0;
  execv(exe, nx8v::g_runtime->argv);
  std::fprintf(stderr, "[window] failed to reopen the window\n");
  g_gtk_relaunching = false;
}

static std::string Nx8vGtkPick(GtkWindow* parent, int kind, const std::string& title,
                               const std::string& name, const std::string& filters) {
  GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_OPEN;
  const char* ok = "_Open";
  if (kind == 1) {
    action = GTK_FILE_CHOOSER_ACTION_SAVE;
    ok = "_Save";
  } else if (kind == 2) {
    action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
  }
  GtkWidget* dlg = gtk_file_chooser_dialog_new(
      title.empty() ? "nx8v" : title.c_str(), parent, action,
      "_Cancel", GTK_RESPONSE_CANCEL, ok, GTK_RESPONSE_ACCEPT, nullptr);
  auto* chooser = GTK_FILE_CHOOSER(dlg);
  if (kind == 1) gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);
  if (!name.empty() && kind == 1) gtk_file_chooser_set_current_name(chooser, name.c_str());
  size_t pos = 0;
  while (pos < filters.size()) {
    auto cut = filters.find("||", pos);
    std::string part = filters.substr(pos, cut == std::string::npos ? std::string::npos : cut - pos);
    auto bar = part.find('|');
    std::string label = bar == std::string::npos ? part : part.substr(0, bar);
    std::string exts = bar == std::string::npos ? "" : part.substr(bar + 1);
    if (!label.empty()) {
      GtkFileFilter* filt = gtk_file_filter_new();
      gtk_file_filter_set_name(filt, label.c_str());
      size_t e = 0;
      bool any = false;
      while (e < exts.size()) {
        auto semi = exts.find(';', e);
        std::string ext = exts.substr(e, semi == std::string::npos ? std::string::npos : semi - e);
        if (ext.size() > 2 && ext[0] == '*' && ext[1] == '.') ext = ext.substr(2);
        if (!ext.empty()) {
          gtk_file_filter_add_pattern(filt, ("*." + ext).c_str());
          any = true;
        }
        if (semi == std::string::npos) break;
        e = semi + 1;
      }
      if (!any) gtk_file_filter_add_pattern(filt, "*");
      gtk_file_chooser_add_filter(chooser, filt);
    }
    if (cut == std::string::npos) break;
    pos = cut + 2;
  }
  std::string result;
  if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
    char* file = gtk_file_chooser_get_filename(chooser);
    if (file) {
      result = file;
      g_free(file);
    }
  }
  gtk_widget_destroy(dlg);
  return result;
}

static void Nx8vGtkHandle(Nx8vGtkWin* st, const std::string& json);

static void Nx8vGtkOnMessage(WebKitUserContentManager*, WebKitJavascriptResult* js,
                             gpointer user) {
  auto* st = static_cast<Nx8vGtkWin*>(user);
  JSCValue* value = webkit_javascript_result_get_js_value(js);
  if (!value || !jsc_value_is_string(value)) return;
  char* text = jsc_value_to_string(value);
  if (!text) return;
  Nx8vGtkHandle(st, text);
  g_free(text);
}

static const char kNx8vGtkBridge[] = R"NX8V(
(function () {
  if (window.nx8v) return;
  const send = (msg) => window.webkit.messageHandlers.nx8v.postMessage(JSON.stringify(msg));
  let nextId = 1;
  const pending = new Map();
  window.__nx8vReply = (id, value) => {
    const cb = pending.get(id);
    if (cb) { pending.delete(id); cb(value); }
  };
  const call = (msg) => new Promise((resolve) => {
    const id = nextId++;
    pending.set(id, resolve);
    send(Object.assign({ id: id }, msg));
  });
  function encodeItems(items, depth) {
    const out = [];
    (items || []).forEach((it) => {
      if (it === '-' || it == null) { out.push('-'); return; }
      const label = String(it.label || '').replace(/[|>]/g, ' ');
      const en = it.enabled === false ? 0 : 1;
      const ck = it.checked ? 1 : 0;
      out.push('>'.repeat(depth) + label + '|' + Number(it.id || 0) + '|' + en + '|' + ck);
      if (it.children) out.push.apply(out, encodeItems(it.children, depth + 1));
    });
    return out;
  }
  function flatten(cmd, o) {
    o = o || {};
    const parts = (o.filters || []).map((f) => (f.name || 'Files') + '|' + (f.extensions || []).join(';'));
    return { cmd: cmd, title: o.title || '', defaultName: o.defaultName || '', filters: parts.join('||') };
  }
  window.nx8v = {
    setTitle: (t) => send({ cmd: 'setTitle', value: String(t) }),
    setSize: (w, h) => send({ cmd: 'setSize', w: Number(w), h: Number(h) }),
    minimize: () => send({ cmd: 'minimize' }),
    maximize: () => send({ cmd: 'maximize' }),
    close: () => send({ cmd: 'close' }),
    openDevTools: () => send({ cmd: 'devtools' }),
    dialog: {
      openFile: (o) => call(flatten('dialogOpen', o)),
      saveFile: (o) => call(flatten('dialogSave', o)),
      openFolder: () => call({ cmd: 'dialogFolder' }),
      alert: (message, title) => call({
        cmd: 'dialogAlert',
        text: String(message == null ? '' : message),
        title: String(title == null ? 'nx8v' : title)
      }),
      confirm: (message, title) => call({
        cmd: 'dialogConfirm',
        text: String(message == null ? '' : message),
        title: String(title == null ? 'nx8v' : title)
      })
    },
    tray: {
      show: (o) => call({ cmd: 'trayShow' }),
      hide: () => send({ cmd: 'trayHide' }),
      setTooltip: (t) => send({ cmd: 'trayTooltip', value: String(t) })
    },
    contextMenu: {
      set: (items) => send({ cmd: 'contextMenu', menu: encodeItems(items, 0).join('||') }),
      clear: () => send({ cmd: 'contextMenu', menu: '' })
    },
    notify: (title, body) => send({
      cmd: 'notify',
      title: String(title == null ? '' : title),
      body: String(body == null ? '' : body)
    }),
    onMenu: function () {},
    window: {
      create: (o) => {
        let url = (o && o.url) ? String(o.url) : location.href;
        try { url = new URL(url, location.href).href; } catch (err) {}
        const msg = {
          cmd: 'windowCreate',
          url: url,
          title: String((o && o.title) || 'nx8v'),
          w: Number((o && o.width) || 900),
          h: Number((o && o.height) || 640)
        };
        if (o && o.x != null && o.y != null) { msg.x = Number(o.x); msg.y = Number(o.y); }
        else if (!o || o.center !== false) msg.center = 1;
        if (o && o.id != null && String(o.id) !== '') msg.win = String(o.id);
        return call(msg);
      },
      close: (id) => send({ cmd: 'windowClose', win: String(id == null ? '' : id) })
    }
  };
})();
)NX8V";

static Nx8vGtkWin* Nx8vGtkMakeView(const std::string& url, const std::string& title,
                                  int width, int height, int x, int y, bool has_pos,
                                  bool center, bool devtools, const std::string& id,
                                  bool primary) {
  auto* st = new Nx8vGtkWin();
  st->id = id;
  st->primary = primary;
  st->devtools = devtools;
  st->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(st->window), title.c_str());
  gtk_window_set_default_size(GTK_WINDOW(st->window), width > 0 ? width : 1100,
                              height > 0 ? height : 760);
  if (has_pos) gtk_window_move(GTK_WINDOW(st->window), x, y);
  else if (center) gtk_window_set_position(GTK_WINDOW(st->window), GTK_WIN_POS_CENTER);

  WebKitUserContentManager* ucm = webkit_user_content_manager_new();
  webkit_user_content_manager_register_script_message_handler(ucm, "nx8v");
  g_signal_connect(ucm, "script-message-received::nx8v", G_CALLBACK(Nx8vGtkOnMessage), st);
  WebKitUserScript* script = webkit_user_script_new(
      kNx8vGtkBridge, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
  webkit_user_content_manager_add_script(ucm, script);
  webkit_user_script_unref(script);

  st->view = WEBKIT_WEB_VIEW(webkit_web_view_new_with_user_content_manager(ucm));
  WebKitSettings* settings = webkit_web_view_get_settings(st->view);
  webkit_settings_set_enable_javascript(settings, TRUE);
  webkit_settings_set_enable_developer_extras(settings, devtools ? TRUE : FALSE);
  gtk_container_add(GTK_CONTAINER(st->window), GTK_WIDGET(st->view));
  g_signal_connect(st->window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer p) {
    auto* w = static_cast<Nx8vGtkWin*>(p);
    w->alive = false;
    w->view = nullptr;
    w->window = nullptr;
    if (w->primary && g_gtk_loop) uv_stop(g_gtk_loop);
  }), st);
  gtk_widget_show_all(st->window);
  webkit_web_view_load_uri(st->view, url.c_str());
  return st;
}

static void Nx8vGtkCloseId(const std::string& id) {
  for (Nx8vGtkWin* w : g_gtk_extra) {
    if (w && w->id == id && w->window) {
      gtk_widget_destroy(w->window);
      return;
    }
  }
}

static void Nx8vGtkHandle(Nx8vGtkWin* st, const std::string& json) {
  if (!st || !st->window) return;
  std::string cmd = Nx8vGtkJsonStr(json, "cmd");
  int id = Nx8vGtkJsonNum(json, "id");
  if (cmd == "setTitle") {
    gtk_window_set_title(GTK_WINDOW(st->window), Nx8vGtkJsonStr(json, "value").c_str());
  } else if (cmd == "setSize") {
    int w = Nx8vGtkJsonNum(json, "w");
    int h = Nx8vGtkJsonNum(json, "h");
    if (w > 0 && h > 0) gtk_window_resize(GTK_WINDOW(st->window), w, h);
  } else if (cmd == "minimize") {
    gtk_window_iconify(GTK_WINDOW(st->window));
  } else if (cmd == "maximize") {
    if (gtk_window_is_maximized(GTK_WINDOW(st->window)))
      gtk_window_unmaximize(GTK_WINDOW(st->window));
    else
      gtk_window_maximize(GTK_WINDOW(st->window));
  } else if (cmd == "close") {
    gtk_widget_destroy(st->window);
  } else if (cmd == "devtools" && st->devtools && st->view) {
    webkit_web_inspector_show(webkit_web_view_get_inspector(st->view));
  } else if (cmd == "dialogAlert" || cmd == "dialogConfirm") {
    GtkMessageType kind = cmd == "dialogConfirm" ? GTK_MESSAGE_QUESTION : GTK_MESSAGE_INFO;
    GtkButtonsType buttons = cmd == "dialogConfirm" ? GTK_BUTTONS_YES_NO : GTK_BUTTONS_OK;
    std::string text = Nx8vGtkJsonStr(json, "text");
    std::string title = Nx8vGtkJsonStr(json, "title");
    GtkWidget* dlg = gtk_message_dialog_new(
        GTK_WINDOW(st->window), GTK_DIALOG_MODAL, kind, buttons, "%s", text.c_str());
    gtk_window_set_title(GTK_WINDOW(dlg), title.empty() ? "nx8v" : title.c_str());
    int ans = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    bool yes = cmd != "dialogConfirm" || ans == GTK_RESPONSE_YES;
    Nx8vGtkReply(st, id, yes ? "true" : "false");
  } else if (cmd == "dialogOpen" || cmd == "dialogSave" || cmd == "dialogFolder") {
    int kind = cmd == "dialogSave" ? 1 : (cmd == "dialogFolder" ? 2 : 0);
    std::string path = Nx8vGtkPick(GTK_WINDOW(st->window), kind,
                                  Nx8vGtkJsonStr(json, "title"),
                                  Nx8vGtkJsonStr(json, "defaultName"),
                                  Nx8vGtkJsonStr(json, "filters"));
    Nx8vGtkReply(st, id, path.empty() ? "null" : Nx8vGtkQuote(path));
  } else if (cmd == "windowCreate") {
    int w = Nx8vGtkJsonNum(json, "w");
    int h = Nx8vGtkJsonNum(json, "h");
    std::string wid = Nx8vGtkJsonStr(json, "win");
    if (wid.empty()) wid = std::to_string(g_gtk_next_id++);
    bool has_pos = Nx8vGtkJsonHas(json, "x") && Nx8vGtkJsonHas(json, "y");
    Nx8vGtkWin* child = Nx8vGtkMakeView(
        Nx8vGtkJsonStr(json, "url"), Nx8vGtkJsonStr(json, "title"), w, h,
        Nx8vGtkJsonNum(json, "x"), Nx8vGtkJsonNum(json, "y"), has_pos,
        Nx8vGtkJsonNum(json, "center") != 0, st->devtools, wid, false);
    g_gtk_extra.push_back(child);
    Nx8vGtkReply(st, id, Nx8vGtkQuote(wid));
  } else if (cmd == "windowClose") {
    Nx8vGtkCloseId(Nx8vGtkJsonStr(json, "win"));
  } else if (cmd == "notify") {
    std::string title = Nx8vGtkJsonStr(json, "title");
    std::string body = Nx8vGtkJsonStr(json, "body");
    const char* argv[] = {"notify-send", title.c_str(), body.c_str(), nullptr};
    g_spawn_async(nullptr, (char**)argv, nullptr, G_SPAWN_SEARCH_PATH,
                  nullptr, nullptr, nullptr, nullptr);
  } else if (cmd == "trayShow") {
    Nx8vGtkReply(st, id, "false");
  }
}

struct Nx8vGtkWatch {
  std::vector<uv_fs_event_t*> watchers;
  uv_timer_t debounce{};
  bool code = false;
};
static Nx8vGtkWatch g_gtk_watch;
static std::map<std::string, std::pair<std::uintmax_t, fs::file_time_type>> g_gtk_stamps;
static uint64_t g_gtk_ignore_until = 0;

static bool Nx8vGtkSkipDir(const fs::path& p) {
  std::string n = p.filename().string();
  return n == "node_modules" || n == ".git" || n == "build" ||
         n == "dist" || n == ".nx8v" || n == "deps";
}

static std::string Nx8vGtkExt(const fs::path& p) {
  std::string ext = p.extension().string();
  for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
  return ext;
}

static bool Nx8vGtkInteresting(const std::string& ext) {
  return ext == ".js" || ext == ".mjs" || ext == ".json" ||
         ext == ".html" || ext == ".css" || ext == ".svg";
}

static bool Nx8vGtkChanged(const fs::path& file) {
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) return false;
  auto size = fs::file_size(file, ec);
  if (ec) return false;
  auto mtime = fs::last_write_time(file, ec);
  if (ec) return false;
  std::string key = fs::absolute(file, ec).lexically_normal().generic_string();
  auto it = g_gtk_stamps.find(key);
  if (it != g_gtk_stamps.end() && it->second.first == size && it->second.second == mtime)
    return false;
  g_gtk_stamps[key] = {size, mtime};
  return true;
}

static void Nx8vGtkRemember(const fs::path& root) {
  std::error_code ec;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;
  while (it != end) {
    if (ec) { it.increment(ec); continue; }
    if (it->is_directory(ec)) {
      if (Nx8vGtkSkipDir(it->path())) it.disable_recursion_pending();
    } else if (Nx8vGtkInteresting(Nx8vGtkExt(it->path()))) {
      Nx8vGtkChanged(it->path());
    }
    it.increment(ec);
  }
}

static void Nx8vGtkOnFile(uv_fs_event_t* handle, const char* filename, int, int status) {
  if (status != 0 || !filename || g_gtk_relaunching) return;
  if (uv_now(handle->loop) < g_gtk_ignore_until) return;
  char dirname[1024];
  size_t dsize = sizeof(dirname);
  if (uv_fs_event_getpath(handle, dirname, &dsize) != 0) return;
  fs::path full = fs::path(std::string(dirname, dsize)) / filename;
  std::string ext = Nx8vGtkExt(full);
  bool code = ext == ".js" || ext == ".mjs" || ext == ".json";
  bool asset = ext == ".html" || ext == ".css" || ext == ".svg";
  if (!code && !asset) return;
  if (!Nx8vGtkChanged(full)) return;
  if (code) g_gtk_watch.code = true;
  uv_timer_stop(&g_gtk_watch.debounce);
  uv_timer_start(&g_gtk_watch.debounce, [](uv_timer_t* t) {
    if (g_gtk_relaunching) return;
    g_gtk_ignore_until = uv_now(t->loop) + 500;
    if (g_gtk_watch.code) {
      g_gtk_watch.code = false;
      Nx8vGtkRelaunch();
      return;
    }
    Nx8vGtkReloadAll();
  }, 120, 0);
}

static void Nx8vGtkWatchDir(uv_loop_t* loop, const fs::path& dir) {
  auto* w = new uv_fs_event_t;
  if (uv_fs_event_init(loop, w) != 0) { delete w; return; }
  if (uv_fs_event_start(w, Nx8vGtkOnFile, dir.string().c_str(), 0) != 0) {
    uv_close(reinterpret_cast<uv_handle_t*>(w),
             [](uv_handle_t* h) { delete reinterpret_cast<uv_fs_event_t*>(h); });
    return;
  }
  g_gtk_watch.watchers.push_back(w);
}

static void Nx8vGtkWatchProject(uv_loop_t* loop) {
  fs::path root = ".";
  if (nx8v::g_runtime && !nx8v::g_runtime->script_dir.empty())
    root = nx8v::g_runtime->script_dir;
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return;
  Nx8vGtkRemember(root);
  uv_timer_init(loop, &g_gtk_watch.debounce);
  Nx8vGtkWatchDir(loop, root);
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;
  while (it != end) {
    if (ec) { it.increment(ec); continue; }
    if (it->is_directory(ec) && !Nx8vGtkSkipDir(it->path())) Nx8vGtkWatchDir(loop, it->path());
    it.increment(ec);
  }
}

static void Nx8vGtkPump(uv_timer_t*) {
  while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
}

static bool Nx8vGtkLock(const std::string& dir) {
  unsigned hash = 2166136261u;
  for (unsigned char c : dir) hash = (hash ^ c) * 16777619u;
  std::string path = "/tmp/nx8v-window-" + std::to_string(hash);
  g_gtk_lock = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (g_gtk_lock < 0) return true;
  bool wait_prev = std::getenv("NX8V_WINDOW_RELAUNCH") != nullptr;
  for (int i = 0; i < (wait_prev ? 300 : 1); ++i) {
    if (flock(g_gtk_lock, LOCK_EX | LOCK_NB) == 0) return true;
    if (!wait_prev) break;
    usleep(50 * 1000);
  }
  close(g_gtk_lock);
  g_gtk_lock = -1;
  return false;
}

inline int Nx8vGtkRunWindow(const std::string& url, const std::string& title,
                     int width, int height, uv_loop_t* loop,
                     const Nx8vWindowOpts& opts = {}) {
  std::string project = ".";
  if (nx8v::g_runtime && !nx8v::g_runtime->script_dir.empty())
    project = nx8v::g_runtime->script_dir;
  if (opts.single_instance && !Nx8vGtkLock(project)) {
    std::puts("nx8v window: the window is already open");
    return 0;
  }

  gtk_init(nullptr, nullptr);
  g_gtk_loop = loop;

  std::string shown = title;
  std::string icon_path = opts.icon;
  fs::path icon_root = project;
  {
    std::error_code ec;
    fs::path manifest = icon_root / "project.json";
    if (fs::exists(manifest, ec)) {
      std::string json = ReadManifestFile(manifest);
      std::string icon_rel, product;
      if (icon_path.empty() && JsonFindString(json, "icon", icon_rel) && !icon_rel.empty())
        icon_path = (icon_root / icon_rel).lexically_normal().string();
      if (shown == "nx8v" && JsonFindString(json, "productName", product) && !product.empty())
        shown = product;
    }
  }

  g_gtk_primary = Nx8vGtkMakeView(url, shown, width, height, opts.x, opts.y,
                                 opts.has_pos, opts.center, opts.devtools, "main", true);
  if (opts.min_w > 0 || opts.min_h > 0)
    gtk_widget_set_size_request(g_gtk_primary->window,
                                opts.min_w > 0 ? opts.min_w : 1,
                                opts.min_h > 0 ? opts.min_h : 1);
  gtk_window_set_resizable(GTK_WINDOW(g_gtk_primary->window), opts.resizable ? TRUE : FALSE);
  if (opts.frameless) gtk_window_set_decorated(GTK_WINDOW(g_gtk_primary->window), FALSE);
  if (!icon_path.empty()) gtk_window_set_icon_from_file(GTK_WINDOW(g_gtk_primary->window),
                                                       icon_path.c_str(), nullptr);
  if (opts.show == 1) gtk_window_maximize(GTK_WINDOW(g_gtk_primary->window));
  else if (opts.show == 2) gtk_window_iconify(GTK_WINDOW(g_gtk_primary->window));

  std::printf("nx8v window: opening %s\n", url.c_str());
  std::fflush(stdout);

  uv_timer_t pump;
  uv_timer_init(loop, &pump);
  uv_timer_start(&pump, Nx8vGtkPump, 10, 10);
  uv_prepare_t prep;
  uv_prepare_init(loop, &prep);
  uv_prepare_start(&prep, [](uv_prepare_t*) { Nx8vGtkPump(nullptr); });
  Nx8vGtkWatchProject(loop);

  uv_run(loop, UV_RUN_DEFAULT);

  uv_timer_stop(&pump);
  uv_prepare_stop(&prep);
  ReleaseLoopHandles(loop);
  if (g_gtk_lock >= 0) close(g_gtk_lock);
  g_gtk_lock = -1;
  g_gtk_primary = nullptr;
  g_gtk_loop = nullptr;
  return 0;
}

}}  // nx8v::bindings
