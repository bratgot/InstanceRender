// InstanceRender - MatchInstances.h
// Pairing instances across the shutter when they have no usable identity.
//
// Motion blur needs to know where each object was at shutter close.  With a
// stable id per object that is a lookup, but Nuke's particles do not give one:
// a scene of emitted copies carries no id at all, and where ids do exist the
// particle system RECYCLES them - a particle that dies hands its id to one born
// somewhere else.
//
// Matching by position in the list is worse than it looks.  It is not that a
// few pairs go wrong: one particle dying shifts every particle after it by one
// place, so they ALL pair with a neighbour instead of themselves, and the whole
// cloud smears by roughly the spacing between particles.  That is also why an
// outlier test cannot save it - the wrong answers are the majority, so they set
// the median.
//
// So they are paired by where they ARE.  Each instance looks for the nearest
// one at the other end of the shutter, and a pair is only believed if each is
// the other's nearest and they are close enough to be the same particle.
// Anything else is held still, which shows as no blur rather than a streak.
//
// Strict ASCII.
#pragma once

#include "Math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ir {

// A uniform grid over a set of points, for nearest-neighbour lookups.
class PointGrid {
public:
  void build(const std::vector<Vec3>& pts, float cell)
  {
    _pts = &pts;
    _cell = (cell > 1e-9f) ? cell : 1.0f;
    _cells.clear();
    for (size_t i = 0; i < pts.size(); ++i) _cells[key(pts[i])].push_back(int(i));
  }

  // The nearest point to p within 'radius', or -1.  'exclude' skips one index,
  // which is how a point asks for its nearest NEIGHBOUR rather than itself.
  // Ties go to the lower index, so the pairing does not depend on which order
  // the cells were walked.
  //
  // The search grows a cube outwards a ring at a time and stops as soon as the
  // best candidate is closer than the ring it just finished - anything nearer
  // would have been inside it.  Scanning the whole radius up front instead
  // costs (2 * radius / cell + 1)^3 cells per query, which for a radius the
  // size of the scene is the difference between a render and a hang.
  int nearest(const Vec3& p, float radius, float* distOut = nullptr, int exclude = -1,
              const std::vector<char>* taken = nullptr) const
  {
    if (!_pts) return -1;
    const int cx = int(std::floor(p.x / _cell)), cy = int(std::floor(p.y / _cell)), cz = int(std::floor(p.z / _cell));
    const int maxReach = int(std::ceil(radius / _cell));
    int best = -1;
    float bestD2 = radius * radius;
    for (int reach = 1; reach <= maxReach; ++reach) {
      for (int dz = -reach; dz <= reach; ++dz) {
        for (int dy = -reach; dy <= reach; ++dy) {
          for (int dx = -reach; dx <= reach; ++dx) {
            // only the new shell: the inside was walked on an earlier pass
            if (reach > 1 && std::abs(dx) != reach && std::abs(dy) != reach && std::abs(dz) != reach) continue;
            ++_probes;
            const Cells::const_iterator it = _cells.find(key(cx + dx, cy + dy, cz + dz));
            if (it == _cells.end()) continue;
            for (size_t k = 0; k < it->second.size(); ++k) {
              const int i = it->second[k];
              if (i == exclude) continue;
              if (taken && (*taken)[size_t(i)]) continue;     // already paired off
              const Vec3& q = (*_pts)[size_t(i)];
              const float ex = q.x - p.x, ey = q.y - p.y, ez = q.z - p.z;
              const float d2 = ex * ex + ey * ey + ez * ez;
              if (d2 < bestD2 || (d2 == bestD2 && best >= 0 && i < best)) { bestD2 = d2; best = i; }
            }
          }
        }
      }
      // a point outside the cube just searched cannot beat one inside it
      const float reached = float(reach) * _cell;
      if (best >= 0 && bestD2 <= reached * reached) break;
    }
    if (best >= 0 && distOut) *distOut = std::sqrt(bestD2);
    return best;
  }

  // Cells examined since the last reset.  This, not the clock, is what says
  // whether the search is doing the right amount of work: a search that scans
  // a fixed radius examines (2 * radius / cell + 1)^3 cells per query and so
  // grows with the cloud, while the ring search examines a handful however big
  // the cloud gets.  test/match_scale_test.cpp holds it to that - wall-clock
  // cannot, because the per-point cost climbs several fold on cache alone.
  size_t probes() const { return _probes; }
  void resetProbes() const { _probes = 0; }

private:
  // cells are hashed, not ordered: nothing here needs them in order, and a
  // lookup per cell per query adds up fast
  typedef std::unordered_map<uint64_t, std::vector<int> > Cells;

  static uint64_t key(int x, int y, int z)
  {
    const uint64_t a = uint64_t(uint32_t(x)) * 0x9E3779B97F4A7C15ull;
    const uint64_t b = uint64_t(uint32_t(y)) * 0xC2B2AE3D27D4EB4Full;
    const uint64_t c = uint64_t(uint32_t(z)) * 0x165667B19E3779F9ull;
    return a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2)) ^ (c + (b << 6) + (b >> 2));
  }
  uint64_t key(const Vec3& p) const
  {
    return key(int(std::floor(p.x / _cell)), int(std::floor(p.y / _cell)), int(std::floor(p.z / _cell)));
  }

  const std::vector<Vec3>* _pts = nullptr;
  float _cell = 1.0f;
  Cells _cells;
  mutable size_t _probes = 0;
};

// The typical distance between neighbouring points: the median of each point's
// distance to its nearest other point.  This is the scale that says whether two
// positions can be the same particle a shutter apart.
inline float medianSpacing(const std::vector<Vec3>& pts)
{
  if (pts.size() < 2) return 0.0f;
  Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
  for (size_t i = 0; i < pts.size(); ++i) {
    lo.x = std::min(lo.x, pts[i].x); hi.x = std::max(hi.x, pts[i].x);
    lo.y = std::min(lo.y, pts[i].y); hi.y = std::max(hi.y, pts[i].y);
    lo.z = std::min(lo.z, pts[i].z); hi.z = std::max(hi.z, pts[i].z);
  }
  const float ex = hi.x - lo.x, ey = hi.y - lo.y, ez = hi.z - lo.z;
  const float diag = std::sqrt(ex * ex + ey * ey + ez * ez);
  if (!(diag > 0.0f)) return 0.0f;
  // a cell sized so roughly one point lands in each, which is what makes the
  // ring search terminate after a ring or two
  const float cell = diag / std::max(2.0f, std::cbrt(float(pts.size())));
  PointGrid grid;
  grid.build(pts, cell);

  // A median needs a sample, not every point: a few hundred is as good an
  // estimate as a million and keeps this linear in the cloud size.
  const size_t want = pts.size() < 512 ? pts.size() : 512;
  const size_t step = pts.size() / want;
  // and each of those looks only a few cells out - a point with no neighbour
  // nearby simply does not contribute
  const float lookRadius = cell * 8.0f;
  std::vector<float> d;
  d.reserve(want);
  for (size_t i = 0; i < pts.size(); i += (step > 0 ? step : 1)) {
    float best = 0.0f;
    const int j = grid.nearest(pts[i], lookRadius, &best, int(i));   // the nearest OTHER point
    if (j >= 0 && best > 0.0f) d.push_back(best);
  }
  if (d.empty()) return 0.0f;
  std::sort(d.begin(), d.end());
  return d[d.size() / 2];
}

// The median distance a pairing says things moved, or -1 if it paired nothing.
// The wrong pairing scatters objects across the gaps between them, so this is
// what tells two candidate pairings apart without a threshold to tune.
inline double medianTravel(const std::vector<Vec3>& open,
                           const std::vector<Vec3>& close,
                           const std::vector<int>& match)
{
  std::vector<double> d;
  d.reserve(match.size());
  for (size_t i = 0; i < match.size() && i < open.size(); ++i) {
    if (match[i] < 0 || size_t(match[i]) >= close.size()) continue;
    const Vec3& a = open[i];
    const Vec3& b = close[size_t(match[i])];
    const double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    d.push_back(std::sqrt(dx * dx + dy * dy + dz * dz));
  }
  if (d.empty()) return -1.0;
  std::sort(d.begin(), d.end());
  return d[d.size() / 2];
}

// Pairs 'open' positions with 'close' ones by proximity.  Returns one index per
// open position: the close one it belongs to, or -1 for held still.
//
// 'reach' is how many multiples of the typical spacing a particle is allowed to
// have travelled.  Beyond that the answer is genuinely ambiguous - the particle
// could be any of several - and holding still is the honest result.
inline std::vector<int> matchByProximity(const std::vector<Vec3>& open,
                                         const std::vector<Vec3>& close,
                                         float reach,
                                         float* spacingOut = nullptr)
{
  std::vector<int> out(open.size(), -1);
  if (open.empty() || close.empty()) return out;

  const float spacing = medianSpacing(open);
  if (spacingOut) *spacingOut = spacing;
  if (!(spacing > 0.0f)) return out;
  const float radius = spacing * ((reach > 0.0f) ? reach : 1.0f);

  // cells sized by the SPACING, not the search radius: a cell as wide as the
  // radius holds reach^3 points and every query walks all of them
  PointGrid closeGrid, openGrid;
  closeGrid.build(close, spacing);
  openGrid.build(open, spacing);

  // ---- pass one: mutual nearest ----------------------------------------------
  // Both have to name each other.  That is a strong pair and it is never wrong,
  // which is why it goes first and is never revisited.
  std::vector<char> taken(close.size(), 0);
  for (size_t i = 0; i < open.size(); ++i) {
    const int j = closeGrid.nearest(open[i], radius);
    if (j < 0) continue;
    const int back = openGrid.nearest(close[size_t(j)], radius);
    if (back != int(i)) continue;
    out[i] = j;
    taken[size_t(j)] = 1;
  }

  // ---- pass two: everything mutual nearest was too strict for ----------------
  //
  // Mutual nearest alone leaves far too much unpaired, and unpaired means held
  // still, which renders as a SHARP object in the middle of a blurred cloud -
  // the thing that gets reported.  Measured on the scene it was reported from:
  // 211 of 475 objects held still, and not one of them for travelling too far
  // (raising the reach from 4x the spacing to 32x changed nothing).  They fail
  // because in a crowd only the closest pair in each little knot names each
  // other; everyone else is somebody's second choice and gets nothing.
  //
  // Being second choice is not evidence of being wrong, though - it is evidence
  // that somebody else was closer.  So the leftovers are paired shortest-first
  // against whatever is still free, which keeps the one-partner-each rule that
  // made mutual nearest safe while pairing almost everything that is really
  // there.  What stays unpaired after this is the genuine churn: particles born
  // or dying inside the shutter, which have no partner to find.
  struct Candidate { float d; int i, j; };
  std::vector<Candidate> spare;
  spare.reserve(open.size());
  // Rounds, because one pass is not enough: a leftover whose choice gets
  // claimed by a closer pair has to be allowed to look again, and asking once
  // leaves it unpaired next to a partner that is standing free.  Measured on
  // the reported scene, asking once left 103 held still where the real churn
  // was 49.  Each round only looks at what is still unpaired, so this converges
  // quickly and costs a fraction of the first pass.
  for (int round = 0; round < 8; ++round) {
    spare.clear();
    for (size_t i = 0; i < open.size(); ++i) {
      if (out[i] >= 0) continue;
      float d = 0.0f;
      const int j = closeGrid.nearest(open[i], radius, &d, -1, &taken);
      if (j >= 0) {
        Candidate c;
        c.d = d; c.i = int(i); c.j = j;
        spare.push_back(c);
      }
    }
    if (spare.empty()) break;
    // shortest first, so the most believable pairs claim their partner before
    // the doubtful ones do
    std::sort(spare.begin(), spare.end(),
              [](const Candidate& a, const Candidate& b) {
                if (a.d != b.d) return a.d < b.d;
                return a.i < b.i;                  // ties by index, so the answer is stable
              });
    size_t made = 0;
    for (size_t k = 0; k < spare.size(); ++k) {
      const Candidate& c = spare[k];
      if (taken[size_t(c.j)]) continue;            // claimed since the list was built
      out[size_t(c.i)] = c.j;
      taken[size_t(c.j)] = 1;
      ++made;
    }
    if (made == 0) break;
  }
  return out;
}

} // namespace ir
