#!/usr/bin/env python3
# PULSE weapon EDITOR workbench builder.
#
# Makes a clean, editable .blend for each ORIGINAL in-game weapon (the bumstrum integrated
# arms+gun viewmodels the engine already uses), so they can be modified by hand and exported
# back. This is NOT the gun-swap workbench (setup_fps_workbench.py) - here you edit the real
# weapon that ships in the game.
#
# For each weapon it: starts an empty file, imports the weapon glTF, switches the rig to thin
# STICK bones + hides the icosphere control widgets (so you see a clean arms+gun, not blobs),
# adds a camera, and saves tools/blender/weapon_edit_<name>.blend.
#
# Run headless (generates all of them):
#   blender -b --python tools/blender/setup_weapon_editor.py
#
# Then edit one:  open_weapon_editor.bat <name>   (default: pistol)
# When done, tell Claude "export <name>" and it round-trips your edit back into the engine.

import bpy
import os
from mathutils import Vector


def project_root():
    try:
        here = os.path.dirname(os.path.abspath(__file__))
        return os.path.abspath(os.path.join(here, "..", "..")).replace("\\", "/")
    except Exception:
        return "C:/Users/rq27/Pulse"


ROOT = project_root()
HERE = ROOT + "/tools/blender"

# short name -> source glTF (the assets the engine loads in config/pulse.weapons)
WEAPONS = [
    ("pistol",  "assets/bumstrum/fps_pistol_animated/scene.gltf"),
    ("ak",      "assets/bumstrum/fps_ak_animated/scene.gltf"),
    ("carbine", "assets/bumstrum/fps_animated_carbine/scene.gltf"),
    ("smg9",    "assets/bumstrum/fps_smg9_animated/scene.gltf"),
    ("shotgun", "assets/bumstrum/shotgun_animated/scene.gltf"),
    ("sniper",  "assets/bumstrum/sniper_animated/scene.gltf"),
]


def clean_display():
    """Thin stick bones, hide custom-shape widgets + icosphere proxies, so the arms+gun read clean."""
    for o in bpy.data.objects:
        if o.type == 'ARMATURE':
            o.data.display_type = 'STICK'
            o.data.show_bone_custom_shapes = False
            o.show_in_front = False
        elif o.type == 'MESH':
            n = o.name.lower()
            if "icosphere" in n or "sphere" in n or "proxy" in n:
                try:
                    o.hide_set(True)
                except Exception:
                    pass
                o.hide_render = True


def add_camera():
    cam_data = bpy.data.cameras.new("EditCam")
    cam = bpy.data.objects.new("EditCam", cam_data)
    bpy.context.scene.collection.objects.link(cam)
    mn = Vector((1e9, 1e9, 1e9))
    mx = Vector((-1e9, -1e9, -1e9))
    for o in bpy.data.objects:
        if o.type != 'MESH':
            continue
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            for i in range(3):
                mn[i] = min(mn[i], w[i]); mx[i] = max(mx[i], w[i])
    center = (mn + mx) * 0.5
    size = max((mx - mn).length, 0.5)
    cam.location = center + Vector((0.0, -size * 1.4, size * 0.4))
    cam.rotation_euler = (1.30, 0.0, 0.0)
    bpy.context.scene.camera = cam


report = []
for name, rel in WEAPONS:
    src = ROOT + "/" + rel
    out = HERE + "/weapon_edit_" + name + ".blend"
    bpy.ops.wm.read_homefile(use_empty=True)
    if not os.path.exists(src):
        report.append("MISS  %-8s %s" % (name, rel))
        continue
    bpy.ops.import_scene.gltf(filepath=src)
    meshes = [o.name for o in bpy.data.objects if o.type == 'MESH']
    arms = [o for o in bpy.data.objects if o.type == 'ARMATURE']
    acts = [a.name for a in bpy.data.actions]
    clean_display()
    add_camera()
    bpy.ops.wm.save_as_mainfile(filepath=out)
    report.append("OK    %-8s meshes=%d armatures=%d clips=%s -> %s"
                  % (name, len(meshes), len(arms), ",".join(acts) if acts else "(none)",
                     os.path.basename(out)))

print("\n=== PULSE weapon editors ===")
for r in report:
    print(r)
try:
    with open(HERE + "/weapon_editors_report.txt", "w", encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
except Exception:
    pass
