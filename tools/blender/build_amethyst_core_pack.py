#!/usr/bin/env python3
# Build the Amethyst melee enemy runtime GLB from the FBX source pack.
#
# Usage:
#   blender -b --python tools/blender/build_amethyst_core_pack.py
#   blender -b --python tools/blender/build_amethyst_core_pack.py -- <pack_dir> <out_glb> [target_tris]
#
# The game loads a single glTF/GLB per enemy. The AMETHYST_CORE_PACK ships as one
# skinned mesh FBX plus separate skeleton-only Mixamo animation FBXs, so this script
# merges those takes into one role-named GLB that PulseGame's enemy state machine can
# discover by clip name.
import os
import json
import struct
import sys
from pathlib import Path

import bpy


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PACK = ROOT / "New Models" / "AMETHYST_CORE_PACK"
DEFAULT_OUT = ROOT / "assets" / "quaternius" / "enemies_relicpunk" / "Meshy_AI_Amethyst_Core_Sentinel.glb"
DEFAULT_TARGET_TRIS = 40000
TARGET_SOURCE_HEIGHT = 7.25


ROLE_TAKES = [
    ("idle",       "mutant breathing idle.fbx"),
    ("walk",       "mutant walking.fbx"),
    ("run",        "mutant run.fbx"),
    ("lunge",     "mutant jump attack.fbx"),
    ("cast",      "mutant roaring.fbx"),
    ("cast_heavy", "mutant swiping.fbx"),
    ("channel",   "mutant flexing muscles.fbx"),
    ("hit",       "mutant flexing muscles.fbx"),
    ("hit_heavy", "mutant roaring.fbx"),
    ("death",     "mutant dying.fbx"),
    # Extra named clips are exported too; the current game does not consume them,
    # but they stay available for future state-machine expansion and QA captures.
    ("punch",      "mutant punch.fbx"),
    ("jump",       "mutant jumping.fbx"),
]


def argv_after_dash():
    if "--" not in sys.argv:
        return []
    return sys.argv[sys.argv.index("--") + 1:]


def tri_count(objs):
    return sum(sum(len(p.vertices) - 2 for p in o.data.polygons) for o in objs if o.type == "MESH")


def find_armature():
    arms = [o for o in bpy.data.objects if o.type == "ARMATURE"]
    if not arms:
        raise RuntimeError("No armature imported")
    return arms[0]


def find_meshes():
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    if not meshes:
        raise RuntimeError("No mesh imported")
    return meshes


def bbox(objs):
    from mathutils import Vector

    mn = Vector((1.0e9, 1.0e9, 1.0e9))
    mx = Vector((-1.0e9, -1.0e9, -1.0e9))
    for obj in objs:
        for corner in obj.bound_box:
            world = obj.matrix_world @ Vector(corner)
            for i in range(3):
                mn[i] = min(mn[i], world[i])
                mx[i] = max(mx[i], world[i])
    return mn, mx


def scale_roots_to_height(meshes, target_height):
    mn, mx = bbox(meshes)
    h = max(1.0e-6, mx.z - mn.z)
    scale = target_height / h
    for obj in bpy.context.scene.objects:
        if obj.parent is None:
            obj.scale = (obj.scale.x * scale, obj.scale.y * scale, obj.scale.z * scale)
    bpy.context.view_layer.update()
    return h, target_height


def decimate_meshes(meshes, target_tris):
    before = tri_count(meshes)
    ratio = min(1.0, float(target_tris) / max(before, 1))
    if ratio >= 0.999:
        return before, before

    for mesh in meshes:
        bpy.context.view_layer.objects.active = mesh
        mesh.select_set(True)
        dec = mesh.modifiers.new("gameplay_tri_budget", "DECIMATE")
        dec.decimate_type = "COLLAPSE"
        dec.ratio = ratio
        # Keep decimation before the Armature modifier so vertex groups survive and
        # we do not bake a posed deformation into the mesh.
        try:
            bpy.ops.object.modifier_move_to_index(modifier=dec.name, index=0)
        except Exception:
            pass
        bpy.ops.object.modifier_apply(modifier=dec.name)
        mesh.select_set(False)

    return before, tri_count(meshes)


def action_fcurves(action):
    if hasattr(action, "fcurves"):
        return list(action.fcurves)

    curves = []
    for layer in getattr(action, "layers", []):
        for strip in getattr(layer, "strips", []):
            for slot in getattr(action, "slots", []):
                try:
                    bag = strip.channelbag(slot)
                except Exception:
                    bag = None
                if bag is not None and hasattr(bag, "fcurves"):
                    curves.extend(list(bag.fcurves))
    return curves


def normalize_action(action, role_name, in_place=True):
    action.name = role_name
    action.use_fake_user = True

    # Shift clips to start at frame 0. glTF stores seconds, but a zero start makes
    # sampled first frames deterministic and easier to inspect in Blender.
    start = action.frame_range[0]
    if abs(start) > 1.0e-4:
        for fc in action_fcurves(action):
            for kp in fc.keyframe_points:
                kp.co.x -= start
                kp.handle_left.x -= start
                kp.handle_right.x -= start

    # Pulse moves enemies in code. Strip horizontal Mixamo root travel so animation
    # sells gait/attack without drifting away from the gameplay entity.
    if in_place:
        for fc in action_fcurves(action):
            if "Hips" not in fc.data_path or not fc.data_path.endswith(".location"):
                continue
            if fc.array_index not in (0, 1):
                continue
            if not fc.keyframe_points:
                continue
            hold = fc.keyframe_points[0].co.y
            for kp in fc.keyframe_points:
                kp.co.y = hold
                kp.handle_left.y = hold
                kp.handle_right.y = hold

    if hasattr(action, "update"):
        action.update()
    return action


def import_take_action(path, role_name):
    before_actions = set(bpy.data.actions)
    before_objects = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=str(path))

    new_actions = [a for a in bpy.data.actions if a not in before_actions]
    if not new_actions:
        raise RuntimeError(f"No action found in {path}")

    # FBX imports one Armature|mixamo.com|Layer0 action. Copy it so deleting the
    # temporary armature cannot take our game action with it.
    action = new_actions[0].copy()
    normalize_action(action, role_name)

    for obj in [o for o in bpy.data.objects if o not in before_objects]:
        bpy.data.objects.remove(obj, do_unlink=True)
    for old in new_actions:
        if old.users == 0:
            bpy.data.actions.remove(old)

    return action


def patch_glb_animation_names(path, names):
    data = path.read_bytes()
    if len(data) < 20 or data[:4] != b"glTF":
        raise RuntimeError(f"{path} is not a GLB")

    version, total_len = struct.unpack_from("<II", data, 4)
    if version != 2 or total_len != len(data):
        raise RuntimeError(f"Unexpected GLB header in {path}")

    chunks = []
    pos = 12
    json_index = -1
    while pos + 8 <= len(data):
        chunk_len, chunk_type = struct.unpack_from("<II", data, pos)
        pos += 8
        chunk_data = data[pos:pos + chunk_len]
        pos += chunk_len
        if chunk_type == 0x4E4F534A:
            json_index = len(chunks)
        chunks.append([chunk_type, chunk_data])
    if json_index < 0:
        raise RuntimeError(f"No JSON chunk in {path}")

    gltf = json.loads(chunks[json_index][1].rstrip(b" \t\r\n\0").decode("utf-8"))
    animations = gltf.get("animations", [])
    if len(animations) < len(names):
        raise RuntimeError(f"Expected at least {len(names)} animations, found {len(animations)}")
    for i, name in enumerate(names):
        animations[i]["name"] = name

    new_json = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    new_json += b" " * ((4 - (len(new_json) % 4)) % 4)
    chunks[json_index][1] = new_json

    rebuilt_len = 12 + sum(8 + len(chunk_data) for _, chunk_data in chunks)
    out = bytearray()
    out += b"glTF"
    out += struct.pack("<II", 2, rebuilt_len)
    for chunk_type, chunk_data in chunks:
        out += struct.pack("<II", len(chunk_data), chunk_type)
        out += chunk_data
    path.write_bytes(out)


def main():
    args = argv_after_dash()
    pack_dir = Path(args[0]) if len(args) >= 1 else DEFAULT_PACK
    out_glb = Path(args[1]) if len(args) >= 2 else DEFAULT_OUT
    target_tris = int(args[2]) if len(args) >= 3 else DEFAULT_TARGET_TRIS

    mesh_fbx = pack_dir / "Meshy_AI_Amethyst_Core_Sentinel.fbx"
    if not mesh_fbx.exists():
        raise FileNotFoundError(mesh_fbx)
    for _, take in ROLE_TAKES:
        if not (pack_dir / take).exists():
            raise FileNotFoundError(pack_dir / take)

    out_glb.parent.mkdir(parents=True, exist_ok=True)

    bpy.ops.wm.read_homefile(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=str(mesh_fbx))

    arm = find_armature()
    arm.name = "AmethystCoreArmature"
    arm.data.name = "AmethystCoreArmature"
    arm.data.pose_position = "REST"
    meshes = find_meshes()

    source_h, target_h = scale_roots_to_height(meshes, TARGET_SOURCE_HEIGHT)
    before, after = decimate_meshes(meshes, target_tris)

    # Remove any source/default actions from the mesh import; the gameplay actions
    # below are the authoritative pack clips.
    for action in list(bpy.data.actions):
        bpy.data.actions.remove(action)

    actions = []
    for role_name, take in ROLE_TAKES:
        action = import_take_action(pack_dir / take, role_name)
        actions.append(action)
        print(f"ACTION {role_name:10s} <- {take} frames={tuple(round(x, 2) for x in action.frame_range)}")

    # Assign the idle action so the exported bind/default preview is stable.
    arm.animation_data_create()
    arm.animation_data.action = actions[0]
    arm.data.pose_position = "POSE"

    bpy.ops.object.select_all(action="DESELECT")
    arm.select_set(True)
    for mesh in meshes:
        mesh.select_set(True)
    bpy.context.view_layer.objects.active = arm

    bpy.ops.export_scene.gltf(
        filepath=str(out_glb),
        export_format="GLB",
        use_selection=True,
        export_apply=False,
        export_animations=True,
        export_animation_mode="ACTIONS",
    )
    patch_glb_animation_names(out_glb, [role for role, _ in ROLE_TAKES])

    print(f"AMETHYST_CORE mesh tris {before} -> {after}")
    print(f"AMETHYST_CORE source height {source_h:.4f} -> {target_h:.2f}")
    print("RENAMED animations: " + ", ".join(role for role, _ in ROLE_TAKES))
    print(f"WROTE {out_glb}")


if __name__ == "__main__":
    main()
