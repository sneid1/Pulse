#!/usr/bin/env python3
# Export the tuned FPS viewmodel (arms + new gun) from the workbench .blend to a GLB the engine
# loads. Excludes the "ORIGINAL GUN (reference)" + "HELPERS" layers. Run headless:
#   blender -b --python tools/blender/export_viewmodel.py
import bpy
import os

HERE = os.path.dirname(os.path.abspath(__file__))
IN_BLEND = os.path.join(HERE, "fps_workbench_pistol.blend")
OUT_GLB = os.path.abspath(os.path.join(HERE, "..", "..", "assets", "quaternius", "viewmodels", "pistol_vm.glb"))

bpy.ops.wm.open_mainfile(filepath=IN_BLEND)

# Select everything EXCEPT the reference-gun + helper layers.
for o in bpy.data.objects:
    try:
        o.select_set(False)
    except Exception:
        pass
exclude = ("original", "helper")
sel = []
for coll in bpy.data.collections:
    if any(x in coll.name.lower() for x in exclude):
        continue
    for o in coll.objects:
        try:
            o.hide_set(False)          # ensure exportable
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
