#include "codec.h"

#include <objidl.h>
#include <gdiplus.h>
#include <cstring>

using namespace Gdiplus;

extern "C" {
size_t WebPEncodeLosslessBGRA(const uint8_t* bgra, int width, int height, int stride, uint8_t** output);
size_t WebPEncodeBGRA(const uint8_t* bgra, int width, int height, int stride, float quality, uint8_t** output);
void WebPFree(void* pointer);
}

namespace snip {
namespace {

bool EncoderClsid(const wchar_t* mime, CLSID& clsid) {
    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0) return false;

    std::vector<uint8_t> buffer(bytes);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) return false;

    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

} // namespace

GdiPlusScope::GdiPlusScope() {
    Gdiplus::GdiplusStartupInput input;
    ok_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok;
    if (!ok_) LogLine("gdiplus: startup failed");
}

GdiPlusScope::~GdiPlusScope() {
    if (ok_) Gdiplus::GdiplusShutdown(token_);
}

std::vector<uint8_t> EncodeWebP(const uint8_t* bgra, int width, int height, int stride, int quality) {
    std::vector<uint8_t> result;
    if (bgra == nullptr || width <= 0 || height <= 0) return result;

    // libwebp's simple API: measured fastest in the VM (216 ms for 800x450 lossless)
    // and byte-identical to the C# build. The advanced API with method/thread tuning was
    // slower (365-2085 ms), so it is not used.
    uint8_t* output = nullptr;
    size_t size = 0;
    if (quality >= 100) {
        size = WebPEncodeLosslessBGRA(bgra, width, height, stride, &output);
    } else {
        float q = static_cast<float>(quality < 1 ? 1 : (quality > 99 ? 99 : quality));
        size = WebPEncodeBGRA(bgra, width, height, stride, q, &output);
    }

    if (size > 0 && output != nullptr) {
        result.assign(output, output + size);
        WebPFree(output);
    }
    return result;
}

bool SaveImage(const std::wstring& path, const uint8_t* bgra, int width, int height, int stride,
               const std::wstring& format) {
    if (format == L"webp") {
        std::vector<uint8_t> bytes = EncodeWebP(bgra, width, height, stride, 100);
        if (bytes.empty()) return false;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                  written == bytes.size();
        CloseHandle(file);
        return ok;
    }

    Bitmap bitmap(width, height, stride, PixelFormat32bppRGB, const_cast<BYTE*>(bgra));
    if (bitmap.GetLastStatus() != Ok) return false;

    CLSID clsid{};
    if (format == L"png") {
        if (!EncoderClsid(L"image/png", clsid)) return false;
        return bitmap.Save(path.c_str(), &clsid, nullptr) == Ok;
    }
    if (format == L"jpg" || format == L"jpeg") {
        if (!EncoderClsid(L"image/jpeg", clsid)) return false;
        EncoderParameters parameters(1);
        parameters.Parameter[0].Guid = EncoderQuality;
        parameters.Parameter[0].Type = EncoderParameterValueTypeLong;
        parameters.Parameter[0].NumberOfValues = 1;
        ULONG quality = 92;
        parameters.Parameter[0].Value = &quality;
        return bitmap.Save(path.c_str(), &clsid, &parameters) == Ok;
    }
    if (format == L"bmp") {
        if (!EncoderClsid(L"image/bmp", clsid)) return false;
        return bitmap.Save(path.c_str(), &clsid, nullptr) == Ok;
    }
    return false;
}

std::vector<uint8_t> EncodePng(const uint8_t* bgra, int width, int height, int stride) {
    std::vector<uint8_t> result;
    if (bgra == nullptr || width <= 0 || height <= 0) return result;

    Gdiplus::Bitmap bitmap(width, height, stride, PixelFormat32bppRGB,
                           const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bgra)));
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        LogLine("png: Bitmap wrap failed");
        return result;
    }

    CLSID clsid{};
    if (!EncoderClsid(L"image/png", clsid)) {
        LogLine("png: encoder not found");
        return result;
    }

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return result;

    Gdiplus::Status status = bitmap.Save(stream, &clsid, nullptr);
    if (status == Gdiplus::Ok) {
        STATSTG stat{};
        if (stream->Stat(&stat, STATFLAG_NONAME) == S_OK) {
            ULONG size = static_cast<ULONG>(stat.cbSize.QuadPart);
            HGLOBAL global = nullptr;
            if (size > 0 && GetHGlobalFromStream(stream, &global) == S_OK) {
                void* data = GlobalLock(global);
                if (data != nullptr) {
                    result.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
                    GlobalUnlock(global);
                }
            }
        }
    } else {
        LogFmt("png: Save failed (%d)", static_cast<int>(status));
    }

    stream->Release();
    return result;
}

} // namespace snip
