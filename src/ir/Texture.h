// InstanceRender - Texture.h
// Image loading for UsdUVTexture maps and dome-light HDRIs.  Images become
// flat float RGBA arrays that BOTH back-ends sample with the same bilinear
// code in Kernel.h - a cudaTextureObject_t would filter at reduced precision
// and break the CPU/GPU parity this project exists for.  Strict ASCII.
#pragma once

#include <map>

#include "Image.h"
#include "Scene.h"      // kMaxMipLevels

#include <string>
#include <vector>

namespace ir {

// UsdUVTexture sourceColorSpace
enum ColorSpace { kColorRaw = 0, kColorSRGB = 1, kColorAuto = 2 };

// Reads anything Nuke's Hio plugins can (exr, png, jpg, tif, hdr, tga ...).
//   colorSpace: kColorAuto decodes sRGB for 8-bit 3/4 channel files (the
//               UsdPreviewSurface rule) and leaves everything else alone
//   maxSize:    0 = full resolution, else box-downsample until max(w,h) <= maxSize
bool loadImageFile(const std::string& path, int colorSpace, int maxSize, ImageData& out, std::string& err);

// Set IR_TEXTURE_PROBE=<file> to have the loader read that file once and report
// through 'log' - the smoke test for Hio plugin discovery inside Nuke.

// A texture, built once and kept.
//
// Building one is expensive and the scene is rebuilt constantly: every drag of a
// GeoCard handle re-reads the stage, and a 4096x2048 texture fed by a Nuke node
// was costing 85 ms to bake and 200 ms to mip and copy - on every redraw, for an
// image that had not changed.  That is most of what made the viewport jerky to
// work in.
//
// So the finished texels are kept, keyed by everything that decides their
// content.  A rebuild then costs one memcpy instead of a bake and a mip chain.
// Nuke's own texture paths make good keys: an "nkop:" path carries a hash of the
// op's state, so it changes exactly when the picture does.
struct CachedTexture {
  std::vector<float> texels;       // level 0 first, then the mip chain
  int width = 0, height = 0;
  int mipCount = 1;
  int mipOffset[kMaxMipLevels] = {0};   // RELATIVE to the start of texels
  int mipW[kMaxMipLevels] = {0};
  int mipH[kMaxMipLevels] = {0};
};

class TextureCache {
public:
  const CachedTexture* find(const std::string& key) const
  {
    std::map<std::string, CachedTexture>::const_iterator it = _byKey.find(key);
    return it == _byKey.end() ? nullptr : &it->second;
  }
  void put(const std::string& key, const CachedTexture& t) { _byKey[key] = t; }
  // Bounded, because these are large and a session can wander through many.
  // Cleared wholesale rather than aged: the case this exists for is one texture
  // rebuilt over and over, not a working set.
  void trim(size_t maxTextures = 8)
  {
    if (_byKey.size() > maxTextures) _byKey.clear();
  }
  size_t size() const { return _byKey.size(); }
private:
  std::map<std::string, CachedTexture> _byKey;
};

void textureProbe(void (*log)(const std::string&));

} // namespace ir
