#pragma once
// nx8v/src/bindings/logs.h
// `nx8v logs` — print the tail of nx8v.log in the current project.
// console.log/error append to that file when a project.json is nearby.

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace nx8v {
namespace bindings {

inline int RunLogs(int argc, char** argv) {
  int want = 200;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--lines=", 0) == 0) {
      int n = std::atoi(a.c_str() + 8);
      if (n > 0) want = n;
    }
  }

  std::error_code ec;
  fs::path log = fs::current_path(ec) / "nx8v.log";
  if (!fs::exists(log, ec)) {
    std::puts("nx8v logs: no nx8v.log in this folder.");
    std::puts("The log is written while the application runs in a folder that has project.json.");
    return 0;
  }

  std::ifstream in(log, std::ios::binary);
  if (!in) {
    std::puts("nx8v logs: cannot read nx8v.log");
    return 1;
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  int start = (int)lines.size() > want ? (int)lines.size() - want : 0;
  std::printf("nx8v logs: %s (%d lines)\n", log.string().c_str(), (int)lines.size());
  for (int i = start; i < (int)lines.size(); ++i)
    std::puts(lines[i].c_str());
  return 0;
}

}}  // nx8v::bindings
