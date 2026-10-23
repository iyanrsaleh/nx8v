#pragma once
// nx8v/src/bindings/tls.h
// HTTPS transport for Windows, implemented with WinHTTP.
//
// WinHTTP handles TLS 1.2/1.3, certificate validation against the Windows
// store, chunked transfer decoding and redirects, so we do not have to drive
// Schannel by hand. Calls are blocking, so they run on a libuv worker thread
// (uv_queue_work) and never block the event loop.
//
// The worker returns a synthesized raw HTTP response string
// ("HTTP/1.1 200 OK\r\n...\r\n\r\n<body>") which the normal client parser
// consumes — so https and http share all response handling.

#include <uv.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")
#elif defined(NX8V_USE_OPENSSL)
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#endif

namespace nx8v {
namespace bindings {

struct HttpsJob {
  uv_work_t   work;
  std::string host;
  int         port = 443;
  std::string path = "/";
  std::string method = "GET";
  std::map<std::string, std::string> headers;
  std::string body;
  bool        reject_unauthorized = true;
  int         timeout_ms = 30000;
  void*       owner = nullptr;   // ClientReq* (opaque here)

  // results
  std::string raw_response;   // synthesized HTTP/1.1 response
  std::string error;
};

#ifdef _WIN32

static std::wstring HttpsWiden(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}

static std::string HttpsNarrow(const wchar_t* w, int len = -1) {
  if (!w) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return "";
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
  if (len == -1 && !s.empty() && s.back() == '\0') s.pop_back();
  return s;
}

// Blocking HTTPS request. Runs on a worker thread.
static void HttpsRun(HttpsJob* job) {
  HINTERNET session = nullptr, connect = nullptr, request = nullptr;

  auto fail = [&](const char* what) {
    DWORD err = GetLastError();
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s failed (WinHTTP error %lu)", what,
                  (unsigned long)err);
    job->error = buf;
  };

  do {
    session = WinHttpOpen(L"nx8v/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { fail("WinHttpOpen"); break; }

    if (job->timeout_ms > 0) {
      WinHttpSetTimeouts(session, job->timeout_ms, job->timeout_ms,
                         job->timeout_ms, job->timeout_ms);
    }

    std::wstring whost = HttpsWiden(job->host);
    connect = WinHttpConnect(session, whost.c_str(), (INTERNET_PORT)job->port, 0);
    if (!connect) { fail("WinHttpConnect"); break; }

    std::wstring wpath = HttpsWiden(job->path.empty() ? "/" : job->path);
    std::wstring wmethod = HttpsWiden(job->method);
    request = WinHttpOpenRequest(connect, wmethod.c_str(), wpath.c_str(),
                                 nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    if (!request) { fail("WinHttpOpenRequest"); break; }

    if (!job->reject_unauthorized) {
      DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                    SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                    SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                    SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
      WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    }

    // Headers
    std::string header_block;
    for (auto& kv : job->headers) {
      std::string k = kv.first;
      for (auto& c : k) c = (char)std::tolower((unsigned char)c);
      if (k == "connection" || k == "content-length" || k == "host") continue;
      header_block += kv.first + ": " + kv.second + "\r\n";
    }
    std::wstring wheaders = header_block.empty() ? std::wstring()
                                                 : HttpsWiden(header_block);

    BOOL ok = WinHttpSendRequest(
        request,
        header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wheaders.c_str(),
        header_block.empty() ? 0 : (DWORD)-1L,
        job->body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)job->body.data(),
        (DWORD)job->body.size(), (DWORD)job->body.size(), 0);
    if (!ok) { fail("WinHttpSendRequest"); break; }

    ok = WinHttpReceiveResponse(request, nullptr);
    if (!ok) { fail("WinHttpReceiveResponse"); break; }

    // Status
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                        WINHTTP_NO_HEADER_INDEX);

    std::wstring status_text;
    sz = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_TEXT,
                        WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    if (sz > 0) {
      status_text.resize(sz / sizeof(wchar_t) + 1);
      if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_TEXT,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status_text[0], &sz,
                              WINHTTP_NO_HEADER_INDEX)) {
        status_text.resize(wcslen(status_text.c_str()));
      } else {
        status_text.clear();
      }
    }

    std::string head = "HTTP/1.1 " + std::to_string(status) + " " +
                       HttpsNarrow(status_text.c_str()) + "\r\n";

    // Response headers (raw block)
    sz = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    if (sz > 0) {
      std::wstring raw(sz / sizeof(wchar_t) + 1, L'\0');
      if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                              WINHTTP_HEADER_NAME_BY_INDEX, &raw[0], &sz,
                              WINHTTP_NO_HEADER_INDEX)) {
        std::string block = HttpsNarrow(raw.c_str());
        // Drop the status line (we build our own).
        auto nl = block.find("\r\n");
        if (nl != std::string::npos) block = block.substr(nl + 2);

        // WinHTTP already de-chunks the body, so Transfer-Encoding must not
        // reach our parser (it would try to decode a second time). The real
        // Content-Length is added after the body is read.
        size_t pos = 0;
        while (pos < block.size()) {
          size_t eol = block.find("\r\n", pos);
          std::string line = (eol == std::string::npos)
                                 ? block.substr(pos)
                                 : block.substr(pos, eol - pos);
          pos = (eol == std::string::npos) ? block.size() : eol + 2;
          if (line.empty()) continue;

          std::string lower = line;
          for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
          if (lower.rfind("transfer-encoding:", 0) == 0) continue;
          if (lower.rfind("content-length:", 0) == 0) continue;
          if (lower.rfind("connection:", 0) == 0) continue;
          head += line + "\r\n";
        }
      }
    } else {
      head += "\r\n";
    }

    // Body
    std::string body;
    for (;;) {
      DWORD avail = 0;
      if (!WinHttpQueryDataAvailable(request, &avail)) {
        fail("WinHttpQueryDataAvailable");
        break;
      }
      if (avail == 0) break;

      std::string chunk;
      chunk.resize(avail);
      DWORD read = 0;
      if (!WinHttpReadData(request, &chunk[0], avail, &read)) {
        fail("WinHttpReadData");
        break;
      }
      if (read == 0) break;
      chunk.resize(read);
      body += chunk;
    }
    if (!job->error.empty()) break;

    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Connection: close\r\n\r\n";
    job->raw_response = head + body;
  } while (false);

  if (request) WinHttpCloseHandle(request);
  if (connect) WinHttpCloseHandle(connect);
  if (session) WinHttpCloseHandle(session);
}

#elif defined(NX8V_USE_OPENSSL)

static void HttpsRun(HttpsJob* job) {
  SSL_library_init();
  SSL_load_error_strings();
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) {
    job->error = "SSL_CTX_new failed";
    return;
  }
  if (job->reject_unauthorized) {
    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
  } else {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
  }

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  struct addrinfo* res = nullptr;
  std::string port = std::to_string(job->port);
  if (getaddrinfo(job->host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
    job->error = "DNS lookup failed";
    SSL_CTX_free(ctx);
    return;
  }

  int fd = -1;
  for (auto* ai = res; ai; ai = ai->ai_next) {
    fd = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    ::close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    job->error = "connect failed";
    SSL_CTX_free(ctx);
    return;
  }

  SSL* ssl = SSL_new(ctx);
  SSL_set_fd(ssl, fd);
  SSL_set_tlsext_host_name(ssl, job->host.c_str());
  if (job->reject_unauthorized) {
    unsigned char ipbuf[16];
    bool ip = inet_pton(AF_INET, job->host.c_str(), ipbuf) == 1 ||
              inet_pton(AF_INET6, job->host.c_str(), ipbuf) == 1;
    int host_ok = 0;
    if (ip) {
      host_ok = X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), job->host.c_str());
    } else {
      host_ok = SSL_set1_host(ssl, job->host.c_str());
    }
    if (!host_ok) {
      job->error = "certificate host check failed";
      SSL_free(ssl);
      SSL_CTX_free(ctx);
      ::close(fd);
      return;
    }
  }
  if (SSL_connect(ssl) != 1) {
    long vr = SSL_get_verify_result(ssl);
    if (vr == X509_V_ERR_HOSTNAME_MISMATCH || vr == X509_V_ERR_IP_ADDRESS_MISMATCH)
      job->error = "certificate name does not match the host";
    else
      job->error = "TLS handshake failed";
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(fd);
    return;
  }

  std::string req = job->method + " " + (job->path.empty() ? "/" : job->path) +
                    " HTTP/1.1\r\nHost: " + job->host + "\r\nConnection: close\r\n";
  for (auto& kv : job->headers) req += kv.first + ": " + kv.second + "\r\n";
  if (!job->body.empty())
    req += "Content-Length: " + std::to_string(job->body.size()) + "\r\n";
  req += "\r\n";
  req += job->body;

  size_t off = 0;
  while (off < req.size()) {
    int n = SSL_write(ssl, req.data() + off, (int)(req.size() - off));
    if (n <= 0) {
      job->error = "SSL_write failed";
      break;
    }
    off += (size_t)n;
  }

  std::string raw;
  if (job->error.empty()) {
    char buf[8192];
    for (;;) {
      int n = SSL_read(ssl, buf, sizeof(buf));
      if (n <= 0) break;
      raw.append(buf, (size_t)n);
      if (raw.size() > 32u * 1024u * 1024u) break;
    }
  }
  SSL_shutdown(ssl);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  ::close(fd);
  if (job->error.empty()) job->raw_response = raw;
}

#else

static void HttpsRun(HttpsJob* job) {
  job->error = "HTTPS on this platform needs a build with -DNX8V_USE_OPENSSL";
}

#endif  // platform TLS

}} // nx8v::bindings
