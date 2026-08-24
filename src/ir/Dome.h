// InstanceRender - Dome.h
// The 2D distribution a dome light is importance sampled with: without it a
// small bright sun in an HDRI is pure noise at any sane sample count.
//
// Shared by both front ends - the USD loader and the Hydra delegate - so a dome
// light is sampled the same way whichever one built the scene.  Strict ASCII.
#pragma once

#include "Scene.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace ir {

// pbrt-style: box-average the luminance of the lat-long image into a coarse
// grid weighted by sin(theta), then build a conditional CDF per row and a
// marginal CDF over the rows.  Clears the distribution if the image is black.
inline void buildDomeDistribution(Scene& scene, int domeTexture)
{
  scene.domeFunc.clear(); scene.domeMarginal.clear(); scene.domeConditional.clear();
  scene.domeW = scene.domeH = 0; scene.domeFuncInt = 0.0f;
  if (domeTexture < 0 || size_t(domeTexture) >= scene.textures.size()) return;
  const TextureDesc& td = scene.textures[size_t(domeTexture)];
  if (td.width <= 0 || td.height <= 0) return;
  const int cw = (td.width < 512) ? td.width : 512;
  const int ch = (td.height < 256) ? td.height : 256;
  scene.domeW = cw; scene.domeH = ch;
  scene.domeFunc.assign(size_t(cw) * size_t(ch), 0.0f);
  for (int y = 0; y < ch; ++y) {
    const int y0 = int((double(y) * td.height) / ch), y1 = int((double(y + 1) * td.height) / ch);
    const float sinT = std::sin(3.14159265f * (float(y) + 0.5f) / float(ch));
    for (int x = 0; x < cw; ++x) {
      const int x0 = int((double(x) * td.width) / cw), x1 = int((double(x + 1) * td.width) / cw);
      double sum = 0.0; int n = 0;
      for (int yy = y0; yy < (y1 > y0 ? y1 : y0 + 1) && yy < td.height; ++yy) {
        for (int xx = x0; xx < (x1 > x0 ? x1 : x0 + 1) && xx < td.width; ++xx) {
          const float* p = &scene.texels[(size_t(td.firstTexel) + size_t(yy) * size_t(td.width) + size_t(xx)) * 4];
          sum += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
          ++n;
        }
      }
      const float lum = (n > 0) ? float(sum / n) : 0.0f;
      scene.domeFunc[size_t(y) * size_t(cw) + size_t(x)] = (lum > 0.0f ? lum : 0.0f) * sinT;
    }
  }
  // conditional CDFs (one per row) and the marginal CDF over rows
  scene.domeConditional.assign(size_t(ch) * size_t(cw + 1), 0.0f);
  scene.domeMarginal.assign(size_t(ch) + 1, 0.0f);
  std::vector<double> rowInt(size_t(ch), 0.0);
  for (int y = 0; y < ch; ++y) {
    float* cdf = &scene.domeConditional[size_t(y) * size_t(cw + 1)];
    double acc = 0.0;
    for (int x = 0; x < cw; ++x) { acc += scene.domeFunc[size_t(y) * size_t(cw) + size_t(x)]; cdf[x + 1] = float(acc); }
    rowInt[size_t(y)] = acc;
    if (acc > 0.0) for (int x = 1; x <= cw; ++x) cdf[x] = float(cdf[x] / acc);
    else for (int x = 1; x <= cw; ++x) cdf[x] = float(x) / float(cw);   // uniform in an empty row
  }
  double total = 0.0;
  for (int y = 0; y < ch; ++y) { total += rowInt[size_t(y)]; scene.domeMarginal[size_t(y) + 1] = float(total); }
  if (total > 0.0) for (int y = 1; y <= ch; ++y) scene.domeMarginal[size_t(y)] = float(scene.domeMarginal[size_t(y)] / total);
  else for (int y = 1; y <= ch; ++y) scene.domeMarginal[size_t(y)] = float(y) / float(ch);
  scene.domeFuncInt = float(total / (double(cw) * double(ch)));
  if (std::getenv("IR_DOME_PROBE")) {
    size_t badF = 0, badC = 0;
    float mxF = 0.0f;
    for (size_t i = 0; i < scene.domeFunc.size(); ++i) {
      const float v = scene.domeFunc[i];
      if (!(v == v) || v > 3.0e38f) ++badF; else if (v > mxF) mxF = v;
    }
    for (size_t i = 0; i < scene.domeConditional.size(); ++i)
      if (!(scene.domeConditional[i] == scene.domeConditional[i])) ++badC;
    std::cerr << "IR_DOME: funcInt " << scene.domeFuncInt << " maxFunc " << mxF
              << " nonFiniteFunc " << badF << " nanCond " << badC << std::endl;
  }
  if (!(scene.domeFuncInt > 0.0f)) {
    scene.domeFunc.clear(); scene.domeMarginal.clear(); scene.domeConditional.clear();
    scene.domeW = scene.domeH = 0;
  }
}

} // namespace ir
