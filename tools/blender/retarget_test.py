#!/usr/bin/env python3
# Validate REUSING an existing rig's animations on a new Meshy mesh: bind the new mesh to an existing
# rigged+animated glTF's skeleton (auto weights), then pose it on an action so we can see if it holds.
#   blender -b --python tools/blender/retarget_test.py -- <rig.gltf> <new_mesh.obj> <out.png>
import bpy, sys, math
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
rig_src, new_src, out = argv[0], argv[1], argv[2]
bpy.ops.wm.read_homefile(use_empty=True)

# --- import the existing rig (skeleton + skinned mesh + animations) ---
bpy.ops.import_scene.gltf(filepath=rig_src)
arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
rig_meshes = [o for o in bpy.data.objects if o.type == 'MESH']
print("rig armature: %s  bones=%d  meshes=%d  actions=%d" %
      (arm.name if arm else None, len(arm.data.bones) if arm else 0, len(rig_meshes), len(bpy.data.actions)))

def bbox(objs):
    mn = Vector((1e9, 1e9, 1e9)); mx = Vector((-1e9, -1e9, -1e9))
    for o in objs:
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            for i in range(3):
                mn[i] = min(mn[i], w[i]); mx[i] = max(mx[i], w[i])
    return mn, mx

# rig at REST so we bind in its bind pose
arm.data.pose_position = 'REST'
bpy.context.view_layer.update()
rmn, rmx = bbox(rig_meshes); rdim = rmx - rmn

# --- import the new mesh + fit it over the rig (height + centre + feet) ---
before = set(bpy.data.objects)
bpy.ops.wm.obj_import(filepath=new_src)
nm = next(o for o in bpy.data.objects if o.type == 'MESH' and o not in before)
nmn, nmx = bbox([nm]); ndim = nmx - nmn
s = rdim.z / max(ndim.z, 1e-4)
nm.scale = (nm.scale.x * s, nm.scale.y * s, nm.scale.z * s)
bpy.context.view_layer.update()
nmn, nmx = bbox([nm])
rc = (rmn + rmx) * 0.5; nc = (nmn + nmx) * 0.5
nm.location += Vector((rc.x - nc.x, rc.y - nc.y, rmn.z - nmn.z))   # match centre xy + feet z
bpy.context.view_layer.update()
bpy.ops.object.select_all(action='DESELECT')
nm.select_set(True); bpy.context.view_layer.objects.active = nm
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

# --- bind: automatic (bone-heat) weights, fall back to envelope on failure ---
bpy.ops.object.select_all(action='DESELECT')
nm.select_set(True); arm.select_set(True); bpy.context.view_layer.objects.active = arm
mode = "AUTO"
try:
    bpy.ops.object.parent_set(type='ARMATURE_AUTO')
except Exception as e:
    print("auto-weights failed (%s) -> envelope" % e); mode = "ENVELOPE"
    bpy.ops.object.parent_set(type='ARMATURE_ENVELOPE')
print("bind mode: " + mode)

# remove the rig's original meshes so only the new mesh shows
for o in rig_meshes:
    bpy.data.objects.remove(o, do_unlink=True)

# --- pose on a walk/run action ---
arm.data.pose_position = 'POSE'
acts = sorted(a.name for a in bpy.data.actions)
pick = next((a for a in acts if 'walk' in a.lower()), None) or \
       next((a for a in acts if 'run' in a.lower()), None) or (acts[0] if acts else None)
print("posing action: %s   (available: %s)" % (pick, ", ".join(acts)[:160]))
if pick:
    if not arm.animation_data:
        arm.animation_data_create()
    act = bpy.data.actions[pick]
    arm.animation_data.action = act
    fr = int((act.frame_range[0] + act.frame_range[1]) * 0.5)
    bpy.context.scene.frame_set(fr)
bpy.context.view_layer.update()

# --- render front ---
mn, mx = bbox([nm]); dim = mx - mn; center = (mn + mx) * 0.5
r = max(dim.x, dim.y, dim.z, 0.1)
cam_d = bpy.data.cameras.new("C"); cam = bpy.data.objects.new("C", cam_d)
bpy.context.scene.collection.objects.link(cam)
cam.location = center + Vector((0.0, -r * 2.4, 0.0)); cam.rotation_euler = (math.radians(90), 0, 0)
cam_d.type = 'ORTHO'; cam_d.ortho_scale = r * 1.8
bpy.context.scene.camera = cam
l = bpy.data.lights.new("L", 'SUN'); lo = bpy.data.objects.new("L", l)
bpy.context.scene.collection.objects.link(lo); lo.rotation_euler = (math.radians(55), 0, math.radians(35)); l.energy = 3.0
sc = bpy.context.scene
sc.render.engine = 'BLENDER_WORKBENCH'; sc.render.resolution_x = 520; sc.render.resolution_y = 680
sc.render.filepath = out
bpy.ops.render.render(write_still=True)
print("RENDERED " + out)
