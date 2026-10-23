#pragma once
// nx8v/src/bindings/window.h
// Desktop window with an embedded WebView2 browser (Windows).
//
//   nx8v window [app.js]        # jalankan app, lalu buka jendela
//
// The app script runs normally (usually an HTTP server); the window then
// navigates to http://127.0.0.1:<port>/ and stays open until closed.
//
// JS bridge (available inside the page as window.nx8v):
//   nx8v.setTitle('judul')
//   nx8v.setSize(1200, 800)
//   nx8v.minimize() / maximize() / close()
//   nx8v.openDevTools()
//
// Windows-only.

#include <uv.h>
#include "loop.h"
#include "../runtime.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "manifest.h"

namespace nx8v {
namespace bindings {
struct Nx8vWindowOpts {
  int x = 0;
  int y = 0;
  int min_w = 0;
  int min_h = 0;
  bool has_pos = false;
  bool center = false;
  bool frameless = false;
  bool resizable = true;
  bool single_instance = true;
  // 0 = ukuran width/height, 1 = maximized, 2 = minimized.
  int show = 0;
  bool maximizable = true;
  bool devtools = false;
  std::string icon;
};

inline int Nx8vWindowShowFromName(const std::string& name) {
  if (name == "maximized" || name == "maximize") return 1;
  if (name == "minimized" || name == "minimize") return 2;
  return 0;
}

// project.json desktop window fields. CLI values already chosen are left alone.
inline void Nx8vApplyDesktopWindow(const std::string& json, int& width, int& height,
                                  Nx8vWindowOpts& opts, bool keep_w, bool keep_h,
                                  bool keep_show, bool keep_max, bool keep_min_w,
                                  bool keep_min_h, bool keep_pos, bool keep_center,
                                  bool keep_frame, bool keep_resize, bool keep_single) {
  if (!keep_w) {
    int n = 0;
    if (JsonFindNumber(json, "width", n) && n > 0) width = n;
  }
  if (!keep_h) {
    int n = 0;
    if (JsonFindNumber(json, "height", n) && n > 0) height = n;
  }
  if (!keep_min_w) {
    int n = 0;
    if (JsonFindNumber(json, "minWidth", n) && n > 0) opts.min_w = n;
  }
  if (!keep_min_h) {
    int n = 0;
    if (JsonFindNumber(json, "minHeight", n) && n > 0) opts.min_h = n;
  }
  if (!keep_pos) {
    int n = 0;
    bool any = false;
    if (JsonFindNumber(json, "x", n)) { opts.x = n; any = true; }
    if (JsonFindNumber(json, "y", n)) { opts.y = n; any = true; }
    if (any) opts.has_pos = true;
  }
  if (!keep_show) {
    std::string state;
    if (JsonFindString(json, "state", state))
      opts.show = Nx8vWindowShowFromName(state);
  }
  if (!keep_max) {
    bool on = true;
    if (JsonFindBool(json, "maximizable", on)) opts.maximizable = on;
  }
  if (!keep_center && !opts.has_pos) {
    bool on = false;
    if (JsonFindBool(json, "center", on)) opts.center = on;
  }
  if (!keep_frame) {
    bool hidden = false;
    if (JsonFindBool(json, "frameless", hidden)) opts.frameless = hidden;
    bool bar = true;
    if (JsonFindBool(json, "titlebar", bar)) opts.frameless = !bar;
  }
  if (!keep_resize) {
    bool on = true;
    if (JsonFindBool(json, "resizable", on)) opts.resizable = on;
  }
  if (!keep_single) {
    bool on = true;
    if (JsonFindBool(json, "singleInstance", on)) opts.single_instance = on;
  }
  {
    bool on = false;
    if (JsonFindBool(json, "devtools", on)) opts.devtools = on;
  }
  if ((!opts.maximizable || !opts.resizable) && opts.show == 1) opts.show = 0;
}
}  // namespace bindings
}  // namespace nx8v

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#include <wrl.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#include <string>
#include <vector>

#include "WebView2.h"
#include "manifest.h"
#include "icon.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")

namespace nx8v {
namespace bindings {

namespace fs = std::filesystem;

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

struct WindowState {
  HWND                            hwnd = nullptr;
  ComPtr<ICoreWebView2Environment> env;
  ComPtr<ICoreWebView2Controller>  controller;
  ComPtr<ICoreWebView2>            webview;
  uv_prepare_t                     pump{};
  uv_timer_t                       pump_timer{};
  bool                             quit = false;
  std::string                      url;
  std::wstring                     user_data_dir;

  // Tray icon + application menu
  NOTIFYICONDATAW                  nid{};
  bool                             tray_active = false;
  HMENU                            app_menu = nullptr;
  bool                             allow_close = false;   // set by "quit"
  int                              min_w = 0;
  int                              min_h = 0;
  bool                             frameless = false;
  bool                             maximizable = true;
  bool                             resizable = true;
  bool                             devtools = false;
  std::wstring                     context_menu;
  POINT                            context_pt{};
  // Custom title bar, in client pixels. Empty drag_rects keeps the top strip.
  std::vector<RECT>                drag_rects;
  std::vector<RECT>                nodrag_rects;
  // Hidden until the page has painted, so the empty Windows frame is not seen.
  int                              show_cmd = SW_SHOWNORMAL;
  bool                             revealed = false;
  std::wstring                     win_id;
};

static WindowState* g_window = nullptr;
static std::vector<WindowState*> g_extra;
static uv_loop_t* g_win_loop = nullptr;

// Custom messages
#define NX8V_WM_TRAY     (WM_APP + 1)
#define NX8V_WM_MENU     (WM_APP + 2)
#define NX8V_WM_CONTEXT  (WM_APP + 3)
#define NX8V_REVEAL_TIMER 1

struct Nx8vMenuItem {
  int depth = 0;
  std::wstring label;
  int id = 0;
  bool enabled = true;
  bool checked = false;
  bool sep = false;
};
static std::vector<Nx8vMenuItem> Nx8vParseMenu(const std::wstring& spec);
static HMENU Nx8vMakeMenu(const std::vector<Nx8vMenuItem>& items);
static void Nx8vInstallContextMenu(WindowState* st);

static void Nx8vReadDragPart(std::vector<RECT>& out, const std::wstring& part) {
  size_t i = 0;
  while (i < part.size()) {
    int v[4] = {};
    bool ok = true;
    for (int k = 0; k < 4; ++k) {
      if (i >= part.size()) { ok = false; break; }
      v[k] = _wtoi(part.c_str() + i);
      while (i < part.size() && part[i] != L',' && part[i] != L';') ++i;
      if (k < 3) {
        if (i >= part.size() || part[i] != L',') { ok = false; break; }
        ++i;
      }
    }
    if (ok && v[2] > v[0] && v[3] > v[1])
      out.push_back(RECT{v[0], v[1], v[2], v[3]});
    while (i < part.size() && part[i] != L';') ++i;
    if (i < part.size() && part[i] == L';') ++i;
  }
}

static void Nx8vSetDragRegions(WindowState* st, const std::wstring& spec) {
  if (!st) return;
  st->drag_rects.clear();
  st->nodrag_rects.clear();
  size_t bar = spec.find(L'|');
  if (bar == std::wstring::npos) {
    Nx8vReadDragPart(st->drag_rects, spec);
  } else {
    Nx8vReadDragPart(st->drag_rects, spec.substr(0, bar));
    Nx8vReadDragPart(st->nodrag_rects, spec.substr(bar + 1));
  }
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
// Windows 11 draws a stroke inside a borderless window. COLOR_NONE removes it.
static void Nx8vHideSystemBorder(HWND hwnd) {
  COLORREF none = 0xFFFFFFFE;
  DwmSetWindowAttribute(hwnd, 34, &none, sizeof(none));  // DWMWA_BORDER_COLOR
  int policy = 1;  // DWMNCRP_DISABLED: do not paint a frame inside the window
  DwmSetWindowAttribute(hwnd, 2, &policy, sizeof(policy));  // DWMWA_NCRENDERING_POLICY
}

// DWMWA_CLOAK keeps the window alive so the page can paint, without showing it.
static void Nx8vCloak(HWND hwnd, bool hide) {
  BOOL cloak = hide ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd, 13, &cloak, sizeof(cloak));
}

static void Nx8vRevealWindow(WindowState* st) {
  if (!st || !st->hwnd || st->revealed) return;
  st->revealed = true;
  KillTimer(st->hwnd, NX8V_REVEAL_TIMER);
  Nx8vCloak(st->hwnd, false);
  if (st->show_cmd != SW_SHOWMINIMIZED) SetForegroundWindow(st->hwnd);
  if (st->frameless) Nx8vHideSystemBorder(st->hwnd);
}

static void Nx8vRevealOnHtml(WindowState* st) {
  if (!st || !st->webview) return;
  ComPtr<ICoreWebView2_2> web2;
  if (FAILED(st->webview.As(&web2)) || !web2) {
    Nx8vRevealWindow(st);
    return;
  }
  EventRegistrationToken tok{};
  web2->add_DOMContentLoaded(
      Callback<ICoreWebView2DOMContentLoadedEventHandler>(
          [st](ICoreWebView2* sender, ICoreWebView2DOMContentLoadedEventArgs*) -> HRESULT {
            LPWSTR uri = nullptr;
            if (sender) sender->get_Source(&uri);
            bool blank = !uri || uri[0] == 0 || wcscmp(uri, L"about:blank") == 0;
            CoTaskMemFree(uri);
            if (!blank) Nx8vRevealWindow(st);
            return S_OK;
          })
          .Get(),
      &tok);
}

static LRESULT CALLBACK Nx8vWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  auto* st = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (msg) {
    case WM_GETMINMAXINFO:
      if (st) {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        if (!st->resizable) {
          RECT rc{};
          GetWindowRect(hwnd, &rc);
          int w = rc.right - rc.left;
          int h = rc.bottom - rc.top;
          if (w > 0 && h > 0) {
            mm->ptMinTrackSize.x = w;
            mm->ptMinTrackSize.y = h;
            mm->ptMaxTrackSize.x = w;
            mm->ptMaxTrackSize.y = h;
          }
        } else {
          if (st->min_w > 0) mm->ptMinTrackSize.x = st->min_w;
          if (st->min_h > 0) mm->ptMinTrackSize.y = st->min_h;
        }
      }
      return 0;

    case WM_NCCALCSIZE:
      // The page is the whole window, so Windows cannot keep a gray frame inside it.
      if (st && st->frameless) {
        if (wp) {
          auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
          if (IsZoomed(hwnd)) {
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) params->rgrc[0] = mi.rcWork;
          }
        }
        return 0;
      }
      break;

    case WM_NCPAINT:
      if (st && st->frameless) return 0;
      break;

    case WM_ACTIVATE:
      if (st && st->frameless) Nx8vHideSystemBorder(hwnd);
      break;

    case WM_TIMER:
      if (wp == NX8V_REVEAL_TIMER && st) Nx8vRevealWindow(st);
      return 0;

    case WM_ERASEBKGND:
      if (st && st->frameless) return 1;
      break;

    case WM_NCHITTEST:
      if (st && st->frameless) {
        POINT pt{ (LONG)(short)LOWORD(lp), (LONG)(short)HIWORD(lp) };
        if (st->resizable && !IsZoomed(hwnd)) {
          RECT wr{};
          GetWindowRect(hwnd, &wr);
          const int edge = 6;
          bool left = pt.x < wr.left + edge;
          bool right = pt.x >= wr.right - edge;
          bool top = pt.y < wr.top + edge;
          bool bottom = pt.y >= wr.bottom - edge;
          if (top && left) return HTTOPLEFT;
          if (top && right) return HTTOPRIGHT;
          if (bottom && left) return HTBOTTOMLEFT;
          if (bottom && right) return HTBOTTOMRIGHT;
          if (left) return HTLEFT;
          if (right) return HTRIGHT;
          if (top) return HTTOP;
          if (bottom) return HTBOTTOM;
        }
        ScreenToClient(hwnd, &pt);
        auto inside = [&](const std::vector<RECT>& rs) {
          for (const RECT& r : rs)
            if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom)
              return true;
          return false;
        };
        if (inside(st->nodrag_rects)) return HTCLIENT;
        if (inside(st->drag_rects)) return HTCAPTION;
        if (st->drag_rects.empty()) {
          RECT rc{};
          GetClientRect(hwnd, &rc);
          if (pt.y >= 0 && pt.y < 36 && pt.x > 6 && pt.x < rc.right - 6)
            return HTCAPTION;
        }
        return HTCLIENT;
      }
      break;

    case WM_SIZE:
      if (st && st->controller) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        st->controller->put_Bounds(rc);
      }
      return 0;

    case WM_CLOSE:
      if (st && st != g_window) {
        DestroyWindow(hwnd);
        return 0;
      }
      if (st && !st->allow_close) {
        // Hide to tray instead of quitting when a tray icon is active.
        if (st->tray_active) {
          ShowWindow(hwnd, SW_HIDE);
          return 0;
        }
      }
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      if (st && st != g_window) {
        st->controller.Reset();
        st->webview.Reset();
        st->hwnd = nullptr;
        return 0;
      }
      if (st) st->quit = true;
      PostQuitMessage(0);
      return 0;

    case NX8V_WM_TRAY:
      if (lp == WM_LBUTTONUP && st) {
        // Restore the window from the tray.
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
      } else if (lp == WM_RBUTTONUP && st && st->app_menu) {
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        TrackPopupMenu(st->app_menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                       pt.x, pt.y, 0, hwnd, nullptr);
      }
      return 0;

    case NX8V_WM_CONTEXT:
      if (st && !st->context_menu.empty()) {
        HMENU menu = Nx8vMakeMenu(Nx8vParseMenu(st->context_menu));
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                 st->context_pt.x, st->context_pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (cmd && st->webview) {
          std::wstring json = L"{\"menu\":" + std::to_wstring(cmd) + L"}";
          st->webview->PostWebMessageAsJson(json.c_str());
        }
      }
      return 0;

    case WM_COMMAND: {
      int id = LOWORD(wp);
      if (st && id > 0) {
        // Notify the page: { menu: id }
        if (st->webview) {
          std::wstring json = L"{\"menu\":" + std::to_wstring(id) + L"}";
          st->webview->PostWebMessageAsJson(json.c_str());
        }
        if (id == 9001 && st) {   // built-in "Keluar"
          st->allow_close = true;
          PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
      }
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// JS bridge exposed to the page as window.nx8v
// ---------------------------------------------------------------------------
// Implemented as a small IDispatch-free COM object would be heavy; instead we
// use AddScriptToExecuteOnDocumentCreated with plain JS that posts messages,
// handled through WebMessageReceived.
static const wchar_t* kNx8vBridgeJs = LR"JS(
(function () {
  if (window.nx8v) return;
  const send = (msg) => window.chrome.webview.postMessage(msg);

  // Request/response over the message channel: each call carries an id, the
  // native side answers with { reply: id, value }.
  let nextId = 1;
  const pending = new Map();
  window.chrome.webview.addEventListener('message', (e) => {
    let data = e.data;
    if (typeof data === 'string') { try { data = JSON.parse(data); } catch (_) { return; } }
    if (!data || data.reply == null) return;
    const cb = pending.get(data.reply);
    if (cb) { pending.delete(data.reply); cb(data.value); }
  });
  const call = (msg) => new Promise((resolve) => {
    const id = nextId++;
    pending.set(id, resolve);
    send(Object.assign({ id }, msg));
  });

  window.nx8v = {
    setTitle: (t) => send({ cmd: 'setTitle', value: String(t) }),
    setSize:  (w, h) => send({ cmd: 'setSize', w: Number(w), h: Number(h) }),
    minimize: () => send({ cmd: 'minimize' }),
    maximize: () => send({ cmd: 'maximize' }),
    close:    () => send({ cmd: 'close' }),
    openDevTools: () => send({ cmd: 'devtools' }),

    dialog: {
      // openFile({ filters: [{name:'JS', extensions:['js','mjs']}], title })
      openFile: (o) => call(flatten('dialogOpen', o)),
      // saveFile({ filters, title, defaultName })
      saveFile: (o) => call(flatten('dialogSave', o)),
      // openFolder() -> Promise<string|null>
      openFolder: () => call({ cmd: 'dialogFolder' }),
      alert: (message, title) => call({
        cmd: 'dialogAlert',
        text: String(message == null ? '' : message),
        title: String(title == null ? 'nx8v' : title),
      }),
      confirm: (message, title) => call({
        cmd: 'dialogConfirm',
        text: String(message == null ? '' : message),
        title: String(title == null ? 'nx8v' : title),
      }),
    },

    tray: {
      // show({ tooltip, menu, exit, exitLabel })
      // menu item: { label, id, enabled, checked, children } or '-'
      show: (o) => call(trayMsg(o)),   // -> Promise<boolean>
      hide: () => send({ cmd: 'trayHide' }),
      setTooltip: (t) => send({ cmd: 'trayTooltip', value: String(t) }),
    },

    contextMenu: {
      set: (items) => send({ cmd: 'contextMenu', menu: encodeItems(items, 0).join('||') }),
      clear: () => send({ cmd: 'contextMenu', menu: '' }),
    },

    // notify('Judul', 'Isi pesan')
    notify: (title, body) => send({
      cmd: 'notify',
      title: String(title == null ? '' : title),
      body: String(body == null ? '' : body),
    }),

    // onMenu(id, fn) — dipanggil saat item menu tray diklik
    onMenu: (id, fn) => { menuHandlers[Number(id)] = fn; },

    window: {
      create: (o) => {
        // /tex/hel and ./tex/hel are page paths. Navigate needs a full address.
        let url = (o && o.url) ? String(o.url) : location.href;
        try { url = new URL(url, location.href).href; } catch (err) {}
        const msg = {
          cmd: 'windowCreate',
          url: url,
          title: String((o && o.title) || 'nx8v'),
          w: Number((o && o.width) || 900),
          h: Number((o && o.height) || 640),
        };
        if (o && o.x != null && o.y != null) {
          msg.x = Number(o.x);
          msg.y = Number(o.y);
        } else if (!o || o.center !== false) {
          msg.center = 1;
        }
        if (o && o.maximizable != null) msg.maximizable = o.maximizable ? 1 : 0;
        if (o && o.id != null && String(o.id) !== '') msg.win = String(o.id);
        return call(msg);
      },
      close: (id) => send({ cmd: 'windowClose', win: String(id == null ? '' : id) }),
    },
  };

  const menuHandlers = {};
  window.chrome.webview.addEventListener('message', (e) => {
    let d = e.data;
    if (typeof d === 'string') { try { d = JSON.parse(d); } catch (_) { return; } }
    if (d && d.menu != null && menuHandlers[d.menu]) menuHandlers[d.menu]();
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

  function trayMsg(o) {
    o = o || {};
    return {
      cmd: 'trayShow',
      tooltip: String(o.tooltip || 'nx8v'),
      menu: encodeItems(o.menu, 0).join('||'),
      exit: o.exit === false ? '0' : '1',
      exitLabel: String(o.exitLabel || 'Keluar'),
    };
  }

  // Flatten dialog options into plain strings so the native side can parse
  // them without a full JSON parser: "Name|ext1;ext2||Name2|ext3".
  function nx8vRects(sel) {
    var list = document.querySelectorAll(sel);
    var out = [];
    for (var i = 0; i < list.length; i++) {
      var r = list[i].getBoundingClientRect();
      if (r.width < 1 || r.height < 1) continue;
      out.push(Math.round(r.left) + ',' + Math.round(r.top) + ',' +
               Math.round(r.right) + ',' + Math.round(r.bottom));
    }
    return out.join(';');
  }
  var nx8vDragTimer = 0;
  function nx8vReportDrag() {
    if (nx8vDragTimer) return;
    nx8vDragTimer = setTimeout(function () {
      nx8vDragTimer = 0;
      send({
        cmd: 'dragRegions',
        value: nx8vRects('[data-nx8v-drag]') + '|' + nx8vRects('[data-nx8v-nodrag]'),
      });
    }, 40);
  }
  window.addEventListener('resize', nx8vReportDrag);
  window.addEventListener('load', nx8vReportDrag);
  function nx8vArmDragWatch() {
    var root = document.documentElement;
    if (!root || root.nodeType !== 1 || root.__nx8vDragWatch) return;
    root.__nx8vDragWatch = true;
    if (window.ResizeObserver)
      new ResizeObserver(nx8vReportDrag).observe(root);
    if (window.MutationObserver) {
      new MutationObserver(nx8vReportDrag).observe(root, {
        subtree: true, childList: true, attributes: true,
        attributeFilter: ['data-nx8v-drag', 'data-nx8v-nodrag', 'class', 'style'],
      });
    }
    nx8vReportDrag();
  }
  if (document.readyState === 'loading')
    document.addEventListener('DOMContentLoaded', nx8vArmDragWatch);
  nx8vArmDragWatch();

  function flatten(cmd, o) {
    o = o || {};
    const parts = (o.filters || []).map((f) => {
      const exts = (f.extensions || []).join(';');
      return (f.name || 'Files') + '|' + exts;
    });
    return {
      cmd,
      title: o.title || '',
      defaultName: o.defaultName || '',
      filters: parts.join('||'),
    };
  }
})();
)JS";

// ---------------------------------------------------------------------------
// Message loop pumping from the libuv loop
// ---------------------------------------------------------------------------
// Pump pending Win32 messages. Called from both the libuv prepare handle and a
// short repeating timer: the timer guarantees we wake up even while libuv is
// blocked waiting for I/O, which keeps the window responsive (no freeze).
static void Nx8vDrainMessages() {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT) {
      if (g_window) g_window->quit = true;
      break;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (g_window && g_window->quit && g_win_loop) {
    uv_stop(g_win_loop);
  }
}

static void Nx8vPumpPrepare(uv_prepare_t* handle) {
  (void)handle;
  Nx8vDrainMessages();
}

static void Nx8vPumpTimer(uv_timer_t* handle) {
  (void)handle;
  Nx8vDrainMessages();
}

// ---------------------------------------------------------------------------
// Native dialogs (IFileOpenDialog / IFileSaveDialog)
// ---------------------------------------------------------------------------
// Filters arrive from JS as { filters: [ { name, ext } ] } where ext is like
// "js" or "*.js". We convert them into COMDLG_FILTERSPEC.
static std::vector<std::wstring> Nx8vSplitFilters(const std::wstring& exts) {
  std::vector<std::wstring> out;
  std::wstring cur;
  for (wchar_t c : exts) {
    if (c == L';' || c == L',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  for (auto& e : out) {
    if (e.size() > 2 && e[0] == L'*' && e[1] == L'.') e = e.substr(2);
  }
  return out;
}

static std::wstring Nx8vDialogResult(IFileDialog* dlg) {
  std::wstring result;
  ComPtr<IShellItem> item;
  if (SUCCEEDED(dlg->GetResult(&item)) && item) {
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
      result = path;
      CoTaskMemFree(path);
    }
  }
  return result;
}

// kind: 0 = open file, 1 = save file, 2 = pick folder
static std::wstring Nx8vShowDialog(HWND owner, int kind,
                                   const std::wstring& title,
                                   const std::wstring& default_name,
                                   const std::vector<std::pair<std::wstring, std::wstring>>& filters) {
  std::wstring result;

  ComPtr<IFileOpenDialog> open;
  ComPtr<IFileSaveDialog> save;
  IFileDialog* dlg = nullptr;

  if (kind == 1) {
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&save))))
      return result;
    dlg = save.Get();
  } else {
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&open))))
      return result;
    dlg = open.Get();
  }

  DWORD opts = 0;
  dlg->GetOptions(&opts);
  opts |= FOS_FORCEFILESYSTEM;
  if (kind == 2) opts |= FOS_PICKFOLDERS;
  dlg->SetOptions(opts);

  if (!title.empty()) dlg->SetTitle(title.c_str());

  if (kind == 1 && !default_name.empty())
    dlg->SetFileName(default_name.c_str());

  // Filters: build the COMDLG_FILTERSPEC array.
  std::vector<std::wstring> spec_storage;
  std::vector<COMDLG_FILTERSPEC> spec;
  if (kind != 2 && !filters.empty()) {
    for (auto& f : filters) {
      spec_storage.push_back(f.first);   // name
      spec_storage.push_back(f.second);  // pattern, e.g. "*.js"
    }
    for (size_t i = 0; i + 1 < spec_storage.size(); i += 2)
      spec.push_back({spec_storage[i].c_str(), spec_storage[i + 1].c_str()});
    dlg->SetFileTypes((UINT)spec.size(), spec.data());
  }

  HRESULT hr = dlg->Show(owner);
  if (SUCCEEDED(hr)) result = Nx8vDialogResult(dlg);
  return result;
}

// Send { reply: id, value: <json> } back to the page.
// Host -> page messages must go through PostWebMessageAsJson; calling
// window.chrome.webview.postMessage() from ExecuteScript would instead send a
// page -> host message (the wrong direction).
static void Nx8vMessageDialog(WindowState* st, const std::wstring& cmd,
                              const std::wstring& text, const std::wstring& title,
                              int id);

static void Nx8vReply(WindowState* st, int id, const std::wstring& value_json) {
  if (!st->webview || id <= 0) return;
  std::wstring json = L"{\"reply\":" + std::to_wstring(id) +
                      L",\"value\":" + value_json + L"}";
  st->webview->PostWebMessageAsJson(json.c_str());
}

// Quote a wide string as a JSON string literal.
static std::wstring Nx8vJsonQuote(const std::wstring& s) {
  std::wstring out = L"\"";
  for (wchar_t c : s) {
    switch (c) {
      case L'"':  out += L"\\\""; break;
      case L'\\': out += L"\\\\"; break;
      case L'\n': out += L"\\n";  break;
      case L'\r': out += L"\\r";  break;
      case L'\t': out += L"\\t";  break;
      default:    out += c;
    }
  }
  out += L"\"";
  return out;
}

static void Nx8vMessageDialog(WindowState* st, const std::wstring& cmd,
                              const std::wstring& text, const std::wstring& title,
                              int id) {
  UINT flags = cmd == L"dialogConfirm" ? (MB_YESNO | MB_ICONQUESTION)
                                       : (MB_OK | MB_ICONINFORMATION);
  std::wstring cap = title.empty() ? L"nx8v" : title;
  int ans = MessageBoxW(st && st->hwnd ? st->hwnd : nullptr, text.c_str(),
                        cap.c_str(), flags);
  bool yes = cmd != L"dialogConfirm" || ans == IDYES;
  Nx8vReply(st, id, yes ? L"true" : L"false");
}

// ---------------------------------------------------------------------------
// Tray icon, notifications, application menu
// ---------------------------------------------------------------------------
static bool Nx8vTrayInit(WindowState* st) {
  if (!st) return false;
  if (st->tray_active) return true;

  st->nid = {};
  st->nid.cbSize = sizeof(NOTIFYICONDATAW);
  st->nid.hWnd   = st->hwnd;
  st->nid.uID    = 1;
  st->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  st->nid.uCallbackMessage = NX8V_WM_TRAY;
  st->nid.hIcon  = LoadIconW(nullptr, IDI_APPLICATION);
  wcscpy_s(st->nid.szTip, L"nx8v");

  if (Shell_NotifyIconW(NIM_ADD, &st->nid)) {
    st->nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &st->nid);
    st->tray_active = true;
    return true;
  }
  return false;
}

static void Nx8vTrayRemove(WindowState* st) {
  if (!st || !st->tray_active) return;
  Shell_NotifyIconW(NIM_DELETE, &st->nid);
  st->tray_active = false;
}

static void Nx8vTrayTooltip(WindowState* st, const std::wstring& text) {
  if (!st || !st->tray_active) return;
  wcsncpy_s(st->nid.szTip, text.c_str(), _TRUNCATE);
  st->nid.uFlags = NIF_TIP;
  Shell_NotifyIconW(NIM_MODIFY, &st->nid);
}

// Balloon/toast notification through the tray icon.
static void Nx8vNotify(WindowState* st, const std::wstring& title,
                       const std::wstring& body) {
  if (!st) return;
  if (!st->tray_active) Nx8vTrayInit(st);
  if (!st->tray_active) return;

  NOTIFYICONDATAW n = st->nid;
  n.uFlags = NIF_INFO;
  n.dwInfoFlags = NIIF_INFO;
  wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
  wcsncpy_s(n.szInfo, body.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &n);
}

static std::vector<Nx8vMenuItem> Nx8vParseMenu(const std::wstring& spec) {
  std::vector<Nx8vMenuItem> items;
  size_t pos = 0;
  while (pos <= spec.size()) {
    size_t sep = spec.find(L"||", pos);
    std::wstring one = (sep == std::wstring::npos) ? spec.substr(pos)
                                                   : spec.substr(pos, sep - pos);
    if (!one.empty()) {
      int depth = 0;
      while (depth < (int)one.size() && one[depth] == L'>') ++depth;
      one = one.substr(depth);
      Nx8vMenuItem it;
      it.depth = depth;
      size_t bar = one.find(L'|');
      it.label = bar == std::wstring::npos ? one : one.substr(0, bar);
      if (it.label == L"-") {
        it.sep = true;
      } else if (bar != std::wstring::npos) {
        it.id = _wtoi(one.c_str() + bar + 1);
        size_t bar2 = one.find(L'|', bar + 1);
        if (bar2 != std::wstring::npos) {
          it.enabled = _wtoi(one.c_str() + bar2 + 1) != 0;
          size_t bar3 = one.find(L'|', bar2 + 1);
          if (bar3 != std::wstring::npos) it.checked = _wtoi(one.c_str() + bar3 + 1) != 0;
        }
      }
      items.push_back(it);
    }
    if (sep == std::wstring::npos) break;
    pos = sep + 2;
  }
  return items;
}

static HMENU Nx8vMakeMenu(const std::vector<Nx8vMenuItem>& items) {
  HMENU root = CreatePopupMenu();
  std::vector<std::pair<int, HMENU>> stack;
  stack.push_back({-1, root});
  for (size_t i = 0; i < items.size(); ++i) {
    const Nx8vMenuItem& it = items[i];
    while (stack.size() > 1 && stack.back().first >= it.depth) stack.pop_back();
    HMENU parent = stack.back().second;
    if (it.sep) {
      AppendMenuW(parent, MF_SEPARATOR, 0, nullptr);
      continue;
    }
    bool has_child = i + 1 < items.size() && items[i + 1].depth > it.depth;
    if (has_child) {
      HMENU sub = CreatePopupMenu();
      AppendMenuW(parent, MF_POPUP | (it.enabled ? 0 : MF_GRAYED),
                  (UINT_PTR)sub, it.label.c_str());
      stack.push_back({it.depth, sub});
      continue;
    }
    UINT flags = MF_STRING;
    if (!it.enabled) flags |= MF_GRAYED;
    if (it.checked) flags |= MF_CHECKED;
    AppendMenuW(parent, flags, (UINT_PTR)it.id, it.label.c_str());
  }
  return root;
}

static void Nx8vBuildTrayMenu(WindowState* st, const std::wstring& spec,
                              bool add_exit, const std::wstring& exit_label) {
  if (!st) return;
  if (st->app_menu) { DestroyMenu(st->app_menu); st->app_menu = nullptr; }
  st->app_menu = Nx8vMakeMenu(Nx8vParseMenu(spec));
  if (!st->app_menu) return;
  if (!add_exit) return;
  AppendMenuW(st->app_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(st->app_menu, MF_STRING, 9001,
              exit_label.empty() ? L"Keluar" : exit_label.c_str());
}

static void Nx8vApplyDevTools(WindowState* st) {
  if (!st || !st->webview) return;
  ComPtr<ICoreWebView2Settings> settings;
  if (FAILED(st->webview->get_Settings(&settings)) || !settings) return;
  settings->put_AreDevToolsEnabled(st->devtools ? TRUE : FALSE);
}

static void Nx8vAllowClipboardRead(WindowState* st) {
  if (!st || !st->webview) return;
  EventRegistrationToken tok{};
  st->webview->add_PermissionRequested(
      Callback<ICoreWebView2PermissionRequestedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_PERMISSION_KIND kind = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
            if (!args) return S_OK;
            args->get_PermissionKind(&kind);
            if (kind == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ)
              args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
            return S_OK;
          })
          .Get(),
      &tok);
}

static void Nx8vInstallContextMenu(WindowState* st) {
  if (!st || !st->webview) return;
  ComPtr<ICoreWebView2_11> web;
  if (FAILED(st->webview.As(&web)) || !web) return;
  EventRegistrationToken tok{};
  web->add_ContextMenuRequested(
      Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
          [st](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
            if (st->context_menu.empty()) return S_OK;
            args->put_Handled(TRUE);
            POINT pt{};
            args->get_Location(&pt);
            ClientToScreen(st->hwnd, &pt);
            st->context_pt = pt;
            PostMessageW(st->hwnd, NX8V_WM_CONTEXT, 0, 0);
            return S_OK;
          })
          .Get(),
      &tok);
}

// ---------------------------------------------------------------------------
// Extra top-level windows. The primary window owns the WebView2 environment;
// each child gets its own controller and HWND.
// ---------------------------------------------------------------------------
static std::wstring Nx8vCreateChild(const std::wstring& url, const std::wstring& title,
                                   int width, int height, int x, int y,
                                   bool has_pos, bool center, bool maximizable,
                                   const std::wstring& win_id);
static void Nx8vCloseWindow(const std::wstring& id);

static void Nx8vAttachChild(WindowState* st) {
  if (!g_window || !g_window->env || !st->hwnd) return;
  g_window->env->CreateCoreWebView2Controller(
      st->hwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [st](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
            if (FAILED(hr) || !controller) return S_OK;
            st->controller = controller;
            controller->get_CoreWebView2(&st->webview);
            RECT rc{};
            GetClientRect(st->hwnd, &rc);
            controller->put_Bounds(rc);
            st->webview->AddScriptToExecuteOnDocumentCreated(kNx8vBridgeJs, nullptr);
            Nx8vApplyDevTools(st);
            Nx8vAllowClipboardRead(st);
            Nx8vInstallContextMenu(st);
            EventRegistrationToken tok{};
            st->webview->add_WebMessageReceived(
                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                    [st](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args)
                        -> HRESULT {
                      LPWSTR raw = nullptr;
                      args->get_WebMessageAsJson(&raw);
                      std::wstring json = raw ? raw : L"";
                      CoTaskMemFree(raw);
                      auto find_str = [&](const wchar_t* key) -> std::wstring {
                        std::wstring pat = std::wstring(L"\"") + key + L"\":\"";
                        auto p = json.find(pat);
                        if (p == std::wstring::npos) return L"";
                        p += pat.size();
                        std::wstring out;
                        for (; p < json.size() && json[p] != L'"'; ++p) {
                          if (json[p] == L'\\' && p + 1 < json.size()) ++p;
                          out += json[p];
                        }
                        return out;
                      };
                      auto find_num = [&](const wchar_t* key) -> int {
                        std::wstring pat = std::wstring(L"\"") + key + L"\":";
                        auto p = json.find(pat);
                        if (p == std::wstring::npos) return 0;
                        return _wtoi(json.c_str() + p + pat.size());
                      };
                      std::wstring cmd = find_str(L"cmd");
                      if (cmd == L"setTitle") {
                        SetWindowTextW(st->hwnd, find_str(L"value").c_str());
                      } else if (cmd == L"dragRegions") {
                        Nx8vSetDragRegions(st, find_str(L"value"));
                      } else if (cmd == L"setSize") {
                        int w = find_num(L"w"), h = find_num(L"h");
                        if (w > 0 && h > 0)
                          SetWindowPos(st->hwnd, nullptr, 0, 0, w, h,
                                       SWP_NOMOVE | SWP_NOZORDER);
                      } else if (cmd == L"close") {
                        PostMessageW(st->hwnd, WM_CLOSE, 0, 0);
                      } else if (cmd == L"devtools" && st->devtools && st->webview) {
                        st->webview->OpenDevToolsWindow();
                      } else if (cmd == L"contextMenu") {
                        st->context_menu = find_str(L"menu");
                      } else if (cmd == L"dialogAlert" || cmd == L"dialogConfirm") {
                        Nx8vMessageDialog(st, cmd, find_str(L"text"),
                                          find_str(L"title"), find_num(L"id"));
                      } else if (cmd == L"windowCreate") {
                        int w = find_num(L"w"), h = find_num(L"h");
                        bool has_pos = json.find(L"\"x\":") != std::wstring::npos &&
                                       json.find(L"\"y\":") != std::wstring::npos;
                        std::wstring wid = Nx8vCreateChild(
                            find_str(L"url"), find_str(L"title"), w, h,
                            find_num(L"x"), find_num(L"y"), has_pos,
                            find_num(L"center") != 0,
                            json.find(L"\"maximizable\":") == std::wstring::npos
                                ? g_window->maximizable
                                : find_num(L"maximizable") != 0,
                            find_str(L"win"));
                        Nx8vReply(st, find_num(L"id"),
                                  wid.empty() ? L"null" : Nx8vJsonQuote(wid));
                      } else if (cmd == L"windowClose") {
                        Nx8vCloseWindow(find_str(L"win"));
                      }
                      return S_OK;
                    })
                    .Get(),
                &tok);
            std::wstring wurl(st->url.begin(), st->url.end());
            Nx8vRevealOnHtml(st);
            st->webview->Navigate(wurl.c_str());
            return S_OK;
          })
          .Get());
}

static int g_next_win_id = 1;

static void Nx8vCloseWindow(const std::wstring& id) {
  if (id.empty()) return;
  for (WindowState* w : g_extra) {
    if (w && w->win_id == id && w->hwnd && IsWindow(w->hwnd)) {
      PostMessageW(w->hwnd, WM_CLOSE, 0, 0);
      return;
    }
  }
}

static std::wstring Nx8vCreateChild(const std::wstring& url, const std::wstring& title,
                                   int width, int height, int x, int y,
                                   bool has_pos, bool center, bool maximizable,
                                   const std::wstring& win_id) {
  if (!g_window || !g_window->env) return L"";
  if (width <= 0) width = 900;
  if (height <= 0) height = 640;
  auto* st = new WindowState();
  g_extra.push_back(st);
  st->url.assign(url.begin(), url.end());
  if (st->url.empty()) st->url = g_window->url;
  st->maximizable = maximizable;
  st->devtools = g_window->devtools;
  st->win_id = win_id.empty() ? std::to_wstring(g_next_win_id++) : win_id;
  HINSTANCE hinst = GetModuleHandleW(nullptr);
  std::wstring wtitle = title.empty() ? L"nx8v" : title;
  int pos_x = CW_USEDEFAULT;
  int pos_y = CW_USEDEFAULT;
  if (has_pos) {
    pos_x = x;
    pos_y = y;
  } else if (center) {
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
      pos_x = work.left + ((work.right - work.left) - width) / 2;
      pos_y = work.top + ((work.bottom - work.top) - height) / 2;
      if (pos_x < work.left) pos_x = work.left;
      if (pos_y < work.top) pos_y = work.top;
    }
  }
  DWORD style = WS_OVERLAPPEDWINDOW;
  if (!maximizable) style &= ~WS_MAXIMIZEBOX;
  st->hwnd = CreateWindowExW(0, L"NX8VWindowClass", wtitle.c_str(),
                             style,
                             pos_x, pos_y, width, height,
                             nullptr, nullptr, hinst, st);
  if (!st->hwnd) return L"";
  Nx8vAttachChild(st);
  st->show_cmd = SW_SHOW;
  Nx8vCloak(st->hwnd, true);
  ShowWindow(st->hwnd, SW_SHOW);
  UpdateWindow(st->hwnd);
  SetTimer(st->hwnd, NX8V_REVEAL_TIMER, 8000, nullptr);
  return st->win_id;
}

// ---------------------------------------------------------------------------
// WebView2 setup
// ---------------------------------------------------------------------------
// The browser process inherits our console, so Ctrl+C is delivered to it too.
// It then logs "Failed to unregister class Chrome_WidgetWin_0" into this
// terminal. Start it while we are detached so it has no console to print on
// and does not receive that Ctrl+C. Our own stdout is attached again after.
struct Nx8vConsoleDetach {
  bool active = false;
  bool out_console = false;
  bool err_console = false;

  void Detach() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    out_console = out && GetFileType(out) == FILE_TYPE_CHAR;
    err_console = err && GetFileType(err) == FILE_TYPE_CHAR;
    active = out_console || err_console;
    if (active) FreeConsole();
  }

  void Attach() {
    if (!active) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
      FILE* f = nullptr;
      if (out_console) freopen_s(&f, "CONOUT$", "w", stdout);
      if (err_console) freopen_s(&f, "CONOUT$", "w", stderr);
      if (out_console) std::fflush(stdout);
      if (err_console) std::fflush(stderr);
    }
    active = false;
  }
};

static void Nx8vSetupWebView(WindowState* st) {
  auto on_env = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
      [st](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
        if (FAILED(result) || !env) {
          std::fprintf(stderr, "nx8v window: failed to create the WebView2 environment "
                               "(0x%08lX). Install the WebView2 Runtime.\n",
                       (unsigned long)result);
          PostQuitMessage(1);
          return S_OK;
        }
        st->env = env;

        auto on_controller =
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [st](HRESULT res2, ICoreWebView2Controller* controller) -> HRESULT {
                  if (FAILED(res2) || !controller) {
                    std::fprintf(stderr, "nx8v window: failed to create the controller "
                                         "(0x%08lX)\n", (unsigned long)res2);
                    PostQuitMessage(1);
                    return S_OK;
                  }
                  st->controller = controller;
                  controller->get_CoreWebView2(&st->webview);

                  RECT rc{};
                  GetClientRect(st->hwnd, &rc);
                  controller->put_Bounds(rc);

                  // Inject the bridge before any page script runs.
                  st->webview->AddScriptToExecuteOnDocumentCreated(
                      kNx8vBridgeJs, nullptr);
                  Nx8vApplyDevTools(st);
                  Nx8vAllowClipboardRead(st);
                  Nx8vInstallContextMenu(st);
                  Nx8vRevealOnHtml(st);

                  // Handle bridge messages.
                  EventRegistrationToken tok{};
                  st->webview->add_WebMessageReceived(
                      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                          [st](ICoreWebView2*,
                               ICoreWebView2WebMessageReceivedEventArgs* args)
                              -> HRESULT {
                            LPWSTR raw = nullptr;
                            args->get_WebMessageAsJson(&raw);
                            std::wstring json = raw ? raw : L"";
                            CoTaskMemFree(raw);

                            auto find_num = [&](const wchar_t* key) -> int {
                              std::wstring pat = std::wstring(L"\"") + key + L"\":";
                              auto p = json.find(pat);
                              if (p == std::wstring::npos) return 0;
                              p += pat.size();
                              return _wtoi(json.c_str() + p);
                            };
                            auto find_str = [&](const wchar_t* key) -> std::wstring {
                              std::wstring pat = std::wstring(L"\"") + key + L"\":\"";
                              auto p = json.find(pat);
                              if (p == std::wstring::npos) return L"";
                              p += pat.size();
                              std::wstring out;
                              for (; p < json.size() && json[p] != L'"'; ++p) {
                                if (json[p] == L'\\' && p + 1 < json.size()) ++p;
                                out += json[p];
                              }
                              return out;
                            };

                            std::wstring cmd = find_str(L"cmd");
                            if (cmd == L"setTitle") {
                              SetWindowTextW(st->hwnd, find_str(L"value").c_str());
                            } else if (cmd == L"dragRegions") {
                              Nx8vSetDragRegions(st, find_str(L"value"));
                            } else if (cmd == L"setSize") {
                              int w = find_num(L"w"), h = find_num(L"h");
                              if (w > 0 && h > 0) SetWindowPos(
                                  st->hwnd, nullptr, 0, 0, w, h,
                                  SWP_NOMOVE | SWP_NOZORDER);
                            } else if (cmd == L"minimize") {
                              ShowWindow(st->hwnd, SW_MINIMIZE);
                            } else if (cmd == L"maximize") {
                              if (!st->maximizable || !st->resizable) return S_OK;
                              ShowWindow(st->hwnd,
                                         IsZoomed(st->hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                            } else if (cmd == L"close") {
                              PostMessageW(st->hwnd, WM_CLOSE, 0, 0);
                            } else if (cmd == L"devtools" && st->devtools) {
                              st->webview->OpenDevToolsWindow();
                            } else if (cmd == L"dialogAlert" ||
                                       cmd == L"dialogConfirm") {
                              Nx8vMessageDialog(st, cmd, find_str(L"text"),
                                                find_str(L"title"), find_num(L"id"));
                            } else if (cmd == L"dialogOpen" ||
                                       cmd == L"dialogSave" ||
                                       cmd == L"dialogFolder") {
                              int kind = (cmd == L"dialogSave")   ? 1
                                       : (cmd == L"dialogFolder") ? 2
                                                                  : 0;
                              int id = find_num(L"id");

                              // Parse "Name|ext1;ext2||Name2|ext3"
                              std::wstring raw_filters = find_str(L"filters");
                              std::vector<std::pair<std::wstring, std::wstring>> filters;
                              {
                                size_t pos = 0;
                                while (pos <= raw_filters.size()) {
                                  size_t sep = raw_filters.find(L"||", pos);
                                  std::wstring one = (sep == std::wstring::npos)
                                                         ? raw_filters.substr(pos)
                                                         : raw_filters.substr(pos, sep - pos);
                                  if (!one.empty()) {
                                    size_t bar = one.find(L'|');
                                    std::wstring name = (bar == std::wstring::npos)
                                                            ? one : one.substr(0, bar);
                                    std::wstring exts = (bar == std::wstring::npos)
                                                            ? L"" : one.substr(bar + 1);
                                    std::wstring pattern;
                                    for (auto& e : Nx8vSplitFilters(exts)) {
                                      if (!pattern.empty()) pattern += L";";
                                      pattern += L"*." + e;
                                    }
                                    if (pattern.empty()) pattern = L"*.*";
                                    filters.push_back({name, pattern});
                                  }
                                  if (sep == std::wstring::npos) break;
                                  pos = sep + 2;
                                }
                              }

                              std::wstring picked = Nx8vShowDialog(
                                  st->hwnd, kind, find_str(L"title"),
                                  find_str(L"defaultName"), filters);

                              Nx8vReply(st, id,
                                        picked.empty() ? L"null"
                                                       : Nx8vJsonQuote(picked));
                            } else if (cmd == L"contextMenu") {
                              st->context_menu = find_str(L"menu");
                            } else if (cmd == L"trayShow") {
                              bool ok = Nx8vTrayInit(st);
                              if (ok) {
                                Nx8vTrayTooltip(st, find_str(L"tooltip"));
                                Nx8vBuildTrayMenu(st, find_str(L"menu"),
                                                  find_str(L"exit") != L"0",
                                                  find_str(L"exitLabel"));
                              }
                              Nx8vReply(st, find_num(L"id"),
                                        ok ? L"true" : L"false");
                            } else if (cmd == L"trayHide") {
                              Nx8vTrayRemove(st);
                            } else if (cmd == L"trayTooltip") {
                              Nx8vTrayTooltip(st, find_str(L"value"));
                            } else if (cmd == L"notify") {
                              Nx8vNotify(st, find_str(L"title"),
                                         find_str(L"body"));
                            } else if (cmd == L"windowCreate") {
                              int w = find_num(L"w"), h = find_num(L"h");
                              bool has_pos = json.find(L"\"x\":") != std::wstring::npos &&
                                             json.find(L"\"y\":") != std::wstring::npos;
                              std::wstring wid = Nx8vCreateChild(
                                  find_str(L"url"), find_str(L"title"), w, h,
                                  find_num(L"x"), find_num(L"y"), has_pos,
                                  find_num(L"center") != 0,
                                  json.find(L"\"maximizable\":") == std::wstring::npos
                                      ? g_window->maximizable
                                      : find_num(L"maximizable") != 0,
                                  find_str(L"win"));
                              Nx8vReply(st, find_num(L"id"),
                                        wid.empty() ? L"null" : Nx8vJsonQuote(wid));
                            } else if (cmd == L"windowClose") {
                              Nx8vCloseWindow(find_str(L"win"));
                            }
                            return S_OK;
                          })
                          .Get(),
                      &tok);

                  st->webview->Navigate(
                      std::wstring(st->url.begin(), st->url.end()).c_str());
                  return S_OK;
                });
        env->CreateCoreWebView2Controller(st->hwnd, on_controller.Get());
        return S_OK;
      });

  Nx8vConsoleDetach console;
  console.Detach();
  CreateCoreWebView2EnvironmentWithOptions(
      nullptr, st->user_data_dir.c_str(), nullptr, on_env.Get());
  console.Attach();
}

// ---------------------------------------------------------------------------
// Live reload while `nx8v window` is open. HTML/CSS/SVG reloads the page.
// JS/JSON starts the same command again so the server code is fresh.
// ---------------------------------------------------------------------------
struct Nx8vWinWatch {
  std::vector<uv_fs_event_t*> watchers;
  uv_timer_t debounce{};
  bool code = false;
  bool relaunching = false;
};
static Nx8vWinWatch g_winwatch;

static bool Nx8vWinSkipDir(const fs::path& p) {
  std::string n = p.filename().string();
  return n == "node_modules" || n == ".git" || n == "build" ||
         n == "dist" || n == ".nx8v" || n == "deps";
}

struct Nx8vWinStamp {
  std::uintmax_t size = 0;
  fs::file_time_type mtime{};
};
static std::map<std::string, Nx8vWinStamp> g_win_stamps;
static uint64_t g_win_ignore_until = 0;

static std::string Nx8vWinExt(const fs::path& p) {
  std::string ext = p.extension().string();
  for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
  return ext;
}

static bool Nx8vWinInteresting(const std::string& ext) {
  return ext == ".js" || ext == ".mjs" || ext == ".json" ||
         ext == ".html" || ext == ".css" || ext == ".svg";
}

static std::string Nx8vWinKey(const fs::path& p) {
  std::error_code ec;
  fs::path abs = fs::absolute(p, ec);
  if (ec) abs = p;
  std::string key = abs.lexically_normal().generic_string();
#ifdef _WIN32
  for (auto& c : key) c = (char)std::tolower((unsigned char)c);
#endif
  return key;
}

// Reading a page updates last-access time. WebView2 then reloads, which
// reads the files again. Only a new size or write time is a real edit.
static bool Nx8vWinChanged(const fs::path& file) {
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) return false;
  Nx8vWinStamp now;
  now.size = fs::file_size(file, ec);
  if (ec) return false;
  now.mtime = fs::last_write_time(file, ec);
  if (ec) return false;
  std::string key = Nx8vWinKey(file);
  auto it = g_win_stamps.find(key);
  if (it != g_win_stamps.end() && it->second.size == now.size &&
      it->second.mtime == now.mtime)
    return false;
  g_win_stamps[key] = now;
  return true;
}

static void Nx8vWinRememberTree(const fs::path& root) {
  std::error_code ec;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;
  while (it != end) {
    if (ec) { it.increment(ec); continue; }
    if (it->is_directory(ec)) {
      if (Nx8vWinSkipDir(it->path())) it.disable_recursion_pending();
    } else if (Nx8vWinInteresting(Nx8vWinExt(it->path()))) {
      Nx8vWinChanged(it->path());
    }
    it.increment(ec);
  }
}

static void Nx8vReloadViews() {
  auto reload = [](WindowState* w) {
    if (w && w->webview) w->webview->Reload();
  };
  reload(g_window);
  for (WindowState* extra : g_extra) reload(extra);
  std::printf("[window] reloading the page\n");
  std::fflush(stdout);
}

static void Nx8vRelaunchWindow() {
  if (g_winwatch.relaunching) return;
  g_winwatch.relaunching = true;
  std::printf("[window] server changed, reopening the window\n");
  std::fflush(stdout);
  SetEnvironmentVariableW(L"NX8V_WINDOW_RELAUNCH", L"1");
  std::wstring cmd = GetCommandLineW();
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(0);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &si, &pi)) {
    std::fprintf(stderr, "[window] failed to reopen the window\n");
    g_winwatch.relaunching = false;
    return;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (g_window) {
    g_window->allow_close = true;
    PostMessageW(g_window->hwnd, WM_CLOSE, 0, 0);
  }
}

static void Nx8vOnWinFile(uv_fs_event_t* handle, const char* filename,
                          int, int status) {
  if (status != 0 || !filename || g_winwatch.relaunching) return;
  if (uv_now(handle->loop) < g_win_ignore_until) return;
  char dirname[1024];
  size_t dsize = sizeof(dirname);
  if (uv_fs_event_getpath(handle, dirname, &dsize) != 0) return;
  fs::path full = fs::path(std::string(dirname, dsize)) / filename;
  std::string ext = Nx8vWinExt(full);
  bool code = ext == ".js" || ext == ".mjs" || ext == ".json";
  bool asset = ext == ".html" || ext == ".css" || ext == ".svg";
  if (!code && !asset) return;
  if (!Nx8vWinChanged(full)) return;
  if (code) g_winwatch.code = true;
  uv_timer_stop(&g_winwatch.debounce);
  uv_timer_start(&g_winwatch.debounce, [](uv_timer_t* t) {
    if (g_winwatch.relaunching) return;
    // Page reads must not count as another edit.
    g_win_ignore_until = uv_now(t->loop) + 500;
    if (g_winwatch.code) {
      g_winwatch.code = false;
      Nx8vRelaunchWindow();
      return;
    }
    Nx8vReloadViews();
  }, 120, 0);
}

static void Nx8vWatchDir(uv_loop_t* loop, const fs::path& dir) {
  auto* w = new uv_fs_event_t;
  if (uv_fs_event_init(loop, w) != 0) { delete w; return; }
  if (uv_fs_event_start(w, Nx8vOnWinFile, dir.string().c_str(), 0) != 0) {
    uv_close(reinterpret_cast<uv_handle_t*>(w),
             [](uv_handle_t* h) { delete reinterpret_cast<uv_fs_event_t*>(h); });
    return;
  }
  g_winwatch.watchers.push_back(w);
}

static void Nx8vWatchProject(uv_loop_t* loop) {
  fs::path root = ".";
  if (nx8v::g_runtime && !nx8v::g_runtime->script_dir.empty())
    root = nx8v::g_runtime->script_dir;
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return;
  Nx8vWinRememberTree(root);
  uv_timer_init(loop, &g_winwatch.debounce);
  Nx8vWatchDir(loop, root);
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;
  while (it != end) {
    if (ec) { it.increment(ec); continue; }
    if (it->is_directory(ec) && !Nx8vWinSkipDir(it->path()))
      Nx8vWatchDir(loop, it->path());
    it.increment(ec);
  }
}

static std::wstring Nx8vWiden(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring out(n, 0);
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
  return out;
}

// One desktop window per project folder. A second launch focuses the first.
static HANDLE g_nx8v_inst_mutex = nullptr;
static HANDLE g_nx8v_inst_map = nullptr;
static HWND* g_nx8v_inst_slot = nullptr;

static std::wstring Nx8vInstanceName(std::wstring key) {
  for (auto& c : key) {
    if (c == L'/') c = L'\\';
    if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
  }
  unsigned hash = 2166136261u;
  for (wchar_t c : key) {
    hash ^= (unsigned)c;
    hash *= 16777619u;
  }
  const wchar_t* hex = L"0123456789abcdef";
  std::wstring name = L"Local\\nx8v-";
  for (int i = 7; i >= 0; --i) name.push_back(hex[(hash >> (i * 4)) & 0xF]);
  return name;
}

static void Nx8vFocusHwnd(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return;
  DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
  DWORD me = GetCurrentThreadId();
  if (fg && fg != me) AttachThreadInput(me, fg, TRUE);
  ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(hwnd);
  BringWindowToTop(hwnd);
  if (fg && fg != me) AttachThreadInput(me, fg, FALSE);
}

static void Nx8vMapInstanceSlot(const std::wstring& name) {
  g_nx8v_inst_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      0, sizeof(HWND), (name + L"-hwnd").c_str());
  if (!g_nx8v_inst_map) return;
  g_nx8v_inst_slot = reinterpret_cast<HWND*>(
      MapViewOfFile(g_nx8v_inst_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HWND)));
  if (g_nx8v_inst_slot) *g_nx8v_inst_slot = nullptr;
}

// Returns true when this process owns the window. False when another window
// was focused and this process should exit.
inline bool Nx8vTakeWindowInstance(const std::wstring& project_dir) {
  std::wstring name = Nx8vInstanceName(project_dir);
  g_nx8v_inst_mutex = CreateMutexW(nullptr, FALSE, name.c_str());
  if (!g_nx8v_inst_mutex) return true;
  wchar_t relaunch[8] = {};
  bool wait_prev = GetEnvironmentVariableW(L"NX8V_WINDOW_RELAUNCH", relaunch, 8) > 0;
  DWORD got = WaitForSingleObject(g_nx8v_inst_mutex, wait_prev ? 15000 : 0);
  if (got == WAIT_OBJECT_0 || got == WAIT_ABANDONED) {
    Nx8vMapInstanceSlot(name);
    return true;
  }
  HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, (name + L"-hwnd").c_str());
  HWND hwnd = nullptr;
  for (int i = 0; i < 80 && !hwnd; ++i) {
    if (!map) map = OpenFileMappingW(FILE_MAP_READ, FALSE, (name + L"-hwnd").c_str());
    if (map) {
      auto* slot = reinterpret_cast<HWND*>(
          MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(HWND)));
      if (slot) {
        if (*slot && IsWindow(*slot)) hwnd = *slot;
        UnmapViewOfFile(slot);
      }
    }
    if (!hwnd) Sleep(50);
  }
  if (map) CloseHandle(map);
  if (hwnd) Nx8vFocusHwnd(hwnd);
  CloseHandle(g_nx8v_inst_mutex);
  g_nx8v_inst_mutex = nullptr;
  return false;
}

inline void Nx8vPublishPrimaryWindow(HWND hwnd) {
  if (g_nx8v_inst_slot) *g_nx8v_inst_slot = hwnd;
}

// ---------------------------------------------------------------------------
// RunWindow: create the window and run the message loop alongside libuv
// ---------------------------------------------------------------------------
inline int RunWindow(const std::string& url, const std::string& title,
                     int width, int height, uv_loop_t* loop,
                     const Nx8vWindowOpts& opts = {}) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  // WebView2's browser process inherits this console. On Ctrl+C it logs
  // "Failed to unregister class Chrome_WidgetWin_0" (error 1411). That is
  // Chromium tearing the window class down, not an NX8V failure. Silence
  // those browser logs so the console stays clean.
  {
    std::wstring args = L"--disable-logging --log-level=3";
    wchar_t existing[2048];
    DWORD n = GetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
                                      existing, 2048);
    if (n > 0 && n < 2048) args = std::wstring(existing) + L" " + args;
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
                            args.c_str());
  }

  static WindowState st;
  g_window = &st;
  g_win_loop = loop;
  st.url = url;
  st.min_w = opts.min_w;
  st.min_h = opts.min_h;
  st.frameless = opts.frameless;
  st.maximizable = opts.maximizable;
  st.resizable = opts.resizable;
  st.devtools = opts.devtools;

  // Per-user data folder for the WebView2 profile.
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  st.user_data_dir = std::wstring(tmp) + L"nx8v-webview2-2";

  HINSTANCE hinst = GetModuleHandleW(nullptr);

  const wchar_t* kClass = L"NX8VWindowClass";
  WNDCLASSEXW wc{};
  wc.cbSize        = sizeof(wc);
  wc.style         = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc   = Nx8vWndProc;
  wc.hInstance     = hinst;
  wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = kClass;

  std::string shown_title = title;
  std::string icon_path = opts.icon;
  fs::path icon_root = ".";
  if (nx8v::g_runtime && !nx8v::g_runtime->script_dir.empty())
    icon_root = nx8v::g_runtime->script_dir;
  {
    std::error_code ec;
    fs::path manifest = icon_root / "project.json";
    if (fs::exists(manifest, ec)) {
      std::string json = ReadManifestFile(manifest);
      std::string icon_rel, product;
      if (icon_path.empty() && JsonFindString(json, "icon", icon_rel) && !icon_rel.empty())
        icon_path = Nx8vResolveIconPath(icon_root, icon_rel).string();
      if (shown_title == "nx8v" && JsonFindString(json, "productName", product) &&
          !product.empty())
        shown_title = product;
    }
  }
  if (icon_path.empty()) {
    fs::path guess = icon_root / "icon.ico";
    std::error_code ec;
    if (fs::is_regular_file(guess, ec)) icon_path = guess.string();
  }
  HICON icon = nullptr;
  if (!icon_path.empty()) {
    icon = Nx8vLoadIconFile(Nx8vWiden(icon_path));
    if (icon) wc.hIcon = icon;
  }
  RegisterClassExW(&wc);

  std::wstring wtitle(shown_title.begin(), shown_title.end());
  DWORD style = opts.frameless
      ? (WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU)
      : (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX));
  if (opts.resizable) style |= WS_THICKFRAME;
  if (opts.maximizable && opts.resizable) style |= WS_MAXIMIZEBOX;
  DWORD ex = opts.frameless ? WS_EX_APPWINDOW : 0;
  int pos_x = CW_USEDEFAULT;
  int pos_y = CW_USEDEFAULT;
  if (opts.has_pos) {
    pos_x = opts.x;
    pos_y = opts.y;
  } else if (opts.center) {
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
      pos_x = work.left + ((work.right - work.left) - width) / 2;
      pos_y = work.top + ((work.bottom - work.top) - height) / 2;
      if (pos_x < work.left) pos_x = work.left;
      if (pos_y < work.top) pos_y = work.top;
    }
  }
  st.hwnd = CreateWindowExW(ex, kClass, wtitle.c_str(), style,
                            pos_x, pos_y, width, height,
                            nullptr, nullptr, hinst, &st);
  if (!st.hwnd) {
    std::fprintf(stderr, "nx8v window: CreateWindowEx failed (%lu)\n",
                 (unsigned long)GetLastError());
    return 1;
  }
  if (opts.frameless) {
    MARGINS margins{0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(st.hwnd, &margins);
    Nx8vHideSystemBorder(st.hwnd);
    SetWindowPos(st.hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
  }
  Nx8vPublishPrimaryWindow(st.hwnd);
  if (icon) {
    SendMessageW(st.hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessageW(st.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
  }

  Nx8vSetupWebView(&st);

  int show_cmd = SW_SHOWNORMAL;
  if (opts.show == 1) show_cmd = SW_SHOWMAXIMIZED;
  else if (opts.show == 2) show_cmd = SW_SHOWMINIMIZED;
  st.show_cmd = show_cmd;
  Nx8vCloak(st.hwnd, true);
  ShowWindow(st.hwnd, show_cmd);
  UpdateWindow(st.hwnd);
  if (opts.frameless) Nx8vHideSystemBorder(st.hwnd);
  SetTimer(st.hwnd, NX8V_REVEAL_TIMER, 8000, nullptr);

  // Pump Win32 messages from the libuv loop.
  //  - prepare: runs on every loop iteration
  //  - 4 ms timer: keeps pumping while libuv blocks on I/O (window stays alive)
  uv_prepare_init(loop, &st.pump);
  uv_prepare_start(&st.pump, Nx8vPumpPrepare);

  uv_timer_init(loop, &st.pump_timer);
  uv_timer_start(&st.pump_timer, Nx8vPumpTimer, 4, 4);
  Nx8vWatchProject(loop);

  uv_run(loop, UV_RUN_DEFAULT);

  uv_prepare_stop(&st.pump);
  uv_timer_stop(&st.pump_timer);
  if (st.controller) st.controller->Close();
  // The page's server is still listening. Drop it so closing the window
  // returns the terminal prompt instead of leaving the process running.
  ReleaseLoopHandles(loop);
  CoUninitialize();
  g_window = nullptr;
  g_win_loop = nullptr;
  return 0;
}

}} // nx8v::bindings

#else

namespace nx8v {
namespace bindings {
inline int RunWindow(const std::string&, const std::string&, int, int, uv_loop_t*,
                     const Nx8vWindowOpts& = {}) {
  std::fprintf(stderr, "nx8v window is supported on Windows only\n");
  return 1;
}
}} // nx8v::bindings

#endif
