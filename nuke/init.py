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
def _preload_runtime(build):
    # The plugin loads Embree, oneTBB and the CUDA runtime from its own folder
    # by absolute path.  A release keeps ONE copy of them in runtime\ instead
    # (six copies would add ~190 MB), so load them from there first: once a
    # DLL is in the process, Windows resolves the plugin's imports of it by
    # name, and the plugin's own side-by-side LoadLibrary simply finds nothing
    # and is harmless.  Copies beside the build (a developer install) win.
    # tbb12 FIRST, because embree4 imports it - and not at all where Nuke 17
    # already has its own oneTBB loaded.
    if os.name != "nt":
        return
    rt = os.path.join(_here, "runtime")
    if not os.path.isdir(rt):
        return
    import ctypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.GetModuleHandleW.restype = ctypes.c_void_p
    k32.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
    for dll in ("tbb12.dll", "embree4.dll", "cudart64_12.dll"):
        if os.path.isfile(os.path.join(build, dll)) or k32.GetModuleHandleW(dll):
            continue
        path = os.path.join(rt, dll)
        if not os.path.isfile(path):
            nuke.tprint("InstanceRender: %s is missing from %s - the plugin will not load" % (dll, rt))
            continue
        try:
            ctypes.WinDLL(path)
        except OSError as e:
            nuke.tprint("InstanceRender: could not load %s (%s)" % (path, e))


_exact = [n for v, n in _versions() if v == _want]
if _exact:
    _build = os.path.join(_here, _exact[0])
    _preload_runtime(_build)
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
