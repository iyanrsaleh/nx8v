#pragma once
// TLS server (Windows Schannel) and HTTP/2 (h2c and h2 via ALPN).

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define SECURITY_WIN32
#include <windows.h>
#include <wincrypt.h>
#include <schannel.h>
#include <security.h>
#include <ncrypt.h>
#include <cstdio>
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ncrypt.lib")
#elif defined(NX8V_USE_OPENSSL)
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/ssl.h>
#endif

namespace nx8v {
namespace bindings {

struct Nx8vSec {
#ifdef _WIN32
  CtxtHandle ctx{};
  bool has_ctx = false;
  bool handshake_done = false;
  SecPkgContext_StreamSizes sizes{};
  std::string inbuf;
#elif defined(NX8V_USE_OPENSSL)
  SSL* ssl = nullptr;
  BIO* rbio = nullptr;
  BIO* wbio = nullptr;
  bool handshake_done = false;
#endif
};

struct Nx8vTlsCred {
#ifdef _WIN32
  CredHandle cred{};
  bool has_cred = false;
  PCCERT_CONTEXT cert = nullptr;
  HCERTSTORE store = nullptr;
  NCRYPT_KEY_HANDLE key = 0;
  std::wstring key_name;
  bool capi = false;
  std::wstring capi_prov;
  DWORD capi_type = 0;
#elif defined(NX8V_USE_OPENSSL)
  SSL_CTX* ctx = nullptr;
#endif
};

static void Nx8vSecFree(void* p) {
  if (!p) return;
  auto* sec = reinterpret_cast<Nx8vSec*>(p);
#ifdef _WIN32
  if (sec->has_ctx) {
    DeleteSecurityContext(&sec->ctx);
    sec->has_ctx = false;
  }
#elif defined(NX8V_USE_OPENSSL)
  if (sec->ssl) {
    SSL_free(sec->ssl);
    sec->ssl = nullptr;
    sec->rbio = nullptr;
    sec->wbio = nullptr;
  }
#endif
  delete sec;
}

static void Nx8vTlsFree(HttpServer* srv) {
  if (!srv || !srv->tls_state) return;
  auto* cred = reinterpret_cast<Nx8vTlsCred*>(srv->tls_state);
#ifdef _WIN32
  if (cred->has_cred) {
    FreeCredentialsHandle(&cred->cred);
    cred->has_cred = false;
  }
  if (cred->cert) {
    CertFreeCertificateContext(cred->cert);
    cred->cert = nullptr;
  }
  if (cred->store) {
    CertCloseStore(cred->store, 0);
    cred->store = nullptr;
  }
  if (cred->key) {
    NCryptFreeObject(cred->key);
    cred->key = 0;
  }
  if (cred->capi && !cred->key_name.empty()) {
    std::string container = std::string(cred->key_name.begin(), cred->key_name.end());
    std::string prov_name = std::string(cred->capi_prov.begin(), cred->capi_prov.end());
    HCRYPTPROV gone = 0;
    CryptAcquireContextA(&gone, container.c_str(),
                         prov_name.empty() ? nullptr : prov_name.c_str(),
                         cred->capi_type, CRYPT_DELETEKEYSET);
    cred->key_name.clear();
  } else if (!cred->key_name.empty()) {
    NCRYPT_PROV_HANDLE prov = 0;
    if (NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0) == ERROR_SUCCESS) {
      NCRYPT_KEY_HANDLE key = 0;
      if (NCryptOpenKey(prov, &key, cred->key_name.c_str(), 0, 0) == ERROR_SUCCESS)
        NCryptDeleteKey(key, 0);
      NCryptFreeObject(prov);
    }
    cred->key_name.clear();
  }
#elif defined(NX8V_USE_OPENSSL)
  if (cred->ctx) {
    SSL_CTX_free(cred->ctx);
    cred->ctx = nullptr;
  }
#endif
  delete cred;
  srv->tls_state = nullptr;
}

static const struct { uint8_t bits; uint32_t code; } kHuff[257] = {
 {13, 0xFFC00000U}, {23, 0xFFFFB000U}, {28, 0xFFFFFE20U}, {28, 0xFFFFFE30U},
 {28, 0xFFFFFE40U}, {28, 0xFFFFFE50U}, {28, 0xFFFFFE60U}, {28, 0xFFFFFE70U},
 {28, 0xFFFFFE80U}, {24, 0xFFFFEA00U}, {30, 0xFFFFFFF0U}, {28, 0xFFFFFE90U},
 {28, 0xFFFFFEA0U}, {30, 0xFFFFFFF4U}, {28, 0xFFFFFEB0U}, {28, 0xFFFFFEC0U},
 {28, 0xFFFFFED0U}, {28, 0xFFFFFEE0U}, {28, 0xFFFFFEF0U}, {28, 0xFFFFFF00U},
 {28, 0xFFFFFF10U}, {28, 0xFFFFFF20U}, {30, 0xFFFFFFF8U}, {28, 0xFFFFFF30U},
 {28, 0xFFFFFF40U}, {28, 0xFFFFFF50U}, {28, 0xFFFFFF60U}, {28, 0xFFFFFF70U},
 {28, 0xFFFFFF80U}, {28, 0xFFFFFF90U}, {28, 0xFFFFFFA0U}, {28, 0xFFFFFFB0U},
 {6, 0x50000000U}, {10, 0xFE000000U}, {10, 0xFE400000U}, {12, 0xFFA00000U},
 {13, 0xFFC80000U}, {6, 0x54000000U}, {8, 0xF8000000U}, {11, 0xFF400000U},
 {10, 0xFE800000U}, {10, 0xFEC00000U}, {8, 0xF9000000U}, {11, 0xFF600000U},
 {8, 0xFA000000U}, {6, 0x58000000U}, {6, 0x5C000000U}, {6, 0x60000000U},
 {5, 0x0U}, {5, 0x8000000U}, {5, 0x10000000U}, {6, 0x64000000U},
 {6, 0x68000000U}, {6, 0x6C000000U}, {6, 0x70000000U}, {6, 0x74000000U},
 {6, 0x78000000U}, {6, 0x7C000000U}, {7, 0xB8000000U}, {8, 0xFB000000U},
 {15, 0xFFF80000U}, {6, 0x80000000U}, {12, 0xFFB00000U}, {10, 0xFF000000U},
 {13, 0xFFD00000U}, {6, 0x84000000U}, {7, 0xBA000000U}, {7, 0xBC000000U},
 {7, 0xBE000000U}, {7, 0xC0000000U}, {7, 0xC2000000U}, {7, 0xC4000000U},
 {7, 0xC6000000U}, {7, 0xC8000000U}, {7, 0xCA000000U}, {7, 0xCC000000U},
 {7, 0xCE000000U}, {7, 0xD0000000U}, {7, 0xD2000000U}, {7, 0xD4000000U},
 {7, 0xD6000000U}, {7, 0xD8000000U}, {7, 0xDA000000U}, {7, 0xDC000000U},
 {7, 0xDE000000U}, {7, 0xE0000000U}, {7, 0xE2000000U}, {7, 0xE4000000U},
 {8, 0xFC000000U}, {7, 0xE6000000U}, {8, 0xFD000000U}, {13, 0xFFD80000U},
 {19, 0xFFFE0000U}, {13, 0xFFE00000U}, {14, 0xFFF00000U}, {6, 0x88000000U},
 {15, 0xFFFA0000U}, {5, 0x18000000U}, {6, 0x8C000000U}, {5, 0x20000000U},
 {6, 0x90000000U}, {5, 0x28000000U}, {6, 0x94000000U}, {6, 0x98000000U},
 {6, 0x9C000000U}, {5, 0x30000000U}, {7, 0xE8000000U}, {7, 0xEA000000U},
 {6, 0xA0000000U}, {6, 0xA4000000U}, {6, 0xA8000000U}, {5, 0x38000000U},
 {6, 0xAC000000U}, {7, 0xEC000000U}, {6, 0xB0000000U}, {5, 0x40000000U},
 {5, 0x48000000U}, {6, 0xB4000000U}, {7, 0xEE000000U}, {7, 0xF0000000U},
 {7, 0xF2000000U}, {7, 0xF4000000U}, {7, 0xF6000000U}, {15, 0xFFFC0000U},
 {11, 0xFF800000U}, {14, 0xFFF40000U}, {13, 0xFFE80000U}, {28, 0xFFFFFFC0U},
 {20, 0xFFFE6000U}, {22, 0xFFFF4800U}, {20, 0xFFFE7000U}, {20, 0xFFFE8000U},
 {22, 0xFFFF4C00U}, {22, 0xFFFF5000U}, {22, 0xFFFF5400U}, {23, 0xFFFFB200U},
 {22, 0xFFFF5800U}, {23, 0xFFFFB400U}, {23, 0xFFFFB600U}, {23, 0xFFFFB800U},
 {23, 0xFFFFBA00U}, {23, 0xFFFFBC00U}, {24, 0xFFFFEB00U}, {23, 0xFFFFBE00U},
 {24, 0xFFFFEC00U}, {24, 0xFFFFED00U}, {22, 0xFFFF5C00U}, {23, 0xFFFFC000U},
 {24, 0xFFFFEE00U}, {23, 0xFFFFC200U}, {23, 0xFFFFC400U}, {23, 0xFFFFC600U},
 {23, 0xFFFFC800U}, {21, 0xFFFEE000U}, {22, 0xFFFF6000U}, {23, 0xFFFFCA00U},
 {22, 0xFFFF6400U}, {23, 0xFFFFCC00U}, {23, 0xFFFFCE00U}, {24, 0xFFFFEF00U},
 {22, 0xFFFF6800U}, {21, 0xFFFEE800U}, {20, 0xFFFE9000U}, {22, 0xFFFF6C00U},
 {22, 0xFFFF7000U}, {23, 0xFFFFD000U}, {23, 0xFFFFD200U}, {21, 0xFFFEF000U},
 {23, 0xFFFFD400U}, {22, 0xFFFF7400U}, {22, 0xFFFF7800U}, {24, 0xFFFFF000U},
 {21, 0xFFFEF800U}, {22, 0xFFFF7C00U}, {23, 0xFFFFD600U}, {23, 0xFFFFD800U},
 {21, 0xFFFF0000U}, {21, 0xFFFF0800U}, {22, 0xFFFF8000U}, {21, 0xFFFF1000U},
 {23, 0xFFFFDA00U}, {22, 0xFFFF8400U}, {23, 0xFFFFDC00U}, {23, 0xFFFFDE00U},
 {20, 0xFFFEA000U}, {22, 0xFFFF8800U}, {22, 0xFFFF8C00U}, {22, 0xFFFF9000U},
 {23, 0xFFFFE000U}, {22, 0xFFFF9400U}, {22, 0xFFFF9800U}, {23, 0xFFFFE200U},
 {26, 0xFFFFF800U}, {26, 0xFFFFF840U}, {20, 0xFFFEB000U}, {19, 0xFFFE2000U},
 {22, 0xFFFF9C00U}, {23, 0xFFFFE400U}, {22, 0xFFFFA000U}, {25, 0xFFFFF600U},
 {26, 0xFFFFF880U}, {26, 0xFFFFF8C0U}, {26, 0xFFFFF900U}, {27, 0xFFFFFBC0U},
 {27, 0xFFFFFBE0U}, {26, 0xFFFFF940U}, {24, 0xFFFFF100U}, {25, 0xFFFFF680U},
 {19, 0xFFFE4000U}, {21, 0xFFFF1800U}, {26, 0xFFFFF980U}, {27, 0xFFFFFC00U},
 {27, 0xFFFFFC20U}, {26, 0xFFFFF9C0U}, {27, 0xFFFFFC40U}, {24, 0xFFFFF200U},
 {21, 0xFFFF2000U}, {21, 0xFFFF2800U}, {26, 0xFFFFFA00U}, {26, 0xFFFFFA40U},
 {28, 0xFFFFFFD0U}, {27, 0xFFFFFC60U}, {27, 0xFFFFFC80U}, {27, 0xFFFFFCA0U},
 {20, 0xFFFEC000U}, {24, 0xFFFFF300U}, {20, 0xFFFED000U}, {21, 0xFFFF3000U},
 {22, 0xFFFFA400U}, {21, 0xFFFF3800U}, {21, 0xFFFF4000U}, {23, 0xFFFFE600U},
 {22, 0xFFFFA800U}, {22, 0xFFFFAC00U}, {25, 0xFFFFF700U}, {25, 0xFFFFF780U},
 {24, 0xFFFFF400U}, {24, 0xFFFFF500U}, {26, 0xFFFFFA80U}, {23, 0xFFFFE800U},
 {26, 0xFFFFFAC0U}, {27, 0xFFFFFCC0U}, {26, 0xFFFFFB00U}, {26, 0xFFFFFB40U},
 {27, 0xFFFFFCE0U}, {27, 0xFFFFFD00U}, {27, 0xFFFFFD20U}, {27, 0xFFFFFD40U},
 {27, 0xFFFFFD60U}, {28, 0xFFFFFFE0U}, {27, 0xFFFFFD80U}, {27, 0xFFFFFDA0U},
 {27, 0xFFFFFDC0U}, {27, 0xFFFFFDE0U}, {27, 0xFFFFFE00U}, {26, 0xFFFFFB80U},
 {30, 0xFFFFFFFCU}
};

static bool Nx8vHuffDecode(const uint8_t* data, size_t len, std::string* out) {
  uint32_t acc = 0;
  int n = 0;
  size_t i = 0;
  int accbits = 0;
  uint32_t curbyte = 0;
  auto bit = [&]() -> int {
    if (accbits == 0) {
      if (i >= len) return -1;
      curbyte = data[i++];
      accbits = 8;
    }
    --accbits;
    return (int)((curbyte >> accbits) & 1);
  };
  for (;;) {
    int b = bit();
    if (b < 0) break;
    acc = (acc << 1) | (uint32_t)b;
    ++n;
    if (n > 30) return false;
    bool found = false;
    for (int s = 0; s < 257; ++s) {
      if (kHuff[s].bits != (uint8_t)n) continue;
      if (kHuff[s].code != (acc << (32 - n))) continue;
      if (s == 256) return false;
      out->push_back((char)s);
      acc = 0;
      n = 0;
      found = true;
      break;
    }
    (void)found;
  }
  if (n > 7) return false;
  if (n > 0) {
    uint32_t mask = 0xFFFFFFFFu << (32 - n);
    uint32_t aligned = acc << (32 - n);
    if ((aligned & mask) != (kHuff[256].code & mask)) return false;
  }
  return true;
}

static const char* kStName[] = {
  "",
  ":authority", ":method", ":method", ":path", ":path", ":scheme", ":scheme",
  ":status", ":status", ":status", ":status", ":status", ":status", ":status",
  "accept-charset", "accept-encoding", "accept-language", "accept-ranges",
  "accept", "access-control-allow-origin", "age", "allow", "authorization",
  "cache-control", "content-disposition", "content-encoding", "content-language",
  "content-length", "content-location", "content-range", "content-type",
  "cookie", "date", "etag", "expect", "expires", "from", "host",
  "if-match", "if-modified-since", "if-none-match", "if-range",
  "if-unmodified-since", "last-modified", "link", "location", "max-forwards",
  "proxy-authenticate", "proxy-authorization", "range", "referer", "refresh",
  "retry-after", "server", "set-cookie", "strict-transport-security",
  "transfer-encoding", "user-agent", "vary", "via", "www-authenticate"
};
static const char* kStValue[] = {
  "",
  "", "GET", "POST", "/", "/index.html", "http", "https",
  "200", "204", "206", "304", "400", "404", "500",
  "", "gzip, deflate", "", "",
  "", "", "", "", "",
  "", "", "", "",
  "", "", "", "",
  "", "", "", "", "", "", "",
  "", "", "", "",
  "", "", "", "", "",
  "", "", "", "", "", "",
  "", "", "", "",
  "", "", "", ""
};

static bool HpackInt(const uint8_t* p, size_t n, int prefix, size_t* used, uint64_t* out) {
  if (!n || prefix < 1 || prefix > 8) return false;
  uint64_t maxv = (1ull << prefix) - 1ull;
  uint64_t v = p[0] & maxv;
  size_t i = 1;
  if (v < maxv) { *used = 1; *out = v; return true; }
  uint32_t m = 0;
  while (i < n && m <= 28) {
    uint8_t b = p[i++];
    v += (uint64_t)(b & 0x7f) << m;
    m += 7;
    if ((b & 0x80) == 0) { *used = i; *out = v; return true; }
  }
  return false;
}

static bool HpackStr(const uint8_t* p, size_t n, size_t* used, std::string* out) {
  uint64_t len = 0;
  size_t u = 0;
  if (!HpackInt(p, n, 7, &u, &len)) return false;
  if (u + (size_t)len > n) return false;
  bool huff = (p[0] & 0x80) != 0;
  out->clear();
  if (!huff) out->assign(reinterpret_cast<const char*>(p + u), (size_t)len);
  else if (!Nx8vHuffDecode(p + u, (size_t)len, out)) return false;
  *used = u + (size_t)len;
  return true;
}

static void H2Put(std::string& o, uint8_t type, uint8_t flags, uint32_t stream,
                  const std::string& payload) {
  uint32_t len = (uint32_t)payload.size();
  o.push_back((char)((len >> 16) & 0xff));
  o.push_back((char)((len >> 8) & 0xff));
  o.push_back((char)(len & 0xff));
  o.push_back((char)type);
  o.push_back((char)flags);
  stream &= 0x7fffffffu;
  o.push_back((char)((stream >> 24) & 0xff));
  o.push_back((char)((stream >> 16) & 0xff));
  o.push_back((char)((stream >> 8) & 0xff));
  o.push_back((char)(stream & 0xff));
  o.append(payload);
}

static void HpackRaw(std::string& o, const std::string& s) {
  if (s.size() < 127) o.push_back((char)s.size());
  else {
    o.push_back((char)127);
    size_t rest = s.size() - 127;
    while (rest >= 128) { o.push_back((char)((rest & 0x7f) | 0x80)); rest >>= 7; }
    o.push_back((char)rest);
  }
  o.append(s);
}
static void HpackLit(std::string& o, const std::string& name, const std::string& value) {
  o.push_back(0);
  HpackRaw(o, name);
  HpackRaw(o, value);
}

static void Nx8vH2Dispatch(HttpConn* conn) {
  if (conn->request_emitted || conn->h2_stream == 0) return;
  std::string path = conn->h2_path.empty() ? "/" : conn->h2_path;
  std::string method = conn->h2_method.empty() ? "GET" : conn->h2_method;
  std::string raw = method + " " + path + " 2.0\r\n";
  if (!conn->h2_authority.empty()) raw += "host: " + conn->h2_authority + "\r\n";
  for (const auto& kv : conn->h2_headers) {
    if (kv.first.empty() || kv.first[0] == ':') continue;
    raw += kv.first + ": " + kv.second + "\r\n";
  }
  raw += "\r\n";
  raw += conn->h2_body;
  conn->read_buf = std::move(raw);
  conn->request_emitted = true;
  EmitRequest(conn);
}

struct Nx8vHpackEnt { std::string name; std::string value; };
struct Nx8vHpackTab {
  std::vector<Nx8vHpackEnt> e;
  size_t bytes = 0;
  size_t cap = 4096;
};
static std::map<HttpConn*, Nx8vHpackTab> g_hpack;

static size_t HpackEntSize(const Nx8vHpackEnt& ent) {
  return ent.name.size() + ent.value.size() + 32;
}
static void HpackEvict(Nx8vHpackTab& tab) {
  while (tab.bytes > tab.cap && !tab.e.empty()) {
    tab.bytes -= HpackEntSize(tab.e.back());
    tab.e.pop_back();
  }
}
static void HpackInsert(Nx8vHpackTab& tab, std::string name, std::string value) {
  Nx8vHpackEnt ent{std::move(name), std::move(value)};
  size_t sz = HpackEntSize(ent);
  if (sz > tab.cap) {
    tab.e.clear();
    tab.bytes = 0;
    return;
  }
  tab.e.insert(tab.e.begin(), std::move(ent));
  tab.bytes += sz;
  HpackEvict(tab);
}
static bool HpackGet(const Nx8vHpackTab& tab, uint64_t idx,
                     std::string* name, std::string* value) {
  if (idx >= 1 && idx <= 61) {
    *name = kStName[idx];
    *value = kStValue[idx];
    return true;
  }
  if (idx < 62) return false;
  size_t at = (size_t)idx - 62;
  if (at >= tab.e.size()) return false;
  *name = tab.e[at].name;
  *value = tab.e[at].value;
  return true;
}
static void Nx8vH2Apply(HttpConn* conn, const std::string& name, const std::string& value) {
  if (name == ":method") conn->h2_method = value;
  else if (name == ":path") conn->h2_path = value;
  else if (name == ":authority" || name == "host") conn->h2_authority = value;
  else if (!name.empty() && name[0] != ':') conn->h2_headers[name] = value;
}

static bool Nx8vH2HeaderBlock(HttpConn* conn, const std::string& block) {
  auto& tab = g_hpack[conn];
  const uint8_t* p = reinterpret_cast<const uint8_t*>(block.data());
  size_t n = block.size();
  size_t i = 0;
  while (i < n) {
    uint8_t b0 = p[i];
    if (b0 & 0x80) {
      uint64_t idx = 0; size_t u = 0;
      if (!HpackInt(p + i, n - i, 7, &u, &idx)) return false;
      i += u;
      std::string name, value;
      if (!HpackGet(tab, idx, &name, &value)) return false;
      Nx8vH2Apply(conn, name, value);
      continue;
    }
    if ((b0 & 0xe0) == 0x20) {
      uint64_t sz = 0; size_t u = 0;
      if (!HpackInt(p + i, n - i, 5, &u, &sz)) return false;
      i += u;
      tab.cap = (size_t)sz;
      HpackEvict(tab);
      continue;
    }
    bool incremental = (b0 & 0x40) != 0;
    int prefix = incremental ? 6 : 4;
    uint64_t name_idx = 0; size_t u = 0;
    if (!HpackInt(p + i, n - i, prefix, &u, &name_idx)) return false;
    i += u;
    std::string name, value;
    if (name_idx == 0) {
      size_t us = 0;
      if (!HpackStr(p + i, n - i, &us, &name)) return false;
      i += us;
    } else {
      std::string ignored;
      if (!HpackGet(tab, name_idx, &name, &ignored)) return false;
    }
    size_t us = 0;
    if (!HpackStr(p + i, n - i, &us, &value)) return false;
    i += us;
    if (incremental) HpackInsert(tab, name, value);
    Nx8vH2Apply(conn, name, value);
  }
  return true;
}

static void Nx8vH2Reply(HttpConn* conn, int status,
                        const std::map<std::string, std::string>& headers,
                        const std::string& body) {
  if (!conn || conn->h2_replied) return;
  conn->h2_replied = true;
  std::string block;
  if (status == 200) block.push_back((char)0x88);
  else if (status == 204) block.push_back((char)0x89);
  else if (status == 206) block.push_back((char)0x8a);
  else if (status == 304) block.push_back((char)0x8b);
  else if (status == 400) block.push_back((char)0x8c);
  else if (status == 404) block.push_back((char)0x8d);
  else if (status == 500) block.push_back((char)0x8e);
  else HpackLit(block, ":status", std::to_string(status));
  for (const auto& kv : headers) {
    std::string k = kv.first;
    for (auto& c : k) c = (char)std::tolower((unsigned char)c);
    if (k == "connection" || k == "transfer-encoding" || k.empty()) continue;
    HpackLit(block, k, kv.second);
  }
  std::string bin;
  uint8_t hflags = 0x4;
  if (body.empty()) hflags = (uint8_t)(hflags | 0x1);
  H2Put(bin, 0x1, hflags, (uint32_t)conn->h2_stream, block);
  if (!body.empty()) H2Put(bin, 0x0, 0x1, (uint32_t)conn->h2_stream, body);
  std::string go(8, '\0');
  uint32_t sid = (uint32_t)conn->h2_stream;
  go[0] = (char)((sid >> 24) & 0xff);
  go[1] = (char)((sid >> 16) & 0xff);
  go[2] = (char)((sid >> 8) & 0xff);
  go[3] = (char)(sid & 0xff);
  H2Put(bin, 0x7, 0, 0, go);
  HttpWriteRaw(conn, bin, true);
}

static void Nx8vH2Feed(HttpConn* conn, const std::string& chunk) {
  static const char kPre[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
  conn->h2_in.append(chunk);
  if (!conn->h2_settings_sent) {
    if (conn->h2_in.size() < 24) return;
    if (conn->h2_in.compare(0, 24, kPre) != 0) return;
    conn->h2_in.erase(0, 24);
    conn->h2 = true;
    std::string settings;
    std::string payload;
    auto putset = [&](uint16_t id, uint32_t val) {
      payload.push_back((char)(id >> 8));
      payload.push_back((char)(id & 0xff));
      payload.push_back((char)((val >> 24) & 0xff));
      payload.push_back((char)((val >> 16) & 0xff));
      payload.push_back((char)((val >> 8) & 0xff));
      payload.push_back((char)(val & 0xff));
    };
    putset(2, 0);
    putset(3, 1);
    H2Put(settings, 0x4, 0, 0, payload);
    conn->h2_settings_sent = true;
    HttpWriteRaw(conn, settings, false);
  }
  while (conn->h2_in.size() >= 9 && !conn->request_emitted) {
    const uint8_t* f = reinterpret_cast<const uint8_t*>(conn->h2_in.data());
    uint32_t len = ((uint32_t)f[0] << 16) | ((uint32_t)f[1] << 8) | f[2];
    if (len > 1024u * 1024u) return;
    if (conn->h2_in.size() < 9u + len) return;
    uint8_t type = f[3];
    uint8_t flags = f[4];
    uint32_t stream = ((uint32_t)f[5] << 24) | ((uint32_t)f[6] << 16) |
                      ((uint32_t)f[7] << 8) | f[8];
    stream &= 0x7fffffffu;
    std::string payload = conn->h2_in.substr(9, len);
    conn->h2_in.erase(0, 9u + len);
    if (type == 0x4 && (flags & 0x1) == 0) {
      for (size_t si = 0; si + 6 <= payload.size(); si += 6) {
        uint16_t id = (uint16_t)(((uint16_t)(uint8_t)payload[si] << 8) |
                                 (uint8_t)payload[si + 1]);
        uint32_t val = ((uint32_t)(uint8_t)payload[si + 2] << 24) |
                       ((uint32_t)(uint8_t)payload[si + 3] << 16) |
                       ((uint32_t)(uint8_t)payload[si + 4] << 8) |
                       (uint32_t)(uint8_t)payload[si + 5];
        if (id == 1) {
          auto& tab = g_hpack[conn];
          tab.cap = val;
          HpackEvict(tab);
        }
      }
      std::string ack;
      H2Put(ack, 0x4, 0x1, 0, "");
      HttpWriteRaw(conn, ack, false);
    } else if (type == 0x6) {
      std::string pong;
      H2Put(pong, 0x6, 0x1, 0, payload);
      HttpWriteRaw(conn, pong, false);
    } else if (type == 0x1 || type == 0x9) {
      size_t off = 0;
      if (type == 0x1 && (flags & 0x8)) {
        if (payload.empty()) return;
        size_t pad = (uint8_t)payload[0];
        off = 1;
        if (off + pad > payload.size()) return;
        payload.resize(payload.size() - pad);
      }
      if (type == 0x1 && (flags & 0x20)) {
        if (payload.size() < off + 5) return;
        off += 5;
      }
      if (off > payload.size()) return;
      conn->h2_block.append(payload, off, payload.size() - off);
      if (stream) conn->h2_stream = (int)stream;
      if (flags & 0x4) {
        if (!Nx8vH2HeaderBlock(conn, conn->h2_block)) {
          if (!uv_is_closing((uv_handle_t*)&conn->handle)) {
            uv_close((uv_handle_t*)&conn->handle, [](uv_handle_t* h) {
              delete reinterpret_cast<HttpConn*>(h->data);
            });
          }
          return;
        }
        conn->h2_block.clear();
        if (flags & 0x1) Nx8vH2Dispatch(conn);
      }
    } else if (type == 0x0) {
      size_t off = 0;
      if (flags & 0x8) {
        if (payload.empty()) return;
        size_t pad = (uint8_t)payload[0];
        off = 1;
        if (off + pad > payload.size()) return;
        payload.resize(payload.size() - pad);
      }
      if (off < payload.size()) conn->h2_body.append(payload, off, payload.size() - off);
      if (flags & 0x1) Nx8vH2Dispatch(conn);
    }
  }
}

static bool Nx8vTakeH2(HttpConn* conn) {
  static const char kPre[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
  if (conn->alpn_h2) conn->h2 = true;
  if (!conn->h2) {
    size_t n = conn->read_buf.size();
    if (n == 0) return false;
    size_t m = n < 24 ? n : 24;
    if (std::memcmp(conn->read_buf.data(), kPre, m) != 0) return false;
    if (n < 24) return true;
    conn->h2 = true;
  }
  std::string chunk = std::move(conn->read_buf);
  conn->read_buf.clear();
  Nx8vH2Feed(conn, chunk);
  return true;
}

#ifdef _WIN32

static std::wstring Nx8vWide(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}

static int Nx8vB64Val(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
static std::string Nx8vB64(const std::string& in) {
  std::string out;
  int val = 0;
  int bits = -8;
  for (unsigned char c : in) {
    if (c == '=') break;
    int d = Nx8vB64Val(c);
    if (d < 0) continue;
    val = (val << 6) + d;
    bits += 6;
    if (bits >= 0) {
      out.push_back((char)((val >> bits) & 0xff));
      bits -= 8;
    }
  }
  return out;
}
static std::string Nx8vPemPart(const std::string& pem, const char* begin, const char* end) {
  auto a = pem.find(begin);
  if (a == std::string::npos) return "";
  a += std::strlen(begin);
  auto b = pem.find(end, a);
  if (b == std::string::npos) return "";
  return Nx8vB64(pem.substr(a, b - a));
}
static void Nx8vDerLen(std::string& o, size_t n) {
  if (n < 128) {
    o.push_back((char)n);
    return;
  }
  unsigned char tmp[8];
  int i = 0;
  while (n && i < 8) {
    tmp[i++] = (unsigned char)(n & 0xff);
    n >>= 8;
  }
  o.push_back((char)(0x80 | i));
  while (i) o.push_back((char)tmp[--i]);
}
static std::string Nx8vDerWrap(unsigned char tag, const std::string& inner) {
  std::string o;
  o.push_back((char)tag);
  Nx8vDerLen(o, inner.size());
  o.append(inner);
  return o;
}
static std::string Nx8vPkcs1ToPkcs8(const std::string& pkcs1) {
  static const char alg[] = "\x30\x0d\x06\x09\x2a\x86\x48\x86\xf7\x0d\x01\x01\x01\x05\x00";
  std::string body("\x02\x01\x00", 3);
  body.append(alg, sizeof(alg) - 1);
  body += Nx8vDerWrap(0x04, pkcs1);
  return Nx8vDerWrap(0x30, body);
}
static bool Nx8vTlsAcquire(Nx8vTlsCred* cred, std::string* err) {
  const DWORD modern[] = {
      SP_PROT_TLS1_2_SERVER | SP_PROT_TLS1_3_SERVER,
      SP_PROT_TLS1_2_SERVER,
      0};
  const DWORD legacy[] = {SP_PROT_TLS1_2_SERVER};
  const DWORD* protocols = cred->capi ? legacy : modern;
  int count = cred->capi ? 1 : 3;
  SECURITY_STATUS st = SEC_E_INTERNAL_ERROR;
  for (int i = 0; i < count; ++i) {
    DWORD proto = protocols[i];
    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.cCreds = 1;
    sc.paCred = &cred->cert;
    sc.grbitEnabledProtocols = proto;
    sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
    st = AcquireCredentialsHandleW(
        nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_INBOUND,
        nullptr, &sc, nullptr, nullptr, &cred->cred, nullptr);
    if (st == SEC_E_OK) {
      cred->has_cred = true;
      return true;
    }
  }
  char buf[96];
  std::snprintf(buf, sizeof(buf), "AcquireCredentialsHandle failed (%08lx).",
                (unsigned long)st);
  *err = buf;
  return false;
}
struct Nx8vProvPick { wchar_t name[96]; };
static BOOL CALLBACK Nx8vResolveProv(CRYPT_PRIVATE_KEY_INFO*, HCRYPTPROV* ph, LPVOID pv) {
  auto* chosen = reinterpret_cast<Nx8vProvPick*>(pv);
  if (CryptAcquireContextW(ph, chosen->name, MS_ENHANCED_PROV_W, PROV_RSA_FULL, CRYPT_NEWKEYSET))
    return TRUE;
  CryptAcquireContextW(ph, chosen->name, MS_ENHANCED_PROV_W, PROV_RSA_FULL, CRYPT_DELETEKEYSET);
  return CryptAcquireContextW(ph, chosen->name, MS_ENHANCED_PROV_W, PROV_RSA_FULL, CRYPT_NEWKEYSET);
}

static bool Nx8vTlsLoadPem(HttpServer* srv, Nx8vTlsCred* cred, std::string* err) {
  if (srv->tls_key.find("BEGIN ENCRYPTED PRIVATE KEY") != std::string::npos) {
    *err = "Encrypted private keys are not supported. Use a pfx and a passphrase.";
    return false;
  }
  std::string cert_der = Nx8vPemPart(srv->tls_cert, "-----BEGIN CERTIFICATE-----",
                                     "-----END CERTIFICATE-----");
  std::string key_der = Nx8vPemPart(srv->tls_key, "-----BEGIN PRIVATE KEY-----",
                                    "-----END PRIVATE KEY-----");
  if (key_der.empty()) {
    std::string pkcs1 = Nx8vPemPart(srv->tls_key, "-----BEGIN RSA PRIVATE KEY-----",
                                    "-----END RSA PRIVATE KEY-----");
    if (!pkcs1.empty()) key_der = Nx8vPkcs1ToPkcs8(pkcs1);
  }
  if (cert_der.empty() || key_der.empty()) {
    *err = "The TLS certificate could not be read. Use a pfx, or a PEM certificate and a PKCS#8 key.";
    return false;
  }
  PCCERT_CONTEXT created = CertCreateCertificateContext(
      X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
      reinterpret_cast<const BYTE*>(cert_der.data()), (DWORD)cert_der.size());
  if (!created) {
    *err = "The PEM certificate is not valid.";
    return false;
  }
  cred->store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
  if (!cred->store ||
      !CertAddCertificateContextToStore(cred->store, created, CERT_STORE_ADD_ALWAYS,
                                        &cred->cert)) {
    CertFreeCertificateContext(created);
    *err = "The PEM certificate could not be stored.";
    return false;
  }
  CertFreeCertificateContext(created);
  const char* btag = "-----BEGIN CERTIFICATE-----";
  const char* etag = "-----END CERTIFICATE-----";
  size_t scan = 0;
  bool first = true;
  while (true) {
    auto a = srv->tls_cert.find(btag, scan);
    if (a == std::string::npos) break;
    a += std::strlen(btag);
    auto b = srv->tls_cert.find(etag, a);
    if (b == std::string::npos) break;
    scan = b + std::strlen(etag);
    if (first) {
      first = false;
      continue;
    }
    std::string extra = Nx8vB64(srv->tls_cert.substr(a, b - a));
    if (extra.empty()) continue;
    PCCERT_CONTEXT more = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        reinterpret_cast<const BYTE*>(extra.data()), (DWORD)extra.size());
    if (!more) continue;
    CertAddCertificateContextToStore(cred->store, more, CERT_STORE_ADD_ALWAYS, nullptr);
    CertFreeCertificateContext(more);
  }
  static unsigned seq = 0;
  Nx8vProvPick pick{};
  std::wstring container = L"nx8v-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(++seq);
  lstrcpynW(pick.name, container.c_str(), 96);
  CRYPT_PKCS8_IMPORT_PARAMS ip{};
  ip.PrivateKey.pbData = reinterpret_cast<BYTE*>(&key_der[0]);
  ip.PrivateKey.cbData = (DWORD)key_der.size();
  ip.pResolvehCryptProvFunc = Nx8vResolveProv;
  ip.pVoidResolveFunc = &pick;
  HCRYPTPROV hprov = 0;
  if (!CryptImportPKCS8(ip, CRYPT_EXPORTABLE, &hprov, nullptr)) {
    *err = "The PEM private key could not be imported. Use PKCS#8 or a pfx.";
    return false;
  }
  CryptReleaseContext(hprov, 0);
  cred->key_name = pick.name;
  cred->capi_prov = MS_ENHANCED_PROV_W;
  cred->capi_type = PROV_RSA_FULL;
  cred->capi = true;
  CRYPT_KEY_PROV_INFO kpi{};
  kpi.pwszContainerName = const_cast<LPWSTR>(cred->key_name.c_str());
  kpi.pwszProvName = const_cast<LPWSTR>(cred->capi_prov.c_str());
  kpi.dwProvType = PROV_RSA_FULL;
  kpi.dwKeySpec = AT_KEYEXCHANGE;
  if (!CertSetCertificateContextProperty(cred->cert, CERT_KEY_PROV_INFO_PROP_ID, 0, &kpi)) {
    *err = "The private key could not be attached to the certificate.";
    return false;
  }
  return true;
}

static bool Nx8vTlsPrepare(HttpServer* srv, std::string* err) {
  auto* cred = new Nx8vTlsCred();
  srv->tls_state = cred;
  if (!srv->tls_pfx.empty()) {
    CRYPT_DATA_BLOB blob{};
    blob.cbData = (DWORD)srv->tls_pfx.size();
    blob.pbData = reinterpret_cast<BYTE*>(&srv->tls_pfx[0]);
    std::wstring pass = Nx8vWide(srv->tls_pass);
    const DWORD flag_sets[] = {
        CRYPT_EXPORTABLE | PKCS12_ALWAYS_CNG_KSP | CRYPT_USER_KEYSET,
        CRYPT_EXPORTABLE | CRYPT_USER_KEYSET,
        CRYPT_EXPORTABLE | PKCS12_ALWAYS_CNG_KSP,
        CRYPT_EXPORTABLE};
    for (DWORD flags : flag_sets) {
      cred->store = PFXImportCertStore(&blob, pass.c_str(), flags);
      if (cred->store) break;
    }
    if (!cred->store) {
      *err = "The pfx file could not be read. Check the passphrase.";
      Nx8vTlsFree(srv);
      return false;
    }
    cred->cert = CertFindCertificateInStore(
        cred->store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
        CERT_FIND_HAS_PRIVATE_KEY, nullptr, nullptr);
    if (!cred->cert) {
      *err = "The pfx file does not contain a private key.";
      Nx8vTlsFree(srv);
      return false;
    }
  } else if (!Nx8vTlsLoadPem(srv, cred, err)) {
    Nx8vTlsFree(srv);
    return false;
  }
  if (!Nx8vTlsAcquire(cred, err)) {
    Nx8vTlsFree(srv);
    return false;
  }
  return true;
}

static std::string Nx8vTlsSeal(HttpConn* conn, const std::string& plain) {
  if (!conn->tls_ready || !conn->sec || plain.empty()) return plain;
  auto* sec = reinterpret_cast<Nx8vSec*>(conn->sec);
  size_t header = sec->sizes.cbHeader;
  size_t trailer = sec->sizes.cbTrailer;
  std::string packet(header + plain.size() + trailer, '\0');
  std::memcpy(&packet[header], plain.data(), plain.size());
  SecBuffer bufs[4];
  bufs[0].cbBuffer = (ULONG)header;
  bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
  bufs[0].pvBuffer = &packet[0];
  bufs[1].cbBuffer = (ULONG)plain.size();
  bufs[1].BufferType = SECBUFFER_DATA;
  bufs[1].pvBuffer = &packet[header];
  bufs[2].cbBuffer = (ULONG)trailer;
  bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
  bufs[2].pvBuffer = &packet[header + plain.size()];
  bufs[3].cbBuffer = 0;
  bufs[3].BufferType = SECBUFFER_EMPTY;
  bufs[3].pvBuffer = nullptr;
  SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};
  if (EncryptMessage(&sec->ctx, 0, &desc, 0) != SEC_E_OK) return "";
  packet.resize(bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer);
  return packet;
}

static bool Nx8vTlsDecrypt(Nx8vSec* sec, std::string* plain) {
  while (!sec->inbuf.empty()) {
    std::string buf = sec->inbuf;
    SecBuffer bufs[4];
    for (int i = 0; i < 4; ++i) {
      bufs[i].cbBuffer = 0;
      bufs[i].BufferType = SECBUFFER_EMPTY;
      bufs[i].pvBuffer = nullptr;
    }
    bufs[0].BufferType = SECBUFFER_DATA;
    bufs[0].pvBuffer = &buf[0];
    bufs[0].cbBuffer = (ULONG)buf.size();
    SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};
    SECURITY_STATUS st = DecryptMessage(&sec->ctx, &desc, 0, nullptr);
    if (st == SEC_E_INCOMPLETE_MESSAGE) return true;
    if (st != SEC_E_OK && st != SEC_I_CONTEXT_EXPIRED) return false;
    std::string extra;
    for (int i = 0; i < 4; ++i) {
      if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer && bufs[i].pvBuffer)
        plain->append(reinterpret_cast<char*>(bufs[i].pvBuffer), bufs[i].cbBuffer);
      if (bufs[i].BufferType == SECBUFFER_EXTRA && bufs[i].cbBuffer && bufs[i].pvBuffer)
        extra.append(reinterpret_cast<char*>(bufs[i].pvBuffer), bufs[i].cbBuffer);
    }
    sec->inbuf.swap(extra);
    if (st == SEC_I_CONTEXT_EXPIRED) return true;
    if (extra.empty()) break;
  }
  return true;
}

static bool Nx8vTlsConsume(HttpConn* conn, const char* data, size_t n) {
  if (!conn->sec) conn->sec = new Nx8vSec();
  auto* sec = reinterpret_cast<Nx8vSec*>(conn->sec);
  auto* cred = reinterpret_cast<Nx8vTlsCred*>(conn->server ? conn->server->tls_state : nullptr);
  if (!cred || !cred->has_cred) return false;
  sec->inbuf.append(data, n);
  if (!sec->handshake_done) {
    unsigned char protolist[] = {
      2, 'h', '2',
      8, 'h', 't', 't', 'p', '/', '1', '.', '1'
    };
#pragma pack(push, 1)
    struct NxAlpn {
      unsigned long ProtocolListsSize;
      unsigned long ProtoNegoExt;
      unsigned short ProtocolListSize;
      unsigned char ProtocolList[sizeof(protolist)];
    };
#pragma pack(pop)
    NxAlpn alpn{};
    alpn.ProtoNegoExt = SecApplicationProtocolNegotiationExt_ALPN;
    alpn.ProtocolListSize = (unsigned short)sizeof(protolist);
    std::memcpy(alpn.ProtocolList, protolist, sizeof(protolist));
    alpn.ProtocolListsSize =
        (unsigned long)(sizeof(alpn.ProtoNegoExt) + sizeof(alpn.ProtocolListSize) +
                        sizeof(protolist));

    for (int guard = 0; guard < 8 && !sec->handshake_done; ++guard) {
      if (sec->inbuf.empty()) return false;
      std::string in = sec->inbuf;
      SecBuffer inbufs[3];
      inbufs[0].cbBuffer = (ULONG)in.size();
      inbufs[0].BufferType = SECBUFFER_TOKEN;
      inbufs[0].pvBuffer = &in[0];
      inbufs[1].cbBuffer = 0;
      inbufs[1].BufferType = SECBUFFER_EMPTY;
      inbufs[1].pvBuffer = nullptr;
      unsigned cbuf = 2;
      if (!sec->has_ctx) {
        inbufs[2].cbBuffer = (ULONG)sizeof(alpn);
        inbufs[2].BufferType = SECBUFFER_APPLICATION_PROTOCOLS;
        inbufs[2].pvBuffer = &alpn;
        cbuf = 3;
      }
      SecBufferDesc ind{SECBUFFER_VERSION, cbuf, inbufs};
      SecBuffer outb{};
      outb.BufferType = SECBUFFER_TOKEN;
      outb.cbBuffer = 0;
      outb.pvBuffer = nullptr;
      SecBufferDesc outd{SECBUFFER_VERSION, 1, &outb};
      DWORD attrs = 0;
      SECURITY_STATUS st = AcceptSecurityContext(
          &cred->cred, sec->has_ctx ? &sec->ctx : nullptr, &ind,
          ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
              ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_EXTENDED_ERROR,
          SECURITY_NATIVE_DREP, &sec->ctx, &outd, &attrs, nullptr);
      if (outb.cbBuffer && outb.pvBuffer) {
        std::string tok(reinterpret_cast<char*>(outb.pvBuffer), outb.cbBuffer);
        FreeContextBuffer(outb.pvBuffer);
        if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED || st == SEC_I_COMPLETE_NEEDED ||
            st == SEC_I_COMPLETE_AND_CONTINUE)
          HttpWriteRaw(conn, tok, false);
      }
      if (st == SEC_E_INCOMPLETE_MESSAGE) return false;
      if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED && st != SEC_I_COMPLETE_NEEDED &&
          st != SEC_I_COMPLETE_AND_CONTINUE) {
        std::fprintf(stderr, "TLS handshake failed (%ld)\n", (long)st);
        if (!uv_is_closing((uv_handle_t*)&conn->handle)) {
          uv_close((uv_handle_t*)&conn->handle, [](uv_handle_t* h) {
            delete reinterpret_cast<HttpConn*>(h->data);
          });
        }
        return false;
      }
      sec->has_ctx = true;
      std::string extra;
      for (unsigned bi = 0; bi < cbuf; ++bi) {
        if (inbufs[bi].BufferType == SECBUFFER_EXTRA && inbufs[bi].cbBuffer &&
            inbufs[bi].pvBuffer) {
          extra.assign(reinterpret_cast<char*>(inbufs[bi].pvBuffer), inbufs[bi].cbBuffer);
          break;
        }
      }
      sec->inbuf.swap(extra);
      if (st == SEC_E_OK) {
        sec->handshake_done = true;
        if (QueryContextAttributes(&sec->ctx, SECPKG_ATTR_STREAM_SIZES, &sec->sizes) != SEC_E_OK) {
          return false;
        }
        SecPkgContext_ApplicationProtocol proto{};
        if (QueryContextAttributes(&sec->ctx, SECPKG_ATTR_APPLICATION_PROTOCOL, &proto) == SEC_E_OK &&
            proto.ProtoNegoStatus == SecApplicationProtocolNegotiationStatus_Success &&
            proto.ProtocolIdSize == 2 && proto.ProtocolId[0] == 'h' && proto.ProtocolId[1] == '2') {
          conn->alpn_h2 = true;
        }
        conn->tls_ready = true;
        break;
      }
    }
  }
  if (!sec->handshake_done) return false;
  std::string plain;
  if (!sec->inbuf.empty()) {
    if (!Nx8vTlsDecrypt(sec, &plain)) return false;
  }
  if (!plain.empty()) conn->read_buf.append(plain);
  return true;
}

#elif defined(NX8V_USE_OPENSSL)

static std::string Nx8vSslDrain(BIO* bio) {
  std::string out;
  char buf[16384];
  for (;;) {
    int n = BIO_read(bio, buf, sizeof(buf));
    if (n <= 0) break;
    out.append(buf, (size_t)n);
  }
  return out;
}

static int Nx8vAlpnSelect(SSL*, const unsigned char** out, unsigned char* outlen,
                          const unsigned char* in, unsigned int inlen, void*) {
  static const unsigned char kProtos[] = {
      2, 'h', '2',
      8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  if (SSL_select_next_proto(const_cast<unsigned char**>(out), outlen, kProtos,
                            sizeof(kProtos), in, inlen) != OPENSSL_NPN_NEGOTIATED)
    return SSL_TLSEXT_ERR_NOACK;
  return SSL_TLSEXT_ERR_OK;
}

static int Nx8vPemPassword(char* buf, int size, int, void* userdata) {
  auto* pass = reinterpret_cast<const std::string*>(userdata);
  if (!pass || pass->empty() || size <= 0) return 0;
  int n = (int)pass->size();
  if (n > size) n = size;
  std::memcpy(buf, pass->data(), (size_t)n);
  return n;
}

static bool Nx8vTlsPrepare(HttpServer* srv, std::string* err) {
  OPENSSL_init_ssl(0, nullptr);
  auto* cred = new Nx8vTlsCred();
  srv->tls_state = cred;
  cred->ctx = SSL_CTX_new(TLS_server_method());
  if (!cred->ctx) {
    *err = "SSL_CTX_new failed.";
    Nx8vTlsFree(srv);
    return false;
  }
  SSL_CTX_set_min_proto_version(cred->ctx, TLS1_2_VERSION);
  SSL_CTX_set_alpn_select_cb(cred->ctx, Nx8vAlpnSelect, nullptr);
  X509* cert = nullptr;
  EVP_PKEY* key = nullptr;
  STACK_OF(X509)* extra = nullptr;
  if (!srv->tls_pfx.empty()) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(srv->tls_pfx.data());
    PKCS12* p12 = d2i_PKCS12(nullptr, &p, (long)srv->tls_pfx.size());
    if (!p12 || !PKCS12_parse(p12, srv->tls_pass.c_str(), &key, &cert, &extra)) {
      if (p12) PKCS12_free(p12);
      *err = "The pfx file could not be read. Check the passphrase.";
      Nx8vTlsFree(srv);
      return false;
    }
    PKCS12_free(p12);
  } else {
    BIO* cbio = BIO_new_mem_buf(srv->tls_cert.data(), (int)srv->tls_cert.size());
    cert = PEM_read_bio_X509(cbio, nullptr, nullptr, nullptr);
    if (cbio && cert) {
      for (;;) {
        X509* more = PEM_read_bio_X509(cbio, nullptr, nullptr, nullptr);
        if (!more) break;
        if (!extra) extra = sk_X509_new_null();
        sk_X509_push(extra, more);
      }
    }
    BIO_free(cbio);
    BIO* kbio = BIO_new_mem_buf(srv->tls_key.data(), (int)srv->tls_key.size());
    key = PEM_read_bio_PrivateKey(kbio, nullptr, Nx8vPemPassword, &srv->tls_pass);
    BIO_free(kbio);
    if (!cert || !key) {
      if (cert) X509_free(cert);
      if (key) EVP_PKEY_free(key);
      if (extra) sk_X509_pop_free(extra, X509_free);
      *err = srv->tls_key.find("BEGIN ENCRYPTED PRIVATE KEY") != std::string::npos
                 ? "Encrypted private keys are not supported. Use a pfx and a passphrase."
                 : "The TLS certificate could not be read. Use a pfx, or a PEM certificate and a PKCS#8 key.";
      Nx8vTlsFree(srv);
      return false;
    }
  }
  bool ok = SSL_CTX_use_certificate(cred->ctx, cert) == 1 &&
            SSL_CTX_use_PrivateKey(cred->ctx, key) == 1 &&
            SSL_CTX_check_private_key(cred->ctx) == 1;
  if (ok && extra) {
    for (int i = 0; i < sk_X509_num(extra); ++i) {
      X509* one = sk_X509_value(extra, i);
      if (one && SSL_CTX_add_extra_chain_cert(cred->ctx, one) == 1)
        X509_up_ref(one);
    }
  }
  X509_free(cert);
  EVP_PKEY_free(key);
  if (extra) sk_X509_pop_free(extra, X509_free);
  if (!ok) {
    *err = "The private key does not match the certificate.";
    Nx8vTlsFree(srv);
    return false;
  }
  return true;
}

static std::string Nx8vTlsSeal(HttpConn* conn, const std::string& plain) {
  if (!conn->tls_ready || !conn->sec || plain.empty()) return plain;
  auto* sec = reinterpret_cast<Nx8vSec*>(conn->sec);
  size_t off = 0;
  while (off < plain.size()) {
    int n = SSL_write(sec->ssl, plain.data() + off, (int)(plain.size() - off));
    if (n <= 0) return "";
    off += (size_t)n;
  }
  return Nx8vSslDrain(sec->wbio);
}

static bool Nx8vTlsConsume(HttpConn* conn, const char* data, size_t n) {
  auto* cred = reinterpret_cast<Nx8vTlsCred*>(conn->server ? conn->server->tls_state : nullptr);
  if (!cred || !cred->ctx) return false;
  if (!conn->sec) conn->sec = new Nx8vSec();
  auto* sec = reinterpret_cast<Nx8vSec*>(conn->sec);
  if (!sec->ssl) {
    sec->ssl = SSL_new(cred->ctx);
    sec->rbio = BIO_new(BIO_s_mem());
    sec->wbio = BIO_new(BIO_s_mem());
    if (!sec->ssl || !sec->rbio || !sec->wbio) return false;
    BIO_set_mem_eof_return(sec->rbio, -1);
    SSL_set_bio(sec->ssl, sec->rbio, sec->wbio);
    SSL_set_accept_state(sec->ssl);
  }
  if (n && BIO_write(sec->rbio, data, (int)n) <= 0) return false;
  if (!sec->handshake_done) {
    int rc = SSL_accept(sec->ssl);
    std::string tok = Nx8vSslDrain(sec->wbio);
    if (!tok.empty()) HttpWriteRaw(conn, tok, false);
    if (rc != 1) {
      int err = SSL_get_error(sec->ssl, rc);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return false;
      char buf[256];
      ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
      std::fprintf(stderr, "TLS handshake failed (%s)\n", buf);
      if (!uv_is_closing((uv_handle_t*)&conn->handle)) {
        uv_close((uv_handle_t*)&conn->handle, [](uv_handle_t* h) {
          delete reinterpret_cast<HttpConn*>(h->data);
        });
      }
      return false;
    }
    sec->handshake_done = true;
    const unsigned char* proto = nullptr;
    unsigned int plen = 0;
    SSL_get0_alpn_selected(sec->ssl, &proto, &plen);
    if (plen == 2 && proto[0] == 'h' && proto[1] == '2') conn->alpn_h2 = true;
    conn->tls_ready = true;
  }
  std::string plain;
  char buf[16384];
  for (;;) {
    int got = SSL_read(sec->ssl, buf, sizeof(buf));
    if (got > 0) {
      plain.append(buf, (size_t)got);
      continue;
    }
    int err = SSL_get_error(sec->ssl, got);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_ZERO_RETURN)
      break;
    return false;
  }
  std::string more = Nx8vSslDrain(sec->wbio);
  if (!more.empty()) {
    bool saved = conn->tls_ready;
    conn->tls_ready = false;
    HttpWriteRaw(conn, more, false);
    conn->tls_ready = saved;
  }
  if (!plain.empty()) conn->read_buf.append(plain);
  return true;
}

#else

static bool Nx8vTlsPrepare(HttpServer*, std::string* err) {
  *err = "The TLS server is not linked in this build.";
  return false;
}
static std::string Nx8vTlsSeal(HttpConn*, const std::string& plain) { return plain; }
static bool Nx8vTlsConsume(HttpConn*, const char*, size_t) { return false; }

#endif

static void Nx8vH2Forget(HttpConn* conn) {
  if (conn) g_hpack.erase(conn);
}

}}  // namespace nx8v::bindings
