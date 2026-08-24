// InstanceRender - NukeOpImage.h
// A texture that is a Nuke node rather than a file.
//
// When a texture is wired from a Nuke node - a CheckerBoard into a
// PreviewSurface, a Read into a GeoDomeLight - Nuke does not give the renderer
// a filename.  It gives an asset path naming the op, with the frame in it:
//
//   nkop:/NkRoot/Read1:12:main:0xffffffffffffffff:0[1,1,0,0]:0x9740....nkiop
//
// Reading one of those through Hio only works while Nuke happens to be holding
// a texture image for that op, and hands back "a default 1x1 grey texture"
// otherwise - which is why such a texture came and went along the timeline.
//
// The op can be fetched directly instead: MaterialOpI::retrieveOpFromAssetPath()
// parses the path and returns the Op at the OutputContext the path names, and
// from there it is an ordinary Nuke image.  Foundry's own header warns that
// getting that context wrong leads to "anomalous texture behavior in Hydra and
// ScanlineRender", which is the bug from the other side.
//
// Both front ends use this - the node and the Hydra delegate - so a scene looks
// the same whichever one renders it.  Strict ASCII.
#pragma once

#include "Image.h"

#include <string>

namespace ir {

// Is this a Nuke op asset path rather than a file?
bool isNukeOpPath(const std::string& path);

// Renders the op the path names into an image.  'maxSize' box-downsamples
// anything larger (0 = full resolution).  Returns false with a reason in 'err'.
bool loadNukeOpImage(const std::string& path, int maxSize, ImageData& out, std::string& err);

} // namespace ir
