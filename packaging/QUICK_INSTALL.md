# Installing InstanceRender - the simple way

Two minutes. You copy one folder and paste two lines of text.
No scripts, no command line.

(Prefer one click? Double-click **`install.bat`** in this zip instead - it does
the same thing for you.)

Close Nuke before you start.

---

## Step 1 - Open your `.nuke` folder

1. Press **Windows key + R**.
2. Paste this and press **Enter**:

   ```
   %USERPROFILE%\.nuke
   ```

A folder window opens. That is where Nuke keeps your personal plugins and
settings. (If Windows says it cannot find it, you have never started Nuke on this
machine - start and close Nuke once, then try again.)

---

## Step 2 - Copy the InstanceRender folder in

From this zip, drag the **`InstanceRender`** folder into the `.nuke` window.

Copy the whole folder just as it is - don't take anything out of it or
rearrange it. It has a version of the plugin for every supported Nuke
(14.1 to 17.1), and it picks the right one when Nuke starts.

It should look like this:

    .nuke\
      InstanceRender\
        init.py
        menu.py
        icons\
        runtime\
        nuke14.1\
        ...
        nuke17.1\

---

## Step 3 - Tell Nuke about it (init.py)

Still in the `.nuke` window, look for a file called **`init.py`** - the one
sitting directly in `.nuke`, **not** the one inside `InstanceRender`.

* **If it is there:** right-click it > **Open with** > **Notepad**.
* **If it is not there:** open Notepad from the Start menu to make a new one.

Paste these two lines at the **bottom** of the file (leave anything already in
it where it is):

```python
import nuke
nuke.pluginAddPath('./InstanceRender')
```

Save it:

* If you opened an existing `init.py`, just **File > Save**.
* If it is a new file: **File > Save As**, go to the `.nuke` folder, set
  **Save as type** to **All files (\*.\*)**, and name it `init.py`.

> **Watch out for `init.py.txt`.** Notepad likes to add `.txt` to the end, and
> Windows hides it, so the file looks right but Nuke ignores it. To check: in the
> folder window click **View > Show > File name extensions**. If you see
> `init.py.txt`, rename it to `init.py`.

---

## Step 4 - Start Nuke

Start Nuke. **InstanceRender** is on the **3D** toolbar (or press Tab and type
`InstanceRender`).

That's it.

---

## Optional - the AOV ToolSet

To get the ready-made AOV setup under **ToolSets**, copy the **`ToolSets`**
folder from this zip into `.nuke` too. If a `ToolSets` folder is already there,
say yes when Windows asks to merge them.

---

## It did not show up?

Open the **Script Editor** in Nuke (or look at the console window that opens
with Nuke) and look for a message that mentions InstanceRender.

| What you see | What it means |
|---|---|
| `no build for Nuke 16.0 ...` | This zip has no version for your Nuke. The list of what it does have is in the same message; `COMPATIBILITY.md` explains. |
| `... is missing from ...\runtime` | Part of the folder did not get copied. Delete `.nuke\InstanceRender` and copy it again, whole. |
| Nothing at all | Nuke did not read `init.py`. Check it is really called `init.py` (not `.txt`) and sits directly in `.nuke`, not inside `InstanceRender`. |

If it renders black, set the node's **device** knob to **CPU**. The GPU mode
needs an NVIDIA card with a recent driver; CPU gives the same picture on any
machine.

More detail, including installing for a whole studio: see **`INSTALL.md`**.

---

## Updating to a new version

Close Nuke, delete `.nuke\InstanceRender`, and copy the new one in. `init.py`
stays as it is.

## Removing it

Delete `.nuke\InstanceRender`, and take the two lines back out of
`.nuke\init.py`.
