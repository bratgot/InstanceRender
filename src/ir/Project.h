#pragma once

// The camera's world -> pixel transform as a 4x4 matrix.
//
// Kernel.h already projects a world point to a pixel - projectToPixel(), which
// the motion vectors use.  This is the same projection expressed as a matrix,
// which is what the viewer needs: a 3D handle is drawn through a matrix, so
// putting this one in ViewerContext::modelmatrix makes handles land on the
// pixels the geometry they belong to produced.
//
// The two MUST agree.  They are separate code because one is a per-point
// function that can branch and the other has to be a single matrix, and
// test/project_test.cpp exists to hold them together: it drives both with the
// same cameras and points and compares the pixels.

#include "Scene.h"

namespace ir {

// Row-major, p' = M * p, with the pixel coordinates recovered as x/w and y/w.
// Returns false if the camera cannot describe a projection (no size).
inline bool worldToPixelMatrix(const Camera& cam, float m[16])
{
  const float W = float(cam.width), H = float(cam.height);
  if (W <= 0.0f || H <= 0.0f) return false;

  // the camera basis and position, read out of camera-to-world exactly as
  // projectToPixel() reads them
  const Xform& c = cam.camToWorld;
  const Vec3 pos(c.m[3], c.m[7], c.m[11]);
  const Vec3 cx = normalize(Vec3(c.m[0], c.m[4], c.m[8]));
  const Vec3 cy = normalize(Vec3(c.m[1], c.m[5], c.m[9]));
  const Vec3 cz = normalize(Vec3(-c.m[2], -c.m[6], -c.m[10]));   // the way it looks

  // world -> camera
  const float w2c[16] = {
    cx.x, cx.y, cx.z, -dot(cx, pos),
    cy.x, cy.y, cy.z, -dot(cy, pos),
    cz.x, cz.y, cz.z, -dot(cz, pos),
    0.0f, 0.0f, 0.0f, 1.0f
  };

  // Depth is only here so that handles occlude one another sensibly; the pixel
  // coordinates are what has to be right.  A camera with no usable clip planes
  // still has to produce a matrix, so these fall back rather than fail.
  const float nearZ = (cam.nearClip > 1e-6f) ? cam.nearClip : 0.1f;
  const float farZ = (cam.farClip > nearZ * 1.0001f) ? cam.farClip : (nearZ + 10000.0f);

  float k[16] = {0};
  if (cam.orthographic) {
    const float oW = (cam.orthoHalfW > 1e-6f) ? cam.orthoHalfW : 1.0f;
    const float oH = (cam.orthoHalfH > 1e-6f) ? cam.orthoHalfH : 1.0f;
    k[0] = W / (2.0f * oW);  k[3] = W * 0.5f;
    k[5] = H / (2.0f * oH);  k[7] = H * 0.5f;
    k[10] = 2.0f / (farZ - nearZ); k[11] = -(farZ + nearZ) / (farZ - nearZ);
    k[15] = 1.0f;
  }
  else {
    // px = (x / (z * tanX) * 0.5 + 0.5) * W.  Over a homogeneous divide by z
    // that is ((W/2)(x/tanX) + (W/2)z) / z, which is the column of W/2 in z.
    const float tx = (cam.tanHalfFovX > 1e-6f) ? cam.tanHalfFovX : 0.5f;
    const float ty = (cam.tanHalfFovY > 1e-6f) ? cam.tanHalfFovY : 0.5f;
    k[0] = W / (2.0f * tx);  k[2] = W * 0.5f;
    k[5] = H / (2.0f * ty);  k[6] = H * 0.5f;
    k[10] = (farZ + nearZ) / (farZ - nearZ);
    k[11] = -2.0f * nearZ * farZ / (farZ - nearZ);
    k[14] = 1.0f;            // w = z, so anything behind the camera clips away
  }

  for (int r = 0; r < 4; ++r)
    for (int col = 0; col < 4; ++col) {
      float sum = 0.0f;
      for (int i = 0; i < 4; ++i) sum += k[r * 4 + i] * w2c[i * 4 + col];
      m[r * 4 + col] = sum;
    }
  return true;
}

} // namespace ir
