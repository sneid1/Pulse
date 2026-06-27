# fix_feet_rig.py - straighten toed-out feet using the EXISTING rig's foot bones: select the
# foot vertices by their LeftFoot/LeftToeBase (resp. Right) skin weights and rotate them about
# the true ankle (foot-bone head) so the toes point along the body's forward axis. Far more
# reliable than a height-band guess: the weights pick exactly the foot and taper at the ankle.
# Exports a clean mesh-only FBX for Mixamo + TOP/FRONT verification renders. ASCII only.
# Usage: blender -b --python fix_feet_rig.py -- <rigged.gltf> <out.fbx> <render_dir>
import bpy, sys, os, math, mathutils

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
src, out, rdir = argv[0], argv[1], argv[2]

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.import_scene.gltf(filepath=src)

arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
mesh = max((o for o in bpy.data.objects
            if o.type == 'MESH' and 'icosphere' not in o.name.lower()),
           key=lambda o: len(o.data.vertices))
assert arm and mesh, "need armature + mesh"

def bone_head(name):
    b = arm.data.bones.get(name)
    return arm.matrix_world @ b.head_local if b else None

# Per-foot forward direction (toe - ankle) in XY, and the body's forward (mean of both feet).
feet = {}
for side in ("Left", "Right"):
    foot = bone_head(side + "Foot"); toe = bone_head(side + "ToeBase")
    if foot is None or toe is None:
        continue
    fwd = mathutils.Vector((toe.x - foot.x, toe.y - foot.y))
    if fwd.length < 1e-6:
        continue
    feet[side] = (foot, fwd.normalized())
mean = mathutils.Vector((0, 0))
for _, f in feet.values():
    mean += f
target = mathutils.Vector((0.0, 1.0 if mean.y >= 0 else -1.0))   # zero the splay, keep facing

mwi = mesh.matrix_world.inverted()
report = []
for side, (foot, fwd) in feet.items():
    gi = set()
    for gn in (side + "Foot", side + "ToeBase"):
        vg = mesh.vertex_groups.get(gn)
        if vg:
            gi.add(vg.index)
    # signed angle that rotates fwd -> target
    cross = fwd.x * target.y - fwd.y * target.x
    dot = fwd.x * target.x + fwd.y * target.y
    ang = math.atan2(cross, dot)
    report.append("%s splay=%.1f deg" % (side, math.degrees(ang)))
    for v in mesh.data.vertices:
        w = sum(g.weight for g in v.groups if g.group in gi)
        if w <= 1e-3:
            continue
        w = min(1.0, w)
        th = ang * w
        wco = mesh.matrix_world @ v.co
        rx = wco.x - foot.x; ry = wco.y - foot.y
        ct = math.cos(th); st = math.sin(th)
        nx = rx * ct - ry * st + foot.x
        ny = rx * st + ry * ct + foot.y
        v.co = mwi @ mathutils.Vector((nx, ny, wco.z))
mesh.data.update()
print("FIXFEET_RIG", "; ".join(report))

# Mesh-only export (Mixamo re-rigs): drop armature + modifiers + proxy, ground + center.
for m in list(mesh.modifiers):
    mesh.modifiers.remove(m)
for o in list(bpy.data.objects):
    if o is not mesh:
        bpy.data.objects.remove(o, do_unlink=True)
bpy.context.view_layer.objects.active = mesh
mesh.select_set(True)
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
co = [mesh.matrix_world @ v.co for v in mesh.data.vertices]
mn = mathutils.Vector((min(c.x for c in co), min(c.y for c in co), min(c.z for c in co)))
mx = mathutils.Vector((max(c.x for c in co), max(c.y for c in co), max(c.z for c in co)))
mesh.location += mathutils.Vector((-0.5 * (mn.x + mx.x), -0.5 * (mn.y + mx.y), -mn.z))
bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)

# verification renders (TOP shows toe direction unambiguously)
os.makedirs(rdir, exist_ok=True)
sc = bpy.context.scene
try: sc.render.engine = 'BLENDER_WORKBENCH'
except Exception: pass
sh = sc.display.shading; sh.light = 'STUDIO'; sh.show_cavity = True
sh.color_type = 'SINGLE'; sh.single_color = (0.55, 0.55, 0.58)
co = [mesh.matrix_world @ v.co for v in mesh.data.vertices]
mn = mathutils.Vector((min(c.x for c in co), min(c.y for c in co), min(c.z for c in co)))
mx = mathutils.Vector((max(c.x for c in co), max(c.y for c in co), max(c.z for c in co)))
ctr = (mn + mx) * 0.5
span = max(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z)
cd = bpy.data.cameras.new("c"); cam = bpy.data.objects.new("c", cd)
sc.collection.objects.link(cam); sc.camera = cam
cd.type = 'ORTHO'; cd.ortho_scale = span * 1.1
def shoot(loc, up, tag, rx, ry):
    sc.render.resolution_x = rx; sc.render.resolution_y = ry
    cam.location = ctr + loc
    cam.rotation_euler = (ctr - cam.location).to_track_quat('-Z', up).to_euler()
    sc.render.filepath = os.path.join(rdir, tag + ".png")
    bpy.ops.render.render(write_still=True)
    print("RENDERED", sc.render.filepath)
d = span * 2.0
shoot(mathutils.Vector((0, 0, d)),  'Y', "rig_top", 700, 700)
shoot(mathutils.Vector((0, -d, 0)), 'Z', "rig_front", 520, 900)

bpy.ops.object.select_all(action='DESELECT')
mesh.select_set(True)
bpy.context.view_layer.objects.active = mesh
os.makedirs(os.path.dirname(out), exist_ok=True)
bpy.ops.export_scene.fbx(filepath=out, use_selection=True, object_types={'MESH'},
                         apply_unit_scale=True, axis_forward='-Z', axis_up='Y',
                         path_mode='COPY', embed_textures=True)
print("PREP_EXPORTED ->", out)
