// InstanceRender - NukeOpImage.cpp
// See NukeOpImage.h.  Strict ASCII.
#include "NukeOpImage.h"
#include "GeoLoader.h"

#include <DDImage/MaterialOpI.h>
#include <DDImage/Op.h>
#include <DDImage/Iop.h>
#include <DDImage/OutputContext.h>

namespace ir {

bool isNukeOpPath(const std::string& path)
{
  if (path.size() > 6 && path.compare(path.size() - 6, 6, ".nkiop") == 0) return true;
  return path.find("/NkRoot/") != std::string::npos;
}

bool loadNukeOpImage(const std::string& path, int maxSize, ImageData& out, std::string& err)
{
  out = ImageData();
  if (path.empty()) { err = "empty texture path"; return false; }

  // The path carries the OutputContext the op was published at - the frame
  // above all - and Foundry's own note says retrieving at the wrong one gets
  // the wrong op, so the context that comes back is the one to use.
  DD::Image::OutputContext ctx;
  DD::Image::Op* op = nullptr;
  try {
#if IR_NUKE_VER >= 1600
    // 16.0 added the overload that hands back the context it parsed
    op = DD::Image::MaterialOpI::retrieveOpFromAssetPath(path, &ctx);
#else
    op = DD::Image::MaterialOpI::retrieveOpFromAssetPath(path);
#endif
  }
  catch (...) {
    err = "Nuke could not resolve the op for " + path;
    return false;
  }
  if (!op) { err = "no Nuke op behind " + path; return false; }

  DD::Image::Iop* iop = dynamic_cast<DD::Image::Iop*>(op);
  if (!iop) { err = "the op behind " + path + " is not an image"; return false; }

  if (!bakeIop(iop, maxSize, out) || !out.valid()) {
    err = "could not render the Nuke op behind " + path;
    return false;
  }
  return true;
}

} // namespace ir
