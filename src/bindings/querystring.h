#pragma once
// nx8v/src/bindings/querystring.h
// require('querystring') and the URLSearchParams global.
//
//   qs.parse('a=1&b=two')  -> { a: '1', b: 'two' }
//   qs.stringify({ a: 1 }) -> 'a=1'
//   new URLSearchParams('a=1').get('a')

#include <v8.h>

#include <cctype>
#include <string>

namespace nx8v {
namespace bindings {

namespace qsdetail {

inline int HexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline std::string Decode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '+') {
      out += ' ';
    } else if (in[i] == '%' && i + 2 < in.size()) {
      int hi = HexVal(in[i + 1]);
      int lo = HexVal(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += (char)((hi << 4) | lo);
        i += 2;
      } else {
        out += in[i];
      }
    } else {
      out += in[i];
    }
  }
  return out;
}

inline std::string Encode(const std::string& in) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : in) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0xF];
    }
  }
  return out;
}

inline v8::Local<v8::Object> ParseToObject(v8::Isolate* isolate,
                                           v8::Local<v8::Context> ctx,
                                           const std::string& s) {
  auto obj = v8::Object::New(isolate);
  size_t i = 0;
  while (i < s.size()) {
    size_t amp = s.find('&', i);
    if (amp == std::string::npos) amp = s.size();
    std::string pair = s.substr(i, amp - i);
    i = amp + 1;
    if (pair.empty()) continue;
    size_t eq = pair.find('=');
    std::string key = Decode(eq == std::string::npos ? pair : pair.substr(0, eq));
    std::string val = Decode(eq == std::string::npos ? "" : pair.substr(eq + 1));
    if (key.empty()) continue;
    auto k = v8::String::NewFromUtf8(isolate, key.c_str(),
                                     v8::NewStringType::kNormal,
                                     (int)key.size())
                 .ToLocalChecked();
    auto v = v8::String::NewFromUtf8(isolate, val.c_str(),
                                     v8::NewStringType::kNormal,
                                     (int)val.size())
                 .ToLocalChecked();
    obj->Set(ctx, k, v).Check();
  }
  return obj;
}

inline v8::Local<v8::String> K(v8::Isolate* isolate, const char* s) {
  return v8::String::NewFromUtf8(isolate, s, v8::NewStringType::kNormal)
      .ToLocalChecked();
}

inline v8::Local<v8::Array> Pairs(v8::Isolate* isolate, v8::Local<v8::Context> ctx,
                                  v8::Local<v8::Object> self) {
  auto v = self->Get(ctx, K(isolate, "_e")).ToLocalChecked();
  if (v->IsArray()) return v.As<v8::Array>();
  auto arr = v8::Array::New(isolate);
  self->Set(ctx, K(isolate, "_e"), arr).Check();
  return arr;
}

inline void PushPair(v8::Isolate* isolate, v8::Local<v8::Context> ctx,
                     v8::Local<v8::Array> list, const std::string& k,
                     const std::string& v) {
  auto pair = v8::Array::New(isolate, 2);
  pair->Set(ctx, 0,
            v8::String::NewFromUtf8(isolate, k.c_str(), v8::NewStringType::kNormal,
                                    (int)k.size())
                .ToLocalChecked())
      .Check();
  pair->Set(ctx, 1,
            v8::String::NewFromUtf8(isolate, v.c_str(), v8::NewStringType::kNormal,
                                    (int)v.size())
                .ToLocalChecked())
      .Check();
  list->Set(ctx, list->Length(), pair).Check();
}

inline void FillFromString(v8::Isolate* isolate, v8::Local<v8::Context> ctx,
                           v8::Local<v8::Array> list, std::string s) {
  if (!s.empty() && s[0] == '?') s.erase(0, 1);
  size_t i = 0;
  while (i < s.size()) {
    size_t amp = s.find('&', i);
    if (amp == std::string::npos) amp = s.size();
    std::string pair = s.substr(i, amp - i);
    i = amp + 1;
    if (pair.empty()) continue;
    size_t eq = pair.find('=');
    std::string key = Decode(eq == std::string::npos ? pair : pair.substr(0, eq));
    std::string val = Decode(eq == std::string::npos ? "" : pair.substr(eq + 1));
    PushPair(isolate, ctx, list, key, val);
  }
}

inline void URLSearchParamsCtor(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::Local<v8::Object> self = args.IsConstructCall() ? args.This()
                                                      : v8::Object::New(isolate);
  auto list = v8::Array::New(isolate);
  self->Set(ctx, K(isolate, "_e"), list).Check();

  if (args.Length() >= 1 && args[0]->IsString()) {
    v8::String::Utf8Value s(isolate, args[0]);
    FillFromString(isolate, ctx, list, *s ? std::string(*s, s.length()) : "");
  } else if (args.Length() >= 1 && args[0]->IsObject() && !args[0]->IsArray()) {
    auto obj = args[0].As<v8::Object>();
    auto names = obj->GetOwnPropertyNames(ctx).ToLocalChecked();
    for (uint32_t i = 0; i < names->Length(); ++i) {
      auto key = names->Get(ctx, i).ToLocalChecked();
      auto val = obj->Get(ctx, key).ToLocalChecked();
      v8::String::Utf8Value ks(isolate, key);
      v8::String::Utf8Value vs(isolate, val);
      PushPair(isolate, ctx, list, *ks ? std::string(*ks, ks.length()) : "",
               *vs ? std::string(*vs, vs.length()) : "");
    }
  }

  auto get = [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    v8::Isolate* iso = a.GetIsolate();
    v8::HandleScope h(iso);
    auto ctx = iso->GetCurrentContext();
    if (a.Length() < 1) return;
    v8::String::Utf8Value want(iso, a[0]);
    std::string w = *want ? std::string(*want, want.length()) : "";
    auto list = Pairs(iso, ctx, a.This());
    for (uint32_t i = 0; i < list->Length(); ++i) {
      auto pair = list->Get(ctx, i).ToLocalChecked().As<v8::Array>();
      v8::String::Utf8Value k(iso, pair->Get(ctx, 0).ToLocalChecked());
      if (*k && std::string(*k, k.length()) == w) {
        a.GetReturnValue().Set(pair->Get(ctx, 1).ToLocalChecked());
        return;
      }
    }
    a.GetReturnValue().SetNull();
  };
  auto has = [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    v8::Isolate* iso = a.GetIsolate();
    v8::HandleScope h(iso);
    auto ctx = iso->GetCurrentContext();
    v8::String::Utf8Value want(iso, a.Length() ? a[0] : v8::Undefined(iso));
    std::string w = *want ? std::string(*want, want.length()) : "";
    auto list = Pairs(iso, ctx, a.This());
    bool found = false;
    for (uint32_t i = 0; i < list->Length(); ++i) {
      auto pair = list->Get(ctx, i).ToLocalChecked().As<v8::Array>();
      v8::String::Utf8Value k(iso, pair->Get(ctx, 0).ToLocalChecked());
      if (*k && std::string(*k, k.length()) == w) found = true;
    }
    a.GetReturnValue().Set(found);
  };
  auto set = [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    v8::Isolate* iso = a.GetIsolate();
    v8::HandleScope h(iso);
    auto ctx = iso->GetCurrentContext();
    if (a.Length() < 2) return;
    v8::String::Utf8Value ks(iso, a[0]);
    v8::String::Utf8Value vs(iso, a[1]);
    std::string key = *ks ? std::string(*ks, ks.length()) : "";
    std::string val = *vs ? std::string(*vs, vs.length()) : "";
    auto list = Pairs(iso, ctx, a.This());
    auto fresh = v8::Array::New(iso);
    bool replaced = false;
    for (uint32_t i = 0; i < list->Length(); ++i) {
      auto pair = list->Get(ctx, i).ToLocalChecked().As<v8::Array>();
      v8::String::Utf8Value k(iso, pair->Get(ctx, 0).ToLocalChecked());
      if (*k && std::string(*k, k.length()) == key) {
        if (!replaced) {
          PushPair(iso, ctx, fresh, key, val);
          replaced = true;
        }
      } else {
        fresh->Set(ctx, fresh->Length(), pair).Check();
      }
    }
    if (!replaced) PushPair(iso, ctx, fresh, key, val);
    a.This()->Set(ctx, K(iso, "_e"), fresh).Check();
  };
  auto append = [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    v8::Isolate* iso = a.GetIsolate();
    v8::HandleScope h(iso);
    auto ctx = iso->GetCurrentContext();
    if (a.Length() < 2) return;
    v8::String::Utf8Value ks(iso, a[0]);
    v8::String::Utf8Value vs(iso, a[1]);
    PushPair(iso, ctx, Pairs(iso, ctx, a.This()),
             *ks ? std::string(*ks, ks.length()) : "",
             *vs ? std::string(*vs, vs.length()) : "");
  };
  auto del = [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    v8::Isolate* iso = a.GetIsolate();
    v8::HandleScope h(iso);
    auto ctx = iso->GetCurrentContext();
    if (a.Length() < 1) return;
    v8::String::Utf8Value ks(iso, a[0]);
    std::string key = *ks ? std::string(*ks, ks.length()) : "";
    auto list = Pairs(iso, ctx, a.This());
    auto fresh = v8::Array::New(iso);
    for (uint32_t i = 0; i < list->Length(); ++i) {
      auto pair = list->Get(ctx, i).ToLocalChecked().As<v8::Array>();
      v8::String::Utf8Value k(iso, pair->Get(ctx, 0).ToLocalChecked());
      if (*k && std::string(*k, k.length()) == key) continue;
      fresh->Set(ctx, fresh->Length(), pair).Check();
    }
    a.This()->Set(ctx, K(iso, "_e"), fresh).Check();
  };
  auto toString = [](const v8::FunctionCallbackInfo<v8::Value>& a) {
    v8::Isolate* iso = a.GetIsolate();
    v8::HandleScope h(iso);
    auto ctx = iso->GetCurrentContext();
    auto list = Pairs(iso, ctx, a.This());
    std::string out;
    for (uint32_t i = 0; i < list->Length(); ++i) {
      auto pair = list->Get(ctx, i).ToLocalChecked().As<v8::Array>();
      v8::String::Utf8Value k(iso, pair->Get(ctx, 0).ToLocalChecked());
      v8::String::Utf8Value v(iso, pair->Get(ctx, 1).ToLocalChecked());
      if (!out.empty()) out += '&';
      out += Encode(*k ? std::string(*k, k.length()) : "");
      out += '=';
      out += Encode(*v ? std::string(*v, v.length()) : "");
    }
    a.GetReturnValue().Set(
        v8::String::NewFromUtf8(iso, out.c_str(), v8::NewStringType::kNormal,
                                (int)out.size())
            .ToLocalChecked());
  };

  auto def = [&](const char* name, v8::FunctionCallback cb) {
    self->Set(ctx, K(isolate, name), v8::Function::New(ctx, cb).ToLocalChecked())
        .Check();
  };
  def("get", get);
  def("has", has);
  def("set", set);
  def("append", append);
  def("delete", del);
  def("toString", toString);

  args.GetReturnValue().Set(self);
}

}  // namespace qsdetail

inline v8::Local<v8::Function> MakeURLSearchParams(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  auto ctx = isolate->GetCurrentContext();
  auto fn = v8::Function::New(ctx, qsdetail::URLSearchParamsCtor).ToLocalChecked();
  fn->SetName(v8::String::NewFromUtf8Literal(isolate, "URLSearchParams"));
  return hs.Escape(fn);
}

inline v8::Local<v8::Object> MakeQueryStringModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  auto ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "parse"),
           v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
             v8::Isolate* iso = args.GetIsolate();
             v8::HandleScope h(iso);
             auto c = iso->GetCurrentContext();
             std::string s;
             if (args.Length() >= 1 && args[0]->IsString()) {
               v8::String::Utf8Value u(iso, args[0]);
               if (*u) s.assign(*u, u.length());
             }
             args.GetReturnValue().Set(qsdetail::ParseToObject(iso, c, s));
           }).ToLocalChecked())
      .Check();

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "stringify"),
           v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
             v8::Isolate* iso = args.GetIsolate();
             v8::HandleScope h(iso);
             auto c = iso->GetCurrentContext();
             std::string out;
             if (args.Length() >= 1 && args[0]->IsObject()) {
               auto o = args[0].As<v8::Object>();
               auto names = o->GetOwnPropertyNames(c).ToLocalChecked();
               for (uint32_t i = 0; i < names->Length(); ++i) {
                 auto key = names->Get(c, i).ToLocalChecked();
                 auto val = o->Get(c, key).ToLocalChecked();
                 v8::String::Utf8Value ks(iso, key);
                 v8::String::Utf8Value vs(iso, val);
                 if (!out.empty()) out += '&';
                 out += qsdetail::Encode(*ks ? std::string(*ks, ks.length()) : "");
                 out += '=';
                 out += qsdetail::Encode(*vs ? std::string(*vs, vs.length()) : "");
               }
             }
             args.GetReturnValue().Set(
                 v8::String::NewFromUtf8(iso, out.c_str(), v8::NewStringType::kNormal,
                                         (int)out.size())
                     .ToLocalChecked());
           }).ToLocalChecked())
      .Check();

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "escape"),
           v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
             v8::Isolate* iso = args.GetIsolate();
             v8::String::Utf8Value s(iso, args.Length() ? args[0] : v8::Undefined(iso));
             std::string enc = qsdetail::Encode(*s ? std::string(*s, s.length()) : "");
             args.GetReturnValue().Set(
                 v8::String::NewFromUtf8(iso, enc.c_str(), v8::NewStringType::kNormal,
                                         (int)enc.size())
                     .ToLocalChecked());
           }).ToLocalChecked())
      .Check();

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "unescape"),
           v8::Function::New(ctx, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
             v8::Isolate* iso = args.GetIsolate();
             v8::String::Utf8Value s(iso, args.Length() ? args[0] : v8::Undefined(iso));
             std::string dec = qsdetail::Decode(*s ? std::string(*s, s.length()) : "");
             args.GetReturnValue().Set(
                 v8::String::NewFromUtf8(iso, dec.c_str(), v8::NewStringType::kNormal,
                                         (int)dec.size())
                     .ToLocalChecked());
           }).ToLocalChecked())
      .Check();

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "URLSearchParams"),
           MakeURLSearchParams(isolate))
      .Check();
  return hs.Escape(obj);
}

}}  // nx8v::bindings
