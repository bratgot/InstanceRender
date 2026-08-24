#pragma once

// Cryptomatte: the hashing, the float conversion and the manifest.
//
// The whole format rests on one agreement - the id written into the image and
// the id written into the manifest have to be the SAME number, arrived at the
// same way. Nuke's Cryptomatte node hashes the name you type, converts it, and
// looks for that float in the image. If the two sides disagree by so much as a
// bit, a name selects nothing at all and there is no error to read: you get an
// empty matte and no reason for it. So this is the one file the format lives in,
// and crypto_hash_test.cpp pins it against published vectors.
//
// Two things that are easy to get wrong, and both are silent:
//
//   the conversion is an EXPONENT CLAMP, not a NaN fix-up. The sign and the
//   mantissa are kept exactly; only the exponent is pulled into [1, 254], which
//   is what keeps the result finite and non-denormal.
//
//   the MANIFEST holds the RAW 32-bit hash as eight hex digits - not the bits of
//   the converted float. Nuke does the conversion itself when it matches.

#include <cstdint>
#include <cstring>
#include <string>

// Matches Math.h, which usually gets here first. It defined IR_HD as "inline"
// while this defined it as nothing and then wrote "IR_HD inline" at every
// function - so in any translation unit holding both, that expanded to
// "inline inline". MSVC accepts that silently; gcc rejects it outright, which
// is how it was found.
#ifndef IR_HD
#  ifdef __CUDACC__
#    define IR_HD __host__ __device__ __forceinline__
#  else
#    define IR_HD inline
#  endif
#endif

namespace ir {

// MurmurHash3 x86 32-bit, the variant the cryptomatte specification names.
inline uint32_t murmur3_32(const void* data, size_t len, uint32_t seed = 0)
{
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  const size_t nblocks = len / 4;
  uint32_t h1 = seed;
  const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;

  for (size_t i = 0; i < nblocks; ++i) {
    uint32_t k1;
    std::memcpy(&k1, bytes + i * 4, 4);          // the blocks are little endian
    k1 *= c1;
    k1 = (k1 << 15) | (k1 >> 17);
    k1 *= c2;
    h1 ^= k1;
    h1 = (h1 << 13) | (h1 >> 19);
    h1 = h1 * 5u + 0xe6546b64u;
  }

  const uint8_t* tail = bytes + nblocks * 4;
  uint32_t k1 = 0;
  switch (len & 3u) {
    case 3: k1 ^= uint32_t(tail[2]) << 16;   // fall through
    case 2: k1 ^= uint32_t(tail[1]) << 8;    // fall through
    case 1: k1 ^= uint32_t(tail[0]);
            k1 *= c1;
            k1 = (k1 << 15) | (k1 >> 17);
            k1 *= c2;
            h1 ^= k1;
    default: break;
  }

  h1 ^= uint32_t(len);
  h1 ^= h1 >> 16;
  h1 *= 0x85ebca6bu;
  h1 ^= h1 >> 13;
  h1 *= 0xc2b2ae35u;
  h1 ^= h1 >> 16;
  return h1;
}

// A 32-bit hash as the float that goes in the image.
//
// The sign and the mantissa survive untouched; the exponent is clamped away from
// 0 and 255, which are the two that would make the result denormal or infinite -
// neither of which survives a trip through an image file and a comparison.
inline float cryptoHashToFloat(uint32_t hash)
{
  const uint32_t mantissa = hash & ((1u << 23) - 1);
  uint32_t exponent = (hash >> 23) & 0xffu;
  if (exponent < 1u) exponent = 1u;
  if (exponent > 254u) exponent = 254u;
  const uint32_t bits = (hash & 0x80000000u) | (exponent << 23) | mantissa;
  float f = 0.0f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

inline float cryptoIdOf(const std::string& name)
{
  return cryptoHashToFloat(murmur3_32(name.data(), name.size()));
}

// Eight lower-case hex digits of the RAW hash, which is what a manifest holds.
inline std::string cryptoManifestHex(const std::string& name)
{
  const uint32_t h = murmur3_32(name.data(), name.size());
  static const char* digits = "0123456789abcdef";
  std::string out(8, '0');
  for (int i = 0; i < 8; ++i) out[7 - i] = digits[(h >> (i * 4)) & 0xfu];
  return out;
}

// The seven hex digits that name a cryptomatte's metadata keys.
inline std::string cryptoTypeKey(const std::string& typeName)
{
  return cryptoManifestHex(typeName).substr(0, 7);
}

// JSON needs its quotes and backslashes escaped, and prim paths are full of
// neither - but a name arriving from a stage is not ours to trust.
inline std::string cryptoJsonEscape(const std::string& s)
{
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\t') out += "\\t";
    else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
    else out += c;
  }
  return out;
}

// ---- the per-pixel table -------------------------------------------------------
//
// Pairs of (id, coverage), kept in the pixel's own slice of the output buffer so
// that nothing per-thread has to hold them - see the note on kMaxLightGroups in
// Scene.h for what a per-thread array of this size costs on the GPU.

// Add one sample's worth of coverage for an id, merging with what is there.
// A table that is full displaces its smallest entry, which is the one that would
// have been dropped by the ranking anyway.
IR_HD void cryptoAdd(float* table, int slots, float id, float weight)
{
  if (!table || slots <= 0 || weight <= 0.0f) return;
  int smallest = 0;
  for (int i = 0; i < slots; ++i) {
    if (table[i * 2 + 1] <= 0.0f) {              // an empty slot: take it
      table[i * 2] = id;
      table[i * 2 + 1] = weight;
      return;
    }
    if (table[i * 2] == id) {                    // seen before: add to it
      table[i * 2 + 1] += weight;
      return;
    }
    if (table[i * 2 + 1] < table[smallest * 2 + 1]) smallest = i;
  }
  if (weight > table[smallest * 2 + 1]) {        // full: displace the least of them
    table[smallest * 2] = id;
    table[smallest * 2 + 1] = weight;
  }
}

// Heaviest coverage first, which is what "rank" means in the format.  Insertion
// sort: the tables are tiny and it is the same code on both devices.
IR_HD void cryptoSort(float* table, int slots)
{
  if (!table) return;
  for (int i = 1; i < slots; ++i) {
    const float id = table[i * 2], w = table[i * 2 + 1];
    int j = i - 1;
    while (j >= 0 && table[j * 2 + 1] < w) {
      table[(j + 1) * 2] = table[j * 2];
      table[(j + 1) * 2 + 1] = table[j * 2 + 1];
      --j;
    }
    table[(j + 1) * 2] = id;
    table[(j + 1) * 2 + 1] = w;
  }
}

// ---- the per-pixel deep table --------------------------------------------------
//
// One entry per SURFACE a pixel sees:
//
//   [id, coverage, nearest depth, farthest depth, r, g, b]
//
// Same shape as the cryptomatte table and for the same reason - it lives in the
// pixel's own slice of the output buffer, so nothing per-thread holds it.
//
// TWO depths, not an average, because of motion blur.  A surface that moves
// through the shutter is not AT a distance during it, it sweeps a range of them,
// and a deep sample says so: front at the nearest it came, back at the farthest.
// A still surface reports the same number twice, which is what a hard surface
// means and what everything downstream expects.  Averaging the two would put a
// blurred object at a distance it never occupied.
//
// Coverage is counted in samples and divided at the end; colour is the whole
// path's radiance for the samples that STARTED on that surface, which is what
// makes the entries add up to the beauty.
static const int kDeepSlotFloats = 7;

// Find this id's entry, or claim an empty one.  Returns -1 when the table is
// full of surfaces that are all covering more than this one - which is the same
// judgement the ranking would make later.
IR_HD int deepSlotFor(float* table, int slots, float id)
{
  if (!table || slots <= 0) return -1;
  int smallest = 0;
  for (int i = 0; i < slots; ++i) {
    float* e = table + i * kDeepSlotFloats;
    if (e[1] <= 0.0f) { e[0] = id; return i; }        // empty
    if (e[0] == id) return i;                          // seen before
    if (e[1] < table[smallest * kDeepSlotFloats + 1]) smallest = i;
  }
  return -1;
}

// Record one sample's distance to a surface, widening the range it has swept.
IR_HD void deepAddDepth(float* table, int slot, float z)
{
  if (!table || slot < 0) return;
  float* e = table + slot * kDeepSlotFloats;
  if (e[1] <= 0.0f) { e[2] = z; e[3] = z; return; }   // first sample on this surface
  if (z < e[2]) e[2] = z;
  if (z > e[3]) e[3] = z;
}

// Heaviest coverage first is NOT what deep wants: it wants FRONT first, because
// that is the order "over" composites in and Nuke's deep ops assume it.
IR_HD void deepSortByDepth(float* table, int slots)
{
  if (!table) return;
  for (int i = 1; i < slots; ++i) {
    float tmp[kDeepSlotFloats];
    for (int k = 0; k < kDeepSlotFloats; ++k) tmp[k] = table[i * kDeepSlotFloats + k];
    if (tmp[1] <= 0.0f) continue;                      // empty entries stay put
    int j = i - 1;
    while (j >= 0) {
      const float* e = table + j * kDeepSlotFloats;
      const bool jEmpty = (e[1] <= 0.0f);
      if (!jEmpty && e[2] <= tmp[2]) break;
      for (int k = 0; k < kDeepSlotFloats; ++k)
        table[(j + 1) * kDeepSlotFloats + k] = e[k];
      --j;
    }
    for (int k = 0; k < kDeepSlotFloats; ++k) table[(j + 1) * kDeepSlotFloats + k] = tmp[k];
  }
}

} // namespace ir
