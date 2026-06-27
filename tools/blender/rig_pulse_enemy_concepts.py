#!/usr/bin/env python3
# Bind the current PULSE Meshy enemy concept GLBs to an existing multi-clip Mixamo rig
# and export role-named animated GLBs for the native renderer.
#
# Usage:
#   blender -b --python tools/blender/rig_pulse_enemy_concepts.py -- ^
#     <rig.gltf> <outdir> rusher=<src.glb> fly:drone=<src.glb>
#
# The donor rig supplies the skeleton + clips. The target concept mesh supplies the
# silhouette/materials. Blender auto-weights first, with envelope fallback for meshes
# whose generated topology defeats bone heat.
import os
import sys

import bpy
from mathutils import Vector


argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(argv) < 3:
    print("RIG_CONCEPT_ERROR: need <rig.gltf> <outdir> and role=src.glb entries")
    sys.exit(1)

RIG_SRC = argv[0]
OUT_DIR = argv[1]
ENTRIES = []
for spec in argv[2:]:
    if "=" not in spec:
        print("RIG_CONCEPT_ERROR: expected role=path, got", spec)
        sys.exit(1)
    mode = "biped"
    left, src = spec.split("=", 1)
    if ":" in left:
        mode, role = left.split(":", 1)
    else:
        role = left
    ENTRIES.append((mode.strip().lower(), role.strip(), src.strip()))

os.makedirs(OUT_DIR, exist_ok=True)


def bbox(objs):
    mn = Vector((1.0e9, 1.0e9, 1.0e9))
    mx = Vector((-1.0e9, -1.0e9, -1.0e9))
    for obj in objs:
        for corner in obj.bound_box:
            world = obj.matrix_world @ Vector(corner)
            for i in range(3):
                mn[i] = min(mn[i], world[i])
                mx[i] = max(mx[i], world[i])
    return mn, mx


def import_meshes(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    objs = [o for o in bpy.data.objects if o not in before]
    return [o for o in objs if o.type == "MESH"], objs


def join_meshes(meshes):
    if not meshes:
        return None
    bpy.ops.object.select_all(action="DESELECT")
    for mesh in meshes:
        mesh.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    return bpy.context.view_layer.objects.active


def keep_all_actions_as_nla(arm):
    arm.animation_data_create()
    for track in list(arm.animation_data.nla_tracks):
        arm.animation_data.nla_tracks.remove(track)
    kept = []
    for action in bpy.data.actions:
        if not action.name:
            continue
        action.use_fake_user = True
        track = arm.animation_data.nla_tracks.new()
        track.name = action.name
        start = int(action.frame_range[0])
        track.strips.new(action.name, start, action)
        kept.append(action.name)
    return kept


def point_segment_distance_sq(p, a, b):
    ab = b - a
    denom = max(ab.dot(ab), 1.0e-8)
    t = max(0.0, min(1.0, (p - a).dot(ab) / denom))
    q = a + ab * t
    d = p - q
    return d.dot(d)


def has_exportable_skin(mesh):
    has_armature = any(mod.type == "ARMATURE" and mod.object for mod in mesh.modifiers)
    if not has_armature or len(mesh.vertex_groups) == 0:
        return False
    for vert in mesh.data.vertices:
        if vert.groups:
            return True
    return False


def clear_vertex_groups(mesh):
    for group in list(mesh.vertex_groups):
        mesh.vertex_groups.remove(group)


def nearest_bone_bind(mesh, arm):
    # Bone-heat regularly fails on AI-generated hard-surface meshes. This fallback
    # creates stable skinning data by blending each vertex to its four nearest deform
    # bone segments in rest pose. It is less anatomical than hand weights, but it is
    # deterministic and exports real JOINTS_0/WEIGHTS_0 for the renderer.
    clear_vertex_groups(mesh)
    bones = []
    for bone in arm.data.bones:
        if not bone.use_deform:
            continue
        head = arm.matrix_world @ bone.head_local
        tail = arm.matrix_world @ bone.tail_local
        if (tail - head).length < 1.0e-4:
            continue
        group = mesh.vertex_groups.new(name=bone.name)
        bones.append((bone.name, head, tail, group))
    if not bones:
        raise RuntimeError("donor rig has no deform bones")

    for vert in mesh.data.vertices:
        p = mesh.matrix_world @ vert.co
        nearest = []
        for _, head, tail, group in bones:
            nearest.append((point_segment_distance_sq(p, head, tail), group))
        nearest.sort(key=lambda item: item[0])
        nearest = nearest[:4]
        weights = [1.0 / (d + 0.015) for d, _ in nearest]
        total = sum(weights) or 1.0
        for (_, group), weight in zip(nearest, weights):
            group.add([vert.index], weight / total, "REPLACE")

    mod = next((m for m in mesh.modifiers if m.type == "ARMATURE"), None)
    if mod is None:
        mod = mesh.modifiers.new("Armature", "ARMATURE")
    mod.object = arm
    mesh.parent = arm
    mesh.matrix_parent_inverse = arm.matrix_world.inverted()


def make_flyer_actions(arm):
    arm.animation_data_create()
    bone = arm.pose.bones["Root"]
    specs = {
        "idle": [
            (1,  (0.0, 0.0, 0.00), (0.0, 0.0, 0.00), (1.00, 1.00, 1.00)),
            (30, (0.0, 0.0, 0.08), (0.0, 0.0, 0.04), (1.02, 1.02, 0.98)),
            (60, (0.0, 0.0, 0.00), (0.0, 0.0, 0.00), (1.00, 1.00, 1.00)),
        ],
        "walk": [
            (1,  (0.00, 0.00, 0.02), (0.10, 0.00, 0.08), (1.00, 1.00, 1.00)),
            (18, (0.06, 0.00, 0.09), (0.02, 0.16, -0.12), (1.03, 0.98, 0.99)),
            (36, (-0.06, 0.00, 0.03), (-0.08, -0.12, 0.12), (0.99, 1.03, 1.00)),
            (54, (0.00, 0.00, 0.02), (0.10, 0.00, 0.08), (1.00, 1.00, 1.00)),
        ],
        "run": [
            (1,  (0.00, 0.00, 0.04), (0.18, 0.00, 0.14), (1.04, 0.98, 0.98)),
            (12, (0.10, 0.00, 0.13), (0.02, 0.24, -0.20), (1.08, 0.95, 0.98)),
            (24, (-0.10, 0.00, 0.04), (-0.14, -0.22, 0.20), (0.98, 1.06, 1.00)),
            (36, (0.00, 0.00, 0.04), (0.18, 0.00, 0.14), (1.04, 0.98, 0.98)),
        ],
        "walk_back": [
            (1,  (0.00, 0.00, 0.02), (-0.12, 0.0, 0.00), (1.00, 1.00, 1.00)),
            (24, (0.00, -0.07, 0.09), (-0.20, 0.0, 0.08), (0.98, 1.02, 1.00)),
            (48, (0.00, 0.00, 0.02), (-0.12, 0.0, 0.00), (1.00, 1.00, 1.00)),
        ],
        "strafe_left": [
            (1,  (0.00, 0.00, 0.03), (0.00, 0.18, 0.20), (1.00, 1.00, 1.00)),
            (20, (-0.10, 0.00, 0.10), (0.05, 0.28, 0.34), (1.03, 0.98, 1.00)),
            (40, (0.00, 0.00, 0.03), (0.00, 0.18, 0.20), (1.00, 1.00, 1.00)),
        ],
        "strafe_right": [
            (1,  (0.00, 0.00, 0.03), (0.00, -0.18, -0.20), (1.00, 1.00, 1.00)),
            (20, (0.10, 0.00, 0.10), (0.05, -0.28, -0.34), (1.03, 0.98, 1.00)),
            (40, (0.00, 0.00, 0.03), (0.00, -0.18, -0.20), (1.00, 1.00, 1.00)),
        ],
        "cast": [
            (1,  (0.0, -0.02, 0.00), (-0.16, 0.0, 0.0), (1.00, 1.00, 1.00)),
            (18, (0.0, -0.08, 0.12), (-0.30, 0.0, 0.0), (1.08, 1.08, 0.94)),
            (30, (0.0, 0.08, 0.04), (0.16, 0.0, 0.0), (0.96, 0.96, 1.08)),
            (45, (0.0, 0.00, 0.00), (0.00, 0.0, 0.0), (1.00, 1.00, 1.00)),
        ],
        "cast_heavy": [
            (1,  (0.0, -0.04, 0.00), (-0.20, 0.0, 0.0), (1.00, 1.00, 1.00)),
            (24, (0.0, -0.12, 0.16), (-0.36, 0.0, 0.0), (1.12, 1.12, 0.90)),
            (42, (0.0, 0.12, 0.03), (0.22, 0.0, 0.0), (0.94, 0.94, 1.12)),
            (62, (0.0, 0.00, 0.00), (0.00, 0.0, 0.0), (1.00, 1.00, 1.00)),
        ],
        "cast_heavy2": [
            (1,  (0.0, -0.03, 0.02), (-0.14, 0.0, 0.0), (1.00, 1.00, 1.00)),
            (20, (0.0, -0.10, 0.13), (-0.24, 0.0, 0.14), (1.10, 1.03, 0.94)),
            (40, (0.0, -0.02, 0.07), (-0.08, 0.0, -0.14), (1.04, 1.08, 0.98)),
            (60, (0.0, -0.03, 0.02), (-0.14, 0.0, 0.0), (1.00, 1.00, 1.00)),
        ],
        "lunge": [
            (1,  (0.0, -0.08, 0.02), (-0.24, 0.0, 0.0), (1.04, 1.04, 0.96)),
            (14, (0.0, 0.18, 0.06), (0.30, 0.0, 0.0), (0.94, 0.94, 1.12)),
            (28, (0.0, 0.00, 0.00), (0.00, 0.0, 0.0), (1.00, 1.00, 1.00)),
        ],
        "hit": [
            (1,  (0.0, 0.00, 0.00), (0.00, 0.0, 0.0), (1.00, 1.00, 1.00)),
            (8,  (0.0, -0.12, 0.06), (-0.30, 0.0, 0.16), (1.12, 0.92, 0.98)),
            (20, (0.0, 0.00, 0.00), (0.00, 0.0, 0.0), (1.00, 1.00, 1.00)),
        ],
        "hit_heavy": [
            (1,  (0.0, 0.00, 0.00), (0.00, 0.0, 0.0), (1.00, 1.00, 1.00)),
            (10, (0.0, -0.20, 0.10), (-0.48, 0.0, 0.24), (1.18, 0.88, 0.96)),
            (28, (0.0, 0.00, 0.00), (0.00, 0.0, 0.0), (1.00, 1.00, 1.00)),
        ],
        "death": [
            (1,  (0.0, 0.0, 0.00), (0.00, 0.00, 0.00), (1.00, 1.00, 1.00)),
            (20, (0.0, 0.0, -0.18), (0.70, 0.20, 0.40), (0.96, 0.96, 0.96)),
            (48, (0.0, 0.0, -0.55), (1.35, 0.55, 0.90), (0.82, 0.82, 0.82)),
        ],
    }

    for name, keys in specs.items():
        action = bpy.data.actions.new(name)
        arm.animation_data.action = action
        for frame, loc, rot, scale in keys:
            bone.location = loc
            bone.rotation_mode = "XYZ"
            bone.rotation_euler = rot
            bone.scale = scale
            bone.keyframe_insert("location", frame=frame)
            bone.keyframe_insert("rotation_euler", frame=frame)
            bone.keyframe_insert("scale", frame=frame)
        action.use_fake_user = True
    arm.animation_data.action = None
    return keep_all_actions_as_nla(arm)


def export_flyer(role, src):
    bpy.ops.wm.read_homefile(use_empty=True)
    target_meshes, target_objs = import_meshes(src)
    target = join_meshes(target_meshes)
    if target is None:
        raise RuntimeError("target has no mesh: " + src)
    target.name = "pulse_" + role + "_flyer_body"

    bpy.context.view_layer.update()
    mn, mx = bbox([target])
    center = (mn + mx) * 0.5
    height = max(mx.z - mn.z, 0.25)

    arm_data = bpy.data.armatures.new("PulseFlyerArmature")
    arm = bpy.data.objects.new("PulseFlyerArmature", arm_data)
    bpy.context.scene.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    arm.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    root = arm_data.edit_bones.new("Root")
    root.head = (center.x, center.y, mn.z)
    root.tail = (center.x, center.y, mn.z + height)
    bpy.ops.object.mode_set(mode="OBJECT")

    clear_vertex_groups(target)
    group = target.vertex_groups.new(name="Root")
    group.add([v.index for v in target.data.vertices], 1.0, "REPLACE")
    mod = target.modifiers.new("Armature", "ARMATURE")
    mod.object = arm
    target.parent = arm
    target.matrix_parent_inverse = arm.matrix_world.inverted()

    for obj in target_objs:
        if obj.type != "MESH" and obj.name in bpy.data.objects:
            bpy.data.objects.remove(obj, do_unlink=True)

    clips = make_flyer_actions(arm)
    out = os.path.join(OUT_DIR, "pulse_enemy_" + role + "_animated.glb")
    bpy.ops.object.select_all(action="DESELECT")
    arm.select_set(True)
    target.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.export_scene.gltf(
        filepath=out,
        export_format="GLB",
        use_selection=True,
        export_skins=True,
        export_animations=True,
        export_animation_mode="NLA_TRACKS",
    )
    print("RIG_CONCEPT %-8s bind=FLYER clips=%d -> %s" % (role, len(clips), out))


def export_one(role, src):
    bpy.ops.wm.read_homefile(use_empty=True)

    rig_meshes, rig_objs = import_meshes(RIG_SRC)
    arm = next((o for o in bpy.data.objects if o.type == "ARMATURE"), None)
    if arm is None:
        raise RuntimeError("donor rig has no armature: " + RIG_SRC)

    arm.data.pose_position = "REST"
    bpy.context.view_layer.update()
    rmn, rmx = bbox(rig_meshes)
    rdim = rmx - rmn

    target_meshes, target_objs = import_meshes(src)
    target = join_meshes(target_meshes)
    if target is None:
        raise RuntimeError("target has no mesh: " + src)
    target.name = "pulse_" + role + "_body"

    # Fit target mesh to the donor rig's rest pose by height, horizontal centre,
    # and foot plane. Runtime worldHeight then sets the final game scale per kind.
    bpy.context.view_layer.update()
    nmn, nmx = bbox([target])
    ndim = nmx - nmn
    scale = rdim.z / max(ndim.z, 1.0e-4)
    target.scale = (target.scale.x * scale, target.scale.y * scale, target.scale.z * scale)
    bpy.context.view_layer.update()
    nmn, nmx = bbox([target])
    rc = (rmn + rmx) * 0.5
    nc = (nmn + nmx) * 0.5
    target.location += Vector((rc.x - nc.x, rc.y - nc.y, rmn.z - nmn.z))

    bpy.ops.object.select_all(action="DESELECT")
    target.select_set(True)
    bpy.context.view_layer.objects.active = target
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

    bpy.ops.object.select_all(action="DESELECT")
    target.select_set(True)
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bind_mode = "AUTO"
    try:
        bpy.ops.object.parent_set(type="ARMATURE_AUTO")
    except Exception as exc:
        print("RIG_CONCEPT_WARN %-8s auto weights failed (%s); using envelope" % (role, exc))
        bind_mode = "ENVELOPE"
        bpy.ops.object.parent_set(type="ARMATURE_ENVELOPE")
    if not has_exportable_skin(target):
        print("RIG_CONCEPT_WARN %-8s blender bind produced no skin; using nearest-bone weights" % role)
        bind_mode = "NEAREST4"
        nearest_bone_bind(target, arm)

    for obj in rig_meshes:
        if obj.name in bpy.data.objects:
            bpy.data.objects.remove(obj, do_unlink=True)
    for obj in target_objs:
        if obj.type != "MESH" and obj.name in bpy.data.objects:
            bpy.data.objects.remove(obj, do_unlink=True)

    arm.data.pose_position = "POSE"
    clips = keep_all_actions_as_nla(arm)

    out = os.path.join(OUT_DIR, "pulse_enemy_" + role + "_animated.glb")
    bpy.ops.object.select_all(action="DESELECT")
    arm.select_set(True)
    target.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.export_scene.gltf(
        filepath=out,
        export_format="GLB",
        use_selection=True,
        export_skins=True,
        export_animations=True,
        export_animation_mode="NLA_TRACKS",
    )
    print("RIG_CONCEPT %-8s bind=%s clips=%d -> %s" % (role, bind_mode, len(clips), out))


for mode, role, src in ENTRIES:
    if mode in ("fly", "flying", "hover"):
        export_flyer(role, src)
    else:
        export_one(role, src)

print("RIG_CONCEPT_DONE %d enemies" % len(ENTRIES))
