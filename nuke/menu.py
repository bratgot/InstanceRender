import os
import nuke
_here = os.path.dirname(os.path.abspath(__file__))
# The build folders are per MINOR version (nuke17.0, nuke17.1).  This used to
# look for "nuke<major>", which stopped matching the moment that changed - the
# plugin still loaded, because init.py adds the path, but the node quietly never
# appeared in the menu.
_build = os.path.join(_here, "nuke%d.%d" % (nuke.NUKE_VERSION_MAJOR, nuke.NUKE_VERSION_MINOR))
if os.path.isdir(_build):
    _m = nuke.menu("Nodes").findItem("3D") or nuke.menu("Nodes").addMenu("3D")
    _m.addCommand("InstanceRender", "nuke.createNode('InstanceRender')",
                  icon="InstanceRender.png",
                  tooltip="Instancing renderer for Nuke's 3D systems (CPU Embree / GPU OptiX, same kernel).")

    # The Hydra render delegate is registered in init.py, but in a GUI session
    # Nuke has already built its renderer list by then, so it is registered
    # again here - menu.py runs late enough for the Viewer's renderer menu and
    # GeoRender to pick it up.  Registering twice is harmless.
    _hydra = os.path.join(_build, "hydra", "hdInstanceRender", "resources")
    if os.path.isdir(_hydra):
        try:
            from pxr import Plug
            Plug.Registry().RegisterPlugins(_hydra)
            # Nuke builds its renderer list once, while it starts up, so in a GUI
            # session this registration is too late to be seen.  Say so, with the
            # one thing that does work, rather than leaving the menu mysteriously
            # short of a renderer that is plainly installed.
            _v = nuke.nodes.Viewer()
            _k = _v.knob("renderer")
            _seen = any("HdIr" in _x for _x in (_k.values() if _k else []))
            nuke.delete(_v)
            if _k and not _seen:
                nuke.tprint("InstanceRender: the Hydra delegate is installed but Nuke had already "
                            "built its renderer list.  Start Nuke with PXR_PLUGINPATH_NAME set to "
                            + _hydra + " (or use hydra_launch.bat next to this file) to use it.")
        except Exception as _e:
            nuke.tprint("InstanceRender: could not register the Hydra delegate (%s)" % _e)


# InstanceRender's "progressive (viewer)" mode hands the viewer a low-sample
# pass and refines it over the following redraws.  A Write must never be given
# one of those passes, so flag the render: the plugin reads IR_EXECUTING with
# GetEnvironmentVariableA on every render and falls back to a single
# full-quality pass while it is set.
def _ir_begin_render():
    os.environ["IR_EXECUTING"] = "1"


def _ir_end_render():
    os.environ["IR_EXECUTING"] = "0"


nuke.addBeforeRender(_ir_begin_render)
nuke.addAfterRender(_ir_end_render)
nuke.addBeforeFrameRender(_ir_begin_render)
nuke.addAfterFrameRender(_ir_end_render)
