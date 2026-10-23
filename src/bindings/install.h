#pragma once
// nx8v/src/bindings/install.h
// `nx8v i <nama>@<versi>` installs one module from project.json "repository".
// `nx8v u <nama>@<versi>` updates an installed module to that version.
// `nx8v p <nama>@<versi>` publishes modules/<nama> as a GitHub release.
// `nx8v d <nama>` removes the module from dependencies, the lock, and modules/.
// `nx8v r <nama>` deletes that module's releases, tags, and folder from the repository.
//
//   "repository": { "type": "git", "url": "https://github.com/nx8v/dependencies.git" }
//
// type git on GitHub: release tag <nama>-<versi> and its assets.
//   https://github.com/nx8v/dependencies/releases/tag/cotoh-1.0.0
//   asset index.js -> modules/cotoh/index.js
// dependencies records the version. nx8v.lock records the release page.

#include "manifest.h"

#include <chrono>
#include <cctype>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace nx8v {
namespace bindings {

static bool InstallNameOk(const std::string& s) {
  if (s.empty() || s == "." || s == "..") return false;
  if (s[0] == '.') return false;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '_' || c == '-' || c == '.') continue;
    return false;
  }
  return true;
}

static std::string InstallEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

static bool InstallUpsertDep(std::string& json, const std::string& name,
                             const std::string& version) {
  std::string needle = "\"dependencies\"";
  size_t key = json.find(needle);
  if (key == std::string::npos) return false;
  size_t brace = json.find('{', key + needle.size());
  if (brace == std::string::npos) return false;
  int depth = 0;
  bool in_str = false;
  size_t end = std::string::npos;
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
      if (depth == 0) { end = i; break; }
    }
  }
  if (end == std::string::npos) return false;

  std::string obj = json.substr(brace, end - brace + 1);
  std::string nl = obj.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  std::string quoted = "\"" + name + "\"";
  size_t found = obj.find(quoted);
  if (found != std::string::npos) {
    size_t colon = obj.find(':', found + quoted.size());
    if (colon == std::string::npos) return false;
    size_t q1 = obj.find('"', colon + 1);
    if (q1 == std::string::npos) return false;
    size_t q2 = q1 + 1;
    while (q2 < obj.size()) {
      if (obj[q2] == '\\' && q2 + 1 < obj.size()) { q2 += 2; continue; }
      if (obj[q2] == '"') break;
      ++q2;
    }
    if (q2 >= obj.size()) return false;
    obj.replace(q1 + 1, q2 - q1 - 1, InstallEscape(version));
  } else {
    size_t close = obj.rfind('}');
    size_t content = close;
    while (content > 0 && std::isspace((unsigned char)obj[content - 1])) --content;
    bool empty = content == 1;
    std::string entry = nl + "    \"" + name + "\": \"" + InstallEscape(version) + "\"";
    if (!empty) entry = "," + entry;
    else entry += nl + "  ";
    obj.insert(content, entry);
  }
  json.replace(brace, end - brace + 1, obj);
  return true;
}

static int InstallExec(const std::string& program, const std::vector<std::string>& args,
                       const std::string& cwd = {}) {
#ifdef _WIN32
  std::wstring cmd;
  for (unsigned char c : program) cmd.push_back((wchar_t)c);
  for (const auto& a : args) {
    cmd += L" \"";
    for (unsigned char c : a) {
      wchar_t w = (wchar_t)c;
      if (w == L'"') cmd += L"\\\"";
      else cmd.push_back(w);
    }
    cmd += L"\"";
  }
  std::wstring cwd_w;
  const wchar_t* cwd_ptr = nullptr;
  if (!cwd.empty()) {
    cwd_w.assign(cwd.begin(), cwd.end());
    cwd_ptr = cwd_w.c_str();
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(0);
  if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      cwd_ptr, &si, &pi))
    return -1;
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return (int)code;
#else
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
    std::vector<std::string> store;
    store.push_back(program);
    store.insert(store.end(), args.begin(), args.end());
    std::vector<char*> av;
    for (auto& a : store) av.push_back(a.data());
    av.push_back(nullptr);
    execvp(program.c_str(), av.data());
    _exit(127);
  }
  int st = 1;
  if (waitpid(pid, &st, 0) < 0) return -1;
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  return 1;
#endif
}

static bool InstallCurl(const std::string& url, const fs::path& dest) {
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
#ifdef _WIN32
  const char* prog = "curl.exe";
#else
  const char* prog = "curl";
#endif
  int rc = InstallExec(prog, {"-fsL", "-A", "nx8v", "-o", dest.string(), url});
  if (rc != 0) {
    fs::remove(dest, ec);
    return false;
  }
  return fs::is_regular_file(dest, ec) && fs::file_size(dest, ec) > 0;
}

static bool InstallJsonStringAt(const std::string& json, size_t key_pos, std::string& out) {
  size_t colon = json.find(':', key_pos);
  if (colon == std::string::npos) return false;
  size_t q1 = json.find('"', colon + 1);
  if (q1 == std::string::npos) return false;
  std::string val;
  for (size_t i = q1 + 1; i < json.size(); ++i) {
    char c = json[i];
    if (c == '\\' && i + 1 < json.size()) {
      char n = json[++i];
      if (n == '"' || n == '\\' || n == '/') val.push_back(n);
      else val.push_back(n);
      continue;
    }
    if (c == '"') { out = val; return true; }
    val.push_back(c);
  }
  return false;
}

static bool InstallAssetNameOk(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  if (name.find("..") != std::string::npos) return false;
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
      name.find(':') != std::string::npos)
    return false;
  return true;
}

// https://github.com/nx8v/dependencies.git -> owner nx8v, repo dependencies
static bool InstallGithub(const std::string& url, std::string& owner, std::string& repo) {
  std::string s = url;
  if (s.size() >= 4 && s.compare(s.size() - 4, 4, ".git") == 0) s.resize(s.size() - 4);
  while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
  auto at = s.find("github.com");
  if (at == std::string::npos) return false;
  std::string rest = s.substr(at + 10);
  if (!rest.empty() && (rest[0] == '/' || rest[0] == ':')) rest.erase(0, 1);
  auto slash = rest.find('/');
  if (slash == std::string::npos || slash == 0) return false;
  owner = rest.substr(0, slash);
  repo = rest.substr(slash + 1);
  auto cut = repo.find_first_of("/?#");
  if (cut != std::string::npos) repo.resize(cut);
  return !owner.empty() && !repo.empty();
}

static bool InstallGithubRelease(const std::string& owner, const std::string& repo,
                                 const std::string& tag, const fs::path& stage) {
  std::error_code ec;
  fs::create_directories(stage, ec);
  std::string api = "https://api.github.com/repos/" + owner + "/" + repo +
                    "/releases/tags/" + tag;
  fs::path api_file = stage.parent_path() / "release.json";
  if (InstallCurl(api, api_file)) {
    std::string json = ReadManifestFile(api_file);
    size_t assets = json.find("\"assets\"");
    size_t pos = assets == std::string::npos ? json.size() : assets;
    while (pos < json.size()) {
      size_t url_key = json.find("\"browser_download_url\"", pos);
      if (url_key == std::string::npos) break;
      size_t name_key = json.rfind("\"name\"", url_key);
      if (name_key == std::string::npos || name_key < pos) break;
      std::string file_name;
      std::string file_url;
      if (!InstallJsonStringAt(json, name_key, file_name) ||
          !InstallJsonStringAt(json, url_key, file_url) ||
          !InstallAssetNameOk(file_name)) {
        pos = url_key + 1;
        continue;
      }
      InstallCurl(file_url, stage / file_name);
      pos = url_key + 1;
    }
  }
  if (!fs::is_regular_file(stage / "index.js", ec)) {
    std::string direct = "https://github.com/" + owner + "/" + repo +
                         "/releases/download/" + tag + "/index.js";
    InstallCurl(direct, stage / "index.js");
  }
  return fs::is_regular_file(stage / "index.js", ec);
}

static bool InstallCollectReleaseTags(const std::string& owner, const std::string& repo,
                                     const fs::path& tmp, std::vector<std::string>& tags) {
  fs::path list = tmp / "releases.json";
  std::string api = "https://api.github.com/repos/" + owner + "/" + repo +
                    "/releases?per_page=100";
  if (!InstallCurl(api, list)) return false;
  std::string json = ReadManifestFile(list);
  size_t pos = 0;
  while (pos < json.size()) {
    size_t key = json.find("\"tag_name\"", pos);
    if (key == std::string::npos) break;
    std::string tag;
    if (InstallJsonStringAt(json, key, tag) && !tag.empty()) tags.push_back(tag);
    pos = key + 10;
  }
  return true;
}

static void InstallPrintReleaseTags(const std::string& owner, const std::string& repo,
                                    const fs::path& tmp) {
  std::vector<std::string> tags;
  std::cout << "Existing releases:\n";
  if (!InstallCollectReleaseTags(owner, repo, tmp, tags) || tags.empty()) {
    std::cout << "  (none)\n";
    return;
  }
  for (const auto& tag : tags) std::cout << "  " << tag << "\n";
}

static std::string InstallLockEntry(const std::string& version, const std::string& url) {
  return "{\n      \"version\": \"" + InstallEscape(version) +
         "\",\n      \"resolved\": \"" + InstallEscape(url) + "\"\n    }";
}

static bool InstallWriteLock(const fs::path& path, const std::string& name,
                             const std::string& version, const std::string& url) {
  std::string json = ReadManifestFile(path);
  bool blank = json.find_first_not_of(" \t\r\n") == std::string::npos;
  if (blank) {
    json = "{\n  \"dependencies\": {\n    \"" + name + "\": " +
           InstallLockEntry(version, url) + "\n  }\n}\n";
  } else {
    std::string needle = "\"dependencies\"";
    size_t key = json.find(needle);
    if (key == std::string::npos) return false;
    size_t brace = json.find('{', key + needle.size());
    if (brace == std::string::npos) return false;
    int depth = 0;
    bool in_str = false;
    size_t end = std::string::npos;
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
        if (depth == 0) { end = i; break; }
      }
    }
    if (end == std::string::npos) return false;
    std::string obj = json.substr(brace, end - brace + 1);
    std::string nl = obj.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string quoted = "\"" + name + "\"";
    size_t found = obj.find(quoted);
    std::string entry = InstallLockEntry(version, url);
    if (found != std::string::npos) {
      size_t colon = obj.find(':', found + quoted.size());
      if (colon == std::string::npos) return false;
      size_t open = obj.find('{', colon + 1);
      if (open == std::string::npos) return false;
      int d = 0;
      bool str = false;
      size_t close = std::string::npos;
      for (size_t i = open; i < obj.size(); ++i) {
        char c = obj[i];
        if (str) {
          if (c == '\\' && i + 1 < obj.size()) { ++i; continue; }
          if (c == '"') str = false;
          continue;
        }
        if (c == '"') { str = true; continue; }
        if (c == '{') ++d;
        else if (c == '}') {
          --d;
          if (d == 0) { close = i; break; }
        }
      }
      if (close == std::string::npos) return false;
      obj.replace(open, close - open + 1, entry);
    } else {
      size_t close = obj.rfind('}');
      size_t content = close;
      while (content > 0 && std::isspace((unsigned char)obj[content - 1])) --content;
      bool empty = content == 1;
      std::string line = nl + "    \"" + name + "\": " + entry;
      if (!empty) line = "," + line;
      else line += nl + "  ";
      obj.insert(content, line);
    }
    json.replace(brace, end - brace + 1, obj);
    if (json.empty() || json.back() != '\n') json.push_back('\n');
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << json;
  return true;
}

static bool InstallCopyTree(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to, ec);
  if (ec) return false;
  for (auto it = fs::recursive_directory_iterator(from, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    fs::path rel = fs::relative(it->path(), from, ec);
    if (ec) return false;
    bool skip = false;
    for (const auto& part : rel) {
      if (part == ".git") { skip = true; break; }
    }
    if (skip) continue;
    fs::path dest = to / rel;
    if (it->is_directory(ec)) {
      fs::create_directories(dest, ec);
      if (ec) return false;
    } else if (it->is_regular_file(ec)) {
      fs::create_directories(dest.parent_path(), ec);
      fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing, ec);
      if (ec) return false;
    }
  }
  return !ec;
}

static bool InstallSkipValue(const std::string& s, size_t i, size_t& end) {
  while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
  if (i >= s.size()) return false;
  if (s[i] == '"') {
    for (size_t k = i + 1; k < s.size(); ++k) {
      if (s[k] == '\\' && k + 1 < s.size()) { ++k; continue; }
      if (s[k] == '"') { end = k + 1; return true; }
    }
    return false;
  }
  if (s[i] == '{' || s[i] == '[') {
    int depth = 0;
    bool str = false;
    for (size_t k = i; k < s.size(); ++k) {
      char c = s[k];
      if (str) {
        if (c == '\\' && k + 1 < s.size()) { ++k; continue; }
        if (c == '"') str = false;
        continue;
      }
      if (c == '"') { str = true; continue; }
      if (c == '{' || c == '[') ++depth;
      else if (c == '}' || c == ']') {
        --depth;
        if (depth == 0) { end = k + 1; return true; }
      }
    }
    return false;
  }
  size_t k = i;
  while (k < s.size() && !std::isspace((unsigned char)s[k]) &&
         s[k] != ',' && s[k] != '}' && s[k] != ']')
    ++k;
  if (k == i) return false;
  end = k;
  return true;
}

static bool InstallEraseKey(std::string& obj, const std::string& name) {
  std::string quoted = "\"" + name + "\"";
  size_t found = obj.find(quoted);
  if (found == std::string::npos) return false;
  size_t colon = obj.find(':', found + quoted.size());
  if (colon == std::string::npos) return false;
  size_t vend = 0;
  if (!InstallSkipValue(obj, colon + 1, vend)) return false;
  size_t start = found;
  size_t end = vend;
  size_t pre = start;
  while (pre > 0 && std::isspace((unsigned char)obj[pre - 1])) --pre;
  if (pre > 0 && obj[pre - 1] == ',') {
    start = pre - 1;
  } else {
    while (start > 0 && (obj[start - 1] == ' ' || obj[start - 1] == '\t')) --start;
    if (start > 0 && (obj[start - 1] == '\n' || obj[start - 1] == '\r')) {
      if (start > 1 && obj[start - 2] == '\r') start -= 2;
      else --start;
    }
    size_t post = end;
    while (post < obj.size() && std::isspace((unsigned char)obj[post])) ++post;
    if (post < obj.size() && obj[post] == ',') end = post + 1;
  }
  obj.erase(start, end - start);
  return true;
}

static bool InstallDropDep(std::string& json, const std::string& name) {
  std::string needle = "\"dependencies\"";
  size_t key = json.find(needle);
  if (key == std::string::npos) return false;
  size_t brace = json.find('{', key + needle.size());
  if (brace == std::string::npos) return false;
  int depth = 0;
  bool in_str = false;
  size_t end = std::string::npos;
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
      if (depth == 0) { end = i; break; }
    }
  }
  if (end == std::string::npos) return false;
  std::string obj = json.substr(brace, end - brace + 1);
  if (!InstallEraseKey(obj, name)) return false;
  json.replace(brace, end - brace + 1, obj);
  return true;
}

static bool InstallListed(const std::string& json, const std::string& name) {
  std::string obj;
  std::string version;
  if (!JsonFindObjectBody(json, "dependencies", obj)) return false;
  return JsonFindString(obj, name, version) && !version.empty();
}

static int InstallOrUpdate(int argc, char* argv[], bool update) {
  const char* usage = update ? "Usage: nx8v u <name>@<version>"
                             : "Usage: nx8v i <name>@<version>";
  if (argc < 3) {
    std::puts(usage);
    std::puts("  Reads repository.type and repository.url in project.json.");
    return 1;
  }
  std::string spec = argv[2];
  auto at = spec.rfind('@');
  if (at == std::string::npos || at == 0 || at + 1 >= spec.size()) {
    std::puts(usage);
    return 1;
  }
  std::string name = spec.substr(0, at);
  std::string version = spec.substr(at + 1);
  if (!InstallNameOk(name) || !InstallNameOk(version)) {
    std::puts("Name and version may contain only letters, digits, dots, underscores, or hyphens.");
    return 1;
  }

  std::error_code ec;
  fs::path root = fs::current_path(ec);
  fs::path manifest = root / "project.json";
  std::string json = ReadManifestFile(manifest);
  if (json.empty()) {
    std::puts("project.json was not found. Run this command in a project folder.");
    return 1;
  }
  std::string repo;
  if (!JsonFindObjectBody(json, "repository", repo)) {
    std::puts("project.json has no repository.");
    return 1;
  }
  std::string type;
  std::string url;
  JsonFindString(repo, "type", type);
  JsonFindString(repo, "url", url);
  if (type.empty()) type = "git";
  if (type != "git") {
    std::puts("repository.type currently supported: git");
    return 1;
  }
  if (url.empty()) url = kNx8vDepsRepository;
  if (update && !InstallListed(json, name)) {
    std::cout << name << " is not in dependencies. Install it with nx8v i "
              << name << "@" << version << "\n";
    return 1;
  }

  auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  fs::path tmp = fs::temp_directory_path(ec) / ("nx8v-i-" + std::to_string(ticks));
  fs::remove_all(tmp, ec);
  struct TmpGuard {
    fs::path path;
    ~TmpGuard() {
      std::error_code e;
      if (!path.empty()) fs::remove_all(path, e);
    }
  } guard{tmp};

  std::string owner;
  std::string repo_name;
  std::string tag = name + "-" + version;
  std::string resolved = url;
  fs::path src = tmp / "files";
  if (InstallGithub(url, owner, repo_name)) {
    resolved = "https://github.com/" + owner + "/" + repo_name + "/releases/tag/" + tag;
    std::cout << "fetching " << name << "@" << version << " from " << resolved << std::endl;
    if (!InstallGithubRelease(owner, repo_name, tag, src)) {
      std::cout << "release " << tag << " was not found, or it has no index.js\n";
      std::cout << resolved << "\n";
      InstallPrintReleaseTags(owner, repo_name, tmp);
      return 1;
    }
  } else {
    std::cout << "fetching " << name << "@" << version << " from " << url << std::endl;
    int rc = InstallExec("git", {"clone", "--depth", "1", "--branch", tag, url, src.string()});
    if (rc == -1) {
      std::puts("git was not found. Install Git, then run the command again.");
      return 1;
    }
    if (rc != 0 || !fs::is_regular_file(src / "index.js", ec)) {
      std::cout << "tag " << tag << " has no index.js\n";
      return 1;
    }
  }
  fs::path dest = root / "modules" / name;
  fs::remove_all(dest, ec);
  if (!InstallCopyTree(src, dest)) {
    std::puts("failed to copy the module into modules/.");
    return 1;
  }
  if (!InstallUpsertDep(json, name, version)) {
    std::puts("failed to write dependencies in project.json.");
    return 1;
  }
  std::ofstream out(manifest, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::puts("failed to save project.json.");
    return 1;
  }
  out << json;
  out.close();
  if (!InstallWriteLock(root / "nx8v.lock", name, version, resolved)) {
    std::puts("failed to write nx8v.lock.");
    return 1;
  }
  std::cout << (update ? "updated " : "installed ")
            << name << "@" << version << " -> modules/" << name << "\n";
  return 0;
}

inline int RunInstall(int argc, char* argv[]) { return InstallOrUpdate(argc, argv, false); }

inline int RunUpdate(int argc, char* argv[]) { return InstallOrUpdate(argc, argv, true); }

inline int RunRemove(int argc, char* argv[]) {
  if (argc < 3 || std::string(argv[2]).find('@') != std::string::npos) {
    std::puts("Usage: nx8v d <name>");
    return 1;
  }
  std::string name = argv[2];
  if (!InstallNameOk(name)) {
    std::puts("Name may contain only letters, digits, dots, underscores, or hyphens.");
    return 1;
  }
  std::error_code ec;
  fs::path root = fs::current_path(ec);
  fs::path manifest = root / "project.json";
  std::string json = ReadManifestFile(manifest);
  if (json.empty()) {
    std::puts("project.json was not found. Run this command in a project folder.");
    return 1;
  }
  if (!InstallListed(json, name)) {
    std::cout << name << " is not in dependencies.\n";
    return 1;
  }
  if (!InstallDropDep(json, name)) {
    std::puts("failed to remove the dependency from project.json.");
    return 1;
  }
  std::ofstream out(manifest, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::puts("failed to save project.json.");
    return 1;
  }
  out << json;
  out.close();

  fs::path lock = root / "nx8v.lock";
  std::string lock_json = ReadManifestFile(lock);
  if (!lock_json.empty() && InstallDropDep(lock_json, name)) {
    std::ofstream lout(lock, std::ios::binary | std::ios::trunc);
    if (lout) lout << lock_json;
  }
  fs::remove_all(root / "modules" / name, ec);
  std::cout << "removed " << name << " from dependencies\n";
  return 0;
}

inline int RunPublish(int argc, char* argv[]) {
  if (argc < 3) {
    std::puts("Usage: nx8v p <name>@<version>");
    std::puts("  Publishes the files in modules/<name> to the repository in project.json.");
    return 1;
  }
  std::string spec = argv[2];
  auto at = spec.rfind('@');
  if (at == std::string::npos || at == 0 || at + 1 >= spec.size()) {
    std::puts("Usage: nx8v p <name>@<version>");
    return 1;
  }
  std::string name = spec.substr(0, at);
  std::string version = spec.substr(at + 1);
  if (!InstallNameOk(name) || !InstallNameOk(version)) {
    std::puts("Name and version may contain only letters, digits, dots, underscores, or hyphens.");
    return 1;
  }

  std::error_code ec;
  fs::path root = fs::current_path(ec);
  fs::path mod = root / "modules" / name;
  if (!fs::is_regular_file(mod / "index.js", ec)) {
    std::cout << "modules/" << name << "/index.js was not found.\n";
    return 1;
  }
  fs::path manifest = root / "project.json";
  std::string json = ReadManifestFile(manifest);
  if (json.empty()) {
    std::puts("project.json was not found. Run this command in a project folder.");
    return 1;
  }
  std::string repo;
  if (!JsonFindObjectBody(json, "repository", repo)) {
    std::puts("project.json has no repository.");
    return 1;
  }
  std::string type;
  std::string url;
  JsonFindString(repo, "type", type);
  JsonFindString(repo, "url", url);
  if (type.empty()) type = "git";
  if (type != "git") {
    std::puts("repository.type currently supported: git");
    return 1;
  }
  if (url.empty()) url = kNx8vDepsRepository;
  std::string owner;
  std::string repo_name;
  if (!InstallGithub(url, owner, repo_name)) {
    std::puts("repository.url must be a GitHub repository to publish a release.");
    return 1;
  }

  std::vector<std::string> files;
  for (auto it = fs::directory_iterator(mod, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    std::string base = it->path().filename().string();
    if (base == ".git" || base.empty() || base[0] == '.') continue;
    files.push_back(it->path().string());
  }
  if (files.empty()) {
    std::cout << "modules/" << name << " contains no files.\n";
    return 1;
  }

#ifdef _WIN32
  const char* gh = "gh.exe";
#else
  const char* gh = "gh";
#endif
  if (InstallExec(gh, {"api", "user", "--silent"}) != 0) {
    std::puts("gh is not logged in. Run: gh auth login");
    return 1;
  }

  std::string tag = name + "-" + version;
  std::string resolved = "https://github.com/" + owner + "/" + repo_name + "/releases/tag/" + tag;
  std::cout << "publishing " << name << "@" << version << " to " << resolved << std::endl;
  std::vector<std::string> args = {
      "release", "create", tag,
      "--repo", owner + "/" + repo_name,
      "--title", name + " " + version,
      "--notes", "NX8V module " + name + " v" + version};
  args.insert(args.end(), files.begin(), files.end());
  int rc = InstallExec(gh, args);
  if (rc == -1) {
    std::puts("gh was not found. Install GitHub CLI, then run the command again.");
    return 1;
  }
  if (rc != 0) {
    std::cout << "failed to publish release " << tag << "\n";
    return 1;
  }
  if (!InstallUpsertDep(json, name, version)) {
    std::puts("the release was published, but dependencies in project.json were not written.");
    return 1;
  }
  std::ofstream out(manifest, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::puts("the release was published, but project.json was not saved.");
    return 1;
  }
  out << json;
  out.close();
  if (!InstallWriteLock(root / "nx8v.lock", name, version, resolved)) {
    std::puts("the release was published, but nx8v.lock was not written.");
    return 1;
  }
  std::cout << "published " << name << "@" << version << "\n";
  return 0;
}

inline int RunRepoRemove(int argc, char* argv[]) {
  if (argc < 3 || std::string(argv[2]).find('@') != std::string::npos) {
    std::puts("Usage: nx8v r <name>");
    std::puts("  Deletes the module release, tag, and folder from the repository in project.json.");
    return 1;
  }
  std::string name = argv[2];
  if (!InstallNameOk(name)) {
    std::puts("Name may contain only letters, digits, dots, underscores, or hyphens.");
    return 1;
  }

  std::error_code ec;
  fs::path root = fs::current_path(ec);
  std::string json = ReadManifestFile(root / "project.json");
  if (json.empty()) {
    std::puts("project.json was not found. Run this command in a project folder.");
    return 1;
  }
  std::string repo;
  if (!JsonFindObjectBody(json, "repository", repo)) {
    std::puts("project.json has no repository.");
    return 1;
  }
  std::string type;
  std::string url;
  JsonFindString(repo, "type", type);
  JsonFindString(repo, "url", url);
  if (type.empty()) type = "git";
  if (type != "git") {
    std::puts("repository.type currently supported: git");
    return 1;
  }
  if (url.empty()) url = kNx8vDepsRepository;
  std::string owner;
  std::string repo_name;
  if (!InstallGithub(url, owner, repo_name)) {
    std::puts("repository.url must be a GitHub repository to delete a release.");
    return 1;
  }

#ifdef _WIN32
  const char* gh = "gh.exe";
#else
  const char* gh = "gh";
#endif
  if (InstallExec(gh, {"api", "user", "--silent"}) != 0) {
    std::puts("gh is not logged in. Run: gh auth login");
    return 1;
  }

  auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  fs::path tmp = fs::temp_directory_path(ec) / ("nx8v-r-" + std::to_string(ticks));
  fs::remove_all(tmp, ec);
  struct TmpGuard {
    fs::path path;
    ~TmpGuard() {
      std::error_code e;
      if (!path.empty()) fs::remove_all(path, e);
    }
  } guard{tmp};
  fs::create_directories(tmp, ec);

  std::string repo_slug = owner + "/" + repo_name;
  std::cout << "removing " << name << " from https://github.com/" << repo_slug << std::endl;

  std::vector<std::string> tags;
  if (!InstallCollectReleaseTags(owner, repo_name, tmp, tags)) {
    std::puts("failed to read the release list.");
    return 1;
  }
  std::string prefix = name + "-";
  bool removed = false;
  for (const auto& tag : tags) {
    if (tag.size() <= prefix.size() || tag.compare(0, prefix.size(), prefix) != 0)
      continue;
    int rc = InstallExec(gh, {"release", "delete", tag, "--repo", repo_slug,
                              "--yes", "--cleanup-tag"});
    if (rc == -1) {
      std::puts("gh was not found. Install GitHub CLI, then run the command again.");
      return 1;
    }
    if (rc != 0) {
      std::cout << "failed to delete release " << tag << "\n";
      return 1;
    }
    std::cout << "deleted release " << tag << "\n";
    removed = true;
  }

  fs::path src = tmp / "repo";
  int clone_rc = InstallExec("git", {"clone", "--depth", "1", url, src.string()});
  if (clone_rc == -1) {
    std::puts("git was not found. Install Git, then run the command again.");
    return 1;
  }
  if (clone_rc != 0) {
    std::puts("failed to clone the repository.");
    return 1;
  }
  if (fs::is_directory(src / name, ec)) {
    int rm_rc = InstallExec("git", {"-C", src.string(), "rm", "-r", "--", name});
    if (rm_rc != 0) {
      std::cout << "failed to delete folder " << name << "\n";
      return 1;
    }
    int commit_rc = InstallExec("git", {"-C", src.string(), "commit", "-m",
                                        "Remove " + name + " from the repository."});
    if (commit_rc != 0) {
      std::puts("failed to commit the folder removal in the repository.");
      return 1;
    }
    int push_rc = InstallExec("git", {"-C", src.string(), "push", "origin", "HEAD"});
    if (push_rc != 0) {
      std::puts("failed to push the folder removal to the repository.");
      return 1;
    }
    std::cout << "deleted folder " << name << "\n";
    removed = true;
  }

  if (!removed) {
    std::cout << name << " was not found in the repository\n";
    InstallPrintReleaseTags(owner, repo_name, tmp);
    return 1;
  }
  std::cout << "removed " << name << " from the repository\n";
  return 0;
}

}} // nx8v::bindings
