// IES photometric profiles (IESNA LM-63).
//
// A real luminaire does not put out the same light in every direction: a
// downlight has a tight pool, a wall washer throws sideways, a bollard makes a
// figure of eight.  Lighting a set from a manufacturer's photometry is the
// normal way to get that, and without it every spot is the same soft cone.
//
// The file is an ASCII table of candela values over a grid of vertical and
// horizontal angles.  It is resampled here onto a FIXED grid, once, on the host:
// a kernel doing a bilinear lookup in a small table is the whole cost at render
// time, and the alternative - carrying each file's own irregular angle lists to
// the GPU - buys accuracy nothing downstream can see.
//
// The values are NORMALISED to their own peak. A profile carries absolute
// candela, often in the thousands, and multiplying that into a light whose
// intensity an artist already set would make every IES light blow out. So this
// decides the SHAPE and the light's own intensity decides the brightness - the
// same split the blackbody code makes for temperature.
// Strict ASCII.
#pragma once

#include "Scene.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ir {

// kIesVRes / kIesHRes live in Scene.h, because the kernel does the lookup.
struct IesProfile {
  std::vector<float> table;    // kIesVRes * kIesHRes, normalised to a peak of 1
  IesProfile() : table(size_t(kIesVRes) * size_t(kIesHRes), 0.0f) {}
};

// Read every number in a stream, ignoring the keyword block and any commas.
// LM-63 files in the wild break the layout rules constantly - values wrapped at
// odd places, tabs, CRLF, trailing junk - so the parse is deliberately about
// "the next number" rather than about lines.
inline bool iesReadNumbers(std::istream& in, std::vector<double>& out)
{
  std::string line;
  bool started = false;
  while (std::getline(in, line)) {
    // TILT= is the last thing before the numbers begin
    if (!started) {
      const size_t t = line.find("TILT");
      if (t != std::string::npos) {
        if (line.find("NONE") == std::string::npos) return false;   // a tilt table needs its own parse
        started = true;
      }
      continue;
    }
    for (size_t i = 0; i < line.size(); ++i)
      if (line[i] == ',') line[i] = ' ';
    std::istringstream ls(line);
    double v;
    while (ls >> v) out.push_back(v);
  }
  return started && !out.empty();
}

// Parse a file into a normalised table.  False means it was not readable as
// LM-63 at all, which the caller reports rather than guessing at.
inline bool iesLoad(const std::string& path, IesProfile& prof)
{
  std::ifstream f(path.c_str());
  if (!f.good()) return false;
  std::vector<double> n;
  if (!iesReadNumbers(f, n)) return false;
  // 10 values on the first data line, 3 on the second, then the angle lists
  if (n.size() < 13) return false;
  const int numV = int(n[3] + 0.5);
  const int numH = int(n[4] + 0.5);
  const double candelaMul = n[2];
  if (numV <= 0 || numH <= 0) return false;
  const size_t need = 13 + size_t(numV) + size_t(numH) + size_t(numV) * size_t(numH);
  if (n.size() < need) return false;
  const double ballast = n[10];
  size_t p = 13;
  std::vector<double> vAng(n.begin() + p, n.begin() + p + numV); p += size_t(numV);
  std::vector<double> hAng(n.begin() + p, n.begin() + p + numH); p += size_t(numH);
  const double scale = ((candelaMul != 0.0) ? candelaMul : 1.0) * ((ballast != 0.0) ? ballast : 1.0);

  // resample onto the fixed grid, vertical 0..180, horizontal 0..360
  double peak = 0.0;
  for (int iv = 0; iv < kIesVRes; ++iv) {
    const double va = 180.0 * double(iv) / double(kIesVRes - 1);
    // the bracketing pair in this file's own vertical list
    int v0 = 0;
    while (v0 + 1 < numV && vAng[size_t(v0) + 1] < va) ++v0;
    const int v1 = (v0 + 1 < numV) ? v0 + 1 : v0;
    const double vspan = vAng[size_t(v1)] - vAng[size_t(v0)];
    const double vt = (vspan > 1e-9) ? (va - vAng[size_t(v0)]) / vspan : 0.0;
    for (int ih = 0; ih < kIesHRes; ++ih) {
      double ha = 360.0 * double(ih) / double(kIesHRes);
      // A file may cover only a quadrant or a half and mean it to be mirrored -
      // that is what the last horizontal angle says. Ignoring it leaves a light
      // lit on one side and black on the other.
      const double hMax = hAng[size_t(numH) - 1];
      if (hMax <= 0.0) ha = 0.0;
      else if (hMax <= 90.0)  { ha = std::fmod(ha, 180.0); if (ha > 90.0) ha = 180.0 - ha; }
      else if (hMax <= 180.0) { if (ha > 180.0) ha = 360.0 - ha; }
      int h0 = 0;
      while (h0 + 1 < numH && hAng[size_t(h0) + 1] < ha) ++h0;
      const int h1 = (h0 + 1 < numH) ? h0 + 1 : h0;
      const double hspan = hAng[size_t(h1)] - hAng[size_t(h0)];
      const double ht = (hspan > 1e-9) ? (ha - hAng[size_t(h0)]) / hspan : 0.0;
      const double c00 = n[p + size_t(h0) * size_t(numV) + size_t(v0)];
      const double c01 = n[p + size_t(h0) * size_t(numV) + size_t(v1)];
      const double c10 = n[p + size_t(h1) * size_t(numV) + size_t(v0)];
      const double c11 = n[p + size_t(h1) * size_t(numV) + size_t(v1)];
      const double c = (c00 * (1.0 - vt) + c01 * vt) * (1.0 - ht)
                     + (c10 * (1.0 - vt) + c11 * vt) * ht;
      const double val = c * scale;
      prof.table[size_t(iv) * size_t(kIesHRes) + size_t(ih)] = float(val);
      if (val > peak) peak = val;
    }
  }
  if (peak <= 0.0) return false;
  const float inv = float(1.0 / peak);
  for (size_t i = 0; i < prof.table.size(); ++i) prof.table[i] *= inv;
  return true;
}

} // namespace ir
