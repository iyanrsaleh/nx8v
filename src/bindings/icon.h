#pragma once
// Load a window icon from PNG, JPEG, BMP, WEBP, or ICO.
// Packaging turns the same file into an .ico resource inside the exe.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

namespace nx8v {
namespace bindings {

inline bool Nx8vIsIco(const std::vector<unsigned char>& d) {
  return d.size() >= 6 && d[0] == 0 && d[1] == 0 && d[2] == 1 && d[3] == 0;
}

inline bool Nx8vIsPng(const std::vector<unsigned char>& d) {
  return d.size() >= 24 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
}

inline bool Nx8vPngSize(const std::vector<unsigned char>& d, uint32_t& w, uint32_t& h) {
  if (!Nx8vIsPng(d)) return false;
  w = (uint32_t)d[16] << 24 | (uint32_t)d[17] << 16 | (uint32_t)d[18] << 8 | d[19];
  h = (uint32_t)d[20] << 24 | (uint32_t)d[21] << 16 | (uint32_t)d[22] << 8 | d[23];
  return w > 0 && h > 0 && w <= 256 && h <= 256;
}

inline void Nx8vPut16(std::vector<unsigned char>& o, uint16_t v) {
  o.push_back((unsigned char)(v & 0xFF));
  o.push_back((unsigned char)((v >> 8) & 0xFF));
}

inline void Nx8vPut32(std::vector<unsigned char>& o, uint32_t v) {
  o.push_back((unsigned char)(v & 0xFF));
  o.push_back((unsigned char)((v >> 8) & 0xFF));
  o.push_back((unsigned char)((v >> 16) & 0xFF));
  o.push_back((unsigned char)((v >> 24) & 0xFF));
}

// One PNG image stored inside an ICO. Windows uses the PNG bytes as the icon.
inline std::vector<unsigned char> Nx8vWrapPngIco(const std::vector<unsigned char>& png) {
  uint32_t w = 0, h = 0;
  if (!Nx8vPngSize(png, w, h)) return {};
  std::vector<unsigned char> out;
  out.reserve(22 + png.size());
  Nx8vPut16(out, 0);
  Nx8vPut16(out, 1);
  Nx8vPut16(out, 1);
  out.push_back(w >= 256 ? 0 : (unsigned char)w);
  out.push_back(h >= 256 ? 0 : (unsigned char)h);
  out.push_back(0);
  out.push_back(0);
  Nx8vPut16(out, 1);
  Nx8vPut16(out, 32);
  Nx8vPut32(out, (uint32_t)png.size());
  Nx8vPut32(out, 22);
  out.insert(out.end(), png.begin(), png.end());
  return out;
}

inline std::vector<unsigned char> Nx8vReadFileBytes(const std::wstring& path) {
  std::ifstream in(std::filesystem::path(path), std::ios::binary);
  if (!in) return {};
  return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
}

inline std::vector<unsigned char> Nx8vDibIco(uint32_t w, uint32_t h,
                                            const std::vector<unsigned char>& bgra) {
  if (w == 0 || h == 0 || w > 256 || h > 256 || bgra.size() < w * h * 4) return {};
  uint32_t xor_stride = w * 4;
  uint32_t and_stride = ((w + 31) / 32) * 4;
  uint32_t xor_size = xor_stride * h;
  uint32_t and_size = and_stride * h;
  std::vector<unsigned char> img(40 + xor_size + and_size, 0);
  // BITMAPINFOHEADER, little-endian, written field by field.
  auto* p = img.data();
  auto u32 = [&](size_t at, uint32_t v) {
    p[at] = (unsigned char)(v & 0xFF);
    p[at + 1] = (unsigned char)((v >> 8) & 0xFF);
    p[at + 2] = (unsigned char)((v >> 16) & 0xFF);
    p[at + 3] = (unsigned char)((v >> 24) & 0xFF);
  };
  auto u16 = [&](size_t at, uint16_t v) {
    p[at] = (unsigned char)(v & 0xFF);
    p[at + 1] = (unsigned char)((v >> 8) & 0xFF);
  };
  u32(0, 40);
  u32(4, w);
  u32(8, h * 2);
  u16(12, 1);
  u16(14, 32);
  u32(20, xor_size);
  for (uint32_t y = 0; y < h; ++y) {
    const unsigned char* src = bgra.data() + (size_t)(h - 1 - y) * xor_stride;
    memcpy(p + 40 + (size_t)y * xor_stride, src, xor_stride);
  }
  std::vector<unsigned char> out;
  Nx8vPut16(out, 0);
  Nx8vPut16(out, 1);
  Nx8vPut16(out, 1);
  out.push_back(w >= 256 ? 0 : (unsigned char)w);
  out.push_back(h >= 256 ? 0 : (unsigned char)h);
  out.push_back(0);
  out.push_back(0);
  Nx8vPut16(out, 1);
  Nx8vPut16(out, 32);
  Nx8vPut32(out, (uint32_t)img.size());
  Nx8vPut32(out, 22);
  out.insert(out.end(), img.begin(), img.end());
  return out;
}

inline std::vector<unsigned char> Nx8vDecodeToIco(const std::wstring& path) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  bool uninit = SUCCEEDED(hr) && hr != S_FALSE;
  IWICImagingFactory* factory = nullptr;
  hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&factory));
  std::vector<unsigned char> ico;
  if (SUCCEEDED(hr) && factory) {
    IWICBitmapDecoder* dec = nullptr;
    factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                      WICDecodeMetadataCacheOnLoad, &dec);
    IWICBitmapFrameDecode* frame = nullptr;
    if (dec) dec->GetFrame(0, &frame);
    IWICFormatConverter* conv = nullptr;
    if (frame) factory->CreateFormatConverter(&conv);
    if (conv && SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                                           WICBitmapDitherTypeNone, nullptr, 0.0,
                                           WICBitmapPaletteTypeCustom))) {
      UINT w = 0, h = 0;
      conv->GetSize(&w, &h);
      IWICBitmapSource* src = conv;
      IWICBitmapScaler* scaler = nullptr;
      if (w > 256 || h > 256) {
        UINT dw = w, dh = h;
        if (dw > 256) { dh = dh * 256 / dw; dw = 256; }
        if (dh > 256) { dw = dw * 256 / dh; dh = 256; }
        if (!dw) dw = 1;
        if (!dh) dh = 1;
        if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(conv, dw, dh, WICBitmapInterpolationModeFant))) {
          src = scaler;
          w = dw;
          h = dh;
        }
      }
      std::vector<unsigned char> px((size_t)w * h * 4);
      if (!px.empty() &&
          SUCCEEDED(src->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data())))
        ico = Nx8vDibIco(w, h, px);
      if (scaler) scaler->Release();
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    factory->Release();
  }
  if (uninit) CoUninitialize();
  return ico;
}

inline std::vector<unsigned char> Nx8vFileToIco(const std::wstring& path) {
  std::vector<unsigned char> raw = Nx8vReadFileBytes(path);
  if (Nx8vIsIco(raw)) return raw;
  if (Nx8vIsPng(raw)) {
    std::vector<unsigned char> wrapped = Nx8vWrapPngIco(raw);
    if (!wrapped.empty()) return wrapped;
  }
  return Nx8vDecodeToIco(path);
}

inline HICON Nx8vLoadIconFile(const std::wstring& path) {
  std::wstring ext;
  auto dot = path.find_last_of(L'.');
  if (dot != std::wstring::npos) ext = path.substr(dot);
  for (auto& c : ext) c = (wchar_t)towlower(c);
  if (ext == L".ico") {
    HICON icon = (HICON)LoadImageW(nullptr, path.c_str(), IMAGE_ICON, 0, 0,
                                  LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (icon) return icon;
  }
  std::vector<unsigned char> ico = Nx8vFileToIco(path);
  if (ico.empty()) return nullptr;
  wchar_t tmp[MAX_PATH];
  wchar_t file[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, tmp)) return nullptr;
  if (!GetTempFileNameW(tmp, L"nx8", 0, file)) return nullptr;
  {
    std::ofstream out(file, std::ios::binary);
    out.write((const char*)ico.data(), (std::streamsize)ico.size());
  }
  HICON icon = (HICON)LoadImageW(nullptr, file, IMAGE_ICON, 0, 0,
                                LR_LOADFROMFILE | LR_DEFAULTSIZE);
  DeleteFileW(file);
  return icon;
}

}  // namespace bindings
}  // namespace nx8v
#endif
