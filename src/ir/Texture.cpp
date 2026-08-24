// InstanceRender - Texture.cpp  (strict ASCII)
#include "Texture.h"

#include <pxr/imaging/hio/image.h>
#include <pxr/imaging/hio/types.h>
#include <pxr/base/gf/half.h>

#include <cmath>
#include <cstdio>
#include <sstream>

#ifdef _WIN32
#  include <windows.h>
#endif

PXR_NAMESPACE_USING_DIRECTIVE

namespace ir {

namespace {

float irMaxf(float a, float b) { return a > b ? a : b; }

float srgbDecode(float c)
{
  if (c <= 0.04045f) return c / 12.92f;
  return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

std::string envVar(const char* name)
{
#ifdef _WIN32
  char buf[1024];
  const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
#else
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
#endif
}

} // namespace

bool loadImageFile(const std::string& path, int colorSpace, int maxSize, ImageData& out, std::string& err)
{
  out = ImageData();
  if (path.empty()) { err = "empty texture path"; return false; }
  HioImageSharedPtr img = HioImage::OpenForReading(path, 0, 0, HioImage::SourceColorSpace::Raw, true);
  if (!img) { err = "cannot open image: " + path; return false; }
  const int w = img->GetWidth(), h = img->GetHeight();
  if (w <= 0 || h <= 0) { err = "empty image: " + path; return false; }
  if (double(w) * double(h) > 268435456.0) { err = "image too large: " + path; return false; }

  // The Hio plugins do not convert between component types or counts, so read
  // in the file's OWN format and normalise to float RGBA here.
  const HioFormat fmt = img->GetFormat();
  if (HioIsCompressed(fmt)) { err = "compressed texture formats are not supported: " + path; return false; }
  const HioType type = HioGetHioType(fmt);
  int nc = HioGetComponentCount(fmt);
  if (nc < 1 || nc > 4) { err = "unsupported channel count in " + path; return false; }
  const size_t compSize = HioGetDataSizeOfType(type);
  if (compSize == 0) { err = "unsupported pixel format in " + path; return false; }
  const size_t n = size_t(w) * size_t(h);
  std::vector<unsigned char> raw(n * size_t(nc) * compSize, 0);
  HioImage::StorageSpec spec;
  spec.width = w; spec.height = h; spec.depth = 1;
  spec.format = fmt;
  spec.flipped = false;             // row 0 = top row of the image
  spec.data = raw.data();
  if (!img->Read(spec)) { err = "cannot read image: " + path; return false; }

  out.width = w; out.height = h;
  out.rgba.assign(n * 4, 1.0f);
  const bool eightBit = (type == HioTypeUnsignedByte || type == HioTypeUnsignedByteSRGB);
  const bool decode = (colorSpace == kColorSRGB)
                   || (colorSpace == kColorAuto && eightBit && nc >= 3)
                   || type == HioTypeUnsignedByteSRGB;
  for (size_t i = 0; i < n; ++i) {
    float c[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (int k = 0; k < nc; ++k) {
      const size_t off = (i * size_t(nc) + size_t(k)) * compSize;
      const void* p = &raw[off];
      switch (type) {
        case HioTypeUnsignedByte:
        case HioTypeUnsignedByteSRGB: c[k] = float(*static_cast<const unsigned char*>(p)) / 255.0f; break;
        case HioTypeSignedByte:       c[k] = irMaxf(float(*static_cast<const signed char*>(p)) / 127.0f, -1.0f); break;
        case HioTypeUnsignedShort:    c[k] = float(*static_cast<const unsigned short*>(p)) / 65535.0f; break;
        case HioTypeSignedShort:      c[k] = irMaxf(float(*static_cast<const short*>(p)) / 32767.0f, -1.0f); break;
        case HioTypeUnsignedInt:      c[k] = float(double(*static_cast<const unsigned int*>(p)) / 4294967295.0); break;
        case HioTypeInt:              c[k] = float(double(*static_cast<const int*>(p)) / 2147483647.0); break;
        case HioTypeHalfFloat:        c[k] = float(*static_cast<const GfHalf*>(p)); break;
        case HioTypeFloat:            c[k] = *static_cast<const float*>(p); break;
        case HioTypeDouble:           c[k] = float(*static_cast<const double*>(p)); break;
        default: break;
      }
    }
    float r = c[0], g = (nc >= 3) ? c[1] : c[0], b = (nc >= 3) ? c[2] : c[0];
    const float a = (nc == 4) ? c[3] : ((nc == 2) ? c[1] : 1.0f);
    if (decode) { r = srgbDecode(r); g = srgbDecode(g); b = srgbDecode(b); }
    float* d = &out.rgba[i * 4];
    d[0] = r; d[1] = g; d[2] = b; d[3] = a;
  }
  if (maxSize > 0) {
    int guard = 0;
    while ((out.width > maxSize || out.height > maxSize) && out.width > 1 && out.height > 1 && guard++ < 16)
      halveImage(out);
  }
  return true;
}

void textureProbe(void (*log)(const std::string&))
{
  static bool done = false;
  if (done || !log) return;
  done = true;
  const std::string list = envVar("IR_TEXTURE_PROBE");
  if (list.empty()) return;
  size_t start = 0;
  while (start <= list.size()) {
    size_t sep = list.find(';', start);
    if (sep == std::string::npos) sep = list.size();
    const std::string path = list.substr(start, sep - start);
    start = sep + 1;
    if (path.empty()) continue;
    ImageData img;
    std::string err;
    const bool ok = loadImageFile(path, kColorRaw, 0, img, err);
    std::ostringstream o;
    o << "textureProbe: " << path << " -> " << (ok ? "OK" : "FAILED") << " " << img.width << "x" << img.height;
    if (ok && img.valid()) {
      // corners, to pin down the row order: row 0 of ImageData is the image's TOP row
      const size_t tl = 0;
      const size_t tr = (size_t(img.width) - 1) * 4;
      const size_t bl = size_t(img.height - 1) * size_t(img.width) * 4;
      const size_t br = bl + (size_t(img.width) - 1) * 4;
      o << " row0-left " << img.rgba[tl] << "," << img.rgba[tl + 1] << "," << img.rgba[tl + 2]
        << " row0-right " << img.rgba[tr] << "," << img.rgba[tr + 1] << "," << img.rgba[tr + 2]
        << " rowN-left " << img.rgba[bl] << "," << img.rgba[bl + 1] << "," << img.rgba[bl + 2]
        << " rowN-right " << img.rgba[br] << "," << img.rgba[br + 1] << "," << img.rgba[br + 2];
    }
    if (!ok) o << " err=" << err;
    log(o.str());
  }
}

} // namespace ir
