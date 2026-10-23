#pragma once
// nx8v/src/bindings/list.h
// `nx8v list` — daftar proses nx8v yang sedang listen di sebuah port.
//
// Cara kerja: ambil tabel TCP listener milik Windows (GetExtendedTcpTable),
// saring yang PID-nya nx8v.exe, lalu tampilkan port + working directory
// proyek (dibaca dari project.json di folder kerja proses tersebut).

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winternl.h>
#endif

#include "manifest.h"

namespace fs = std::filesystem;

namespace nx8v {
namespace bindings {

struct RunningServer {
  unsigned long pid = 0;
  int           port = 0;
  std::string   project;   // name from project.json (or folder name)
  std::string   dir;
};

#ifdef _WIN32
// Returns the full image path of a PID (empty on failure).
static std::string ProcessImagePath(unsigned long pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return "";
  char buf[MAX_PATH * 4];
  DWORD size = sizeof(buf);
  std::string out;
  if (QueryFullProcessImageNameA(h, 0, buf, &size)) out.assign(buf, size);
  CloseHandle(h);
  return out;
}

// Reads the process working directory via the PEB.
// x64 layout: PEB->ProcessParameters at +0x20,
//             RTL_USER_PROCESS_PARAMETERS->CurrentDirectory.DosPath at +0x38.
static std::string ProcessWorkingDir(unsigned long pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!h) return "";

  typedef LONG(WINAPI * NtQueryInformationProcessFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  auto fn = (NtQueryInformationProcessFn)GetProcAddress(
      GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");

  std::string out;
  PROCESS_BASIC_INFORMATION pbi{};
  if (fn && fn(h, 0 /*ProcessBasicInformation*/, &pbi, sizeof(pbi), nullptr) == 0) {
    void* params = nullptr;
    if (ReadProcessMemory(h, (BYTE*)pbi.PebBaseAddress + 0x20, &params,
                          sizeof(params), nullptr) && params) {
      UNICODE_STRING us{};
      if (ReadProcessMemory(h, (BYTE*)params + 0x38, &us, sizeof(us), nullptr) &&
          us.Buffer && us.Length > 0) {
        std::vector<wchar_t> wbuf(us.Length / sizeof(wchar_t) + 1, 0);
        if (ReadProcessMemory(h, us.Buffer, wbuf.data(),
                              us.Length, nullptr)) {
          int n = WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), -1,
                                      nullptr, 0, nullptr, nullptr);
          if (n > 0) {
            out.resize((size_t)n - 1);
            WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), -1, out.data(), n,
                                nullptr, nullptr);
          }
        }
      }
    }
  }
  CloseHandle(h);
  return out;
}

static std::string ProcessCommandLine(unsigned long pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return "";

  typedef LONG(WINAPI * NtQueryInformationProcessFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  auto fn = (NtQueryInformationProcessFn)GetProcAddress(
      GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
  std::string out;
  if (fn) {
    // ProcessCommandLineInformation = 60
    ULONG size = 0;
    fn(h, 60, nullptr, 0, &size);
    if (size > 0) {
      std::vector<char> buf(size);
      if (fn(h, 60, buf.data(), size, &size) == 0) {
        auto* us = reinterpret_cast<PUNICODE_STRING>(buf.data());
        if (us->Buffer && us->Length > 0) {
          int n = WideCharToMultiByte(CP_UTF8, 0, us->Buffer, us->Length / sizeof(WCHAR),
                                      nullptr, 0, nullptr, nullptr);
          if (n > 0) {
            out.resize((size_t)n);
            WideCharToMultiByte(CP_UTF8, 0, us->Buffer, us->Length / sizeof(WCHAR),
                                out.data(), n, nullptr, nullptr);
          }
        }
      }
    }
  }
  CloseHandle(h);
  return out;
}
#endif

// Enumerate listening TCP ports owned by nx8v.exe.
inline std::vector<RunningServer> ListRunningServers() {
  std::vector<RunningServer> out;
#ifdef _WIN32
  DWORD size = 0;
  GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                      TCP_TABLE_OWNER_PID_LISTENER, 0);
  if (size == 0) return out;

  std::vector<char> buf(size);
  if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET,
                          TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
    return out;

  auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
  for (DWORD i = 0; i < table->dwNumEntries; ++i) {
    const auto& row = table->table[i];
    unsigned long pid = row.dwOwningPid;
    if (pid == 0) continue;

    std::string image = ProcessImagePath(pid);
    if (image.empty()) continue;

    // Match ...\nx8v.exe (case-insensitive)
    std::string lower = image;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    if (lower.size() < 8 || lower.compare(lower.size() - 8, 8, "nx8v.exe") != 0)
      continue;

    RunningServer srv;
    srv.pid  = pid;
    srv.port = (int)ntohs((u_short)row.dwLocalPort);

    // Avoid duplicates (IPv4/IPv6 rows) for the same pid+port.
    bool dup = false;
    for (const auto& s : out)
      if (s.pid == srv.pid && s.port == srv.port) { dup = true; break; }
    if (dup) continue;

    // Project info from the command line: "<exe>" <script> [args]
    // Split into tokens honouring quotes, skip token 0 (the exe itself).
    std::string cmd = ProcessCommandLine(pid);
    std::string script;
    if (!cmd.empty()) {
      std::vector<std::string> tokens;
      std::string cur;
      bool in_quotes = false;
      for (char c : cmd) {
        if (c == '"') { in_quotes = !in_quotes; continue; }
        if (!in_quotes && (c == ' ' || c == '\t')) {
          if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
          continue;
        }
        cur += c;
      }
      if (!cur.empty()) tokens.push_back(cur);
      if (tokens.size() >= 2) script = tokens[1];
    }
    if (!script.empty()) {
      std::error_code ec;
      fs::path sp(script);
      if (sp.is_relative()) {
        // Resolve against the process working directory (read from its PEB).
        std::string cwd = ProcessWorkingDir(pid);
        if (!cwd.empty()) sp = fs::path(cwd) / sp;
      }
      if (fs::exists(sp, ec)) {
        srv.dir = sp.parent_path().string();
        fs::path manifest = sp.parent_path() / "project.json";
        if (fs::exists(manifest, ec)) {
          std::string json = ReadManifestFile(manifest);
          std::string name;
          if (JsonFindString(json, "name", name) && !name.empty())
            srv.project = name;
        }
        if (srv.project.empty())
          srv.project = sp.parent_path().filename().string();
      } else {
        // Script not found; still show the cwd we know about.
        std::string cwd = ProcessWorkingDir(pid);
        if (!cwd.empty()) {
          srv.dir = cwd;
          srv.project = fs::path(cwd).filename().string();
        }
      }
    }

    out.push_back(srv);
  }
#endif
  return out;
}

inline int RunList(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  auto servers = ListRunningServers();
  if (servers.empty()) {
    std::printf("Tidak ada server nx8v yang sedang berjalan.\n");
    return 0;
  }

  std::printf("%-8s %-7s %-22s %s\n", "PID", "PORT", "PROJECT", "DIR");
  std::printf("%-8s %-7s %-22s %s\n", "-------", "------", "----------------------",
              "---");
  for (const auto& s : servers) {
    std::printf("%-8lu %-7d %-22s %s\n", s.pid, s.port,
                s.project.empty() ? "(unknown)" : s.project.c_str(),
                s.dir.empty() ? "(unknown)" : s.dir.c_str());
  }
  std::printf("\nHentikan dengan: nx8v stop --port=<PORT>\n");
  return 0;
}

}} // nx8v::bindings
