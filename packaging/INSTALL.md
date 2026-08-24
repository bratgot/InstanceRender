# Installing InstanceRender

Windows x64. Nuke 14.1, 15.2, 16.0, 16.1, 17.0 or 17.1.

## The quick way

Unpack the zip anywhere and double-click **`install.bat`**.

It copies the plugin to `%USERPROFILE%\.nuke\InstanceRender`, puts the runtime
libraries beside each build, and adds one line to `%USERPROFILE%\.nuke\init.py`
so Nuke looks in that folder. Start Nuke; **InstanceRender** is on the **3D**
toolbar.

## From PowerShell, with options

```powershell
.\install.ps1                              # every build in the zip
.\install.ps1 -Versions 17.1               # only Nuke 17.1
.\install.ps1 -Versions 16.1,17.1          # two of them
.\install.ps1 -Prefix D:\studio\nuke       # somewhere other than ~/.nuke
```

**`-Versions` is worth using.** Each build needs its own copy of Embree and the
CUDA runtime beside it - about 38 MB - because the plugin loads them from its
own folder by absolute path. Installing all six costs ~230 MB; installing the
one you actually run costs ~40 MB.

**`-Prefix`** installs somewhere else, for a shared or per-project location. That
folder must be on Nuke's plugin path: either set `NUKE_PATH` to it, or add
`nuke.pluginAddPath('...')` to a `init.py` Nuke already reads. The installer
writes its registration line into `<prefix>\init.py`, which Nuke only reads if
the prefix itself is on the path.

## Installing by hand

If you would rather not run a script:

1. Copy the `InstanceRender` folder from this zip into `%USERPROFILE%\.nuke\`.
2. Copy the three DLLs from `runtime\` into **each** `nuke<version>` folder you
   want to use - and also into `nuke17.0\hydra\` and `nuke17.1\hydra\` if you
   want the Hydra delegate. This step is not optional: the plugin loads them
   from its own directory and will not find them anywhere else.
3. Add this to `%USERPROFILE%\.nuke\init.py` (create it if it is not there):

   ```python
   import nuke
   nuke.pluginAddPath('./InstanceRender')
   ```

4. Optionally copy `ToolSets\InstanceRender` into `%USERPROFILE%\.nuke\ToolSets\`.

## Uninstalling

```powershell
.\uninstall.ps1
```

It removes the plugin folder and the ToolSet, and takes out only its own block
from `init.py` - anything else you have in that file is left alone.

## When the node does not appear

Nuke prints the reason. Open the **Script Editor** or start Nuke from a console
and look at the output as it starts up.

**"InstanceRender: no build for Nuke 16.1 ... (installed: nuke17.1)"**
The build has to match the Nuke minor version exactly. Install the right one -
`.\install.ps1 -Versions 16.1`. See `COMPATIBILITY.md`.

**Nothing at all is printed**
Nuke is not reading the folder. Check that `%USERPROFILE%\.nuke\init.py` contains
the `pluginAddPath` line above, and that `%USERPROFILE%\.nuke` is where Nuke
actually looks - if `NUKE_PATH` is set in your environment it may be reading
somewhere else entirely.

**"The specified module could not be found" / "the specified procedure could not
be found"**
Almost always the runtime DLLs: `embree4.dll`, `tbb12.dll` and `cudart64_12.dll`
must be **in the same folder as `InstanceRender.dll`**. Re-run `install.ps1`,
which does that for you.

**The renderer does not appear in the Viewer's renderer menu (Nuke 17)**
That is the Hydra delegate, and Nuke builds its renderer list once while it
starts, before any `init.py` runs. Launch Nuke through `hydra_launch.bat` in the
plugin folder:

```
%USERPROFILE%\.nuke\InstanceRender\hydra_launch.bat "C:\Program Files\Nuke17.1v1\Nuke17.1.exe"
```

The **InstanceRender node itself** does not need this - only the delegate does.

**It renders black, or the GPU device fails**
Switch the `device` knob to CPU. The GPU back-end needs an NVIDIA card and a
current driver; the CPU back-end has no such requirement and renders the same
image. If CPU renders and GPU does not, that is a driver or card issue rather
than an install problem.

## Studio install

Put the plugin folder on a share, point `NUKE_PATH` at its parent, and every
workstation picks it up:

```
set NUKE_PATH=\share\nuke_plugins
```

with the tree laid out as `\share\nuke_plugins\InstanceRender\...`. The
`init.py` in the plugin folder selects the build matching whichever Nuke starts,
so one install serves every version at once. Note that loading a 38 MB Embree
over a network share on every Nuke launch is slower than a local copy.
