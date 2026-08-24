# InstanceRender plugin folder init.py (installed as ~/.nuke/InstanceRender/init.py)
#
# One build per Nuke MINOR version - the NDK is not compatible across 16.0 and
# 16.1, let alone 14 and 17 - so load the folder that matches what is running,
# and say so plainly when there is not one.
import os
import nuke

_here = os.path.dirname(os.path.abspath(__file__))
_want = (nuke.NUKE_VERSION_MAJOR, nuke.NUKE_VERSION_MINOR)


def _versions():
    for name in sorted(os.listdir(_here)):
        if not name.startswith("nuke"):
            continue
        try:
            parts = name[4:].split(".")
            yield (int(parts[0]), int(parts[1]) if len(parts) > 1 else 0), name
        except ValueError:
            continue


# Exact match only.  An older build in a newer Nuke does not merely misbehave -
# it fails to load with "the specified procedure could not be found", so saying
# which build is missing is far more use than quietly feeding it the wrong one.
_exact = [n for v, n in _versions() if v == _want]
if _exact:
    _build = os.path.join(_here, _exact[0])
    nuke.pluginAddPath(_build)
    # Icons live in one folder rather than a copy per version, and Nuke resolves
    # a menu icon="Foo.png" against the plugin path - so the folder has to be on
    # it. The 24px file is the icon; Nuke picks up the @2x twin beside it on a
    # high-dpi display by itself.
    _icons = os.path.join(_here, "icons")
    if os.path.isdir(_icons):
        nuke.pluginAddPath(_icons)
    # The Hydra render delegate, where this build has one.  Nuke finds renderers
    # through USD's own plugin registry, and it scans that registry BEFORE this
    # file runs - so setting PXR_PLUGINPATH_NAME here is too late and the
    # delegate would never appear.  Registering the plugin directly does work at
    # this point, and the renderer then shows up in the Viewer's renderer menu
    # and in GeoRender.  The environment variable is set as well, for anything
    # that scans later (and so a child process inherits it).
    _hydra = os.path.join(_build, "hydra", "hdInstanceRender", "resources")
    if os.path.isdir(_hydra):
        _paths = [p for p in os.environ.get("PXR_PLUGINPATH_NAME", "").split(os.pathsep) if p]
        if _hydra not in _paths:
            _paths.append(_hydra)
            os.environ["PXR_PLUGINPATH_NAME"] = os.pathsep.join(_paths)
        try:
            from pxr import Plug
            Plug.Registry().RegisterPlugins(_hydra)
        except Exception as _e:
            nuke.tprint("InstanceRender: could not register the Hydra delegate (%s)" % _e)
else:
    _have = ", ".join(n for _, n in sorted(_versions())) or "none"
    nuke.tprint("InstanceRender: no build for Nuke %d.%d in %s (installed: %s)"
                % (_want[0], _want[1], _here, _have))
