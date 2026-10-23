#pragma once
// nx8v/src/bindings/os.h
// require('os') — operating-system information.
//
//   os.platform()   os.arch()      os.type()      os.release()
//   os.hostname()   os.homedir()   os.tmpdir()    os.endianness()
//   os.EOL          os.cpus()      os.totalmem()  os.freemem()
//   os.uptime()

#include <v8.h>
#include <uv.h>

#include <string>
#include <vector>
#include <cstdlib>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace nx8v {
namespace bindings {

namespace osdetail {

inline v8::Local<v8::String> Str(v8::Isolate* iso, const std::string& s) {
  return v8::String::NewFromUtf8(iso, s.c_str(), v8::NewStringType::kNormal,
                                 (int)s.size()).ToLocalChecked();
}

inline std::string Hostname() {
  char buf[256];
  size_t len = sizeof(buf);
  if (uv_os_gethostname(buf, &len) == 0) return std::string(buf, len);
  return "localhost";
}

inline std::string HomeDir() {
  char buf[1024];
  size_t len = sizeof(buf);
  if (uv_os_homedir(buf, &len) == 0) return std::string(buf, len);
  const char* h = std::getenv("USERPROFILE");
  if (h) return h;
#ifdef _WIN32
  h = std::getenv("HOMEDRIVE");
  const char* p = std::getenv("HOMEPATH");
  if (h && p) return std::string(h) + p;
#endif
  return "";
}

inline std::string TempDir() {
  size_t len = 0;
  if (uv_os_tmpdir(nullptr, &len) == UV_ENOBUFS && len > 0) {
    std::vector<char> buf(len);
    if (uv_os_tmpdir(buf.data(), &len) == 0) return std::string(buf.data());
  }
  const char* t = std::getenv("TEMP");
  if (!t) t = std::getenv("TMP");
  return t ? t : ".";
}

#ifdef _WIN32
inline std::string WindowsProductName() {
  HKEY key;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
    return "Windows";
  wchar_t buf[256];
  DWORD size = sizeof(buf);
  DWORD type = 0;
  std::string result = "Windows";
  if (RegQueryValueExW(key, L"ProductName", nullptr, &type,
                       reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
      std::string utf8(n - 1, '\0');
      WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), n, nullptr, nullptr);
      result = utf8;
    }
  }
  RegCloseKey(key);
  return result;
}
#endif
} // namespace osdetail

// ---------------------------------------------------------------------------
// require('os')
// ---------------------------------------------------------------------------
inline v8::Local<v8::Object> MakeOsModule(v8::Isolate* isolate) {
  v8::EscapableHandleScope hs(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  auto obj = v8::Object::New(isolate);

  auto def = [&](const char* name,
                 v8::FunctionCallback callback) {
    auto fn = v8::Function::New(ctx, callback).ToLocalChecked();
    obj->Set(ctx, osdetail::Str(isolate, name), fn).Check();
  };

  def("platform", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    const char* pl =
#ifdef _WIN32
        "win32";
#elif defined(__APPLE__)
        "darwin";
#else
        "linux";
#endif
    args.GetReturnValue().Set(
        v8::String::NewFromUtf8(iso, pl, v8::NewStringType::kNormal).ToLocalChecked());
  });

  def("arch", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    const char* a =
#if defined(_M_X64) || defined(__x86_64__)
        "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
        "arm64";
#elif defined(_M_IX86) || defined(__i386__)
        "ia32";
#else
        "unknown";
#endif
    args.GetReturnValue().Set(
        v8::String::NewFromUtf8(iso, a, v8::NewStringType::kNormal).ToLocalChecked());
  });

  def("type", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    args.GetReturnValue().Set(v8::String::NewFromUtf8Literal(iso,
#ifdef _WIN32
        "Windows_NT"
#elif defined(__APPLE__)
        "Darwin"
#else
        "Linux"
#endif
    ));
  });

  def("release", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    uv_utsname_t u;
    if (uv_os_uname(&u) == 0)
      args.GetReturnValue().Set(osdetail::Str(iso, u.release));
    else
      args.GetReturnValue().SetEmptyString();
  });

  def("hostname", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    args.GetReturnValue().Set(osdetail::Str(iso, osdetail::Hostname()));
  });

  def("homedir", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    args.GetReturnValue().Set(osdetail::Str(iso, osdetail::HomeDir()));
  });

  def("tmpdir", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    args.GetReturnValue().Set(osdetail::Str(iso, osdetail::TempDir()));
  });

  def("endianness", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    uint16_t x = 1;
    const std::string e = (*reinterpret_cast<uint8_t*>(&x) == 1) ? "LE" : "BE";
    args.GetReturnValue().Set(osdetail::Str(iso, e));
  });

  def("totalmem", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(
        v8::Number::New(args.GetIsolate(), (double)uv_get_total_memory()));
  });

  def("freemem", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(
        v8::Number::New(args.GetIsolate(), (double)uv_get_free_memory()));
  });

  def("uptime", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    double up = 0.0;
    if (uv_uptime(&up) != 0) up = 0.0;
    args.GetReturnValue().Set(v8::Number::New(args.GetIsolate(), up));
  });

  def("cpus", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    auto c = iso->GetCurrentContext();
    auto out = v8::Array::New(iso);

    uv_cpu_info_t* cpu_infos = nullptr;
    int count = 0;
    if (uv_cpu_info(&cpu_infos, &count) != 0 || count == 0) {
      args.GetReturnValue().Set(out);
      return;
    }
    for (int i = 0; i < count; ++i) {
      auto o = v8::Object::New(iso);
      auto model = osdetail::Str(iso, cpu_infos[i].model ? cpu_infos[i].model : "");
      int speed = cpu_infos[i].speed;
      auto times = v8::Object::New(iso);
      auto& t = cpu_infos[i].cpu_times;
      times->Set(c, v8::String::NewFromUtf8Literal(iso, "user"),
                 v8::Number::New(iso, (double)t.user)).Check();
      times->Set(c, v8::String::NewFromUtf8Literal(iso, "nice"),
                 v8::Number::New(iso, (double)t.nice)).Check();
      times->Set(c, v8::String::NewFromUtf8Literal(iso, "sys"),
                 v8::Number::New(iso, (double)t.sys)).Check();
      times->Set(c, v8::String::NewFromUtf8Literal(iso, "idle"),
                 v8::Number::New(iso, (double)t.idle)).Check();
      times->Set(c, v8::String::NewFromUtf8Literal(iso, "irq"),
                 v8::Number::New(iso, (double)t.irq)).Check();
      o->Set(c, v8::String::NewFromUtf8Literal(iso, "model"), model).Check();
      o->Set(c, v8::String::NewFromUtf8Literal(iso, "speed"),
             v8::Integer::New(iso, speed)).Check();
      o->Set(c, v8::String::NewFromUtf8Literal(iso, "times"), times).Check();
      out->Set(c, i, o).Check();
    }
    uv_free_cpu_info(cpu_infos, count);
    args.GetReturnValue().Set(out);
  });

  // Constant properties
  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "EOL"),
           v8::String::NewFromUtf8Literal(isolate,
#ifdef _WIN32
               "\r\n"
#else
               "\n"
#endif
           )).Check();

  obj->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "devNull"),
           v8::String::NewFromUtf8Literal(isolate,
#ifdef _WIN32
               "\\\\.\\nul"
#else
               "/dev/null"
#endif
           )).Check();

  // os.platform() also exposed as a string on some builds is not standard;
  // keep the function form. Provide os.version() too (Node 12+).
  def("networkInterfaces", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    v8::HandleScope h(iso);
    auto c = iso->GetCurrentContext();
    auto out = v8::Object::New(iso);

    auto add = [&](const std::string& name, const std::string& address,
                   const std::string& family, const std::string& netmask,
                   bool internal) {
      auto key = osdetail::Str(iso, name);
      auto cur = out->Get(c, key).ToLocalChecked();
      v8::Local<v8::Array> list;
      if (cur->IsArray()) list = cur.As<v8::Array>();
      else {
        list = v8::Array::New(iso);
        out->Set(c, key, list).Check();
      }
      auto item = v8::Object::New(iso);
      item->Set(c, v8::String::NewFromUtf8Literal(iso, "address"),
                osdetail::Str(iso, address)).Check();
      item->Set(c, v8::String::NewFromUtf8Literal(iso, "family"),
                osdetail::Str(iso, family)).Check();
      item->Set(c, v8::String::NewFromUtf8Literal(iso, "netmask"),
                osdetail::Str(iso, netmask)).Check();
      item->Set(c, v8::String::NewFromUtf8Literal(iso, "internal"),
                v8::Boolean::New(iso, internal)).Check();
      list->Set(c, list->Length(), item).Check();
    };

    uv_interface_address_t* info = nullptr;
    int count = 0;
    if (uv_interface_addresses(&info, &count) == 0) {
      for (int i = 0; i < count; ++i) {
        char ip[128];
        char mask[128];
        ip[0] = mask[0] = 0;
        std::string family = "IPv4";
        if (info[i].address.address4.sin_family == AF_INET6) {
          family = "IPv6";
          uv_ip6_name(&info[i].address.address6, ip, sizeof(ip));
          uv_ip6_name(&info[i].netmask.netmask6, mask, sizeof(mask));
        } else {
          uv_ip4_name(&info[i].address.address4, ip, sizeof(ip));
          uv_ip4_name(&info[i].netmask.netmask4, mask, sizeof(mask));
        }
        std::string addr = ip;
        auto pct = addr.find('%');
        if (pct != std::string::npos) addr.resize(pct);
        add(info[i].name ? info[i].name : "if", addr, family, mask,
            info[i].is_internal != 0);
      }
      uv_free_interface_addresses(info, count);
    }
    args.GetReturnValue().Set(out);
  });

  def("version", [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    v8::Isolate* iso = args.GetIsolate();
    uv_utsname_t u;
    if (uv_os_uname(&u) == 0)
      args.GetReturnValue().Set(
          v8::String::NewFromUtf8(iso, u.version, v8::NewStringType::kNormal)
              .ToLocalChecked());
    else
      args.GetReturnValue().SetEmptyString();
  });

  return hs.Escape(obj);
}

} // namespace bindings
} // namespace nx8v
