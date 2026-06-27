# fix_feet2.py - straighten toed-out feet on a RIGLESS standing mesh (the raw Meshy glb).
# Robust method: per foot (split by x side, central tail excluded), define the foot axis as
# (toe-third centroid) - (heel-third centroid) along the body's forward; rotate the foot about
# the ankle so that axis points straight forward. Height-weighted so the shin/ankle stay put.
# Verifies with TOP + FRONT renders (top shows toe-out at a glance) and exports a Mixamo FBX.
# ASCII only.  Usage: blender -b --python fix_feet2.py -- <in.glb> <out.fbx> <render_dir>
import bpy, sys, os, math, mathutils

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
src, out, rdir = argv[0], argv[1], argv[2]

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.import_scene.gltf(filepath=src)
mesh = max((o for o in bpy.data.objects
            if o.type == 'MESH' and 'icosphere' not in o.name.lower()),
           key=lambda o: len(o.data.vertices))
for o in list(bpy.data.objects):
    if o is not mesh:
        bpy.data.objects.remove(o, do_unlink=True)
bpy.context.view_layer.objects.active = mesh
mesh.select_set(True)
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

co = [mesh.matrix_world @ v.co for v in mesh.data.vertices]
minz = min(c.z for c in co); maxz = max(c.z for c in co); H = maxz - minz
cx = 0.5 * (min(c.x for c in co) + max(c.x for c in co))
ankle_z = minz + 0.12 * H
tail_band = 0.06 * H   # exclude the thin central tail from foot processing

# Body forward (-Y or +Y): the feet extend along the long horizontal axis; use the foot bbox.
foot_all = [co[i] for i in range(len(co))
            if co[i].z < ankle_z and abs(co[i].x - cx) > tail_band]
yspan = max(c.y for c in foot_all) - min(c.y for c in foot_all)
# determine which Y end is the toe: toes are the clawed end; assume forward = the side with
# the longer reach from the leg. We resolve per-foot below via heel/toe thirds; facing sign
# is taken from the mean foot elongation relative to body center.
ymid = 0.5 * (max(c.y for c in foot_all) + min(c.y for c in foot_all))

mwi = mesh.matrix_world.inverted()
report = []
for sign, label in ((-1, "L"), (1, "R")):
    idx = [i for i in range(len(co))
           if co[i].z < ankle_z and ((co[i].x - cx) * sign) > tail_band]
    if len(idx) < 12:
        continue
    ys = sorted(co[i].y for i in idx)
    lo = ys[max(0, int(0.25 * len(ys)))]; hi = ys[min(len(ys) - 1, int(0.75 * len(ys)))]
    front_pts = [co[i] for i in idx if co[i].y <= lo]   # one Y extreme
    back_pts = [co[i] for i in idx if co[i].y >= hi]     # other Y extreme
    def cen(pts):
        n = len(pts) or 1
        return mathutils.Vector((sum(p.x for p in pts) / n, sum(p.y for p in pts) / n))
    cf = cen(front_pts); cb = cen(back_pts)
    axis = cf - cb                       # heel->toe (sign resolved next)
    if axis.length < 1e-6:
        continue
    axis.normalize()
    target = mathutils.Vector((0.0, -1.0 if axis.y < 0 else 1.0))   # straighten, keep its facing
    cross = axis.x * target.y - axis.y * target.x
    dot = axis.x * target.x + axis.y * target.y
    ang = math.atan2(cross, dot)
    # ankle pivot: centroid of this foot's verts in the upper (near-ankle) band
    band = [co[i] for i in idx if co[i].z > ankle_z - 0.30 * (ankle_z - minz)]
    if not band:
        band = [co[i] for i in idx]
    px = sum(c.x for c in band) / len(band); py = sum(c.y for c in band) / len(band)
    report.append("%s splay=%.1f deg (n=%d)" % (label, math.degrees(ang), len(idx)))
    for i in idx:
        c = co[i]
        w = max(0.0, min(1.0, (ankle_z - c.z) / max(1e-6, ankle_z - minz)))
        th = ang * w
        ct = math.cos(th); st = math.sin(th)
        rx = c.x - px; ry = c.y - py
        nx = rx * ct - ry * st + px
        ny = rx * st + ry * ct + py
        mesh.data.vertices[i].co = mwi @ mathutils.Vector((nx, ny, c.z))
mesh.data.update()
print("FIXFEET2", "; ".join(report))

co = [mesh.matrix_world @ v.co for v in mesh.data.vertices]
mn = mathutils.Vector((min(c.x for c in co), min(c.y for c in co), min(c.z for c in co)))
mx = mathutils.Vector((max(c.x for c in co), max(c.y for c in co), max(c.z for c in co)))
mesh.location += mathutils.Vector((-0.5 * (mn.x + mx.x), -0.5 * (mn.y + mx.y), -mn.z))
bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)

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
shoot(mathutils.Vector((0, 0, d)),  'Y', "fix2_top", 700, 700)
shoot(mathutils.Vector((0, -d, 0)), 'Z', "fix2_front", 520, 900)

bpy.ops.object.select_all(action='DESELECT')
mesh.select_set(True); bpy.context.view_layer.objects.active = mesh
os.makedirs(os.path.dirname(out), exist_ok=True)
bpy.ops.export_scene.fbx(filepath=out, use_selection=True, object_types={'MESH'},
                         apply_unit_scale=True, axis_forward='-Z', axis_up='Y',
                         path_mode='COPY', embed_textures=True)
print("PREP_EXPORTED ->", out)
