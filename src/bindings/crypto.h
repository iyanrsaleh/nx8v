#pragma once
// nx8v/src/bindings/crypto.h
// Minimal crypto module (self-contained, no external deps):
//
//   crypto.createHash('sha256'|'sha1'|'md5')
//     .update(data[, encoding])  -> this
//     .digest([encoding])        -> hex (default) | base64 | buffer
//   crypto.randomBytes(size)     -> Buffer
//   crypto.randomUUID()          -> string
//   crypto.getHashes()           -> ['md5','sha1','sha256']

#include <v8.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "buffer.h"

namespace nx8v {
namespace bindings {

// ===========================================================================
// MD5
// ===========================================================================
struct Md5Ctx {
  uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
  uint64_t total = 0;
  uint8_t  buf[64];
  size_t   buflen = 0;
};

inline uint32_t Md5Rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

inline void Md5Block(Md5Ctx& ctx, const uint8_t* p) {
  static const uint32_t K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
  static const int S[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };

  uint32_t M[16];
  for (int i = 0; i < 16; ++i) {
    M[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
           ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
  }

  uint32_t A = ctx.a, B = ctx.b, C = ctx.c, D = ctx.d;
  for (int i = 0; i < 64; ++i) {
    uint32_t F; int g;
    if (i < 16)      { F = (B & C) | (~B & D); g = i; }
    else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
    else if (i < 48) { F = B ^ C ^ D;          g = (3 * i + 5) % 16; }
    else             { F = C ^ (B | ~D);       g = (7 * i) % 16; }
    uint32_t tmp = D;
    D = C;
    C = B;
    B = B + Md5Rotl(A + F + K[i] + M[g], S[i]);
    A = tmp;
  }
  ctx.a += A; ctx.b += B; ctx.c += C; ctx.d += D;
}

inline void Md5Update(Md5Ctx& ctx, const uint8_t* data, size_t len) {
  ctx.total += len;
  while (len > 0) {
    size_t take = 64 - ctx.buflen;
    if (take > len) take = len;
    std::memcpy(ctx.buf + ctx.buflen, data, take);
    ctx.buflen += take;
    data += take;
    len -= take;
    if (ctx.buflen == 64) {
      Md5Block(ctx, ctx.buf);
      ctx.buflen = 0;
    }
  }
}

inline std::vector<uint8_t> Md5Final(Md5Ctx& ctx) {
  uint64_t bits = ctx.total * 8;
  uint8_t pad = 0x80;
  Md5Update(ctx, &pad, 1);
  uint8_t zero = 0;
  while (ctx.buflen != 56) Md5Update(ctx, &zero, 1);
  uint8_t lenbuf[8];
  for (int i = 0; i < 8; ++i) lenbuf[i] = (uint8_t)((bits >> (8 * i)) & 0xFF);
  Md5Update(ctx, lenbuf, 8);

  std::vector<uint8_t> out(16);
  uint32_t vals[4] = { ctx.a, ctx.b, ctx.c, ctx.d };
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      out[i * 4 + j] = (uint8_t)((vals[i] >> (8 * j)) & 0xFF);
  return out;
}

// ===========================================================================
// SHA-1
// ===========================================================================
struct Sha1Ctx {
  uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
  uint64_t total = 0;
  uint8_t  buf[64];
  size_t   buflen = 0;
};

inline uint32_t Sha1Rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

inline void Sha1Block(Sha1Ctx& ctx, const uint8_t* p) {
  uint32_t w[80];
  for (int i = 0; i < 16; ++i)
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  for (int i = 16; i < 80; ++i)
    w[i] = Sha1Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

  uint32_t a = ctx.h[0], b = ctx.h[1], c = ctx.h[2], d = ctx.h[3], e = ctx.h[4];
  for (int i = 0; i < 80; ++i) {
    uint32_t f, k;
    if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
    else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
    else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
    uint32_t tmp = Sha1Rotl(a, 5) + f + e + k + w[i];
    e = d; d = c; c = Sha1Rotl(b, 30); b = a; a = tmp;
  }
  ctx.h[0] += a; ctx.h[1] += b; ctx.h[2] += c; ctx.h[3] += d; ctx.h[4] += e;
}

inline void Sha1Update(Sha1Ctx& ctx, const uint8_t* data, size_t len) {
  ctx.total += len;
  while (len > 0) {
    size_t take = 64 - ctx.buflen;
    if (take > len) take = len;
    std::memcpy(ctx.buf + ctx.buflen, data, take);
    ctx.buflen += take;
    data += take;
    len -= take;
    if (ctx.buflen == 64) { Sha1Block(ctx, ctx.buf); ctx.buflen = 0; }
  }
}

inline std::vector<uint8_t> Sha1Final(Sha1Ctx& ctx) {
  uint64_t bits = ctx.total * 8;
  uint8_t pad = 0x80, zero = 0;
  Sha1Update(ctx, &pad, 1);
  while (ctx.buflen != 56) Sha1Update(ctx, &zero, 1);
  uint8_t lenbuf[8];
  for (int i = 0; i < 8; ++i) lenbuf[i] = (uint8_t)((bits >> (8 * (7 - i))) & 0xFF);
  Sha1Update(ctx, lenbuf, 8);

  std::vector<uint8_t> out(20);
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j)
      out[i * 4 + j] = (uint8_t)((ctx.h[i] >> (8 * (3 - j))) & 0xFF);
  return out;
}

// ===========================================================================
// SHA-256
// ===========================================================================
struct Sha256Ctx {
  uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
  uint64_t total = 0;
  uint8_t  buf[64];
  size_t   buflen = 0;
};

inline uint32_t Sha256Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline void Sha256Block(Sha256Ctx& ctx, const uint8_t* p) {
  static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = Sha256Rotr(w[i - 15], 7) ^ Sha256Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = Sha256Rotr(w[i - 2], 17) ^ Sha256Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = ctx.h[0], b = ctx.h[1], c = ctx.h[2], d = ctx.h[3];
  uint32_t e = ctx.h[4], f = ctx.h[5], g = ctx.h[6], h = ctx.h[7];

  for (int i = 0; i < 64; ++i) {
    uint32_t S1 = Sha256Rotr(e, 6) ^ Sha256Rotr(e, 11) ^ Sha256Rotr(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = Sha256Rotr(a, 2) ^ Sha256Rotr(a, 13) ^ Sha256Rotr(a, 22);
    uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  ctx.h[0] += a; ctx.h[1] += b; ctx.h[2] += c; ctx.h[3] += d;
  ctx.h[4] += e; ctx.h[5] += f; ctx.h[6] += g; ctx.h[7] += h;
}

inline void Sha256Update(Sha256Ctx& ctx, const uint8_t* data, size_t len) {
  ctx.total += len;
  while (len > 0) {
    size_t take = 64 - ctx.buflen;
    if (take > len) take = len;
    std::memcpy(ctx.buf + ctx.buflen, data, take);
    ctx.buflen += take;
    data += take;
    len -= take;
    if (ctx.buflen == 64) { Sha256Block(ctx, ctx.buf); ctx.buflen = 0; }
  }
}

inline std::vector<uint8_t> Sha256Final(Sha256Ctx& ctx) {
  uint64_t bits = ctx.total * 8;
  uint8_t pad = 0x80, zero = 0;
  Sha256Update(ctx, &pad, 1);
  while (ctx.buflen != 56) Sha256Update(ctx, &zero, 1);
  uint8_t lenbuf[8];
  for (int i = 0; i < 8; ++i) lenbuf[i] = (uint8_t)((bits >> (8 * (7 - i))) & 0xFF);
  Sha256Update(ctx, lenbuf, 8);

  std::vector<uint8_t> out(32);
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 4; ++j)
      out[i * 4 + j] = (uint8_t)((ctx.h[i] >> (8 * (3 - j))) & 0xFF);
  return out;
}

// ===========================================================================
// Hash object exposed to JS
// ===========================================================================
struct HashState {
  std::string        alg;
  Md5Ctx             md5;
  Sha1Ctx            sha1;
  Sha256Ctx          sha256;
  std::vector<uint8_t> pending;   // fed before final
};

inline HashState* HashFromObject(v8::Local<v8::Object> obj) {
  auto ext = obj->GetAlignedPointerFromInternalField(0, v8::kEmbedderDataTypeTagDefault);
  return reinterpret_cast<HashState*>(ext);
}

inline void HashUpdateData(HashState* st, const uint8_t* data, size_t len) {
  if (st->alg == "md5")        Md5Update(st->md5, data, len);
  else if (st->alg == "sha1")  Sha1Update(st->sha1, data, len);
  else                         Sha256Update(st->sha256, data, len);
}

inline void HashUpdate(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  auto* st = HashFromObject(args.This());
  if (!st || args.Length() < 1) { args.GetReturnValue().Set(args.This()); return; }

  if (args[0]->IsString()) {
    std::string enc = "utf8";
    if (args.Length() >= 2 && args[1]->IsString())
      enc = BufGetString(isolate, args[1]);
    auto bytes = BufDecode(BufGetString(isolate, args[0]), enc);
    HashUpdateData(st, bytes.data(), bytes.size());
  } else {
    size_t len = 0;
    uint8_t* data = BufBytes(isolate, args[0], &len);
    if (data) HashUpdateData(st, data, len);
  }
  args.GetReturnValue().Set(args.This());
}

inline void HashDigest(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);

  auto* st = HashFromObject(args.This());
  if (!st) { args.GetReturnValue().SetEmptyString(); return; }

  std::vector<uint8_t> out;
  if (st->alg == "md5")        out = Md5Final(st->md5);
  else if (st->alg == "sha1")  out = Sha1Final(st->sha1);
  else                         out = Sha256Final(st->sha256);

  std::string enc = "hex";
  if (args.Length() >= 1 && args[0]->IsString()) enc = BufGetString(isolate, args[0]);

  if (BufLower(enc) == "buffer")
    args.GetReturnValue().Set(MakeBufferFromBytes(isolate, out.data(), out.size()));
  else {
    std::string s = BufEncode(out.data(), out.size(), enc);
    args.GetReturnValue().Set(
        v8::String::NewFromUtf8(isolate, s.c_str(), v8::NewStringType::kNormal,
                                (int)s.size()).ToLocalChecked());
  }
}

inline void CryptoCreateHash(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  std::string alg = args.Length() >= 1 ? BufLower(BufGetString(isolate, args[0])) : "sha256";
  if (alg != "md5" && alg != "sha1" && alg != "sha256") {
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8(isolate, ("unsupported hash: " + alg).c_str(),
                                v8::NewStringType::kNormal).ToLocalChecked()));
    return;
  }

  auto* st = new HashState();
  st->alg = alg;

  // Object::New() has no internal fields — use a template with one slot.
  auto tmpl = v8::ObjectTemplate::New(isolate);
  tmpl->SetInternalFieldCount(1);
  auto obj = tmpl->NewInstance(ctx).ToLocalChecked();
  obj->SetAlignedPointerInInternalField(0, st, v8::kEmbedderDataTypeTagDefault);

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    obj->Set(ctx, v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal,
                                          (int)std::strlen(name)).ToLocalChecked(),
             v8::Function::New(ctx, cb).ToLocalChecked()).Check();
  };
  def("update", HashUpdate);
  def("digest", HashDigest);

  // Note: HashState is intentionally leaked per hash object — hashes are
  // short-lived and freeing needs a weak callback; acceptable for now.
  args.GetReturnValue().Set(obj);
}

inline void CryptoRandomBytes(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  int n = args.Length() >= 1 ? args[0]->Int32Value(ctx).FromMaybe(0) : 0;
  if (n < 0) n = 0;

  std::vector<uint8_t> bytes((size_t)n);
  std::random_device rd;
  for (int i = 0; i < n; ++i) bytes[i] = (uint8_t)(rd() & 0xFF);

  auto buf = MakeBufferFromBytes(isolate, bytes.data(), bytes.size());
  if (args.Length() >= 2 && args[1]->IsFunction()) {
    auto cb = args[1].As<v8::Function>();
    v8::Local<v8::Value> argv[] = { v8::Null(isolate), buf };
    v8::Local<v8::Value> ignored;
    (void)cb->Call(ctx, v8::Undefined(isolate), 2, argv).ToLocal(&ignored);
  }
  args.GetReturnValue().Set(buf);
}

inline void CryptoRandomUUID(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);

  std::random_device rd;
  uint8_t b[16];
  for (int i = 0; i < 16; ++i) b[i] = (uint8_t)(rd() & 0xFF);
  b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);   // version 4
  b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);   // variant

  char out[37];
  std::snprintf(out, sizeof(out),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(isolate, out));
}

inline void CryptoGetHashes(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto arr = v8::Array::New(isolate, 3);
  arr->Set(ctx, 0, v8::String::NewFromUtf8Literal(isolate, "md5")).Check();
  arr->Set(ctx, 1, v8::String::NewFromUtf8Literal(isolate, "sha1")).Check();
  arr->Set(ctx, 2, v8::String::NewFromUtf8Literal(isolate, "sha256")).Check();
  args.GetReturnValue().Set(arr);
}

inline v8::Local<v8::Object> MakeCryptoModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    obj->Set(ctx, v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal,
                                          (int)std::strlen(name)).ToLocalChecked(),
             v8::Function::New(ctx, cb).ToLocalChecked()).Check();
  };
  def("createHash", CryptoCreateHash);
  def("randomBytes", CryptoRandomBytes);
  def("randomUUID", CryptoRandomUUID);
  def("getHashes", CryptoGetHashes);

  return hs.Escape(obj);
}

}} // nx8v::bindings
