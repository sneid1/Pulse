#!/usr/bin/env python3
# Round-trip an edited PULSE weapon editor .blend back into the engine.
# Exports weapon_edit_<name>.blend -> assets/bumstrum/<dir>/edited.glb (non-destructive; the
# original scene.gltf is left intact). Excludes the editor camera + icosphere control widgets.
#
#   blender -b --python tools/blender/export_weapon.py -- ak
import bpy
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

# short name -> source asset folder (matches setup_weapon_editor.py / config/pulse.weapons)
DIRS = {
    "pistol":  "fps_pistol_animated",
    "ak":      "fps_ak_animated",
    "carbine": "fps_animated_carbine",
    "smg9":    "fps_smg9_animated",
    "shotgun": "shotgun_animated",
    "sniper":  "sniper_animated",
}

name = "pistol"
if "--" in sys.argv:
    extra = sys.argv[sys.argv.index("--") + 1:]
    if extra:
        name = extra[0]
if name not in DIRS:
    raise RuntimeError("unknown weapon '%s' (use: %s)" % (name, ", ".join(DIRS)))

IN_BLEND = os.path.join(HERE, "weapon_edit_%s.blend" % name)
OUT_GLB = os.path.join(ROOT, "assets", "bumstrum", DIRS[name], "edited.glb")

bpy.ops.wm.open_mainfile(filepath=IN_BLEND)

for o in bpy.data.objects:
    try:
        o.select_set(False)
    except Exception:
        pass

skip = ("icosphere", "sphere", "proxy")
sel = []
for o in bpy.data.objects:
    if o.type == 'CAMERA':
        continue
    if o.type == 'MESH' and any(s in o.name.lower() for s in skip):
        continue
    try:
        o.hide_set(False)
        o.select_set(True)
        sel.append("%s[%s]" % (o.name, o.type))
    except Exception:
        pass

os.makedirs(os.path.dirname(OUT_GLB), exist_ok=True)
bpy.ops.export_scene.gltf(
    filepath=OUT_GLB,
    export_format='GLB',
    use_selection=True,
    export_animations=True,
    export_apply=False,
)
print("EXPORTED: " + OUT_GLB)
print("objects: " + ", ".join(sel))
print("actions: " + ", ".join(a.name for a in bpy.data.actions))
