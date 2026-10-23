#pragma once
// nx8v/src/bindings/compress.h
// require('zlib') — gzip/deflate backed by the V8 tree's zlib.
//
//   zlib.gzipSync(data) / gunzipSync(data)
//   zlib.deflateSync(data) / inflateSync(data)
// data is a string or Uint8Array. Results are Buffers.

#include <v8.h>
#include <zlib.h>

#include <cstring>
#include <string>

#include "buffer.h"

namespace nx8v {
namespace bindings {

inline bool Nx8vCompress(const std::string& in, std::string& out, int window_bits) {
  if (in.size() > 32u * 1024u * 1024u) return false;
  z_stream strm;
  std::memset(&strm, 0, sizeof(strm));
  if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, window_bits, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK)
    return false;

  uLong bound = deflateBound(&strm, (uLong)in.size());
  if (bound > 64u * 1024u * 1024u) {
    deflateEnd(&strm);
    return false;
  }
  out.resize((size_t)bound);
  strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  strm.avail_in = (uInt)in.size();
  strm.next_out = reinterpret_cast<Bytef*>(&out[0]);
  strm.avail_out = (uInt)out.size();
  int rc = deflate(&strm, Z_FINISH);
  size_t produced = (size_t)strm.total_out;
  deflateEnd(&strm);
  if (rc != Z_STREAM_END) return false;
  out.resize(produced);
  return true;
}

inline bool Nx8vDecompress(const std::string& in, std::string& out, int window_bits) {
  if (in.size() > 32u * 1024u * 1024u) return false;
  z_stream strm;
  std::memset(&strm, 0, sizeof(strm));
  if (inflateInit2(&strm, window_bits) != Z_OK) return false;

  strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  strm.avail_in = (uInt)in.size();
  out.clear();
  int rc = Z_OK;
  while (rc != Z_STREAM_END) {
    size_t old = out.size();
    out.resize(old + 65536);
    strm.next_out = reinterpret_cast<Bytef*>(&out[old]);
    strm.avail_out = 65536;
    rc = inflate(&strm, Z_NO_FLUSH);
    if (rc != Z_OK && rc != Z_STREAM_END) {
      inflateEnd(&strm);
      return false;
    }
    out.resize(old + (65536 - strm.avail_out));
    if (out.size() > 64u * 1024u * 1024u) {
      inflateEnd(&strm);
      return false;
    }
    if (strm.avail_in == 0 && rc != Z_STREAM_END) break;
  }
  inflateEnd(&strm);
  return rc == Z_STREAM_END;
}

inline bool Nx8vGzip(const std::string& in, std::string& out) {
  return Nx8vCompress(in, out, 15 + 16);
}
inline bool Nx8vGunzip(const std::string& in, std::string& out) {
  return Nx8vDecompress(in, out, 15 + 16);
}

namespace zdetail {

inline std::string ArgBytes(v8::Isolate* isolate, v8::Local<v8::Value> v) {
  if (v->IsUint8Array()) {
    size_t n = 0;
    uint8_t* p = BufBytes(isolate, v, &n);
    if (p && n) return std::string(reinterpret_cast<char*>(p), n);
    return {};
  }
  v8::String::Utf8Value s(isolate, v);
  return *s ? std::string(*s, s.length()) : std::string();
}

inline void SyncCompress(const v8::FunctionCallbackInfo<v8::Value>& args,
                         bool gzip, bool decompress) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  if (args.Length() < 1) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "zlib: data required")));
    return;
  }
  std::string in = ArgBytes(isolate, args[0]);
  std::string out;
  int bits = gzip ? (15 + 16) : 15;
  bool ok = decompress ? Nx8vDecompress(in, out, bits)
                       : Nx8vCompress(in, out, bits);
  if (!ok) {
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(isolate, "zlib: compression failed")));
    return;
  }
  args.GetReturnValue().Set(MakeBufferFromBytes(
      isolate, reinterpret_cast<const uint8_t*>(out.data()), out.size()));
}

}  // namespace zdetail

inline v8::Local<v8::Object> MakeZlibModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    obj->Set(ctx,
             v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal)
                 .ToLocalChecked(),
             v8::Function::New(ctx, cb).ToLocalChecked())
        .Check();
  };

  def("gzipSync", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    zdetail::SyncCompress(a, true, false);
  });
  def("gunzipSync", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    zdetail::SyncCompress(a, true, true);
  });
  def("deflateSync", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    zdetail::SyncCompress(a, false, false);
  });
  def("inflateSync", [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    zdetail::SyncCompress(a, false, true);
  });

  return hs.Escape(obj);
}

}}  // nx8v::bindings
