#pragma once
// nx8v/src/bindings/manifest.h
// Minimal project.json reader for `nx8v start`.
//
// Supported shape:
//   {
//     "main": "server.js",
//     "nx8v": { "port": 4000 }
//   }

#include <cstdlib>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace nx8v {
namespace bindings {

// Mega-repo modul internal NX8V. project.json boleh menimpa url ini
// saat dependensi dipublikasikan dari repositori milik user.
inline constexpr const char kNx8vDepsRepository[] =
    "https://github.com/nx8v/dependencies.git";

inline std::string ReadManifestFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Crude extraction of a top-level string field:  "main": "server.js"
// Skips string literals / comments naively — good enough for generated files.
inline bool JsonFindString(const std::string& json,
                           const std::string& key,
                           std::string& out) {
  std::string needle = "\"" + key + "\"";
  size_t k = json.find(needle);
  if (k == std::string::npos) return false;
  size_t colon = json.find(':', k + needle.size());
  if (colon == std::string::npos) return false;
  size_t q1 = json.find('"', colon + 1);
  if (q1 == std::string::npos) return false;
  size_t q2 = q1 + 1;
  while (q2 < json.size()) {
    if (json[q2] == '\\' && q2 + 1 < json.size()) { q2 += 2; continue; }
    if (json[q2] == '"') break;
    ++q2;
  }
  if (q2 >= json.size()) return false;
  out = json.substr(q1 + 1, q2 - q1 - 1);
  return true;
}

inline bool JsonFindBool(const std::string& json,
                         const std::string& key,
                         bool& out) {
  std::string needle = "\"" + key + "\"";
  size_t k = json.find(needle);
  if (k == std::string::npos) return false;
  size_t colon = json.find(':', k + needle.size());
  if (colon == std::string::npos) return false;
  size_t i = colon + 1;
  while (i < json.size() &&
         (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r'))
    ++i;
  if (json.compare(i, 4, "true") == 0) { out = true; return true; }
  if (json.compare(i, 5, "false") == 0) { out = false; return true; }
  return false;
}

inline bool JsonFindNumber(const std::string& json,
                           const std::string& key,
                           int& out) {
  std::string needle = "\"" + key + "\"";
  size_t k = json.find(needle);
  if (k == std::string::npos) return false;
  size_t colon = json.find(':', k + needle.size());
  if (colon == std::string::npos) return false;
  size_t i = colon + 1;
  while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
  if (i >= json.size() || (json[i] < '0' || json[i] > '9')) return false;
  out = std::atoi(json.c_str() + i);
  return true;
}

// Chromium / WebView2 refuse these ports (ERR_UNSAFE_PORT). 6000 is one of them.
inline bool Nx8vIsUnsafePort(int port) {
  static const int kUnsafe[] = {
    1,7,9,11,13,15,17,19,21,23,25,37,42,43,53,69,77,79,87,95,101,102,103,104,
    109,110,111,113,115,117,119,123,135,137,139,143,161,179,389,427,465,512,
    513,514,515,526,530,531,532,540,548,554,556,563,587,601,636,989,990,993,
    995,1719,1720,1723,2049,3659,4045,5060,5061,6000,6566,6665,6666,6667,6668,
    6669,6697,10080
  };
  for (int p : kUnsafe) if (p == port) return true;
  return false;
}

// The next port WebView2 will open. 6000 becomes 6001.
inline int Nx8vSafeListenPort(int port) {
  if (port <= 0 || port > 65535) port = 4000;
  while (port <= 65535 && Nx8vIsUnsafePort(port)) ++port;
  if (port > 65535) return 4000;
  return port;
}

// When a script runs inside a project, expose nx8v.port as NX8V_PORT
// (unless the environment already has it) and return the log file path.
inline void ApplyProjectEnv(const std::string& script_path, std::string& log_path) {
  std::error_code ec;
  fs::path manifest;
  if (!script_path.empty()) {
    fs::path script = fs::absolute(fs::path(script_path), ec);
    fs::path beside = script.parent_path() / "project.json";
    if (fs::exists(beside, ec)) manifest = beside;
  }
  if (manifest.empty()) {
    fs::path cwd_manifest = fs::current_path(ec) / "project.json";
    if (fs::exists(cwd_manifest, ec)) manifest = cwd_manifest;
  }
  if (manifest.empty()) return;

  std::string json = ReadManifestFile(manifest);
  int port = 0;
  JsonFindNumber(json, "port", port);
  if (port > 0 && !std::getenv("NX8V_PORT")) {
    std::string port_str = std::to_string(port);
#ifdef _WIN32
    _putenv_s("NX8V_PORT", port_str.c_str());
#else
    setenv("NX8V_PORT", port_str.c_str(), 0);
#endif
  }
  log_path = (manifest.parent_path() / "nx8v.log").string();
}

// The object value of a top-level key, including the surrounding braces.
inline bool JsonFindObjectBody(const std::string& json, const std::string& key,
                               std::string& out) {
  std::string needle = "\"" + key + "\"";
  size_t k = json.find(needle);
  if (k == std::string::npos) return false;
  size_t brace = json.find('{', k + needle.size());
  if (brace == std::string::npos) return false;
  int depth = 0;
  bool in_str = false;
  for (size_t i = brace; i < json.size(); ++i) {
    char c = json[i];
    if (in_str) {
      if (c == '\\' && i + 1 < json.size()) { ++i; continue; }
      if (c == '"') in_str = false;
      continue;
    }
    if (c == '"') { in_str = true; continue; }
    if (c == '{') ++depth;
    else if (c == '}') {
      --depth;
      if (depth == 0) {
        out = json.substr(brace, i - brace + 1);
        return true;
      }
    }
  }
  return false;
}

// "dependencies" in project.json. Only listed names are mapped to a URL.
// ./images/32x32.png is also accepted from public/assets/ or public/.
inline fs::path Nx8vResolveIconPath(const fs::path& root, const std::string& rel) {
  std::error_code ec;
  fs::path direct = root / rel;
  if (fs::is_regular_file(direct, ec)) return direct;
  std::string trimmed = rel;
  while (trimmed.size() >= 2 && trimmed[0] == '.' &&
         (trimmed[1] == '/' || trimmed[1] == '\\'))
    trimmed.erase(0, 2);
  while (!trimmed.empty() && (trimmed[0] == '/' || trimmed[0] == '\\'))
    trimmed.erase(0, 1);
  fs::path in_assets = root / "public" / "assets" / trimmed;
  if (fs::is_regular_file(in_assets, ec)) return in_assets;
  fs::path in_public = root / "public" / trimmed;
  if (fs::is_regular_file(in_public, ec)) return in_public;
  return direct;
}

}} // nx8v::bindings
