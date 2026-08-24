// Blackbody emission: a temperature in Kelvin as a colour.
//
// WHY THIS EXISTS.  A simulation's "temperature" grid holds KELVIN, not
// radiance.  Measured on an aerial explosion: density peaks at 0.89 and flames at
// 7.3, but temperature peaks at 8336 with a mean of 1059.  Multiplying that in as
// though it were brightness is what turned an emission scale of 1 into 46361 in
// the viewer.
//
// What every renderer does instead - Houdini's Karma, Arnold, Cycles - is treat
// the grid as a temperature, evaluate Planck's law at it, and NORMALISE, so that
// the temperature decides the COLOUR and an intensity decides the brightness.
// Without the normalisation Planck's law is worse than useless here: its absolute
// radiance at 3000 K is around 1e12, and the whole point is that 3000 K should
// look like a flame rather than obliterate the frame.
//
// Two ways to get the colour, and they disagree in a way that is visible:
//
//   PRIMARIES  evaluate the normalised Planck curve at one wavelength per
//              channel.  Cheap, and what the spectral renderer next door does for
//              its light colour temperature.  It is a three-point sample of a
//              smooth curve, so it drifts from the real colour at the ends - too
//              magenta down at 1000 K.
//
//   SPECTRAL   integrate the curve against the CIE colour matching functions
//              across the visible range and convert XYZ to RGB.  This is the
//              colour an eye would actually see, and it is what makes the cool
//              part of a fireball read as deep red rather than pink.
//
// Both are built into a lookup table on the host, once, so the kernel does one
// texture-free lookup per sample instead of an integral.
#pragma once

#include "Scene.h"

#include <cmath>
#include <vector>

namespace ir {

// Planck's law normalised so the curve PEAKS AT ONE, whatever the temperature.
//
// Wien's displacement gives the peak wavelength, and the value there is the same
// constant for every temperature, so dividing by it costs nothing and removes the
// 1e12 that makes the absolute form unusable.  Same shape the spectral renderer
// uses for its light colour temperatures.
inline float blackbodyNorm(float lambdaNm, float kelvin)
{
  if (kelvin < 100.0f) return 0.0f;
  const float x = 14387768.0f / (lambdaNm * kelvin);   // hc/k in nm.K
  if (x > 80.0f) return 0.0f;                          // underflow guard
  const float peakL = 2898000.0f / kelvin;             // Wien
  const float ratio = peakL / lambdaNm;
  const float r5 = ratio * ratio * ratio * ratio * ratio;
  return r5 * 142.327f / (std::exp(x) - 1.0f);         // exp(4.96511)-1
}

// The CIE colour matching functions, as the multi-lobe Gaussian fits from Wyman,
// Sloan and Shirley.  A table would be more exact and a great deal more code; the
// error here is far below what a fireball's colour is ever asked to hold.
inline float cieG(float x, float mu, float s1, float s2)
{
  const float t = (x - mu) * ((x < mu) ? 1.0f / s1 : 1.0f / s2);
  return std::exp(-0.5f * t * t);
}
inline float cieX(float w)
{
  return 1.056f * cieG(w, 599.8f, 37.9f, 31.0f)
       + 0.362f * cieG(w, 442.0f, 16.0f, 26.7f)
       - 0.065f * cieG(w, 501.1f, 20.4f, 26.2f);
}
inline float cieY(float w)
{
  return 0.821f * cieG(w, 568.8f, 46.9f, 40.5f)
       + 0.286f * cieG(w, 530.9f, 16.3f, 31.1f);
}
inline float cieZ(float w)
{
  return 1.217f * cieG(w, 437.0f, 11.8f, 36.0f)
       + 0.681f * cieG(w, 459.0f, 26.0f, 13.8f);
}

// One temperature as linear RGB, normalised so the brightest channel is 1.
//
// Normalising on the MAX rather than on luminance keeps every temperature at the
// same headroom - an artist raising the intensity gets a predictable result
// instead of one that also depends on how far into the red the fire is.
inline Vec3 blackbodyRGB(float kelvin, bool spectral)
{
  float r, g, b;
  if (spectral) {
    double X = 0.0, Y = 0.0, Z = 0.0;
    for (float w = 380.0f; w <= 780.0f; w += 5.0f) {
      const double p = double(blackbodyNorm(w, kelvin));
      X += p * double(cieX(w));
      Y += p * double(cieY(w));
      Z += p * double(cieZ(w));
    }
    // XYZ -> linear sRGB (Rec.709 primaries, D65)
    r = float( 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z);
    g = float(-0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z);
    b = float( 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z);
  }
  else {
    // one wavelength per channel, near the centre of each primary
    r = blackbodyNorm(630.0f, kelvin);
    g = blackbodyNorm(532.0f, kelvin);
    b = blackbodyNorm(465.0f, kelvin);
  }
  // A negative here means the colour is outside the RGB gamut, which the very
  // cool end genuinely is.  Clipping to zero is what keeps it a colour.
  if (r < 0.0f) r = 0.0f;
  if (g < 0.0f) g = 0.0f;
  if (b < 0.0f) b = 0.0f;
  const float m = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
  if (m <= 0.0f) return Vec3(0.0f);
  return Vec3(r / m, g / m, b / m);
}

// The table the kernel reads.  Built once per render; a lookup and a lerp is all
// that is left at sample time, where an integral would be absurd.
inline void buildBlackbodyLut(Scene& scene, float minK, float maxK, bool spectral)
{
  scene.bbMinK = minK;
  scene.bbMaxK = maxK;
  scene.blackbody.resize(size_t(kBlackbodyLutSize));
  for (int i = 0; i < kBlackbodyLutSize; ++i) {
    const float t = float(i) / float(kBlackbodyLutSize - 1);
    scene.blackbody[size_t(i)] = blackbodyRGB(minK + (maxK - minK) * t, spectral);
  }
}

} // namespace ir
