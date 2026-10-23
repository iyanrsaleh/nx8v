#pragma once
// nx8v/src/bindings/package.h
// `nx8v package [--out=dist] [--name=app]` — build a distributable folder:
//
//   dist/<name>/
//     <name>.exe          # nx8v.exe plus the app packed inside it
//     WebView2Loader.dll  # only when the project uses nx8v window
//     start.cmd
//     README.txt
//
// Source files are not copied loose. A double-click unpacks them into a
// private cache and runs from there.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "uxtheme.lib")
#endif

#include "manifest.h"
#include "icon.h"

namespace fs = std::filesystem;

namespace nx8v {
namespace bindings {

// Directories never copied into a package.
static bool PackageSkipDir(const std::string& name) {
  return name == "node_modules" || name == ".git" || name == "build" ||
         name == "dist" || name == ".nx8v" || name == "deps" ||
         name == ".vscode" || name == "out";
}

// Files never copied (build artefacts / caches).
static bool PackageSkipFile(const std::string& name) {
  return name == "nx8v.exe" || name == "WebView2Loader.dll" ||
         name == ".nx8v-pid" || name == "package.ps1" || name == "nx8v.log";
}

inline std::string PackageExePath() {
#ifdef _WIN32
  char buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
  if (n > 0) return std::string(buf, n);
#endif
  return "";
}

inline bool PackageCopyTree(const fs::path& from, const fs::path& to,
                            std::vector<std::string>& copied,
                            std::vector<std::string>& skipped) {
  std::error_code ec;
  fs::create_directories(to, ec);
  if (ec) return false;

  fs::recursive_directory_iterator it(
      from, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;
  if (ec) return false;

  while (it != end) {
    if (ec) { it.increment(ec); continue; }
    const fs::path p = it->path();
    fs::path rel = fs::relative(p, from, ec);
    if (ec) { it.increment(ec); continue; }

    std::string name = p.filename().string();

    if (it->is_directory(ec)) {
      if (PackageSkipDir(name)) {
        it.disable_recursion_pending();
        skipped.push_back(rel.string() + "/");
      }
      it.increment(ec);
      continue;
    }

    if (PackageSkipFile(name)) {
      skipped.push_back(rel.string());
      it.increment(ec);
      continue;
    }

    fs::path dest = to / rel;
    std::error_code ec2;
    fs::create_directories(dest.parent_path(), ec2);
    fs::copy_file(p, dest, fs::copy_options::overwrite_existing, ec2);
    if (!ec2) copied.push_back(rel.string());
    else skipped.push_back(rel.string() + " (gagal)");

    it.increment(ec);
  }
  return true;
}

static const char kNx8vPackMagic[8] = {'N','X','8','V','P','K','0','1'};
static const char kNx8vSetupMagic[8] = {'N','X','8','V','S','E','T','1'};

static bool PackRead32(std::istream& in, uint32_t& v) {
  unsigned char b[4];
  in.read(reinterpret_cast<char*>(b), 4);
  if (in.gcount() != 4) return false;
  v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
      ((uint32_t)b[3] << 24);
  return true;
}

static bool PackRead64(std::istream& in, uint64_t& v) {
  unsigned char b[8];
  in.read(reinterpret_cast<char*>(b), 8);
  if (in.gcount() != 8) return false;
  v = 0;
  for (int i = 0; i < 8; ++i) v |= (uint64_t)b[i] << (8 * i);
  return true;
}

static bool PackPathSafe(const std::string& rel) {
  if (rel.empty() || rel[0] == '/' || rel[0] == '\\' || rel.find(':') != std::string::npos)
    return false;
  std::string cur;
  for (size_t i = 0; i <= rel.size(); ++i) {
    char c = i < rel.size() ? rel[i] : '/';
    if (c == '/' || c == '\\') {
      if (cur == ".." || cur == ".") return false;
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  return true;
}

// Append the project onto the exe. The folder that is shipped has no raw sources.
inline int PackageAppendTree(const fs::path& exe, const fs::path& root, const char* magic,
                             const std::string& extra_name, const std::string& extra_body,
                             bool keep_loader = false) {
  int count = 0;
  uint64_t written = 0;
  char buf[65536];
#ifdef _WIN32
  HANDLE hout = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 25 && hout == INVALID_HANDLE_VALUE; ++attempt) {
    hout = CreateFileW(exe.wstring().c_str(), FILE_APPEND_DATA,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hout == INVALID_HANDLE_VALUE) Sleep(100);
  }
  if (hout == INVALID_HANDLE_VALUE) {
    std::printf("  ! berkas exe terkunci (%lu)\n", GetLastError());
    return -1;
  }
  auto emit = [&](const void* data, size_t n) -> bool {
    auto* p = static_cast<const char*>(data);
    while (n) {
      DWORD chunk = n > (1u << 20) ? (1u << 20) : (DWORD)n;
      DWORD got = 0;
      if (!::WriteFile(hout, p, chunk, &got, nullptr) || got != chunk) return false;
      p += got;
      n -= got;
    }
    return true;
  };
#else
  std::ofstream hout(exe, std::ios::binary | std::ios::app);
  if (!hout) {
    std::printf("  ! berkas exe tidak bisa dibuka\n");
    return -1;
  }
  auto emit = [&](const void* data, size_t n) -> bool {
    hout.write(static_cast<const char*>(data), (std::streamsize)n);
    return (bool)hout;
  };
#endif
  std::error_code ec;
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;
  if (ec) {
    std::printf("  ! folder %s: %s (%d)\n", root.string().c_str(), ec.message().c_str(), ec.value());
#ifdef _WIN32
    CloseHandle(hout);
#endif
    return -1;
  }
  auto write_one = [&](const std::string& rels, std::istream& in, uint64_t sz) -> bool {
    if (!PackPathSafe(rels) || rels.size() > 1024) return true;
    unsigned char hdr[12];
    uint32_t plen = (uint32_t)rels.size();
    for (int i = 0; i < 4; ++i) hdr[i] = (unsigned char)((plen >> (8 * i)) & 0xff);
    for (int i = 0; i < 8; ++i) hdr[4 + i] = (unsigned char)((sz >> (8 * i)) & 0xff);
    if (!emit(hdr, 4) || !emit(rels.data(), rels.size()) || !emit(hdr + 4, 8)) return false;
    written += 4 + rels.size() + 8;
    uint64_t left = sz;
    while (left) {
      size_t n = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
      in.read(buf, (std::streamsize)n);
      if (in.gcount() != (std::streamsize)n) return false;
      if (!emit(buf, n)) return false;
      left -= n;
      written += n;
    }
    return true;
  };
  while (it != end) {
    if (ec) { it.increment(ec); continue; }
    const fs::path p = it->path();
    std::string name = p.filename().string();
    if (it->is_directory(ec)) {
      if (PackageSkipDir(name)) it.disable_recursion_pending();
      it.increment(ec);
      continue;
    }
    if (PackageSkipFile(name) && !(keep_loader && name == "WebView2Loader.dll")) {
      it.increment(ec);
      continue;
    }
    fs::path rel = fs::relative(p, root, ec);
    if (ec) { it.increment(ec); continue; }
    std::string rels = rel.generic_string();
    std::ifstream in(p, std::ios::binary);
    if (!in) { it.increment(ec); continue; }
    in.seekg(0, std::ios::end);
    auto sz = in.tellg();
    in.seekg(0);
    if (sz < 0) { it.increment(ec); continue; }
    if (!write_one(rels, in, (uint64_t)sz)) {
      std::printf("  ! gagal menulis %s\n", rels.c_str());
      return -1;
    }
    ++count;
    it.increment(ec);
  }
  if (!extra_name.empty()) {
    std::istringstream in(extra_body);
    if (!write_one(extra_name, in, (uint64_t)extra_body.size())) return -1;
    ++count;
  }
  unsigned char tail[8];
  for (int i = 0; i < 8; ++i) tail[i] = (unsigned char)((written >> (8 * i)) & 0xff);
  if (!emit(tail, 8) || !emit(magic, 8)) {
#ifdef _WIN32
    CloseHandle(hout);
#endif
    return -1;
  }
#ifdef _WIN32
  CloseHandle(hout);
#endif
  return count;
}

inline int PackageAppendPayload(const fs::path& exe, const fs::path& root) {
  return PackageAppendTree(exe, root, kNx8vPackMagic, "", "");
}

#ifdef _WIN32
// If this exe carries a packed app, unpack it to a private cache and chdir there.
inline bool Nx8vPreparePackedApp() {
  wchar_t mod[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, mod, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  fs::path exe(mod);
  std::ifstream in(exe, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  auto total = in.tellg();
  if (total < 16) return false;
  in.seekg((std::streamoff)total - 16);
  uint64_t payload = 0;
  if (!PackRead64(in, payload)) return false;
  char magic[8];
  in.read(magic, 8);
  if (in.gcount() != 8 || std::memcmp(magic, kNx8vPackMagic, 8) != 0) return false;
  if (payload > (uint64_t)total - 16) return false;

  std::error_code ec;
  auto stamp_sz = fs::file_size(exe, ec);
  auto stamp_tm = fs::last_write_time(exe, ec).time_since_epoch().count();
  wchar_t local[MAX_PATH];
  DWORD ln = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  fs::path base = ln > 0 && ln < MAX_PATH ? fs::path(local) / "nx8v" / "apps"
                                          : fs::temp_directory_path(ec) / "nx8v-apps";
  unsigned hash = 2166136261u;
  for (wchar_t c : exe.wstring()) {
    hash ^= (unsigned)c;
    hash *= 16777619u;
  }
  hash ^= (unsigned)stamp_sz;
  hash *= 16777619u;
  hash ^= (unsigned)stamp_tm;
  hash *= 16777619u;
  char hex[16];
  std::snprintf(hex, sizeof(hex), "%08x", hash);
  fs::path dir = base / hex;
  fs::path stamp = dir / ".nx8v-stamp";
  std::string expect = std::to_string(stamp_sz) + " " + std::to_string(stamp_tm);
  if (fs::is_regular_file(stamp, ec)) {
    std::ifstream s(stamp);
    std::string have;
    std::getline(s, have);
    if (have == expect && fs::is_regular_file(dir / "project.json", ec)) {
      SetCurrentDirectoryW(dir.wstring().c_str());
      return true;
    }
  }
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  if (ec) {
    std::fprintf(stderr, "nx8v: gagal menyiapkan aplikasi\n");
    return false;
  }

  in.clear();
  in.seekg((std::streamoff)((uint64_t)total - 16 - payload));
  uint64_t consumed = 0;
  char buf[65536];
  while (consumed < payload) {
    uint32_t plen = 0;
    if (!PackRead32(in, plen)) return false;
    consumed += 4;
    if (plen == 0 || plen > 1024 || consumed + plen + 8 > payload) return false;
    std::string rel(plen, '\0');
    in.read(rel.data(), plen);
    if (in.gcount() != (std::streamsize)plen || !PackPathSafe(rel)) return false;
    consumed += plen;
    uint64_t flen = 0;
    if (!PackRead64(in, flen) || consumed + flen > payload) return false;
    consumed += 8;
    fs::path dest = dir / fs::path(rel);
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream file(dest, std::ios::binary);
    if (!file) return false;
    uint64_t left = flen;
    while (left) {
      size_t chunk = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
      in.read(buf, (std::streamsize)chunk);
      if (in.gcount() != (std::streamsize)chunk) return false;
      file.write(buf, (std::streamsize)chunk);
      left -= chunk;
    }
    consumed += flen;
  }
  {
    std::ofstream s(stamp, std::ios::binary);
    s << expect;
  }
  SetCurrentDirectoryW(dir.wstring().c_str());
  return true;
}

static bool Nx8vOpenMagic(const fs::path& exe, const char* magic, uint64_t& payload,
                          std::ifstream& in) {
  in.open(exe, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  auto total = in.tellg();
  if (total < 16) return false;
  in.seekg((std::streamoff)total - 16);
  if (!PackRead64(in, payload)) return false;
  char got[8];
  in.read(got, 8);
  if (in.gcount() != 8 || std::memcmp(got, magic, 8) != 0) return false;
  if (payload > (uint64_t)total - 16) return false;
  in.clear();
  in.seekg((std::streamoff)((uint64_t)total - 16 - payload));
  return true;
}

static bool Nx8vExtractPayload(std::istream& in, const fs::path& dir, uint64_t payload) {
  uint64_t consumed = 0;
  char buf[65536];
  std::error_code ec;
  while (consumed < payload) {
    uint32_t plen = 0;
    if (!PackRead32(in, plen)) return false;
    consumed += 4;
    if (plen == 0 || plen > 1024 || consumed + plen + 8 > payload) return false;
    std::string rel(plen, '\0');
    in.read(rel.data(), plen);
    if (in.gcount() != (std::streamsize)plen || !PackPathSafe(rel)) return false;
    consumed += plen;
    uint64_t flen = 0;
    if (!PackRead64(in, flen) || consumed + flen > payload) return false;
    consumed += 8;
    fs::path dest = dir / fs::path(rel);
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream file(dest, std::ios::binary);
    if (!file) return false;
    uint64_t left = flen;
    while (left) {
      size_t chunk = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
      in.read(buf, (std::streamsize)chunk);
      if (in.gcount() != (std::streamsize)chunk) return false;
      file.write(buf, (std::streamsize)chunk);
      left -= chunk;
    }
    consumed += flen;
  }
  return true;
}

static std::wstring Nx8vUtf8(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring out(n, 0);
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
  return out;
}

inline bool PackageSetGuiSubsystem(const fs::path& exe) {
  HANDLE h = CreateFileW(exe.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  auto read_at = [&](DWORD off, void* buf, DWORD n) -> bool {
    DWORD got = 0;
    if (SetFilePointer(h, (LONG)off, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
        GetLastError() != NO_ERROR)
      return false;
    return ::ReadFile(h, buf, n, &got, nullptr) && got == n;
  };
  auto write_at = [&](DWORD off, const void* buf, DWORD n) -> bool {
    DWORD got = 0;
    if (SetFilePointer(h, (LONG)off, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
        GetLastError() != NO_ERROR)
      return false;
    return ::WriteFile(h, buf, n, &got, nullptr) && got == n;
  };
  bool ok = false;
  unsigned char mz[2] = {};
  if (read_at(0, mz, 2) && mz[0] == 'M' && mz[1] == 'Z') {
    uint32_t peoff = 0;
    if (read_at(0x3C, &peoff, 4) && peoff > 0 && peoff < 1024) {
      unsigned char sig[4] = {};
      if (read_at(peoff, sig, 4) && sig[0] == 'P' && sig[1] == 'E' &&
          sig[2] == 0 && sig[3] == 0) {
        uint16_t gui = 2;
        ok = write_at(peoff + 4 + 20 + 68, &gui, 2);
      }
    }
  }
  CloseHandle(h);
  return ok;
}

static bool Nx8vShortcut(const fs::path& lnk, const fs::path& target, const wchar_t* args = nullptr) {
  IShellLinkW* link = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&link);
  if (FAILED(hr) || !link) return false;
  link->SetPath(target.wstring().c_str());
  link->SetWorkingDirectory(target.parent_path().wstring().c_str());
  link->SetIconLocation(target.wstring().c_str(), 0);
  if (args && args[0]) link->SetArguments(args);
  IPersistFile* file = nullptr;
  hr = link->QueryInterface(IID_IPersistFile, (void**)&file);
  if (SUCCEEDED(hr) && file) {
    hr = file->Save(lnk.wstring().c_str(), TRUE);
    file->Release();
  }
  link->Release();
  return SUCCEEDED(hr);
}

struct Nx8vSetupUi;

struct Nx8vBtn {
  Nx8vSetupUi* ui = nullptr;
  int id = 0;
  bool primary = false;
  bool hover = false;
  bool down = false;
};

struct Nx8vCheck {
  Nx8vSetupUi* ui = nullptr;
  bool on = true;
  bool hover = false;
};

struct Nx8vSetupUi {
  std::wstring title;
  std::wstring shortcut;
  std::wstring dir_name;
  std::wstring exe_name;
  std::wstring folder;
  fs::path stage;
  HFONT font = nullptr;
  HFONT font_title = nullptr;
  HBRUSH bg = nullptr;
  HBRUSH header = nullptr;
  HBRUSH footer = nullptr;
  HWND path = nullptr;
  HWND status = nullptr;
  HWND go = nullptr;
  HWND cancel = nullptr;
  HWND ubah = nullptr;
  HWND folder_lbl = nullptr;
  HWND shortcut_chk = nullptr;
  HWND launch_chk = nullptr;
  HWND hwnd = nullptr;
  WNDPROC edit_proc = nullptr;
  Nx8vBtn btn_go;
  Nx8vBtn btn_cancel;
  Nx8vBtn btn_browse;
  Nx8vCheck chk_shortcut;
  Nx8vCheck chk_launch;
  std::wstring version;
  std::wstring reg_key;
  bool uninstall = false;
  int dpi = 96;
  int progress = 0;
  bool show_progress = false;
  bool finished = false;
  bool launch_after = true;
};

static void Nx8vSetupText(HWND hwnd, const wchar_t* text) {
  SetWindowTextW(hwnd, text);
}

static bool Nx8vPickFolder(HWND owner, std::wstring& path) {
  IFileDialog* dlg = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileDialog, (void**)&dlg);
  if (FAILED(hr) || !dlg) return false;
  DWORD opt = 0;
  dlg->GetOptions(&opt);
  dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(L"Choose install folder");
  hr = dlg->Show(owner);
  if (SUCCEEDED(hr)) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item)) && item) {
      PWSTR name = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
        path = name;
        CoTaskMemFree(name);
      }
      item->Release();
    }
  }
  dlg->Release();
  return SUCCEEDED(hr) && !path.empty();
}

static void Nx8vPumpUi(Nx8vSetupUi* ui) {
  if (!ui || !ui->hwnd) return;
  InvalidateRect(ui->hwnd, nullptr, FALSE);
  UpdateWindow(ui->hwnd);
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

static bool Nx8vCopyWithProgress(Nx8vSetupUi* ui, const fs::path& dest) {
  std::error_code ec;
  struct Item { fs::path src; uint64_t size; };
  std::vector<Item> items;
  uint64_t total = 0;
  fs::recursive_directory_iterator it(ui->stage, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec || !it->is_regular_file(ec)) continue;
    uint64_t sz = fs::file_size(it->path(), ec);
    if (ec) { ec.clear(); sz = 0; }
    items.push_back({it->path(), sz});
    total += sz;
  }
  if (items.empty()) return false;
  fs::create_directories(dest, ec);
  uint64_t done = 0;
  std::vector<char> buf(256 * 1024);
  ui->show_progress = true;
  ui->progress = 0;
  Nx8vPumpUi(ui);
  for (const Item& item : items) {
    fs::path rel = fs::relative(item.src, ui->stage, ec);
    if (ec) return false;
    fs::path out = dest / rel;
    fs::create_directories(out.parent_path(), ec);
    std::ifstream in(item.src, std::ios::binary);
    std::ofstream file(out, std::ios::binary);
    if (!in || !file) return false;
    while (in) {
      in.read(buf.data(), (std::streamsize)buf.size());
      std::streamsize n = in.gcount();
      if (n <= 0) break;
      file.write(buf.data(), n);
      if (!file) return false;
      done += (uint64_t)n;
      ui->progress = total ? (int)(done * 100 / total) : 100;
      if (ui->progress > 100) ui->progress = 100;
      Nx8vPumpUi(ui);
    }
  }
  ui->progress = 100;
  Nx8vPumpUi(ui);
  return true;
}

static bool Nx8vSamePath(const fs::path& a, const fs::path& b);

static bool Nx8vApplyInstall(Nx8vSetupUi* ui, const fs::path& dest, bool shortcuts) {
  std::error_code ec;
  if (!Nx8vCopyWithProgress(ui, dest)) return false;
  fs::remove_all(ui->stage, ec);
  if (ui->exe_name.empty()) ui->exe_name = ui->dir_name + L".exe";
  fs::path app = dest / ui->exe_name;
  if (!fs::exists(app, ec)) return false;

  if (shortcuts) {
    wchar_t desktop[MAX_PATH];
    wchar_t programs[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_DESKTOP, nullptr, 0, desktop) == S_OK)
      Nx8vShortcut(fs::path(desktop) / (ui->shortcut + L".lnk"), app);
    if (SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, programs) == S_OK)
      Nx8vShortcut(fs::path(programs) / (ui->shortcut + L".lnk"), app);
  }
  {
    wchar_t programs[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, programs) == S_OK)
      fs::remove(fs::path(programs) / (L"Uninstall " + ui->shortcut + L".lnk"), ec);
    fs::path un_exe = dest / L"Uninstall.exe";
    if (!Nx8vSamePath(un_exe, app)) {
      fs::remove(un_exe, ec);
      if (!CreateHardLinkW(un_exe.wstring().c_str(), app.wstring().c_str(), nullptr))
        fs::copy_file(app, un_exe, fs::copy_options::overwrite_existing, ec);
    }
  }

  fs::remove(dest / L"uninstall.cmd", ec);
  std::wstring key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + ui->dir_name;
  std::wstring un = L"\"" + (dest / L"Uninstall.exe").wstring() + L"\"";
  std::wstring icon = L"\"" + app.wstring() + L"\"";
  auto put = [&](const wchar_t* name, const std::wstring& value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ,
                    value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
  };
  put(L"DisplayName", ui->title);
  if (!ui->version.empty()) put(L"DisplayVersion", ui->version);
  put(L"UninstallString", un);
  put(L"DisplayIcon", icon);
  put(L"InstallLocation", dest.wstring());
  put(L"ShortcutName", ui->shortcut);
  DWORD one = 1;
  RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"NoModify", REG_DWORD, &one, sizeof(one));
  RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"NoRepair", REG_DWORD, &one, sizeof(one));

  if (ui->launch_after)
    ShellExecuteW(nullptr, L"open", app.wstring().c_str(), nullptr,
                  app.parent_path().wstring().c_str(), SW_SHOWNORMAL);
  return true;
}

static std::wstring Nx8vRegRead(HKEY key, const wchar_t* name) {
  wchar_t buf[1024];
  DWORD bytes = sizeof(buf);
  DWORD type = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, (BYTE*)buf, &bytes) != ERROR_SUCCESS ||
      type != REG_SZ)
    return L"";
  return std::wstring(buf);
}

static bool Nx8vContainsI(const std::wstring& hay, const std::wstring& needle) {
  if (needle.empty() || hay.size() < needle.size()) return false;
  for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    if (_wcsnicmp(hay.c_str() + i, needle.c_str(), needle.size()) == 0) return true;
  }
  return false;
}

static bool Nx8vSamePath(const fs::path& a, const fs::path& b) {
  std::wstring as = a.wstring();
  std::wstring bs = b.wstring();
  return as.size() == bs.size() && _wcsicmp(as.c_str(), bs.c_str()) == 0;
}

static bool Nx8vFindUninstall(const std::wstring& exe, std::wstring& key_name,
                              std::wstring& display, std::wstring& version,
                              std::wstring& shortcut) {
  HKEY root = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                    0, KEY_READ, &root) != ERROR_SUCCESS)
    return false;
  bool found = false;
  wchar_t name[256];
  for (DWORD i = 0; ; ++i) {
    DWORD nlen = 256;
    if (RegEnumKeyExW(root, i, name, &nlen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
      break;
    HKEY sub = nullptr;
    if (RegOpenKeyExW(root, name, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
        std::wstring un = Nx8vRegRead(sub, L"UninstallString");
        std::wstring loc = Nx8vRegRead(sub, L"InstallLocation");
        std::wstring parent = fs::path(exe).parent_path().wstring();
        bool hit = Nx8vContainsI(un, exe) || Nx8vContainsI(un, parent + L"\\Uninstall.exe");
        if (!hit && !loc.empty() && Nx8vSamePath(fs::path(loc), fs::path(parent)))
          hit = Nx8vContainsI(Nx8vRegRead(sub, L"DisplayIcon"), parent);
        if (hit) {
      key_name = name;
      std::wstring got = Nx8vRegRead(sub, L"DisplayName");
      if (!got.empty()) display = got;
      version = Nx8vRegRead(sub, L"DisplayVersion");
      shortcut = Nx8vRegRead(sub, L"ShortcutName");
      found = true;
    }
    RegCloseKey(sub);
    if (found) break;
  }
  RegCloseKey(root);
  return found;
}

static void Nx8vStopOtherApps(const std::wstring& exe_path) {
  DWORD self = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  PROCESSENTRY32W pe = {};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (pe.th32ProcessID == 0 || pe.th32ProcessID == self) continue;
      HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                                FALSE, pe.th32ProcessID);
      if (!proc) continue;
      wchar_t image[MAX_PATH];
      DWORD nlen = MAX_PATH;
      if (QueryFullProcessImageNameW(proc, 0, image, &nlen)) {
        fs::path other(image);
        fs::path home = fs::path(exe_path).parent_path();
        if (Nx8vSamePath(other.parent_path(), home))
          TerminateProcess(proc, 0);
      }
      CloseHandle(proc);
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
}

static bool Nx8vApplyUninstall(Nx8vSetupUi* ui) {
  wchar_t mod[MAX_PATH];
  DWORD got = GetModuleFileNameW(nullptr, mod, MAX_PATH);
  if (got == 0 || got >= MAX_PATH) return false;
  fs::path self(mod);
  fs::path dest = self.parent_path();
  Nx8vStopOtherApps(self.wstring());
  Sleep(200);
  ui->progress = 8;
  Nx8vPumpUi(ui);

  if (!ui->reg_key.empty() && !ui->shortcut.empty()) {
    wchar_t desktop[MAX_PATH];
    wchar_t programs[MAX_PATH];
    std::error_code sec;
    if (SHGetFolderPathW(nullptr, CSIDL_DESKTOP, nullptr, 0, desktop) == S_OK)
      fs::remove(fs::path(desktop) / (ui->shortcut + L".lnk"), sec);
    if (SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, programs) == S_OK) {
      fs::remove(fs::path(programs) / (ui->shortcut + L".lnk"), sec);
      fs::remove(fs::path(programs) / (L"Uninstall " + ui->shortcut + L".lnk"), sec);
    }
  }
  ui->progress = 20;
  Nx8vPumpUi(ui);

  std::error_code ec;
  std::vector<fs::path> files;
  std::vector<fs::path> dirs;
  fs::recursive_directory_iterator it(dest, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) { ec.clear(); continue; }
    if (it->is_directory(ec)) dirs.push_back(it->path());
    else if (it->is_regular_file(ec)) files.push_back(it->path());
  }
  std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
    return a.wstring().size() > b.wstring().size();
  });
  size_t total = files.size() + dirs.size();
  size_t done = 0;
  auto tick = [&]() {
    ++done;
    int span = total ? (int)(done * 70 / total) : 70;
    ui->progress = 20 + span;
    if (ui->progress > 95) ui->progress = 95;
    Nx8vPumpUi(ui);
  };
  for (const fs::path& file : files) {
    if (!Nx8vSamePath(file, self)) fs::remove(file, ec);
    tick();
  }
  for (const fs::path& dir : dirs) {
    fs::remove(dir, ec);
    tick();
  }
  if (!ui->reg_key.empty()) {
    std::wstring full = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + ui->reg_key;
    RegDeleteKeyW(HKEY_CURRENT_USER, full.c_str());
  }
  ui->progress = 100;
  Nx8vPumpUi(ui);
  return true;
}

static void Nx8vScheduleFolderDelete(const std::wstring& dir) {
  wchar_t mod[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, mod, MAX_PATH);
  wchar_t temp[MAX_PATH];
  DWORD tn = GetTempPathW(MAX_PATH, temp);
  std::wstring away;
  bool moved = false;
  if (n > 0 && n < MAX_PATH && tn > 0 && tn < MAX_PATH) {
    away = std::wstring(temp) + L"nx8v-" + std::to_wstring(GetCurrentProcessId()) + L".exe";
    moved = MoveFileExW(mod, away.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
  }
  std::error_code ec;
  if (moved) fs::remove_all(fs::path(dir), ec);
  if (tn == 0 || tn >= MAX_PATH) return;
  std::wstring bat = std::wstring(temp) + L"nx8v-" + std::to_wstring(GetCurrentProcessId()) + L".cmd";
  std::wstring body = L"@echo off\r\n";
  body += L"ping 127.0.0.1 -n 2 >nul\r\n";
  body += L"rmdir /s /q \"" + dir + L"\"\r\n";
  if (moved) {
    body += L"set /a n=0\r\n:again\r\n";
    body += L"del /f /q \"" + away + L"\"\r\n";
    body += L"if not exist \"" + away + L"\" goto done\r\n";
    body += L"set /a n+=1\r\n";
    body += L"if %n% GEQ 15 goto done\r\n";
    body += L"ping 127.0.0.1 -n 2 >nul\r\n";
    body += L"goto again\r\n:done\r\n";
  }
  body += L"del /f /q \"%~f0\"\r\n";
  int bytes = WideCharToMultiByte(CP_ACP, 0, body.c_str(), (int)body.size(), nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) return;
  std::string narrow((size_t)bytes, '\0');
  WideCharToMultiByte(CP_ACP, 0, body.c_str(), (int)body.size(), narrow.data(), bytes, nullptr, nullptr);
  HANDLE file = CreateFileW(bat.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD wrote = 0;
  WriteFile(file, narrow.data(), (DWORD)narrow.size(), &wrote, nullptr);
  CloseHandle(file);
  std::wstring cmd = L"cmd.exe /c \"\"" + bat + L"\"\"";
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(0);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi = {};
  if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                     nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

static int Nx8vPx(const Nx8vSetupUi* ui, int v) {
  return MulDiv(v, ui->dpi, 96);
}

static LRESULT CALLBACK Nx8vBtnProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* b = (Nx8vBtn*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  switch (msg) {
    case WM_CREATE: {
      auto* cs = (CREATESTRUCTW*)lp;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
      return 0;
    }
    case WM_GETDLGCODE:
      return DLGC_BUTTON;
    case WM_ERASEBKGND:
      return 1;
    case WM_SETTEXT:
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
      LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
      InvalidateRect(hwnd, nullptr, FALSE);
      return r;
    }
    case WM_MOUSEMOVE:
      if (b && !b->hover) {
        b->hover = true;
        TRACKMOUSEEVENT ev = {};
        ev.cbSize = sizeof(ev);
        ev.dwFlags = TME_LEAVE;
        ev.hwndTrack = hwnd;
        TrackMouseEvent(&ev);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_MOUSELEAVE:
      if (b) { b->hover = false; b->down = false; }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_LBUTTONDOWN:
      if (b && IsWindowEnabled(hwnd)) {
        b->down = true;
        SetCapture(hwnd);
        SetFocus(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP:
      if (b && b->down) {
        b->down = false;
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (PtInRect(&rc, pt) && IsWindowEnabled(hwnd))
          SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(b->id, BN_CLICKED), (LPARAM)hwnd);
      }
      return 0;
    case WM_KEYDOWN:
      if ((wp == VK_SPACE || wp == VK_RETURN) && b && IsWindowEnabled(hwnd)) {
        SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(b->id, BN_CLICKED), (LPARAM)hwnd);
        return 0;
      }
      break;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      bool on = b && b->primary;
      bool hot = b && (b->hover || b->down);
      bool disabled = !IsWindowEnabled(hwnd);
      bool focus = GetFocus() == hwnd;
      COLORREF fill = RGB(255, 255, 255);
      COLORREF ink = RGB(15, 23, 42);
      COLORREF edge = focus ? RGB(37, 99, 235) : RGB(203, 213, 225);
      if (on) {
        fill = disabled ? RGB(148, 163, 184) : (b->down ? RGB(30, 64, 175) : (hot ? RGB(29, 78, 216) : RGB(37, 99, 235)));
        ink = RGB(255, 255, 255);
        edge = fill;
      } else if (hot && !disabled) {
        fill = b->down ? RGB(226, 232, 240) : RGB(241, 245, 249);
      }
      int rad = b && b->ui ? Nx8vPx(b->ui, 6) : 6;
      HBRUSH br = CreateSolidBrush(fill);
      HPEN pen = CreatePen(PS_SOLID, 1, edge);
      HGDIOBJ old_br = SelectObject(dc, br);
      HGDIOBJ old_pen = SelectObject(dc, pen);
      RoundRect(dc, 0, 0, rc.right - 1, rc.bottom - 1, rad, rad);
      SelectObject(dc, old_pen);
      SelectObject(dc, old_br);
      DeleteObject(pen);
      DeleteObject(br);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, disabled && !on ? RGB(148, 163, 184) : ink);
      if (b && b->ui && b->ui->font) SelectObject(dc, b->ui->font);
      wchar_t label[80];
      GetWindowTextW(hwnd, label, 80);
      DrawTextW(dc, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      EndPaint(hwnd, &ps);
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK Nx8vCheckProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* c = (Nx8vCheck*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  switch (msg) {
    case WM_CREATE: {
      auto* cs = (CREATESTRUCTW*)lp;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
      return 0;
    }
    case WM_GETDLGCODE:
      return DLGC_BUTTON;
    case BM_GETCHECK:
      return c && c->on ? BST_CHECKED : BST_UNCHECKED;
    case BM_SETCHECK:
      if (c) {
        c->on = wp == BST_CHECKED;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_MOUSEMOVE:
      if (c && !c->hover) {
        c->hover = true;
        TRACKMOUSEEVENT ev = {};
        ev.cbSize = sizeof(ev);
        ev.dwFlags = TME_LEAVE;
        ev.hwndTrack = hwnd;
        TrackMouseEvent(&ev);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_MOUSELEAVE:
      if (c) c->hover = false;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_LBUTTONDOWN:
      SetFocus(hwnd);
      return 0;
    case WM_LBUTTONUP:
      if (c) {
        c->on = !c->on;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_KEYDOWN:
      if (wp == VK_SPACE && c) {
        c->on = !c->on;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      HBRUSH bg = c && c->ui && c->ui->bg ? c->ui->bg : (HBRUSH)GetStockObject(WHITE_BRUSH);
      FillRect(dc, &rc, bg);
      int box = c && c->ui ? Nx8vPx(c->ui, 16) : 16;
      int y = (rc.bottom - box) / 2;
      RECT br = {0, y, box, y + box};
      bool on = c && c->on;
      bool hot = c && c->hover;
      bool focus = GetFocus() == hwnd;
      COLORREF fill = on ? RGB(37, 99, 235) : RGB(255, 255, 255);
      COLORREF edge = on ? RGB(37, 99, 235) : (hot || focus ? RGB(37, 99, 235) : RGB(100, 116, 139));
      HBRUSH brsh = CreateSolidBrush(fill);
      HPEN pen = CreatePen(PS_SOLID, 1, edge);
      HGDIOBJ old_br = SelectObject(dc, brsh);
      HGDIOBJ old_pen = SelectObject(dc, pen);
      int rad = c && c->ui ? Nx8vPx(c->ui, 4) : 4;
      RoundRect(dc, br.left, br.top, br.right, br.bottom, rad, rad);
      SelectObject(dc, old_pen);
      SelectObject(dc, old_br);
      DeleteObject(pen);
      DeleteObject(brsh);
      if (on) {
        HPEN mark = CreatePen(PS_SOLID, c && c->ui ? Nx8vPx(c->ui, 2) : 2, RGB(255, 255, 255));
        old_pen = SelectObject(dc, mark);
        int m = box / 5;
        MoveToEx(dc, br.left + m, br.top + box / 2, nullptr);
        LineTo(dc, br.left + box * 2 / 5, br.bottom - m);
        LineTo(dc, br.right - m, br.top + m);
        SelectObject(dc, old_pen);
        DeleteObject(mark);
      }
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(15, 23, 42));
      if (c && c->ui && c->ui->font) SelectObject(dc, c->ui->font);
      int gap = c && c->ui ? Nx8vPx(c->ui, 10) : 10;
      RECT tr = {box + gap, 0, rc.right, rc.bottom};
      wchar_t label[120];
      GetWindowTextW(hwnd, label, 120);
      DrawTextW(dc, label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      EndPaint(hwnd, &ps);
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK Nx8vEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* ui = (Nx8vSetupUi*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  WNDPROC orig = ui ? ui->edit_proc : nullptr;
  if (!orig) return DefWindowProcW(hwnd, msg, wp, lp);
  if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
    LRESULT r = CallWindowProcW(orig, hwnd, msg, wp, lp);
    HWND parent = GetParent(hwnd);
    if (parent) InvalidateRect(parent, nullptr, FALSE);
    return r;
  }
  return CallWindowProcW(orig, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK Nx8vSetupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* ui = (Nx8vSetupUi*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  switch (msg) {
    case WM_CREATE: {
      auto* cs = (CREATESTRUCTW*)lp;
      ui = (Nx8vSetupUi*)cs->lpCreateParams;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ui);
      ui->hwnd = hwnd;
      ui->bg = CreateSolidBrush(RGB(255, 255, 255));
      ui->header = CreateSolidBrush(RGB(15, 23, 42));
      ui->footer = CreateSolidBrush(RGB(248, 250, 252));
      ui->font = CreateFontW(-Nx8vPx(ui, 15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      ui->font_title = CreateFontW(-Nx8vPx(ui, 22), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex,
                     int x, int y, int w, int h, int id) -> HWND {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                                 Nx8vPx(ui, x), Nx8vPx(ui, y), Nx8vPx(ui, w), Nx8vPx(ui, h),
                                 hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)ui->font, TRUE);
        return c;
      };
      ui->folder_lbl = add(L"STATIC", ui->uninstall ? L"Location" : L"Install folder",
                           SS_LEFT, 0, 28, 104, 540, 18, 108);
      auto make_btn = [&](Nx8vBtn* b, const wchar_t* text, int id, bool primary,
                          int x, int y, int w, int h) -> HWND {
        b->ui = ui;
        b->id = id;
        b->primary = primary;
        b->hover = false;
        b->down = false;
        return CreateWindowExW(0, L"NX8VBtn", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               Nx8vPx(ui, x), Nx8vPx(ui, y), Nx8vPx(ui, w), Nx8vPx(ui, h),
                               hwnd, (HMENU)(INT_PTR)id, nullptr, b);
      };
      if (!ui->uninstall) {
        int box_x = Nx8vPx(ui, 28);
        int box_y = Nx8vPx(ui, 124);
        int box_w = Nx8vPx(ui, 448);
        int box_h = Nx8vPx(ui, 36);
        HDC mdc = GetDC(hwnd);
        HGDIOBJ old_font = SelectObject(mdc, ui->font);
        TEXTMETRICW tm = {};
        GetTextMetricsW(mdc, &tm);
        SelectObject(mdc, old_font);
        ReleaseDC(hwnd, mdc);
        int edit_h = tm.tmHeight + tm.tmExternalLeading;
        if (edit_h < box_h) edit_h += Nx8vPx(ui, 2);
        if (edit_h > box_h - Nx8vPx(ui, 8)) edit_h = box_h - Nx8vPx(ui, 8);
        int edit_y = box_y + (box_h - edit_h) / 2;
        ui->path = CreateWindowExW(0, L"EDIT", ui->folder.c_str(),
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                                   box_x + Nx8vPx(ui, 2), edit_y,
                                   box_w - Nx8vPx(ui, 4), edit_h,
                                   hwnd, (HMENU)(INT_PTR)100, nullptr, nullptr);
        SendMessageW(ui->path, WM_SETFONT, (WPARAM)ui->font, TRUE);
        SendMessageW(ui->path, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELONG(Nx8vPx(ui, 8), Nx8vPx(ui, 8)));
        ui->edit_proc = (WNDPROC)GetWindowLongPtrW(ui->path, GWLP_WNDPROC);
        SetWindowLongPtrW(ui->path, GWLP_USERDATA, (LONG_PTR)ui);
        SetWindowLongPtrW(ui->path, GWLP_WNDPROC, (LONG_PTR)Nx8vEditProc);
        ui->ubah = make_btn(&ui->btn_browse, L"Browse", 103, false, 484, 124, 88, 36);
        auto make_chk = [&](Nx8vCheck* c, const wchar_t* text, int y) -> HWND {
          c->ui = ui;
          c->on = true;
          c->hover = false;
          return CreateWindowExW(0, L"NX8VCheck", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 Nx8vPx(ui, 28), Nx8vPx(ui, y), Nx8vPx(ui, 520), Nx8vPx(ui, 24),
                                 hwnd, nullptr, nullptr, c);
        };
        ui->shortcut_chk = make_chk(&ui->chk_shortcut, L"Create desktop and Start Menu shortcuts", 176);
        ui->launch_chk = make_chk(&ui->chk_launch, L"Launch the application after install", 208);
      }
      ui->status = add(L"STATIC", L"", SS_LEFT, 0, 28, 236, 540, 20, 106);
      ui->cancel = make_btn(&ui->btn_cancel, L"Cancel", 102, false, 384, 292, 96, 36);
      ui->go = make_btn(&ui->btn_go, ui->uninstall ? L"Uninstall" : L"Install",
                        101, true, 488, 292, 96, 36);
      return 0;
    }
    case WM_ERASEBKGND: {
      if (!ui) break;
      RECT rc;
      GetClientRect(hwnd, &rc);
      RECT head = rc;
      head.bottom = Nx8vPx(ui, 84);
      FillRect((HDC)wp, &head, ui->header);
      RECT body = rc;
      body.top = head.bottom;
      FillRect((HDC)wp, &body, ui->bg);
      RECT foot = rc;
      foot.top = Nx8vPx(ui, 276);
      if (ui->footer) FillRect((HDC)wp, &foot, ui->footer);
      HPEN line = CreatePen(PS_SOLID, 1, RGB(226, 232, 240));
      HGDIOBJ old = SelectObject((HDC)wp, line);
      MoveToEx((HDC)wp, 0, foot.top, nullptr);
      LineTo((HDC)wp, rc.right, foot.top);
      SelectObject((HDC)wp, old);
      DeleteObject(line);
      return 1;
    }
    case WM_PAINT: {
      if (!ui) break;
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT head = {Nx8vPx(ui, 28), Nx8vPx(ui, 16), Nx8vPx(ui, 572), Nx8vPx(ui, 46)};
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(248, 250, 252));
      SelectObject(dc, ui->font_title);
      DrawTextW(dc, ui->title.c_str(), -1, &head, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
      RECT sub = {Nx8vPx(ui, 28), Nx8vPx(ui, 46), Nx8vPx(ui, 572), Nx8vPx(ui, 72)};
      SetTextColor(dc, RGB(148, 163, 184));
      SelectObject(dc, ui->font);
      std::wstring hint = ui->uninstall
          ? (ui->finished ? L"The application has been removed."
                         : L"This will remove the application from this computer.")
          : (ui->finished ? L"The application is ready to use."
                         : L"Choose a folder, then click Install.");
      if (!ui->finished && !ui->version.empty())
        hint = L"Version " + ui->version + L"   ·   " + hint;
      DrawTextW(dc, hint.c_str(), -1, &sub, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
      if (ui->show_progress) {
        int l = Nx8vPx(ui, 28);
        int t = Nx8vPx(ui, 262);
        int w = Nx8vPx(ui, 544);
        int h = Nx8vPx(ui, 8);
        RECT track = {l, t, l + w, t + h};
        HBRUSH track_br = CreateSolidBrush(RGB(226, 232, 240));
        FillRect(dc, &track, track_br);
        DeleteObject(track_br);
        int fill_w = ui->progress <= 0 ? 0 : w * ui->progress / 100;
        if (fill_w > 0) {
          if (fill_w > w) fill_w = w;
          RECT fill = {l, t, l + fill_w, t + h};
          HBRUSH fill_br = CreateSolidBrush(RGB(37, 99, 235));
          FillRect(dc, &fill, fill_br);
          DeleteObject(fill_br);
        }
      }
      if (ui->path && IsWindowVisible(ui->path)) {
        int l = Nx8vPx(ui, 28);
        int t = Nx8vPx(ui, 124);
        int r = l + Nx8vPx(ui, 448);
        int btm = t + Nx8vPx(ui, 36);
        bool focus = GetFocus() == ui->path;
        HPEN pen = CreatePen(PS_SOLID, 1, focus ? RGB(37, 99, 235) : RGB(100, 116, 139));
        HGDIOBJ old_pen = SelectObject(dc, pen);
        HGDIOBJ old_br = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, l, t, r, btm, Nx8vPx(ui, 6), Nx8vPx(ui, 6));
        SelectObject(dc, old_br);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
      } else if (ui->uninstall && !ui->finished) {
        int l = Nx8vPx(ui, 28);
        int t = Nx8vPx(ui, 124);
        int r = l + Nx8vPx(ui, 544);
        int btm = t + Nx8vPx(ui, 36);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(100, 116, 139));
        HGDIOBJ old_pen = SelectObject(dc, pen);
        HGDIOBJ old_br = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, l, t, r, btm, Nx8vPx(ui, 6), Nx8vPx(ui, 6));
        SelectObject(dc, old_br);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        RECT tr = {l + Nx8vPx(ui, 10), t, r - Nx8vPx(ui, 10), btm};
        SetTextColor(dc, RGB(15, 23, 42));
        DrawTextW(dc, ui->folder.c_str(), -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
      }
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      if (!ui) break;
      HDC dc = (HDC)wp;
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(30, 41, 59));
      return (LRESULT)ui->bg;
    }
    case WM_CTLCOLOREDIT: {
      if (!ui) break;
      HDC dc = (HDC)wp;
      SetBkColor(dc, RGB(255, 255, 255));
      SetTextColor(dc, RGB(15, 23, 42));
      return (LRESULT)ui->bg;
    }
    case WM_COMMAND: {
      int id = LOWORD(wp);
      if (!ui) break;
      if (id == 102 || (id == 101 && ui->finished)) {
        DestroyWindow(hwnd);
        return 0;
      }
      if (id == 103 && !ui->finished) {
        std::wstring picked;
        if (Nx8vPickFolder(hwnd, picked)) SetWindowTextW(ui->path, picked.c_str());
        return 0;
      }
      if (id == 101 && !ui->finished && ui->uninstall) {
        ui->show_progress = true;
        ui->progress = 0;
        Nx8vSetupText(ui->status, L"Removing...");
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
        EnableWindow(ui->go, FALSE);
        if (!Nx8vApplyUninstall(ui)) {
          Nx8vSetupText(ui->status, L"Uninstall failed. Close the application and try again.");
          ui->show_progress = false;
          EnableWindow(ui->go, TRUE);
          return 0;
        }
        ui->finished = true;
        ui->show_progress = false;
        ShowWindow(ui->folder_lbl, SW_HIDE);
        ShowWindow(ui->cancel, SW_HIDE);
        Nx8vSetupText(ui->status, (ui->title + L" has been removed.").c_str());
        SendMessageW(ui->status, WM_SETFONT, (WPARAM)ui->font_title, TRUE);
        MoveWindow(ui->status, Nx8vPx(ui, 28), Nx8vPx(ui, 150), Nx8vPx(ui, 544), Nx8vPx(ui, 36), TRUE);
        SetWindowTextW(ui->go, L"Finish");
        EnableWindow(ui->go, TRUE);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
      }
      if (id == 101 && !ui->finished) {
        wchar_t buf[MAX_PATH];
        GetWindowTextW(ui->path, buf, MAX_PATH);
        if (buf[0] == 0) {
          Nx8vSetupText(ui->status, L"Enter an install folder.");
          return 0;
        }
        ui->launch_after = SendMessageW(ui->launch_chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
        bool shortcuts = SendMessageW(ui->shortcut_chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
        ui->show_progress = true;
        ui->progress = 0;
        Nx8vSetupText(ui->status, L"Installing...");
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
        EnableWindow(ui->go, FALSE);
        if (!Nx8vApplyInstall(ui, fs::path(buf), shortcuts)) {
          Nx8vSetupText(ui->status, L"Installation failed. Check that folder.");
          EnableWindow(ui->go, TRUE);
          return 0;
        }
        ui->finished = true;
        ui->show_progress = false;
        ShowWindow(ui->folder_lbl, SW_HIDE);
        ShowWindow(ui->path, SW_HIDE);
        ShowWindow(ui->ubah, SW_HIDE);
        ShowWindow(ui->shortcut_chk, SW_HIDE);
        ShowWindow(ui->launch_chk, SW_HIDE);
        ShowWindow(ui->cancel, SW_HIDE);
        Nx8vSetupText(ui->status, (ui->title + L" has been installed.").c_str());
        SendMessageW(ui->status, WM_SETFONT, (WPARAM)ui->font_title, TRUE);
        MoveWindow(ui->status, Nx8vPx(ui, 28), Nx8vPx(ui, 150), Nx8vPx(ui, 544), Nx8vPx(ui, 36), TRUE);
        SetWindowTextW(ui->go, L"Finish");
        EnableWindow(ui->go, TRUE);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
      }
      break;
    }
    case WM_DESTROY:
      if (ui) {
        if (ui->font) DeleteObject(ui->font);
        if (ui->font_title) DeleteObject(ui->font_title);
        if (ui->bg) DeleteObject(ui->bg);
        if (ui->header) DeleteObject(ui->header);
        if (ui->footer) DeleteObject(ui->footer);
      }
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static int Nx8vShowInstaller(Nx8vSetupUi* ui, const std::wstring& default_dir) {
  SetProcessDPIAware();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  HDC screen = GetDC(nullptr);
  ui->dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
  if (screen) ReleaseDC(nullptr, screen);
  if (ui->dpi < 96) ui->dpi = 96;
  ui->folder = default_dir;
  WNDCLASSW bc = {};
  bc.lpfnWndProc = Nx8vBtnProc;
  bc.hInstance = GetModuleHandleW(nullptr);
  bc.lpszClassName = L"NX8VBtn";
  bc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassW(&bc);
  WNDCLASSW cc = {};
  cc.lpfnWndProc = Nx8vCheckProc;
  cc.hInstance = bc.hInstance;
  cc.lpszClassName = L"NX8VCheck";
  cc.hCursor = bc.hCursor;
  RegisterClassW(&cc);
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = Nx8vSetupProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"NX8VSetupClass";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.hIcon = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                               GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                               LR_SHARED);
  wc.hIconSm = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                 LR_SHARED);
  RegisterClassExW(&wc);
  RECT rc = {0, 0, Nx8vPx(ui, 600), Nx8vPx(ui, 352)};
  AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
  int w = rc.right - rc.left;
  int h = rc.bottom - rc.top;
  int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
  int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
  std::wstring caption = (ui->uninstall ? L"Uninstall " : L"Install ") + ui->title;
  HWND hwnd = CreateWindowExW(0, L"NX8VSetupClass", caption.c_str(),
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              x, y, w, h, nullptr, nullptr, wc.hInstance, ui);
  if (!hwnd) return 1;
  if (wc.hIcon) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
  if (wc.hIconSm) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);
  if (ui->path) SetWindowTextW(ui->path, default_dir.c_str());
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  std::error_code ec;
  if (!ui->uninstall && !ui->finished) fs::remove_all(ui->stage, ec);
  if (ui->uninstall && ui->finished) Nx8vScheduleFolderDelete(ui->folder);
  return 0;
}

static int Nx8vRunUninstall(bool quiet) {
  FreeConsole();
  wchar_t mod[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, mod, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return 1;
  fs::path self(mod);
  Nx8vSetupUi ui;
  ui.uninstall = true;
  ui.folder = self.parent_path().wstring();
  ui.dir_name = self.parent_path().filename().wstring();
  ui.title = ui.dir_name;
  if (Nx8vFindUninstall(self.wstring(), ui.reg_key, ui.title, ui.version, ui.shortcut)) {
    if (ui.title.empty()) ui.title = ui.dir_name;
  }
  if (quiet) {
    if (!Nx8vApplyUninstall(&ui)) return 1;
    Nx8vScheduleFolderDelete(ui.folder);
    return 0;
  }
  return Nx8vShowInstaller(&ui, ui.folder);
}

// Returns -1 when this exe is not an installer.
inline int Nx8vRunInstaller(int argc, char** argv) {
  wchar_t mod[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, mod, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return -1;
  fs::path self(mod);
  bool want_un = false;
  bool quiet_un = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i] ? argv[i] : "";
    if (arg == "--uninstall") want_un = true;
    if (arg == "--quiet" || arg == "/S") quiet_un = true;
  }
  std::wstring file = self.filename().wstring();
  bool named = file.size() == wcslen(L"Uninstall.exe") &&
               _wcsicmp(file.c_str(), L"Uninstall.exe") == 0;
  if (want_un || named) {
    std::ifstream packed_in;
    uint64_t packed_len = 0;
    if (Nx8vOpenMagic(self, kNx8vPackMagic, packed_len, packed_in)) {
      packed_in.close();
      return Nx8vRunUninstall(quiet_un);
    }
  }
  std::ifstream in;
  uint64_t payload = 0;
  if (!Nx8vOpenMagic(self, kNx8vSetupMagic, payload, in)) return -1;

  FreeConsole();
  bool quiet = false;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--install") quiet = true;

  wchar_t local[MAX_PATH];
  DWORD ln = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  std::error_code ec;
  fs::path local_dir = ln > 0 && ln < MAX_PATH ? fs::path(local)
                                               : fs::temp_directory_path(ec);
  fs::path stage = local_dir / "nx8v" / "setup-stage";
  fs::remove_all(stage, ec);
  fs::create_directories(stage, ec);
  if (ec || !Nx8vExtractPayload(in, stage, payload)) {
    MessageBoxW(nullptr, L"The installer file could not be opened.", L"Setup", MB_ICONERROR);
    return 1;
  }

  Nx8vSetupUi ui;
  ui.stage = stage;
  ui.dir_name = L"app";
  ui.shortcut = L"Aplikasi";
  ui.title = L"Aplikasi";
  {
    std::ifstream meta(stage / "nx8v.install");
    std::string line;
    while (std::getline(meta, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string k = line.substr(0, eq);
      std::string v = line.substr(eq + 1);
      if (k == "dir") ui.dir_name = Nx8vUtf8(v);
      else if (k == "exe") ui.exe_name = Nx8vUtf8(v);
      else if (k == "shortcut") ui.shortcut = Nx8vUtf8(v);
      else if (k == "display") ui.title = Nx8vUtf8(v);
      else if (k == "version") ui.version = Nx8vUtf8(v);
    }
  }
  fs::path dest = local_dir / ui.dir_name;
  if (quiet) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ui.launch_after = true;
    return Nx8vApplyInstall(&ui, dest, true) ? 0 : 1;
  }
  return Nx8vShowInstaller(&ui, dest.wstring());
}
#endif

// Does the project reference the desktop window bridge?
inline bool PackageUsesWindow(const fs::path& root) {
  std::error_code ec;
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;
  while (it != end) {
    if (ec) { it.increment(ec); continue; }
    const fs::path p = it->path();
    if (it->is_directory(ec)) {
      std::string n = p.filename().string();
      if (PackageSkipDir(n)) it.disable_recursion_pending();
      it.increment(ec);
      continue;
    }
    if (it->is_regular_file(ec)) {
      std::string n = p.filename().string();
      if (n.size() >= 3 &&
          (n.compare(n.size() - 3, 3, ".js") == 0 ||
           n.compare(n.size() - 5, 5, ".html") == 0)) {
        std::ifstream in(p, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        if (content.find("nx8v.dialog") != std::string::npos ||
            content.find("window.nx8v") != std::string::npos ||
            content.find("nx8v.setTitle") != std::string::npos) {
          return true;
        }
      }
    }
    it.increment(ec);
  }
  return false;
}

#ifdef _WIN32
#pragma pack(push, 1)
struct Nx8vIcoDir { uint16_t reserved, type, count; };
struct Nx8vIcoEntry {
  uint8_t width, height, colors, reserved;
  uint16_t planes, bitcount;
  uint32_t bytes, offset;
};
struct Nx8vGrpEntry {
  uint8_t width, height, colors, reserved;
  uint16_t planes, bitcount;
  uint32_t bytes;
  uint16_t id;
};
#pragma pack(pop)
#endif

inline bool PackageEmbedIcon(const fs::path& exe, const fs::path& image) {
#ifdef _WIN32
  std::vector<unsigned char> data = Nx8vFileToIco(image.wstring());
  if (data.size() < sizeof(Nx8vIcoDir)) return false;
  auto* dir = reinterpret_cast<Nx8vIcoDir*>(data.data());
  if (dir->reserved != 0 || dir->type != 1 || dir->count == 0) return false;
  if (data.size() < sizeof(Nx8vIcoDir) + dir->count * sizeof(Nx8vIcoEntry)) return false;

  HANDLE res = BeginUpdateResourceW(exe.wstring().c_str(), FALSE);
  if (!res) return false;
  std::vector<unsigned char> group(sizeof(Nx8vIcoDir) + dir->count * sizeof(Nx8vGrpEntry));
  auto* gdir = reinterpret_cast<Nx8vIcoDir*>(group.data());
  gdir->reserved = 0;
  gdir->type = 1;
  gdir->count = dir->count;
  bool ok = true;
  for (uint16_t i = 0; i < dir->count; ++i) {
    auto* ent = reinterpret_cast<Nx8vIcoEntry*>(
        data.data() + sizeof(Nx8vIcoDir) + i * sizeof(Nx8vIcoEntry));
    if ((size_t)ent->offset + ent->bytes > data.size()) { ok = false; break; }
    if (!UpdateResourceW(res, RT_ICON, MAKEINTRESOURCEW(i + 1),
                         MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                         data.data() + ent->offset, ent->bytes)) {
      ok = false;
      break;
    }
    auto* ge = reinterpret_cast<Nx8vGrpEntry*>(
        group.data() + sizeof(Nx8vIcoDir) + i * sizeof(Nx8vGrpEntry));
    ge->width = ent->width;
    ge->height = ent->height;
    ge->colors = ent->colors;
    ge->reserved = 0;
    ge->planes = ent->planes ? ent->planes : 1;
    ge->bitcount = ent->bitcount ? ent->bitcount : 32;
    ge->bytes = ent->bytes;
    ge->id = (uint16_t)(i + 1);
  }
  if (ok) {
    ok = UpdateResourceW(res, RT_GROUP_ICON, MAKEINTRESOURCEW(1),
                         MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                         group.data(), (DWORD)group.size()) != FALSE;
  }
  if (!EndUpdateResourceW(res, ok ? FALSE : TRUE)) ok = false;
  return ok;
#else
  (void)exe;
  (void)image;
  return false;
#endif
}

inline std::string PackageExpand(std::string text, const std::string& name,
                                 const std::string& version) {
  auto put = [&](const char* key, const std::string& value) {
    std::string k = key;
    for (size_t at = text.find(k); at != std::string::npos; at = text.find(k, at)) {
      text.replace(at, k.size(), value);
      at += value.size();
    }
  };
  put("${name}", name);
  put("${version}", version);
  return text;
}

inline std::string PackageNsis(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c == '$') out += "$$";
    else if (c == '"') out += "'";
    else if (c != '\r' && c != '\n') out += (char)c;
  }
  return out;
}

inline fs::path PackageFindMakensis() {
#ifdef _WIN32
  const wchar_t* fixed[] = {
    L"C:\\Program Files (x86)\\NSIS\\makensis.exe",
    L"C:\\Program Files\\NSIS\\makensis.exe",
  };
  std::error_code ec;
  for (const wchar_t* p : fixed) {
    if (fs::exists(p, ec)) return p;
  }
  FILE* pipe = _popen("where makensis 2>nul", "r");
  if (!pipe) return {};
  char buf[1024];
  std::string line;
  if (fgets(buf, sizeof(buf), pipe)) line = buf;
  _pclose(pipe);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
    line.pop_back();
  if (!line.empty() && fs::exists(line, ec)) return line;
#endif
  return {};
}

static fs::path PackageFindLinuxRuntime(const fs::path& project) {
  std::error_code ec;
  fs::path dir = project;
  for (int i = 0; i < 6; ++i) {
    fs::path candidate = dir / "build" / "linux" / "nx8v";
    if (fs::is_regular_file(candidate, ec)) {
      std::ifstream in(candidate, std::ios::binary);
      char mag[4] = {};
      in.read(mag, 4);
      if (in.gcount() == 4 && mag[0] == 0x7f && mag[1] == 'E' && mag[2] == 'L' && mag[3] == 'F')
        return candidate;
    }
    fs::path parent = dir.parent_path();
    if (parent.empty() || parent == dir) break;
    dir = parent;
  }
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    fs::path beside = fs::path(buf).parent_path() / L"nx8v-linux";
    if (fs::is_regular_file(beside, ec)) return beside;
  }
#endif
  return {};
}

inline int RunPackage(int argc, char* argv[]) {
  std::string out_dir = "dist";
  std::string app_name;

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--out=", 0) == 0) out_dir = a.substr(6);
    else if (a.rfind("--name=", 0) == 0) app_name = a.substr(7);
  }

  std::error_code ec;
  fs::path root = fs::current_path(ec);
  fs::path manifest_path = root / "project.json";

  std::string main_file = "server.js";
  std::string project_name;
  std::string icon_rel, product_name, shortcut_name, artifact_tmpl, version;
  int port = 4000;

  if (fs::exists(manifest_path, ec)) {
    std::string json = ReadManifestFile(manifest_path);
    JsonFindString(json, "main", main_file);
    JsonFindString(json, "name", project_name);
    JsonFindString(json, "icon", icon_rel);
    JsonFindString(json, "productName", product_name);
    JsonFindString(json, "shortcutName", shortcut_name);
    JsonFindString(json, "artifactName", artifact_tmpl);
    JsonFindString(json, "version", version);
    JsonFindNumber(json, "port", port);
  } else {
    std::printf("Peringatan: project.json tidak ditemukan di %s\n",
                root.string().c_str());
  }

  if (app_name.empty()) {
    app_name = project_name.empty() ? root.filename().string() : project_name;
  }
  // Sanitise: no spaces/slashes in the exe name.
  for (auto& c : app_name) {
    if (c == ' ' || c == '/' || c == '\\' || c == ':') c = '-';
  }
  if (app_name.empty()) app_name = "app";

  fs::path target = root / out_dir / app_name;
  if (fs::exists(target, ec)) {
    std::printf("Menghapus paket lama: %s\n", target.string().c_str());
    fs::remove_all(target, ec);
  }
  fs::create_directories(target, ec);

  std::printf("Membuat paket '%s'\n", app_name.c_str());
  std::printf("  sumber : %s\n", root.string().c_str());
  std::printf("  tujuan : %s\n", target.string().c_str());

  // The exe is the app. Raw project files are not placed in the folder.
#ifdef _WIN32
  wchar_t exe_buf[MAX_PATH];
  DWORD exe_n = GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
  if (exe_n == 0 || exe_n >= MAX_PATH) {
    std::printf("Gagal menemukan path nx8v.exe\n");
    return 1;
  }
  std::wstring exe_w(exe_buf, exe_n);
  fs::path exe_src(exe_w);
  std::string exe = exe_src.string();
#else
  std::string exe = PackageExePath();
  if (exe.empty()) {
    std::printf("Gagal menemukan path nx8v.exe\n");
    return 1;
  }
  fs::path exe_src(exe);
#endif
  fs::path exe_target = target / (app_name + ".exe");
#ifdef _WIN32
  {
    std::error_code cec;
    fs::create_directories(target, cec);
    fs::copy_file(exe_src, exe_target, fs::copy_options::overwrite_existing, cec);
    if (cec) {
      HANDLE h = CreateFileW(exe_w.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      std::ofstream raw(exe_target, std::ios::binary);
      bool ok = h != INVALID_HANDLE_VALUE && raw;
      if (ok) {
        char buf[65536];
        DWORD got = 0;
        while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got)
          raw.write(buf, got);
        ok = (bool)raw;
      }
      if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
      if (!ok) {
        std::printf("Gagal menyalin exe: %s\n", cec.message().c_str());
        return 1;
      }
    }
  }
#else
  fs::copy_file(exe_src, exe_target, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    std::printf("Gagal menyalin exe: %s\n", ec.message().c_str());
    return 1;
  }
#endif
  std::printf("  + %s.exe\n", app_name.c_str());

  fs::path icon_file;
  if (!icon_rel.empty()) icon_file = Nx8vResolveIconPath(root, icon_rel);
  else if (fs::exists(root / "icon.ico", ec)) icon_file = root / "icon.ico";
  if (!icon_file.empty() && fs::is_regular_file(icon_file, ec)) {
    if (PackageEmbedIcon(exe_target, icon_file))
      std::printf("  + ikon %s\n", icon_file.filename().string().c_str());
    else
      std::printf("  ! ikon gagal ditanam ke exe\n");
  }
#ifdef _WIN32
  PackageSetGuiSubsystem(exe_target);
#endif

  int packed = PackageAppendPayload(exe_target, root);
  if (packed < 0) {
    std::printf("Gagal menaruh aplikasi di dalam exe\n");
    return 1;
  }
  std::printf("  + %d file di dalam exe\n", packed);

  // ---- WebView2 loader when the project uses the desktop bridge -----------
  bool uses_window = PackageUsesWindow(root);
  if (uses_window) {
    fs::path loader = fs::path(exe).parent_path() / "WebView2Loader.dll";
    if (fs::exists(loader, ec)) {
      fs::copy_file(loader, target / "WebView2Loader.dll",
                    fs::copy_options::overwrite_existing, ec);
      if (!ec) std::printf("  + WebView2Loader.dll\n");
    } else {
      std::printf("  ! WebView2Loader.dll tidak ditemukan di %s\n",
                  loader.string().c_str());
    }
  }

  // ---- launcher .cmd ------------------------------------------------------
  {
    std::ofstream cmd(target / "start.cmd");
    cmd << "@echo off\r\n";
    cmd << "cd /d \"%~dp0\"\r\n";
    if (uses_window) {
      cmd << "\"%~dp0" << app_name << ".exe\" window\r\n";
    } else {
      cmd << "\"%~dp0" << app_name << ".exe\" start\r\n";
    }
  }

  std::string display = !product_name.empty() ? product_name
                      : (!project_name.empty() ? project_name : app_name);
  if (shortcut_name.empty()) shortcut_name = display;
  if (artifact_tmpl.empty()) artifact_tmpl = "${name}-Setup-${version}.exe";
  std::string artifact = PackageExpand(artifact_tmpl, app_name, version);
  for (auto& c : artifact) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
        c == '"' || c == '<' || c == '>' || c == '|') c = '-';
  }
  fs::path setup_path = target.parent_path() / artifact;
  fs::path nsi_path = target.parent_path() / (app_name + ".nsi");

  // ---- README -------------------------------------------------------------
  {
    std::ofstream readme(target / "README.txt");
    readme << "Application package: " << app_name << "\r\n\r\n";
    readme << "How to run:\r\n";
    if (uses_window) {
      readme << "Installer (one folder above this file):\r\n";
      readme << "  " << artifact << "\r\n\r\n";
      readme << "This folder is the application, ready to run.\r\n";
      readme << "  Double-click " << app_name << ".exe\r\n\r\n";
      readme << "The application opens as a desktop window (WebView2).\r\n";
      readme << "The application contents are inside the exe.\r\n";
      readme << "WebView2 Runtime is required (already present on Windows 11).\r\n";
    } else {
      readme << "  Run: " << app_name << ".exe start\r\n\r\n";
      readme << "The server listens at http://localhost:" << port << "/\r\n";
      readme << "Stop it with Ctrl+C or: " << app_name << ".exe stop\r\n";
    }
    readme << "\r\nBuilt with nx8v (V8 + libuv).\r\n";
  }

  // ---- summary ------------------------------------------------------------
  {
    std::string files = target.string();
    for (auto& c : files) if (c == '/') c = '\\';
    std::ofstream nsi(nsi_path, std::ios::binary);
    nsi << "Unicode true\r\n";
    nsi << "!include MUI2.nsh\r\n";
    nsi << "Name \"" << PackageNsis(display) << "\"\r\n";
    nsi << "OutFile \"" << PackageNsis(setup_path.string()) << "\"\r\n";
    nsi << "InstallDir \"$LOCALAPPDATA\\" << PackageNsis(app_name) << "\"\r\n";
    nsi << "RequestExecutionLevel user\r\n";
    nsi << "SetCompressor zlib\r\n";
    nsi << "!insertmacro MUI_PAGE_DIRECTORY\r\n";
    nsi << "!insertmacro MUI_PAGE_INSTFILES\r\n";
    nsi << "!insertmacro MUI_UNPAGE_CONFIRM\r\n";
    nsi << "!insertmacro MUI_UNPAGE_INSTFILES\r\n";
    nsi << "!insertmacro MUI_LANGUAGE \"Indonesian\"\r\n";
    nsi << "Section\r\n";
    nsi << "  SetOutPath \"$INSTDIR\"\r\n";
    nsi << "  File /r \"" << PackageNsis(files) << "\\*.*\"\r\n";
    nsi << "  CreateShortcut \"$SMPROGRAMS\\" << PackageNsis(shortcut_name) << ".lnk\" \"$INSTDIR\\" << app_name << ".exe\"\r\n";
    nsi << "  CreateShortcut \"$DESKTOP\\" << PackageNsis(shortcut_name) << ".lnk\" \"$INSTDIR\\" << app_name << ".exe\"\r\n";
    nsi << "  WriteUninstaller \"$INSTDIR\\Uninstall.exe\"\r\n";
    nsi << "  WriteRegStr HKCU \"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" << PackageNsis(app_name) << "\" \"DisplayName\" \"" << PackageNsis(display) << "\"\r\n";
    nsi << "  WriteRegStr HKCU \"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" << PackageNsis(app_name) << "\" \"UninstallString\" \"$\\\"$INSTDIR\\Uninstall.exe$\\\"\"\r\n";
    nsi << "  WriteRegStr HKCU \"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" << PackageNsis(app_name) << "\" \"DisplayIcon\" \"$\\\"$INSTDIR\\" << app_name << ".exe$\\\"\"\r\n";
    nsi << "SectionEnd\r\n";
    nsi << "Section \"Uninstall\"\r\n";
    nsi << "  Delete \"$DESKTOP\\" << PackageNsis(shortcut_name) << ".lnk\"\r\n";
    nsi << "  Delete \"$SMPROGRAMS\\" << PackageNsis(shortcut_name) << ".lnk\"\r\n";
    nsi << "  RMDir /r \"$INSTDIR\"\r\n";
    nsi << "  DeleteRegKey HKCU \"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" << PackageNsis(app_name) << "\"\r\n";
    nsi << "SectionEnd\r\n";
  }
  std::printf("  + %s\n", nsi_path.filename().string().c_str());
  fs::path makensis = PackageFindMakensis();
  if (makensis.empty()) {
    std::printf("  ! makensis tidak ada, pemasang nx8v yang dipakai\n");
  } else {
    std::string cmd = "\"" + makensis.string() + "\" /V2 \"" + nsi_path.string() + "\"";
    int rc = std::system(cmd.c_str());
    if (rc == 0 && fs::exists(setup_path, ec))
      std::printf("  + %s (NSIS)\n", setup_path.filename().string().c_str());
    else
      std::printf("  ! makensis gagal (kode %d)\n", rc);
  }

#ifdef _WIN32
  {
    std::string install_meta;
    install_meta += "dir=" + app_name + "\n";
    install_meta += "exe=" + app_name + ".exe\n";
    install_meta += "shortcut=" + shortcut_name + "\n";
    install_meta += "display=" + display + "\n";
    if (!version.empty()) install_meta += "version=" + version + "\n";
    std::error_code sec;
    bool copied = false;
    fs::copy_file(exe_src, setup_path, fs::copy_options::overwrite_existing, sec);
    copied = !sec;
    if (!copied) {
      HANDLE h = CreateFileW(exe_w.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      std::ofstream raw(setup_path, std::ios::binary);
      copied = h != INVALID_HANDLE_VALUE && raw;
      if (copied) {
        char buf[65536];
        DWORD got = 0;
        while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got)
          raw.write(buf, got);
        copied = (bool)raw;
      }
      if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    if (copied && !icon_file.empty()) PackageEmbedIcon(setup_path, icon_file);
    if (copied) PackageSetGuiSubsystem(setup_path);
    int setup_files = copied ? PackageAppendTree(setup_path, target, kNx8vSetupMagic,
                                                 "nx8v.install", install_meta, true)
                             : -1;
    if (setup_files >= 0)
      std::printf("  + %s\n", setup_path.filename().string().c_str());
    else
      std::printf("  ! pemasang gagal dibuat\n");
  }
#endif

  fs::path linux_rt = PackageFindLinuxRuntime(root);
  std::string linux_ver = version;
  for (auto& c : linux_ver) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
        c == '"' || c == '<' || c == '>' || c == '|') c = '-';
  }
  std::string linux_name = app_name;
  if (!linux_ver.empty()) linux_name += "-" + linux_ver;
  linux_name += "-linux-x64";
  fs::path linux_dir = target.parent_path() / linux_name;
  fs::path linux_old = target.parent_path() / (app_name + "-linux");
  fs::path linux_archive = target.parent_path() / (linux_name + ".tar.gz");
  if (!linux_rt.empty()) {
    std::error_code lec;
    if (fs::exists(linux_old, lec)) fs::remove_all(linux_old, lec);
    if (fs::exists(linux_dir, lec)) fs::remove_all(linux_dir, lec);
    fs::create_directories(linux_dir, lec);
    fs::path linux_bin = linux_dir / app_name;
    fs::copy_file(linux_rt, linux_bin, fs::copy_options::overwrite_existing, lec);
    if (lec) {
      std::printf("  ! paket Linux gagal: %s\n", lec.message().c_str());
    } else {
      std::vector<std::string> linux_files, linux_skipped;
      PackageCopyTree(root, linux_dir, linux_files, linux_skipped);
      (void)linux_skipped;
      {
        std::ofstream sh(linux_dir / "start.sh", std::ios::binary);
        sh << "#!/bin/sh\n";
        sh << "cd \"$(dirname \"$0\")\" || exit 1\n";
        sh << "chmod +x \"./" << app_name << "\"\n";
        sh << "exec \"./" << app_name << "\" start\n";
      }
      std::string tar_cmd = "tar -czf \"" + linux_archive.string() + "\" -C \"" +
                            linux_dir.parent_path().string() + "\" \"" + linux_name + "\"";
      int tar_rc = std::system(tar_cmd.c_str());
      if (tar_rc == 0 && fs::is_regular_file(linux_archive, lec)) {
        fs::remove_all(linux_dir, lec);
        std::printf("  + %s  (Linux, %d file)\n",
                    linux_archive.filename().string().c_str(), (int)linux_files.size());
      } else {
        std::printf("  ! arsip Linux gagal, folder %s dibiarkan\n", linux_name.c_str());
      }
    }
  } else {
    std::printf("  ! runtime Linux tidak ada, paket Windows tetap dibuat\n");
  }

  std::printf("\nSelesai. %d file di dalam exe\n", packed);
  if (uses_window) {
    std::printf("\nJalankan:\n  \"%s\"\n", exe_target.string().c_str());
  } else {
    std::printf("\n\nJalankan:\n  cd \"%s\"\n  start.cmd\n", target.string().c_str());
  }
  return 0;
}

}} // nx8v::bindings
