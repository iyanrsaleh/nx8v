#pragma once
// nx8v/src/bindings/buffer.h
// Node.js-style Buffer built on top of a real v8::Uint8Array, so indexing
// (buf[0] = 65), .length and .byteLength work natively.
//
//   Buffer.alloc(size[, fill])
//   Buffer.allocUnsafe(size)
//   Buffer.from(string[, encoding]) | Buffer.from(array) | Buffer.from(buffer)
//   Buffer.concat(list[, totalLength])
//   Buffer.isBuffer(v) / Buffer.byteLength(s[, enc])
//
// Instance:
//   buf.length, buf[i]
//   buf.toString([encoding][, start][, end])   utf8|hex|base64|ascii|latin1
//   buf.toJSON() -> { type: 'Buffer', data: [...] }
//   buf.slice([start][, end])  (copy, not view — simpler and safe)
//   buf.equals(other)
//   buf.fill(value)
//   buf.write(string[, offset][, encoding])

#include <v8.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nx8v {
namespace bindings {

// ---------------------------------------------------------------------------
// Encoding helpers
// ---------------------------------------------------------------------------
inline const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string BufBase64Encode(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 2 < len; i += 3) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out += kBase64Chars[(v >> 18) & 0x3F];
    out += kBase64Chars[(v >> 12) & 0x3F];
    out += kBase64Chars[(v >> 6) & 0x3F];
    out += kBase64Chars[v & 0x3F];
  }
  if (i < len) {
    uint32_t v = data[i] << 16;
    bool two = (i + 1 < len);
    if (two) v |= data[i + 1] << 8;
    out += kBase64Chars[(v >> 18) & 0x3F];
    out += kBase64Chars[(v >> 12) & 0x3F];
    out += two ? kBase64Chars[(v >> 6) & 0x3F] : '=';
    out += '=';
  }
  return out;
}

inline int BufBase64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

inline std::vector<uint8_t> BufBase64Decode(const std::string& in) {
  std::vector<uint8_t> out;
  out.reserve(in.size() * 3 / 4 + 3);
  int acc = 0, bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    int v = BufBase64Value(c);
    if (v < 0) continue;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((uint8_t)((acc >> bits) & 0xFF));
    }
  }
  return out;
}

inline std::string BufHexEncode(const uint8_t* data, size_t len) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += kHex[data[i] >> 4];
    out += kHex[data[i] & 0x0F];
  }
  return out;
}

inline int BufHexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline std::vector<uint8_t> BufHexDecode(const std::string& in) {
  std::vector<uint8_t> out;
  out.reserve(in.size() / 2);
  int hi = -1;
  for (char c : in) {
    int v = BufHexNibble(c);
    if (v < 0) continue;
    if (hi < 0) {
      hi = v;
    } else {
      out.push_back((uint8_t)((hi << 4) | v));
      hi = -1;
    }
  }
  return out;
}

inline std::string BufLower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// Encode raw bytes into the requested encoding.
inline std::string BufEncode(const uint8_t* data, size_t len,
                             const std::string& enc) {
  std::string e = BufLower(enc.empty() ? "utf8" : enc);
  if (e == "hex") return BufHexEncode(data, len);
  if (e == "base64") return BufBase64Encode(data, len);
  if (e == "base64url") {
    std::string b = BufBase64Encode(data, len);
    for (auto& c : b) {
      if (c == '+') c = '-';
      else if (c == '/') c = '_';
    }
    while (!b.empty() && b.back() == '=') b.pop_back();
    return b;
  }
  if (e == "ascii" || e == "latin1" || e == "binary")
    return std::string(reinterpret_cast<const char*>(data), len);
  // utf8 (default): bytes are already UTF-8
  return std::string(reinterpret_cast<const char*>(data), len);
}

// Decode a JS string into raw bytes.
inline std::vector<uint8_t> BufDecode(const std::string& s,
                                      const std::string& enc) {
  std::string e = BufLower(enc.empty() ? "utf8" : enc);
  if (e == "hex") return BufHexDecode(s);
  if (e == "base64") return BufBase64Decode(s);
  if (e == "base64url") {
    std::string b = s;
    for (auto& c : b) {
      if (c == '-') c = '+';
      else if (c == '_') c = '/';
    }
    while (b.size() % 4) b += '=';
    return BufBase64Decode(b);
  }
  return std::vector<uint8_t>(s.begin(), s.end());
}

// ---------------------------------------------------------------------------
// Uint8Array <-> raw bytes
// ---------------------------------------------------------------------------
inline uint8_t* BufBytes(v8::Isolate* isolate, v8::Local<v8::Value> v,
                         size_t* len) {
  if (!v->IsUint8Array()) {
    if (len) *len = 0;
    return nullptr;
  }
  auto arr = v.As<v8::Uint8Array>();
  auto ab = arr->Buffer();
  std::shared_ptr<v8::BackingStore> bs = ab->GetBackingStore();
  if (len) *len = arr->ByteLength();
  return static_cast<uint8_t*>(bs->Data()) + arr->ByteOffset();
}

// Create a Buffer (Uint8Array with Node-ish methods) from raw bytes.
inline v8::Local<v8::Object> MakeBufferFromBytes(v8::Isolate* isolate,
                                                 const uint8_t* data,
                                                 size_t len);

// ---------------------------------------------------------------------------
// Instance methods
// ---------------------------------------------------------------------------
inline v8::Local<v8::Value> BufArgString(v8::Isolate* isolate,
                                         v8::Local<v8::Value> v) {
  if (v->IsString()) return v;
  if (v->IsNullOrUndefined()) return v8::String::NewFromUtf8Literal(isolate, "utf8");
  return v8::String::NewFromUtf8Literal(isolate, "utf8");
}

inline std::string BufGetString(v8::Isolate* isolate, v8::Local<v8::Value> v) {
  v8::String::Utf8Value s(isolate, v);
  return *s ? std::string(*s, s.length()) : std::string();
}

inline void BufToString(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  size_t len = 0;
  uint8_t* data = BufBytes(isolate, args.This(), &len);
  if (!data) { args.GetReturnValue().SetEmptyString(); return; }

  std::string enc = "utf8";
  if (args.Length() >= 1 && args[0]->IsString()) enc = BufGetString(isolate, args[0]);

  size_t start = 0, end = len;
  if (args.Length() >= 2 && args[1]->IsNumber())
    start = (size_t)std::max(0, args[1]->Int32Value(ctx).FromMaybe(0));
  if (args.Length() >= 3 && args[2]->IsNumber())
    end = (size_t)std::max(0, args[2]->Int32Value(ctx).FromMaybe((int)len));
  if (start > len) start = len;
  if (end > len) end = len;
  if (end < start) end = start;

  std::string out = BufEncode(data + start, end - start, enc);
  args.GetReturnValue().Set(
      v8::String::NewFromUtf8(isolate, out.c_str(), v8::NewStringType::kNormal,
                              (int)out.size()).ToLocalChecked());
}

inline void BufToJSON(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  size_t len = 0;
  uint8_t* data = BufBytes(isolate, args.This(), &len);
  auto obj = v8::Object::New(isolate);
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "type"),
           v8::String::NewFromUtf8Literal(isolate, "Buffer")).Check();
  auto arr = v8::Array::New(isolate, (int)(data ? len : 0));
  for (size_t i = 0; data && i < len; ++i)
    arr->Set(ctx, (uint32_t)i, v8::Integer::New(isolate, data[i])).Check();
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "data"), arr).Check();
  args.GetReturnValue().Set(obj);
}

inline void BufSlice(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  size_t len = 0;
  uint8_t* data = BufBytes(isolate, args.This(), &len);
  size_t start = 0, end = len;
  if (args.Length() >= 1 && args[0]->IsNumber())
    start = (size_t)std::max(0, args[0]->Int32Value(ctx).FromMaybe(0));
  if (args.Length() >= 2 && args[1]->IsNumber())
    end = (size_t)std::max(0, args[1]->Int32Value(ctx).FromMaybe((int)len));
  if (start > len) start = len;
  if (end > len) end = len;
  if (end < start) end = start;

  args.GetReturnValue().Set(MakeBufferFromBytes(isolate, data + start, end - start));
}

inline void BufEquals(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);

  size_t a_len = 0, b_len = 0;
  uint8_t* a = BufBytes(isolate, args.This(), &a_len);
  uint8_t* b = args.Length() >= 1 ? BufBytes(isolate, args[0], &b_len) : nullptr;
  if (!a || !b) { args.GetReturnValue().Set(false); return; }
  args.GetReturnValue().Set(a_len == b_len && std::memcmp(a, b, a_len) == 0);
}

inline void BufFill(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  size_t len = 0;
  uint8_t* data = BufBytes(isolate, args.This(), &len);
  if (!data) return;
  int v = 0;
  if (args.Length() >= 1) {
    if (args[0]->IsNumber()) v = args[0]->Int32Value(ctx).FromMaybe(0);
    else if (args[0]->IsString()) {
      auto s = BufGetString(isolate, args[0]);
      v = s.empty() ? 0 : (uint8_t)s[0];
    }
  }
  std::memset(data, v & 0xFF, len);
  args.GetReturnValue().Set(args.This());
}

inline void BufWrite(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  size_t len = 0;
  uint8_t* data = BufBytes(isolate, args.This(), &len);
  if (!data || args.Length() < 1 || !args[0]->IsString()) {
    args.GetReturnValue().Set(0);
    return;
  }
  std::string s = BufGetString(isolate, args[0]);
  size_t off = 0;
  if (args.Length() >= 2 && args[1]->IsNumber())
    off = (size_t)std::max(0, args[1]->Int32Value(ctx).FromMaybe(0));
  std::string enc = "utf8";
  if (args.Length() >= 3 && args[2]->IsString()) enc = BufGetString(isolate, args[2]);

  auto bytes = BufDecode(s, enc);
  size_t n = bytes.size();
  if (off + n > len) n = (off < len) ? len - off : 0;
  std::memcpy(data + off, bytes.data(), n);
  args.GetReturnValue().Set((int)n);
}

// ---------------------------------------------------------------------------
// Build a Buffer object
// ---------------------------------------------------------------------------
inline v8::Local<v8::Object> MakeBufferFromBytes(v8::Isolate* isolate,
                                                 const uint8_t* data,
                                                 size_t len) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  auto ab = v8::ArrayBuffer::New(isolate, len);
  if (len > 0 && data)
    std::memcpy(ab->GetBackingStore()->Data(), data, len);

  auto u8 = v8::Uint8Array::New(ab, 0, len);
  v8::Local<v8::Object> obj = u8.As<v8::Object>();

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    obj->Set(ctx, v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal,
                                          (int)std::strlen(name)).ToLocalChecked(),
             v8::Function::New(ctx, cb).ToLocalChecked()).Check();
  };
  def("toString", BufToString);
  def("toJSON", BufToJSON);
  def("slice", BufSlice);
  def("equals", BufEquals);
  def("fill", BufFill);
  def("write", BufWrite);

  // Mark as a Buffer so isBuffer()/instanceof-ish checks work.
  obj->Set(ctx, v8::Symbol::For(isolate, v8::String::NewFromUtf8Literal(isolate, "nx8v.buffer")),
           v8::Boolean::New(isolate, true)).Check();

  return hs.Escape(obj);
}

inline bool IsBuffer(v8::Isolate* isolate, v8::Local<v8::Value> v) {
  if (!v->IsObject()) return false;
  auto sym = v8::Symbol::For(isolate, v8::String::NewFromUtf8Literal(isolate, "nx8v.buffer"));
  auto r = v.As<v8::Object>()->Get(isolate->GetCurrentContext(), sym);
  v8::Local<v8::Value> got;
  if (!r.ToLocal(&got)) return false;
  return got->IsBoolean() && got.As<v8::Boolean>()->Value();
}

// ---------------------------------------------------------------------------
// Buffer static methods
// ---------------------------------------------------------------------------
inline void BufAlloc(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  int size = args.Length() >= 1 ? args[0]->Int32Value(ctx).FromMaybe(0) : 0;
  if (size < 0) size = 0;

  auto buf = MakeBufferFromBytes(isolate, nullptr, (size_t)size);
  if (args.Length() >= 2) {
    size_t len = 0;
    uint8_t* data = BufBytes(isolate, buf, &len);
    int v = 0;
    if (args[1]->IsNumber()) v = args[1]->Int32Value(ctx).FromMaybe(0);
    else if (args[1]->IsString()) {
      auto s = BufGetString(isolate, args[1]);
      v = s.empty() ? 0 : (uint8_t)s[0];
    }
    if (data) std::memset(data, v & 0xFF, len);
  }
  args.GetReturnValue().Set(buf);
}

inline void BufFrom(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  if (args.Length() < 1) {
    args.GetReturnValue().Set(MakeBufferFromBytes(isolate, nullptr, 0));
    return;
  }
  auto v = args[0];

  if (v->IsString()) {
    std::string enc = "utf8";
    if (args.Length() >= 2 && args[1]->IsString()) enc = BufGetString(isolate, args[1]);
    auto bytes = BufDecode(BufGetString(isolate, v), enc);
    args.GetReturnValue().Set(MakeBufferFromBytes(isolate, bytes.data(), bytes.size()));
    return;
  }
  if (v->IsUint8Array()) {
    size_t len = 0;
    uint8_t* data = BufBytes(isolate, v, &len);
    args.GetReturnValue().Set(MakeBufferFromBytes(isolate, data, len));
    return;
  }
  if (v->IsArrayBuffer()) {
    auto ab = v.As<v8::ArrayBuffer>();
    auto bs = ab->GetBackingStore();
    args.GetReturnValue().Set(MakeBufferFromBytes(
        isolate, static_cast<uint8_t*>(bs->Data()), ab->ByteLength()));
    return;
  }
  if (v->IsArray()) {
    auto arr = v.As<v8::Array>();
    uint32_t n = arr->Length();
    std::vector<uint8_t> bytes(n);
    for (uint32_t i = 0; i < n; ++i) {
      auto item = arr->Get(ctx, i).ToLocalChecked();
      bytes[i] = (uint8_t)(item->Int32Value(ctx).FromMaybe(0) & 0xFF);
    }
    args.GetReturnValue().Set(MakeBufferFromBytes(isolate, bytes.data(), bytes.size()));
    return;
  }
  args.GetReturnValue().Set(MakeBufferFromBytes(isolate, nullptr, 0));
}

inline void BufConcat(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  std::vector<uint8_t> out;
  if (args.Length() >= 1 && args[0]->IsArray()) {
    auto arr = args[0].As<v8::Array>();
    uint32_t n = arr->Length();
    for (uint32_t i = 0; i < n; ++i) {
      auto item = arr->Get(ctx, i).ToLocalChecked();
      size_t len = 0;
      uint8_t* data = BufBytes(isolate, item, &len);
      if (data) out.insert(out.end(), data, data + len);
    }
  }
  args.GetReturnValue().Set(MakeBufferFromBytes(isolate, out.data(), out.size()));
}

inline void BufIsBuffer(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  args.GetReturnValue().Set(args.Length() >= 1 && IsBuffer(isolate, args[0]));
}

inline void BufByteLength(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);

  if (args.Length() < 1) { args.GetReturnValue().Set(0); return; }
  if (args[0]->IsString()) {
    std::string enc = "utf8";
    if (args.Length() >= 2 && args[1]->IsString())
      enc = BufGetString(isolate, args[1]);
    auto bytes = BufDecode(BufGetString(isolate, args[0]), enc);
    args.GetReturnValue().Set((int)bytes.size());
    return;
  }
  size_t len = 0;
  BufBytes(isolate, args[0], &len);
  args.GetReturnValue().Set((int)len);
}

// ---------------------------------------------------------------------------
// Module + global
// ---------------------------------------------------------------------------
inline v8::Local<v8::Object> MakeBufferModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    obj->Set(ctx, v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal,
                                          (int)std::strlen(name)).ToLocalChecked(),
             v8::Function::New(ctx, cb).ToLocalChecked()).Check();
  };
  def("alloc", BufAlloc);
  def("allocUnsafe", BufAlloc);
  def("from", BufFrom);
  def("concat", BufConcat);
  def("isBuffer", BufIsBuffer);
  def("byteLength", BufByteLength);

  return hs.Escape(obj);
}

}} // nx8v::bindings
