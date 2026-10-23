#pragma once
// nx8v/src/bindings/init.h
// `nx8v init` — scaffolding wizard (like `npm init`).
//
// Interactive prompts: name, description, port. Generates in the current
// directory:
//   project.json   — package metadata
//   server.js      — starter HTTP server on the chosen port
//   public\index.html — starter page served by server.js
//   public\about.html
//   public\assets\css\style.css
//   public\assets\js\app.js
//   public\assets\fonts\
//   public\assets\images\32x32.png — default NX8V mark
//   modules\                       — empty; names go in dependencies
//     desktop.icon writes ./images/32x32.png; the file lives in public\assets\images

#include <v8.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "manifest.h"
#include "brand_icon.h"

#include "../utils.h"

namespace fs = std::filesystem;

namespace nx8v {
namespace bindings {

// ---------------------------------------------------------------------------
// Prompt helper: returns trimmed input, or default when empty.
// ---------------------------------------------------------------------------
static std::string Prompt(const std::string& label, const std::string& def) {
  std::cout << "  " << label;
  if (!def.empty()) std::cout << " [" << def << "]";
  std::cout << ": " << std::flush;

  std::string line;
  if (!std::getline(std::cin, line)) return def;
  while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
    line.pop_back();
  // trim leading whitespace
  size_t start = line.find_first_not_of(" \t");
  if (start == std::string::npos) return def;
  line = line.substr(start);
  return line.empty() ? def : line;
}

// ---------------------------------------------------------------------------
// Simple JSON string escape
// ---------------------------------------------------------------------------
static std::string JsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Write file, aborting init on failure
// ---------------------------------------------------------------------------
static bool WriteInitFile(const fs::path& path, const std::string& content) {
  std::error_code ec;
  fs::path parent = path.parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::cout << "  ERROR: cannot write " << path.string() << '\n';
    return false;
  }
  out << content;
  return out.good();
}

// ---------------------------------------------------------------------------
// Run the init wizard
// ---------------------------------------------------------------------------
inline int RunInit() {
  std::cout << "\nNX8V project wizard\n";
  std::cout << "Press Enter to accept the default.\n\n";

  std::error_code ec;
  fs::path cwd = fs::current_path(ec);
  std::string dir_name = cwd.filename().string();
  if (dir_name.empty()) dir_name = "my-app";

  std::string name    = Prompt("name",        dir_name);
  std::string desc    = Prompt("description", "An NX8V application");
  std::string port_in = Prompt("port",        "3000");

  int port = 3000;
  {
    // stoi is unavailable (exceptions disabled) — manual parse
    const char* p = port_in.c_str();
    while (*p == ' ') ++p;
    if (*p) {
      char* end = nullptr;
      long v = std::strtol(p, &end, 10);
      if (end && *end == '\0' && v > 0 && v <= 65535) port = (int)v;
    }
  }
  if (port <= 0 || port > 65535) port = 3000;
  std::string port_str = std::to_string(port);
  std::string slug = name;
  for (char& c : slug) {
    if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '"' ||
        c == '*' || c == '?' || c == '<' || c == '>' || c == '|')
      c = '-';
  }
  if (slug.empty()) slug = "app";

  // ------------------------------------------------------------------
  // project.json
  // ------------------------------------------------------------------
  std::ostringstream pj;
  pj << "{\n"
     << "  \"name\": \""        << JsonEscape(name)    << "\",\n"
     << "  \"version\": \"1.0.0\",\n"
     << "  \"description\": \"" << JsonEscape(desc)    << "\",\n"
     << "  \"main\": \"server.js\",\n"
     << "  \"nx8v\": {\n"
     << "    \"port\": "        << port_str             << ",\n"
     << "    \"desktop\": {\n"
     << "      \"icon\": \"./images/32x32.png\",\n"
     << "      \"state\": \"normal\",\n"
     << "      \"width\": 800,\n"
     << "      \"height\": 560,\n"
     << "      \"minWidth\": 640,\n"
     << "      \"minHeight\": 400,\n"
     << "      \"center\": true,\n"
     << "      \"maximizable\": true,\n"
     << "      \"resizable\": true,\n"
     << "      \"titlebar\": true,\n"
     << "      \"devtools\": false,\n"
     << "      \"singleInstance\": true,\n"
     << "      \"productName\": \"" << JsonEscape(name) << "\",\n"
     << "      \"shortcutName\": \"" << JsonEscape(name) << "\",\n"
     << "      \"artifactName\": \"" << JsonEscape(slug) << "-Setup-${version}.exe\"\n"
     << "    }\n"
     << "  },\n"
     << "  \"dependencies\": {},\n"
     << "  \"repository\": {\n"
     << "    \"type\": \"git\",\n"
     << "    \"url\": \"" << kNx8vDepsRepository << "\"\n"
     << "  },\n"
     << "  \"scripts\": {\n"
     << "    \"start\": \"nx8v start\",\n"
     << "    \"dev\": \"nx8v dev\",\n"
     << "    \"stop\": \"nx8v stop\"\n"
     << "  }\n"
     << "}\n";

  if (!WriteInitFile(cwd / "project.json", pj.str())) return 1;

  // ------------------------------------------------------------------
  // server.js
  // ------------------------------------------------------------------
  std::ostringstream sj;
  sj << "// " << name << " - generated by nx8v init\n"
     << "const http = require('http');\n"
     << "\n"
     << "// Port: env NX8V_PORT (set by `nx8v start`) > PORT env > fallback\n"
     << "// HTML, CSS, JS, images, and fonts in public/ are served by NX8V.\n"
     << "const PORT = Number(process.env.NX8V_PORT) || Number(process.env.PORT) || " << port_str << ";\n"
     << "\n"
     << "const server = http.createServer((req, res) => {\n"
     << "  const clean = req.url.split('?')[0];\n"
     << "  console.log(req.method, req.url);\n"
     << "\n"
     << "  if (clean === '/api/hello') {\n"
     << "    res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });\n"
     << "    res.end(JSON.stringify({ message: 'Halo dari " << JsonEscape(name) << "!' }));\n"
     << "    return;\n"
     << "  }\n"
     << "\n"
     << "  res.writeHead(404, { 'Content-Type': 'text/plain' });\n"
     << "  res.end('404 Not Found: ' + clean);\n"
     << "});\n"
     << "\n"
     << "server.listen(PORT, () => {\n"
     << "  console.log('" << JsonEscape(name) << " running at http://localhost:' + PORT);\n"
     << "});\n";

  if (!WriteInitFile(cwd / "server.js", sj.str())) return 1;

  // ------------------------------------------------------------------
  // public/index.html
  // ------------------------------------------------------------------
  std::ostringstream hj;
  hj << "<!DOCTYPE html>\n"
     << "<html lang=\"id\">\n"
     << "<head>\n"
     << "  <meta charset=\"utf-8\">\n"
     << "  <title>" << name << "</title>\n"
     << "  <link rel=\"stylesheet\" href=\"/css/style.css\">\n"
     << "  <link rel=\"icon\" href=\"/images/32x32.png\">\n"
     << "  <style>\n"
     << "    body { font-family: system-ui, sans-serif; max-width: 40rem;\n"
     << "           margin: 4rem auto; padding: 0 1rem; }\n"
     << "    h1 { color: #3b82f6; }\n"
     << "    nav a { margin-right: 1rem; }\n"
     << "  </style>\n"
     << "</head>\n"
     << "<body>\n"
     << "  <nav><a href=\"/\">Home</a><a href=\"/about\">About</a></nav>\n"
     << "  <h1>" << name << "</h1>\n"
     << "  <p>" << desc << "</p>\n"
     << "  <p>Disajikan oleh <strong>NX8V</strong> di port " << port_str << ".</p>\n"
     << "  <p>API: <a href=\"/api/hello\">/api/hello</a></p>\n"
     << "  <script src=\"/js/app.js\"></script>\n"
     << "</body>\n"
     << "</html>\n";

  if (!WriteInitFile(cwd / "public" / "index.html", hj.str())) return 1;

  // ------------------------------------------------------------------
  // public/about.html — demo of smart file routing
  // ------------------------------------------------------------------
  std::ostringstream aj;
  aj << "<!DOCTYPE html>\n"
     << "<html lang=\"id\">\n"
     << "<head>\n"
     << "  <meta charset=\"utf-8\">\n"
     << "  <title>About - " << name << "</title>\n"
     << "  <link rel=\"stylesheet\" href=\"/css/style.css\">\n"
     << "  <link rel=\"icon\" href=\"/images/32x32.png\">\n"
     << "  <style>\n"
     << "    body { font-family: system-ui, sans-serif; max-width: 40rem;\n"
     << "           margin: 4rem auto; padding: 0 1rem; }\n"
     << "    h1 { color: #3b82f6; }\n"
     << "    nav a { margin-right: 1rem; }\n"
     << "  </style>\n"
     << "</head>\n"
     << "<body>\n"
     << "  <nav><a href=\"/\">Home</a><a href=\"/about\">About</a></nav>\n"
     << "  <h1>Halaman About</h1>\n"
     << "  <p>Halaman ini membuktikan <strong>smart file routing</strong>:</p>\n"
     << "  <ul>\n"
     << "    <li><code>/about</code> &rarr; about.html</li>\n"
     << "    <li><code>/about/data/u</code> &rarr; juga about.html\n"
     << "        (segmen <code>data/u</code> adalah data; lihat log server)</li>\n"
     << "  </ul>\n"
     << "  <p>Segmen URL bisa dibaca di server dari <code>req.url</code>.</p>\n"
     << "  <p><a href=\"/\">&larr; Kembali ke Home</a></p>\n"
     << "  <script src=\"/js/app.js\"></script>\n"
     << "</body>\n"
     << "</html>\n";

  if (!WriteInitFile(cwd / "public" / "about.html", aj.str())) return 1;

  if (!WriteInitFile(cwd / "public" / "assets" / "css" / "style.css",
                     "/* Styles for this project. */\n")) return 1;
  if (!WriteInitFile(cwd / "public" / "assets" / "js" / "app.js",
                     "// Scripts for this project.\n")) return 1;
  fs::create_directories(cwd / "public" / "assets" / "fonts", ec);
  fs::create_directories(cwd / "modules", ec);
  {
    std::string icon(reinterpret_cast<const char*>(kNx8vBrandIcon32), kNx8vBrandIcon32Size);
    if (!WriteInitFile(cwd / "public" / "assets" / "images" / "32x32.png", icon)) return 1;
  }

  std::cout << "\nProject '" << name << "' created:\n";
  std::cout << "  project.json\n";
  std::cout << "  server.js\n";
  std::cout << "  public\\index.html\n";
  std::cout << "  public\\about.html\n";
  std::cout << "  public\\assets\\css\\style.css\n";
  std::cout << "  public\\assets\\js\\app.js\n";
  std::cout << "  public\\assets\\fonts\\\n";
  std::cout << "  public\\assets\\images\\32x32.png\n";
  std::cout << "  modules\\\n";
  std::cout << "\nRun it:\n";
  std::cout << "  nx8v dev     (development, auto-restart)\n";
  std::cout << "  nx8v start   (production)\n";
  std::cout << "  nx8v stop\n";
  std::cout << "  nx8v package (Windows setup + Linux tar.gz)\n\n";
  return 0;
}

// ---------------------------------------------------------------------------
// `nx8v stop` — find the nx8v.exe process listening on the project's port
// and terminate it.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

inline int RunStop(int argc, char* argv[]) {
#ifdef _WIN32
  std::error_code ec;
  fs::path cwd = fs::current_path(ec);
  fs::path manifest_path = cwd / "project.json";

  int port = 0;
  // --port=N override
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--port=", 0) == 0) {
      long v = std::strtol(a.c_str() + 7, nullptr, 10);
      if (v > 0 && v <= 65535) port = (int)v;
    }
  }

  if (port == 0) {
    if (!fs::exists(manifest_path, ec)) {
      std::cout << "Error: project.json not found in " << cwd.string() << '\n'
                << "Run from the project directory, or use --port=N.\n";
      return 1;
    }
    std::string manifest = ReadManifestFile(manifest_path);
    JsonFindNumber(manifest, "port", port);
  }

  if (port <= 0) {
    std::cout << "Error: no port found (project.json 'nx8v.port' or --port=N).\n";
    return 1;
  }

  // 1. Find the listening PID via GetExtendedTcpTable
  DWORD pid = 0;
  DWORD size = 0;
  GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                      TCP_TABLE_OWNER_PID_LISTENER, 0);
  std::vector<char> buf(size);
  if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET,
                          TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
      const auto& row = table->table[i];
      if (ntohs((u_short)row.dwLocalPort) == (u_short)port) {
        pid = row.dwOwningPid;
        break;
      }
    }
  }

  if (pid == 0) {
    std::cout << "No process is listening on port " << port << ".\n";
    return 0;
  }

  // 2. Verify it is an nx8v.exe process
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  bool is_nx8v = false;
  if (snap != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
      do {
        if (pe.th32ProcessID == pid) {
          std::wstring name(pe.szExeFile);
          for (auto& c : name) c = (wchar_t)towlower(c);
          is_nx8v = (name == L"nx8v.exe");
          break;
        }
      } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
  }

  if (!is_nx8v) {
    std::cout << "Port " << port << " is used by PID " << pid
              << " which is NOT nx8v.exe — refusing to kill.\n";
    return 1;
  }

  // 3. Kill it
  HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
  if (!h) {
    std::cout << "Error: cannot open PID " << pid << " (access denied?).\n";
    return 1;
  }
  if (TerminateProcess(h, 0)) {
    std::cout << "Stopped nx8v server (PID " << pid << ", port " << port << ").\n";
    CloseHandle(h);
    return 0;
  }
  std::cout << "Error: TerminateProcess failed for PID " << pid << ".\n";
  CloseHandle(h);
  return 1;
#else
  (void)argc; (void)argv;
  std::cout << "nx8v stop is only supported on Windows.\n";
  return 1;
#endif
}

// ---------------------------------------------------------------------------
// Unsafe ports blocked by Chromium (ERR_UNSAFE_PORT). Warn + suggest.
// ---------------------------------------------------------------------------
static bool IsUnsafePort(int port) { return Nx8vIsUnsafePort(port); }

// ---------------------------------------------------------------------------
// `nx8v start` — run the project in the current directory.
// Reads project.json (main entry, default port) and executes it.
// Args after `start` override: nx8v start [script.js] [--port=N]
// ---------------------------------------------------------------------------
inline int RunStart(int argc, char* argv[]) {
  std::error_code ec;
  fs::path cwd = fs::current_path(ec);
  fs::path manifest_path = cwd / "project.json";

  if (!fs::exists(manifest_path, ec)) {
    std::cout << "Error: project.json not found in " << cwd.string() << '\n'
              << "Run `nx8v init` first.\n";
    return 1;
  }

  std::string manifest = ReadManifestFile(manifest_path);
  std::string main_file = "server.js";
  int default_port = 0;
  JsonFindString(manifest, "main", main_file);
  JsonFindNumber(manifest, "port", default_port);

  // CLI overrides: first non-flag arg = script, --port=N
  std::string script = main_file;
  int port = default_port;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--port=", 0) == 0) {
      long v = std::strtol(a.c_str() + 7, nullptr, 10);
      if (v > 0 && v <= 65535) port = (int)v;
    } else if (a.rfind("--", 0) != 0) {
      script = a;
    }
  }

  fs::path script_path = cwd / script;
  if (!fs::exists(script_path, ec)) {
    std::cout << "Error: '" << script << "' not found in project.\n";
    return 1;
  }

  if (port > 0 && IsUnsafePort(port)) {
    std::cout << "Warning: port " << port
              << " is blocked by browsers (ERR_UNSAFE_PORT).\n"
              << "Suggest changing 'nx8v.port' in project.json, or open the "
              << "site via 127.0.0.1 with a different browser/port.\n";
  }

  // Expose the resolved port so server scripts can pick it up:
  //   process.env.NX8V_PORT
  if (port > 0) {
    std::string port_str = std::to_string(port);
#ifdef _WIN32
    _putenv_s("NX8V_PORT", port_str.c_str());
#else
    setenv("NX8V_PORT", port_str.c_str(), 1);
#endif
  }

  // Hand off to the normal script execution path.
  static char script_buf[1024];
  strncpy(script_buf, script_path.string().c_str(), sizeof(script_buf) - 1);
  script_buf[sizeof(script_buf) - 1] = '\0';
  argv[1] = script_buf;
  return -1;  // signal main(): continue with normal V8 boot
}

}} // nx8v::bindings
