#pragma once
// Built-in page terminal. Included from http.h inside namespace nx8v::bindings,
// after HttpConn and WsSendFrame. Do not include system headers from here.

// One interactive shell per <nx8v-terminal>. Windows uses ConPTY so the
// program sees a real console (prompt, color, Ctrl+C). The page draws the
// VT stream. Paste into HTML:
//
//   <nx8v-terminal></nx8v-terminal>
//
// The runtime inserts this script when that tag is in the page.

struct Nx8vTerm {
  HttpConn* conn = nullptr;
#ifdef _WIN32
  void* hpcon = nullptr;
  HANDLE input = nullptr;
  HANDLE output = nullptr;
  HANDLE process = nullptr;
#endif
  std::thread reader;
  uv_async_t async{};
  std::mutex mu;
  std::string pending;
  bool stop = false;
  bool exited = false;
  bool told = false;
};

#ifdef _WIN32
using Nx8vCreatePseudoConsole = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, void**);
using Nx8vResizePseudoConsole = HRESULT(WINAPI*)(void*, COORD);
using Nx8vClosePseudoConsole = void(WINAPI*)(void*);

struct Nx8vConPtyFns {
  Nx8vCreatePseudoConsole create = nullptr;
  Nx8vResizePseudoConsole resize = nullptr;
  Nx8vClosePseudoConsole close = nullptr;
  bool ok() const { return create && resize && close; }
};

static const Nx8vConPtyFns& Nx8vConPty() {
  static const Nx8vConPtyFns api = [] {
    Nx8vConPtyFns a;
    if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
      a.create = reinterpret_cast<Nx8vCreatePseudoConsole>(
          reinterpret_cast<void*>(GetProcAddress(kernel, "CreatePseudoConsole")));
      a.resize = reinterpret_cast<Nx8vResizePseudoConsole>(
          reinterpret_cast<void*>(GetProcAddress(kernel, "ResizePseudoConsole")));
      a.close = reinterpret_cast<Nx8vClosePseudoConsole>(
          reinterpret_cast<void*>(GetProcAddress(kernel, "ClosePseudoConsole")));
    }
    return a;
  }();
  return api;
}

static bool Nx8vTermFile(const wchar_t* path) {
  DWORD a = GetFileAttributesW(path);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static std::wstring Nx8vTermShell() {
  wchar_t buf[MAX_PATH];
  if (SearchPathW(nullptr, L"pwsh.exe", nullptr, MAX_PATH, buf, nullptr)) return buf;
  const wchar_t* cands[] = {
      L"C:\\Program Files\\PowerShell\\7\\pwsh.exe",
      L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
      L"C:\\Windows\\System32\\cmd.exe"};
  for (const wchar_t* c : cands)
    if (Nx8vTermFile(c)) return c;
  return L"cmd.exe";
}

static std::wstring Nx8vTermQuote(const std::wstring& s) {
  if (s.find_first_of(L" \t\"") == std::wstring::npos) return s;
  std::wstring out = L"\"";
  for (wchar_t c : s) {
    if (c == L'"') out += L"\\\"";
    else out.push_back(c);
  }
  out.push_back(L'"');
  return out;
}

static std::wstring Nx8vTermEnv() {
  std::wstring block;
  bool has_term = false, has_color = false;
  wchar_t* src = GetEnvironmentStringsW();
  if (src) {
    for (wchar_t* p = src; *p;) {
      std::wstring line = p;
      if (line.rfind(L"TERM=", 0) == 0) has_term = true;
      if (line.rfind(L"COLORTERM=", 0) == 0) has_color = true;
      block += line;
      block.push_back(L'\0');
      p += line.size() + 1;
    }
    FreeEnvironmentStringsW(src);
  }
  if (!has_term) {
    block += L"TERM=xterm-256color";
    block.push_back(L'\0');
  }
  if (!has_color) {
    block += L"COLORTERM=truecolor";
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}
#endif

static void Nx8vTermFlush(uv_async_t* async) {
  auto* t = static_cast<Nx8vTerm*>(async->data);
  std::string data;
  bool tell = false;
  {
    std::lock_guard<std::mutex> lock(t->mu);
    data.swap(t->pending);
    if (t->exited && !t->told) {
      t->told = true;
      tell = true;
    }
  }
  if (!t->conn) return;
  if (!data.empty()) WsSendFrame(t->conn, 0x2, data);
  if (tell) WsSendFrame(t->conn, 0x1, "\r\n[proses selesai]\r\n");
}

static void Nx8vTermClose(HttpConn* conn) {
  if (!conn || !conn->term) return;
  auto* t = static_cast<Nx8vTerm*>(conn->term);
  conn->term = nullptr;
  {
    std::lock_guard<std::mutex> lock(t->mu);
    t->conn = nullptr;
    t->stop = true;
  }
#ifdef _WIN32
  if (t->hpcon && Nx8vConPty().close) Nx8vConPty().close(t->hpcon);
  t->hpcon = nullptr;
  if (t->output) {
    CloseHandle(t->output);
    t->output = nullptr;
  }
  if (t->reader.joinable()) t->reader.join();
  if (t->process) {
    TerminateProcess(t->process, 1);
    CloseHandle(t->process);
    t->process = nullptr;
  }
  if (t->input) {
    CloseHandle(t->input);
    t->input = nullptr;
  }
#else
  if (t->reader.joinable()) t->reader.join();
#endif
  uv_close(reinterpret_cast<uv_handle_t*>(&t->async), [](uv_handle_t* h) {
    delete static_cast<Nx8vTerm*>(h->data);
  });
}

static void Nx8vTermWrite(Nx8vTerm* t, const std::string& data) {
#ifdef _WIN32
  if (!t || !t->input || data.empty()) return;
  DWORD wrote = 0;
  WriteFile(t->input, data.data(), (DWORD)data.size(), &wrote, nullptr);
#else
  (void)t;
  (void)data;
#endif
}

static void Nx8vTermResize(Nx8vTerm* t, int cols, int rows) {
#ifdef _WIN32
  if (!t || !t->hpcon || !Nx8vConPty().resize) return;
  if (cols < 2) cols = 2;
  if (rows < 2) rows = 2;
  if (cols > 500) cols = 500;
  if (rows > 200) rows = 200;
  Nx8vConPty().resize(t->hpcon, COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)});
#else
  (void)t;
  (void)cols;
  (void)rows;
#endif
}

static int Nx8vTermJsonInt(const std::string& s, const char* key) {
  auto p = s.find(key);
  if (p == std::string::npos) return 0;
  p = s.find(':', p);
  if (p == std::string::npos) return 0;
  return std::atoi(s.c_str() + p + 1);
}

static bool Nx8vTermStart(HttpConn* conn, int cols, int rows, std::string* err);

static void Nx8vTermConsume(HttpConn* conn) {
  if (!conn || (!conn->term && !conn->term_wait)) return;
  for (;;) {
    const std::string& buf = conn->read_buf;
    if (buf.size() < 2) return;
    uint8_t b0 = (uint8_t)buf[0];
    uint8_t b1 = (uint8_t)buf[1];
    uint8_t opcode = b0 & 0x0F;
    bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7F;
    size_t offset = 2;
    if (len == 126) {
      if (buf.size() < 4) return;
      len = ((uint64_t)(uint8_t)buf[2] << 8) | (uint8_t)buf[3];
      offset = 4;
    } else if (len == 127) {
      if (buf.size() < 10) return;
      len = 0;
      for (int i = 0; i < 8; ++i) len = (len << 8) | (uint8_t)buf[2 + i];
      offset = 10;
    }
    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
      if (buf.size() < offset + 4) return;
      for (int i = 0; i < 4; ++i) mask[i] = (uint8_t)buf[offset + i];
      offset += 4;
    }
    if (buf.size() < offset + len) return;
    std::string payload = buf.substr(offset, (size_t)len);
    if (masked) {
      for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = (char)((uint8_t)payload[i] ^ mask[i % 4]);
    }
    conn->read_buf.erase(0, offset + (size_t)len);

    if (opcode == 0x8) {
      Nx8vTermClose(conn);
      WsSendFrame(conn, 0x8, "");
      if (!uv_is_closing((uv_handle_t*)&conn->handle)) {
        uv_close((uv_handle_t*)&conn->handle, [](uv_handle_t* h) {
          delete reinterpret_cast<HttpConn*>(h->data);
        });
      }
      return;
    }
    if (opcode == 0x9) {
      WsSendFrame(conn, 0xA, payload);
      continue;
    }
    if (opcode == 0xA) continue;
    if (opcode != 0x1 && opcode != 0x2 && opcode != 0x0) continue;
    if (!conn->term) {
      int cols = 0, rows = 0;
      if (!payload.empty() && payload[0] == '{') {
        cols = Nx8vTermJsonInt(payload, "cols");
        rows = Nx8vTermJsonInt(payload, "rows");
      }
      if (cols > 0 && rows > 0) {
        conn->term_wait = false;
        std::string err;
        if (!Nx8vTermStart(conn, cols, rows, &err)) {
          WsSendFrame(conn, 0x1, err.empty() ? "terminal failed to open" : err);
          WsSendFrame(conn, 0x8, "");
          if (!uv_is_closing((uv_handle_t*)&conn->handle)) {
            uv_close((uv_handle_t*)&conn->handle, [](uv_handle_t* h) {
              delete reinterpret_cast<HttpConn*>(h->data);
            });
          }
          return;
        }
        if (!conn->term_keys.empty()) {
          Nx8vTermWrite(static_cast<Nx8vTerm*>(conn->term), conn->term_keys);
          conn->term_keys.clear();
        }
      } else if (payload.empty() || payload[0] != '{') {
        conn->term_keys.append(payload);
      }
      continue;
    }
    auto* t = static_cast<Nx8vTerm*>(conn->term);
    if (!payload.empty() && payload[0] == '{') {
      int cols = Nx8vTermJsonInt(payload, "cols");
      int rows = Nx8vTermJsonInt(payload, "rows");
      if (cols > 0 && rows > 0) Nx8vTermResize(t, cols, rows);
      continue;
    }
    Nx8vTermWrite(t, payload);
  }
}

static bool Nx8vTermStart(HttpConn* conn, int cols, int rows, std::string* err) {
  if (!conn) return false;
  if (conn->term) return true;
#ifdef _WIN32
  if (!Nx8vConPty().ok()) {
    if (err) *err = "ConPTY is not available. Windows 10 1809 or newer is required.";
    return false;
  }
  if (cols < 2) cols = 80;
  if (rows < 2) rows = 24;
  auto* t = new Nx8vTerm();
  t->conn = conn;
  HANDLE in_read = nullptr, out_write = nullptr;
  if (!CreatePipe(&in_read, &t->input, nullptr, 0) ||
      !CreatePipe(&t->output, &out_write, nullptr, 0)) {
    if (in_read) CloseHandle(in_read);
    if (t->input) CloseHandle(t->input);
    if (t->output) CloseHandle(t->output);
    if (out_write) CloseHandle(out_write);
    delete t;
    if (err) *err = "Failed to create the terminal pipe.";
    return false;
  }
  HRESULT hr = Nx8vConPty().create(
      COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)},
      in_read, out_write, 0, &t->hpcon);
  CloseHandle(in_read);
  CloseHandle(out_write);
  if (FAILED(hr) || !t->hpcon) {
    if (t->input) CloseHandle(t->input);
    if (t->output) CloseHandle(t->output);
    delete t;
    if (err) *err = "CreatePseudoConsole failed.";
    return false;
  }

  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::vector<char> attr_buf(attr_size);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
  bool attrs_ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                  UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                            t->hpcon, sizeof(t->hpcon), nullptr, nullptr);
  if (!attrs_ok) {
    Nx8vConPty().close(t->hpcon);
    CloseHandle(t->input);
    CloseHandle(t->output);
    delete t;
    if (err) *err = "Failed to attach the pseudoconsole.";
    return false;
  }

  std::wstring shell = Nx8vTermShell();
  std::wstring cmd = Nx8vTermQuote(shell);
  std::wstring env = Nx8vTermEnv();
  // Same start folder as a new PowerShell window: the user profile.
  // The project folder is not applied here.
  std::wstring dir;
  wchar_t profile[MAX_PATH];
  DWORD profile_n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
  if (profile_n > 0 && profile_n < MAX_PATH) dir = profile;

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.lpAttributeList = attrs;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
  cmdline.push_back(0);
  // No CREATE_NEW_PROCESS_GROUP: that flag disables Ctrl+C for the
  // whole group, so a foreground program such as ping never stops.
  // One keypress is one byte (see sendCtrl). The shell and the agent
  // both see the same Ctrl+C a normal terminal would deliver.
  BOOL created = CreateProcessW(
      nullptr, cmdline.data(), nullptr, nullptr, FALSE,
      EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
      env.empty() ? nullptr : env.data(),
      dir.empty() ? nullptr : dir.c_str(), &si.StartupInfo, &pi);
  DeleteProcThreadAttributeList(attrs);
  if (!created) {
    Nx8vConPty().close(t->hpcon);
    CloseHandle(t->input);
    CloseHandle(t->output);
    delete t;
    if (err) *err = "Failed to start the shell.";
    return false;
  }
  CloseHandle(pi.hThread);
  t->process = pi.hProcess;
  conn->term = t;
  if (uv_async_init(conn->handle.loop, &t->async, Nx8vTermFlush) != 0) {
    conn->term = nullptr;
    TerminateProcess(t->process, 1);
    Nx8vConPty().close(t->hpcon);
    CloseHandle(t->input);
    CloseHandle(t->output);
    CloseHandle(t->process);
    delete t;
    if (err) *err = "Failed to attach the terminal to the loop.";
    return false;
  }
  t->async.data = t;
  HANDLE pipe = t->output;
  t->reader = std::thread([t, pipe] {
    char buf[8192];
    DWORD n = 0;
    while (::ReadFile(pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
      std::lock_guard<std::mutex> lock(t->mu);
      if (t->stop) break;
      t->pending.append(buf, buf + n);
      uv_async_send(&t->async);
    }
    {
      std::lock_guard<std::mutex> lock(t->mu);
      t->exited = true;
    }
    uv_async_send(&t->async);
  });
  std::printf("terminal: shell open\n");
  std::fflush(stdout);
  return true;
#else
  (void)cols;
  (void)rows;
  if (err) *err = "The terminal PTY in this version is available on Windows only.";
  return false;
#endif
}

static std::string Nx8vTerminalJs() {
  return R"NX8VJS(
class Nx8vTerminal extends HTMLElement {
  static get observedAttributes() { return ['height']; }
  constructor() {
    super();
    this.attachShadow({ mode: 'open' });
    this.shadowRoot.innerHTML =
      '<style>' +
      ':host{display:block;position:relative;width:100%;height:360px;min-height:144px;background:#0c0c0c;color:#ccc}' +
      '.view{position:absolute;left:0;right:0;top:0;bottom:0;box-sizing:border-box;overflow:hidden;padding:0;margin:0;' +
      'font:14px/18px Consolas,"Cascadia Mono",monospace;font-variant-ligatures:none;' +
      'line-height:0;white-space:pre;cursor:text;outline:none;user-select:text}' +
      '.view.scroll{overflow:auto;scrollbar-gutter:stable}' +
      '.row{display:flex;flex-wrap:nowrap;height:18px;min-height:18px;max-height:18px;' +
      'line-height:18px;overflow:hidden;margin:0;padding:0}' +
      '.row span{display:block;height:18px;line-height:18px;flex:0 0 auto;white-space:pre}' +
      '.cur{box-shadow:inset 0 0 0 1px #ccc}' +
      '</style>' +
      '<div class="view" tabindex="0"></div>';
    this.view = this.shadowRoot.querySelector('.view');
    this.cols = 80;
    this.rows = 24;
    this.x = 0;
    this.y = 0;
    this.fg = null;
    this.bg = null;
    this.bold = false;
    this.inverse = false;
    this.ansi = ['#0c0c0c','#c50f1f','#13a10e','#c19c00','#0037da','#881798','#3a96dd','#cccccc',
      '#767676','#e74856','#16c60c','#f9f1a5','#3b78ff','#b4009e','#61d6d6','#f2f2f2'];
    this.scrollTop = 0;
    this.scrollBot = 23;
    this.back = [];
    this.alt = false;
    this.saved = null;
    this.curSave = null;
    this.wrapPending = false;
    this.autoWrap = true;
    this.cursorVisible = true;
    this.appCursor = false;
    this.bracketPaste = false;
    this.mouse = 0;
    this.mouseSgr = false;
    this.kitty = 0;
    this.inter = '';
    this.lastCtrlAt = 0;
    this.lastCtrlByte = -1;
    this.cw = 8;
    this.sync = false;
    this.sized = false;
    this.st = 0;
    this.params = [];
    this.priv = false;
    this.osc = '';
    this.dec = new TextDecoder('utf-8');
    this.ws = null;
    this.blank = () => ({ c: ' ', fg: this.fg, bg: this.bg, b: this.bold ? 1 : 0 });
    this.grid = [];
    this.resetGrid();
  }
  applyHeight() {
    this.style.display = 'block';
    this.style.position = 'relative';
    this.style.width = '100%';
    this.style.boxSizing = 'border-box';
    const h = (this.getAttribute('height') || '').trim();
    this.style.height = h || '360px';
    this.style.minHeight = '144px';
  }
  attributeChangedCallback() {
    this.applyHeight();
    if (this.view) requestAnimationFrame(() => this.fit());
  }
  resetGrid() {
    this.grid = [];
    for (let r = 0; r < this.rows; r++) {
      const row = [];
      for (let c = 0; c < this.cols; c++) row.push(this.cell());
      this.grid.push(row);
    }
    this.scrollTop = 0;
    this.scrollBot = this.rows - 1;
  }
  hex(r, g, b) {
    const h = (n) => {
      n = Math.max(0, Math.min(255, n | 0));
      const s = n.toString(16);
      return s.length < 2 ? '0' + s : s;
    };
    return '#' + h(r) + h(g) + h(b);
  }
  resolve(color, foreground, bold) {
    if (color == null) return foreground ? '#cccccc' : '#0c0c0c';
    if (typeof color === 'string') return color;
    let n = color & 255;
    if (foreground && bold && n < 8) n += 8;
    if (n < 16) return this.ansi[n];
    if (n < 232) {
      const levels = [0, 95, 135, 175, 215, 255];
      const v = n - 16;
      return this.hex(levels[(v / 36) | 0], levels[((v / 6) | 0) % 6], levels[v % 6]);
    }
    const gray = 8 + (n - 232) * 10;
    return this.hex(gray, gray, gray);
  }
  connectedCallback() {
    this.applyHeight();
    this.view.addEventListener('keydown', (e) => this.onKey(e));
    this.view.addEventListener('wheel', (e) => this.onWheel(e), { passive: false });
    this.view.addEventListener('paste', (e) => {
      e.preventDefault();
      const t = (e.clipboardData && e.clipboardData.getData('text')) || '';
      if (!t) return;
      if (this.bracketPaste) this.send('\x1b[200~' + t + '\x1b[201~');
      else this.send(t);
    });
    this.resizeObserver = new ResizeObserver(() => this.fit());
    this.resizeObserver.observe(this);
    this.resizeObserver.observe(this.view);
    this.paint();
    this.open();
    this.view.focus();
  }
  disconnectedCallback() {
    if (this.resizeObserver) this.resizeObserver.disconnect();
    if (this.ws) this.ws.close();
  }
  fit() {
    const probe = document.createElement('span');
    probe.textContent = 'M';
    probe.style.font = '14px/18px Consolas,"Cascadia Mono",monospace';
    probe.style.visibility = 'hidden';
    probe.style.position = 'absolute';
    document.body.appendChild(probe);
    const w = probe.getBoundingClientRect().width || 8;
    const h = 18;
    this.cw = w;
    probe.remove();
    let cols = this.cols;
    let rows = this.rows;
    const boxW = Math.max(this.clientWidth || 0, this.view.clientWidth || 0);
    const boxH = Math.max(this.clientHeight || 0, this.view.clientHeight || 0);
    if (boxW >= 8 && boxH >= 18) {
      cols = Math.max(2, Math.floor(boxW / w));
      rows = Math.max(2, Math.floor(boxH / h));
    }
    const changed = cols !== this.cols || rows !== this.rows;
    if (changed) {
      this.resizeGrid(cols, rows);
      this.paint();
    }
    if (!this.ws || this.ws.readyState !== 1) return;
    if (this.sized && !changed) return;
    this.sized = true;
    this.ws.send(JSON.stringify({ cols: this.cols, rows: this.rows }));
    this.view.focus();
  }
  resizeGrid(cols, rows) {
    const old = this.grid;
    const next = [];
    for (let r = 0; r < rows; r++) {
      const row = [];
      const src = old[r] || [];
      for (let c = 0; c < cols; c++) row.push(src[c] ? src[c] : { c: ' ', fg: null, bg: null, b: 0, i: 0 });
      next.push(row);
    }
    this.grid = next;
    this.cols = cols;
    this.rows = rows;
    this.scrollTop = 0;
    this.scrollBot = rows - 1;
    if (this.x >= cols) this.x = cols - 1;
    if (this.y >= rows) this.y = rows - 1;
    this.wrapPending = false;
  }
  open() {
    const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
    const ws = new WebSocket(proto + location.host + '/__nx8v_terminal');
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    ws.onopen = () => this.fit();
    ws.onmessage = (ev) => {
      if (typeof ev.data === 'string') this.feedText(ev.data);
      else this.feedText(this.dec.decode(new Uint8Array(ev.data), { stream: true }));
    };
    ws.onclose = () => { if (!this.closedNote) { this.feedText('\r\n[terminal tertutup]\r\n'); this.closedNote = true; } };
  }
  send(text) {
    if (!this.ws || this.ws.readyState !== 1) return;
    this.ws.send(text);
  }
  sendCtrl(byte) {
    const now = Date.now();
    if (byte === this.lastCtrlByte && now - this.lastCtrlAt < 30) return;
    this.lastCtrlAt = now;
    this.lastCtrlByte = byte;
    if (this.kitty && byte >= 1 && byte <= 26)
      this.send('\x1b[' + (byte + 96) + ';5u');
    else
      this.send(String.fromCharCode(byte));
  }
  selectionText() {
    const read = (sel) => {
      if (!sel || sel.isCollapsed) return '';
      const node = sel.anchorNode;
      if (!node) return '';
      const inside = node === this || node === this.view
        || (this.view && this.view.contains(node))
        || (this.shadowRoot && this.shadowRoot.contains && this.shadowRoot.contains(node))
        || (this.contains && this.contains(node));
      if (!inside) return '';
      return String(sel).replace(/\u00a0/g, ' ');
    };
    if (this.shadowRoot && this.shadowRoot.getSelection) {
      const text = read(this.shadowRoot.getSelection());
      if (text) return text;
    }
    return typeof getSelection === 'function' ? read(getSelection()) : '';
  }
  copySelection() {
    const text = this.selectionText();
    if (!text) return false;
    const box = document.createElement('textarea');
    box.value = text;
    box.setAttribute('readonly', '');
    box.style.position = 'fixed';
    box.style.left = '-9999px';
    document.body.appendChild(box);
    box.select();
    try { document.execCommand('copy'); } catch (err) {}
    box.remove();
    if (navigator.clipboard && navigator.clipboard.writeText)
      navigator.clipboard.writeText(text).catch(() => {});
    const drop = (sel) => { if (sel && sel.removeAllRanges) sel.removeAllRanges(); };
    if (this.shadowRoot && this.shadowRoot.getSelection) drop(this.shadowRoot.getSelection());
    if (typeof getSelection === 'function') drop(getSelection());
    if (this.view) this.view.focus();
    return true;
  }
  pasteText(text) {
    if (!text) return;
    let t = String(text).replace(/\r\n/g, '\r').replace(/\n/g, '\r');
    if (this.bracketPaste) t = '\x1b[200~' + t + '\x1b[201~';
    this.send(t);
  }
  pasteClipboard() {
    const clip = navigator.clipboard;
    if (!clip || !clip.readText) return;
    clip.readText().then((t) => this.pasteText(t)).catch(() => {});
  }
  onKey(e) {
    if (e.key === 'Shift' || e.key === 'Control' || e.key === 'Alt' || e.key === 'Meta') return;
    if ((e.ctrlKey && !e.altKey && !e.metaKey && (e.key === 'v' || e.key === 'V'))
        || (!e.ctrlKey && !e.altKey && !e.metaKey && e.shiftKey && e.key === 'Insert')) {
      e.preventDefault();
      this.pasteClipboard();
      return;
    }
    if (e.ctrlKey && !e.altKey && !e.metaKey && (e.key === 'c' || e.key === 'C') && this.copySelection()) {
      e.preventDefault();
      return;
    }
    if (e.ctrlKey && e.shiftKey && (e.key === 'C' || e.key === 'c')) return;
    if (e.shiftKey && !e.ctrlKey && !e.altKey && !this.alt
        && (e.key === 'PageUp' || e.key === 'PageDown')) {
      e.preventDefault();
      const step = this.view.clientHeight || 216;
      this.view.scrollTop += e.key === 'PageUp' ? -step : step;
      return;
    }
    e.preventDefault();
    if (e.ctrlKey && !e.altKey && e.key.length === 1) {
      const code = e.key.toLowerCase().charCodeAt(0);
      if (code >= 97 && code <= 122) { this.sendCtrl(code & 31); return; }
    }
    const m = 1 + (e.shiftKey ? 1 : 0) + (e.altKey ? 2 : 0) + (e.ctrlKey ? 4 : 0);
    const cur = (ch) => (m > 1 ? '\x1b[1;' + m + ch : (this.appCursor ? '\x1bO' : '\x1b[') + ch);
    const tilde = (n) => '\x1b[' + n + (m > 1 ? ';' + m : '') + '~';
    const fn = (ch) => (m > 1 ? '\x1b[1;' + m + ch : '\x1bO' + ch);
    if (e.key === 'Enter') { this.send(e.altKey ? '\x1b\r' : '\r'); return; }
    if (e.key === 'Backspace') {
      this.send(e.ctrlKey ? '\x08' : (e.altKey ? '\x1b\x7f' : '\x7f'));
      return;
    }
    if (e.key === 'Tab') { this.send(e.shiftKey ? '\x1b[Z' : '\t'); return; }
    if (e.key === 'Escape') { this.send('\x1b'); return; }
    const named = {
      ArrowUp: cur('A'), ArrowDown: cur('B'), ArrowRight: cur('C'), ArrowLeft: cur('D'),
      Home: cur('H'), End: cur('F'),
      Insert: tilde(2), Delete: tilde(3), PageUp: tilde(5), PageDown: tilde(6),
      F1: fn('P'), F2: fn('Q'), F3: fn('R'), F4: fn('S'),
      F5: tilde(15), F6: tilde(17), F7: tilde(18), F8: tilde(19),
      F9: tilde(20), F10: tilde(21), F11: tilde(23), F12: tilde(24)
    };
    if (named[e.key]) { this.send(named[e.key]); return; }
    if (e.altKey && !e.ctrlKey && e.key.length === 1) { this.send('\x1b' + e.key); return; }
    if (e.key.length === 1 && !e.metaKey && !e.altKey) this.send(e.key);
  }
  cell() {
    return { c: ' ', fg: this.fg, bg: this.bg, b: this.bold ? 1 : 0, i: this.inverse ? 1 : 0 };
  }
  ensure() {
    while (this.grid.length < this.rows) {
      const row = [];
      for (let c = 0; c < this.cols; c++) row.push(this.cell());
      this.grid.push(row);
    }
    for (const row of this.grid) while (row.length < this.cols) row.push(this.cell());
  }
  scrollOne() {
    if (!this.alt && this.scrollTop === 0) {
      this.back.push(this.grid[0].map((c) => ({ c: c.c, fg: c.fg, bg: c.bg, b: c.b, i: c.i })));
      if (this.back.length > 2000) this.back.shift();
    }
    this.grid.splice(this.scrollTop, 1);
    const row = [];
    for (let c = 0; c < this.cols; c++) row.push(this.cell());
    this.grid.splice(this.scrollBot, 0, row);
  }
  index() {
    this.wrapPending = false;
    if (this.y === this.scrollBot) this.scrollOne();
    else if (this.y < this.rows - 1) this.y++;
  }
  reverseIndex() {
    this.wrapPending = false;
    if (this.y === this.scrollTop) {
      this.grid.splice(this.scrollTop, 0, this.grid.splice(this.scrollBot, 1)[0] || []);
      const row = this.grid[this.scrollTop];
      for (let c = 0; c < this.cols; c++) row[c] = this.cell();
    } else if (this.y > 0) this.y--;
  }
  saveCursor() {
    this.curSave = { x: this.x, y: this.y, wrap: this.wrapPending, fg: this.fg, bg: this.bg, b: this.bold, i: this.inverse };
  }
  restoreCursor() {
    const s = this.curSave;
    if (!s) return;
    this.x = s.x; this.y = s.y; this.wrapPending = s.wrap;
    this.fg = s.fg; this.bg = s.bg; this.bold = s.b; this.inverse = s.i;
  }
  enterAlt() {
    if (this.alt) return;
    this.saved = {
      g: this.grid.map((row) => row.map((c) => ({ c: c.c, fg: c.fg, bg: c.bg, b: c.b, i: c.i }))),
      x: this.x, y: this.y, wrap: this.wrapPending
    };
    this.alt = true;
    this.resetGrid();
    this.x = 0;
    this.y = 0;
    this.wrapPending = false;
  }
  leaveAlt() {
    if (!this.alt || !this.saved) { this.alt = false; return; }
    this.grid = this.saved.g;
    this.x = this.saved.x;
    this.y = this.saved.y;
    this.wrapPending = this.saved.wrap;
    this.alt = false;
    this.saved = null;
    if (this.grid.length !== this.rows || (this.grid[0] && this.grid[0].length !== this.cols))
      this.resizeGrid(this.cols, this.rows);
  }
  put(ch) {
    this.ensure();
    if (this.wrapPending && this.autoWrap) {
      this.wrapPending = false;
      this.x = 0;
      this.index();
    }
    if (this.y < 0) this.y = 0;
    if (this.y >= this.rows) this.y = this.rows - 1;
    if (this.x < 0) this.x = 0;
    if (this.x >= this.cols) this.x = this.cols - 1;
    const cell = this.grid[this.y][this.x];
    cell.c = ch; cell.fg = this.fg; cell.bg = this.bg; cell.b = this.bold ? 1 : 0; cell.i = this.inverse ? 1 : 0;
    if (this.x >= this.cols - 1) this.wrapPending = !!this.autoWrap;
    else this.x++;
  }
  feedText(text) {
    for (const ch of text) this.step(ch);
    if (!this.sync) this.paint();
  }
  step(ch) {
    if (this.st === 2) {
      if (ch === '\x07' || ch === '\x1b') { this.st = ch === '\x1b' ? 3 : 0; this.osc = ''; return; }
      this.osc += ch; return;
    }
    if (this.st === 3) { this.st = 0; return; }
    if (this.st === 1) {
      if (ch === '[') { this.st = 4; this.params = [[0]]; this.priv = false; this.inter = ''; return; }
      if (ch === ']') { this.st = 2; this.osc = ''; return; }
      if (ch === '7') { this.saveCursor(); this.st = 0; return; }
      if (ch === '8') { this.restoreCursor(); this.st = 0; return; }
      if (ch === 'D') { this.index(); this.st = 0; return; }
      if (ch === 'M') { this.reverseIndex(); this.st = 0; return; }
      if (ch === 'E') { this.x = 0; this.index(); this.st = 0; return; }
      this.st = 0;
      return;
    }
    if (this.st === 4) {
      if (ch === '?' || ch === '>' || ch === '!' || ch === '=') {
        this.inter = ch;
        if (ch === '?') this.priv = true;
        return;
      }
      if (ch >= ' ' && ch <= '/') return;
      if (ch >= '0' && ch <= '9') {
        const cur = this.params[this.params.length - 1];
        const last = cur.length - 1;
        cur[last] = cur[last] * 10 + (ch.charCodeAt(0) - 48);
        return;
      }
      if (ch === ';' || ch === ':') {
        if (ch === ':') this.params[this.params.length - 1].push(0);
        else this.params.push([0]);
        return;
      }
      if (ch >= '@' && ch <= '~') { this.csi(ch); this.st = 0; return; }
      if (ch === '\x1b') { this.st = 1; return; }
      return;
    }
    if (ch === '\x1b') { this.st = 1; return; }
    if (ch === '\r') { this.x = 0; this.wrapPending = false; return; }
    if (ch === '\n' || ch === '\v' || ch === '\f') { this.index(); return; }
    if (ch === '\b') {
      if (this.wrapPending) { this.wrapPending = false; return; }
      if (this.x > 0) this.x--;
      return;
    }
    if (ch === '\t') {
      this.wrapPending = false;
      this.x = Math.min(this.cols - 1, (Math.floor(this.x / 8) + 1) * 8);
      return;
    }
    if (ch === '\x07') return;
    if (ch >= ' ') this.put(ch);
  }
  csi(final) {
    const p = this.params;
    const n = (i, d) => {
      const g = p[i];
      const v = g ? g[0] : 0;
      return v > 0 ? v : d;
    };
    this.wrapPending = final === 'm' ? this.wrapPending : false;
    const clamp = () => {
      if (this.x < 0) this.x = 0;
      if (this.y < 0) this.y = 0;
      if (this.x >= this.cols) this.x = this.cols - 1;
      if (this.y >= this.rows) this.y = this.rows - 1;
    };
    if (final === 'A') this.y -= n(0, 1);
    else if (final === 'B') this.y += n(0, 1);
    else if (final === 'C') this.x += n(0, 1);
    else if (final === 'D') this.x -= n(0, 1);
    else if (final === 'E') { this.y += n(0, 1); this.x = 0; }
    else if (final === 'F') { this.y -= n(0, 1); this.x = 0; }
    else if (final === 'H' || final === 'f') {
      this.y = n(0, 1) - 1;
      this.x = n(1, 1) - 1;
    } else if (final === 'G' || final === '`') this.x = n(0, 1) - 1;
    else if (final === 'd') this.y = n(0, 1) - 1;
    else if (final === 'J') {
      this.ensure();
      const mode = (p[0] && p[0][0]) || 0;
      if (mode === 3) this.back = [];
      const from = mode === 0 ? this.y : 0;
      const to = mode === 1 ? this.y : this.rows - 1;
      for (let r = from; r <= to && r < this.grid.length; r++) for (let c = 0; c < this.cols; c++) {
        if (mode === 0 && r === this.y && c < this.x) continue;
        if (mode === 1 && r === this.y && c > this.x) continue;
        this.grid[r][c] = this.cell();
      }
    } else if (final === 'K' || final === 'X') {
      this.ensure();
      const mode = final === 'X' ? 0 : ((p[0] && p[0][0]) || 0);
      let a = this.x;
      let b = this.cols - 1;
      if (final === 'X') b = Math.min(this.cols - 1, this.x + n(0, 1) - 1);
      else if (mode === 1) { a = 0; b = this.x; }
      else if (mode === 2) { a = 0; b = this.cols - 1; }
      for (let c = a; c <= b && c < this.cols; c++) this.grid[this.y][c] = this.cell();
    } else if (final === 'L' || final === 'M') {
      this.ensure();
      const count = n(0, 1);
      for (let k = 0; k < count; k++) {
        if (this.y < this.scrollTop || this.y > this.scrollBot) break;
        const row = [];
        for (let c = 0; c < this.cols; c++) row.push(this.cell());
        if (final === 'L') {
          this.grid.splice(this.scrollBot, 1);
          this.grid.splice(this.y, 0, row);
        } else {
          this.grid.splice(this.y, 1);
          this.grid.splice(this.scrollBot, 0, row);
        }
      }
    } else if (final === 'r' && !this.priv) {
      let top = n(0, 1) - 1;
      let bot = (p[1] ? n(1, this.rows) : this.rows) - 1;
      if (top < 0) top = 0;
      if (bot >= this.rows) bot = this.rows - 1;
      if (top >= bot) { this.scrollTop = 0; this.scrollBot = this.rows - 1; }
      else { this.scrollTop = top; this.scrollBot = bot; }
      this.x = 0;
      this.y = 0;
    } else if (final === 's') this.saveCursor();
    else if (final === 'u' && (this.inter === '>' || this.inter === '=')) {
      this.kitty = (p[0] && p[0][0]) || 0;
    } else if (final === 'u' && this.inter === '?') {
      this.send('\x1b[?' + (this.kitty || 0) + 'u');
    } else if (final === 'u') this.restoreCursor();
    else if (final === 'm') this.sgr(p);
    else if (final === 'n') {
      const mode = (p[0] && p[0][0]) || 0;
      if (mode === 6) this.send('\x1b[' + (this.y + 1) + ';' + (this.x + 1) + 'R');
      else if (mode === 5) this.send('\x1b[0n');
    } else if (final === 'c' && !this.priv) this.send('\x1b[?1;2c');
    else if ((final === 'h' || final === 'l')) this.setMode(final === 'h');
    if (final !== 's' && final !== 'u') clamp();
  }
  setMode(on) {
    for (const g of this.params) {
      const v = g[0] || 0;
      if (!this.priv) continue;
      if (v === 7) this.autoWrap = on;
      else if (v === 25) this.cursorVisible = on;
      else if (v === 1) this.appCursor = on;
      else if (v === 2004) this.bracketPaste = on;
      else if (v === 1000 || v === 1002 || v === 1003) this.mouse = on ? v : 0;
      else if (v === 1006) this.mouseSgr = on;
      else if (v === 2026) {
        this.sync = on;
        if (!on) this.paint();
      } else if (v === 1049 || v === 1047 || v === 47) {
        if (on) this.enterAlt();
        else this.leaveAlt();
      } else if (v === 1048) {
        if (on) this.saveCursor();
        else this.restoreCursor();
      }
    }
  }
  sgr(groups) {
    if (!groups.length) groups = [[0]];
    for (let i = 0; i < groups.length; i++) {
      const g = groups[i];
      const v = g[0] || 0;
      const slot = v === 38 ? 'fg' : 'bg';
      if (v === 0) { this.fg = null; this.bg = null; this.bold = false; this.inverse = false; }
      else if (v === 1) this.bold = true;
      else if (v === 22) this.bold = false;
      else if (v === 7) this.inverse = true;
      else if (v === 27) this.inverse = false;
      else if (v >= 30 && v <= 37) this.fg = v - 30;
      else if (v >= 90 && v <= 97) this.fg = v - 90 + 8;
      else if (v >= 40 && v <= 47) this.bg = v - 40;
      else if (v >= 100 && v <= 107) this.bg = v - 100 + 8;
      else if (v === 39) this.fg = null;
      else if (v === 49) this.bg = null;
      else if (v === 38 || v === 48) {
        if (g.length > 1) {
          if (g[1] === 5 && g.length > 2) this[slot] = g[2] & 255;
          else if (g[1] === 2 && g.length > 4) {
            const base = g.length >= 6 ? 3 : 2;
            this[slot] = this.hex(g[base], g[base + 1], g[base + 2]);
          }
        } else if (groups[i + 1] && groups[i + 1][0] === 5) {
          this[slot] = (groups[i + 2] ? groups[i + 2][0] : 0) & 255;
          i += 2;
        } else if (groups[i + 1] && groups[i + 1][0] === 2) {
          const r = groups[i + 2] ? groups[i + 2][0] : 0;
          const gc = groups[i + 3] ? groups[i + 3][0] : 0;
          const b = groups[i + 4] ? groups[i + 4][0] : 0;
          this[slot] = this.hex(r, gc, b);
          i += 4;
        }
      }
    }
  }
  esc(s) {
    return s.replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
  }
  rowHtml(row, cursor) {
    let html = '';
    let i = 0;
    while (i < row.length) {
      const cell = row[i];
      let j = i + 1;
      while (j < row.length && row[j].fg === cell.fg && row[j].bg === cell.bg && row[j].b === cell.b && row[j].i === cell.i && j !== cursor) j++;
      if (cursor === i) j = i + 1;
      let text = '';
      for (let k = i; k < j; k++) text += row[k].c;
      let fg = this.resolve(cell.fg, true, !!cell.b);
      let bg = this.resolve(cell.bg, false, false);
      if (cell.i) { const swap = fg; fg = bg; bg = swap; }
      const style = 'color:' + fg + ';background:' + bg;
      const cls = cursor === i ? ' class="cur"' : '';
      html += '<span' + cls + ' style="' + style + '">' + this.esc(text) + '</span>';
      i = j;
    }
    return html;
  }
  cellFromEvent(e) {
    const rect = this.view.getBoundingClientRect();
    let x = Math.floor((e.clientX - rect.left) / (this.cw || 8)) + 1;
    let y = Math.floor((e.clientY - rect.top) / 18) + 1;
    if (!this.alt) y -= Math.min(this.back.length, 500);
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (x > this.cols) x = this.cols;
    if (y > this.rows) y = this.rows;
    return { x: x, y: y };
  }
  onWheel(e) {
    e.preventDefault();
    if (this.mouse) {
      const pos = this.cellFromEvent(e);
      const btn = e.deltaY < 0 ? 64 : 65;
      this.send('\x1b[<' + btn + ';' + pos.x + ';' + pos.y + 'M');
      return;
    }
    this.view.scrollTop += e.deltaY;
  }
  paint() {
    const near = !this.alt && this.view.scrollHeight - this.view.scrollTop - this.view.clientHeight < 48;
    const lines = [];
    const push = (row, cur) => {
      const bg = this.resolve(row && row[0] ? row[0].bg : null, false, false);
      lines.push('<div class="row" style="background:' + bg + '">' + this.rowHtml(row || [], cur) + '</div>');
    };
    if (!this.alt) {
      for (const row of this.back.slice(-500)) push(row, -1);
    }
    for (let r = 0; r < this.rows; r++) {
      const cur = this.cursorVisible && r === this.y ? this.x : -1;
      push(this.grid[r] || [], cur);
    }
    this.view.classList.toggle('scroll', !this.alt);
    this.view.innerHTML = lines.join('');
    if (this.alt) this.view.scrollTop = 0;
    else if (near) this.view.scrollTop = this.view.scrollHeight;
  }
}
if (!customElements.get('nx8v-terminal')) customElements.define('nx8v-terminal', Nx8vTerminal);
)NX8VJS";
}
