// InstanceRender - Image.h
// A float RGBA image and its mip chain, with no dependency on USD or on Nuke:
// both front ends fill one of these (the USD one through pxr Hio, the classic
// one by baking a material Iop), and Nuke 14.1 has no pxr at all.
// Strict ASCII.
#pragma once

#include <cstddef>   // size_t: MSVC gets it transitively, gcc does not
#include <vector>

namespace ir {

struct ImageData {
  int width = 0, height = 0;
  std::vector<float> rgba;      // 4 floats per texel, row 0 = TOP row of the image
  bool valid() const { return width > 0 && height > 0 && rgba.size() == size_t(width) * size_t(height) * 4; }
};

// average 2x2 texels; leaves an odd last row/column by clamping
inline void halveImage(ImageData& img)
{
  const int w = img.width, h = img.height;
  const int nw = (w + 1) / 2, nh = (h + 1) / 2;
  std::vector<float> dst(size_t(nw) * size_t(nh) * 4, 0.0f);
  for (int y = 0; y < nh; ++y) {
    const int y0 = 2 * y, y1 = (2 * y + 1 < h) ? 2 * y + 1 : y0;
    for (int x = 0; x < nw; ++x) {
      const int x0 = 2 * x, x1 = (2 * x + 1 < w) ? 2 * x + 1 : x0;
      for (int c = 0; c < 4; ++c) {
        const float s = img.rgba[(size_t(y0) * w + x0) * 4 + c] + img.rgba[(size_t(y0) * w + x1) * 4 + c]
                      + img.rgba[(size_t(y1) * w + x0) * 4 + c] + img.rgba[(size_t(y1) * w + x1) * 4 + c];
        dst[(size_t(y) * nw + x) * 4 + c] = s * 0.25f;
      }
    }
  }
  img.width = nw; img.height = nh; img.rgba.swap(dst);
}

// Box-filtered mip chain, level 0 first, each level half the size of the one
// before it (down to 1x1).  Returns the number of levels.
inline int buildMipChain(const ImageData& base, std::vector<ImageData>& levels)
{
  levels.clear();
  if (!base.valid()) return 0;
  levels.push_back(base);
  while (levels.back().width > 1 || levels.back().height > 1) {
    ImageData next = levels.back();
    halveImage(next);
    if (next.width == levels.back().width && next.height == levels.back().height) break;
    levels.push_back(next);
    if (levels.size() >= 14) break;
  }
  return int(levels.size());
}

} // namespace ir
