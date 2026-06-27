# fix_feet.py - straighten a Meshy character's toed-out feet so they point forward (+Y),
# then export a clean single-mesh FBX for Mixamo + a front verification render. The twist is
# height-weighted: full correction at the sole, fading to zero at the ankle, so the shin is
# untouched and the ankle stays put. Drops armature/empty/proxy. ASCII only.
# Usage: blender -b --python fix_feet.py -- <in.glb> <out.fbx> <render_dir>
import bpy, sys, os, math, mathutils

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
src, out, rdir = argv[0], argv[1], argv[2]

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.import_scene.gltf(filepath=src)
meshes = [o for o in bpy.data.objects if o.type == 'MESH' and 'icosphere' not in o.name.lower()]
for o in list(bpy.data.objects):
    if o not in meshes:
        bpy.data.objects.remove(o, do_unlink=True)
ob = meshes[0]
bpy.context.view_layer.objects.active = ob
ob.select_set(True)
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

mw = ob.matrix_world
co = [mw @ v.co for v in ob.data.vertices]
minz = min(c.z for c in co); maxz = max(c.z for c in co); H = maxz - minz
cx = 0.5 * (min(c.x for c in co) + max(c.x for c in co))
ankle_z = minz + 0.11 * H

def long_axis(pts):
    n = len(pts)
    mx = sum(p[0] for p in pts) / n; my = sum(p[1] for p in pts) / n
    a = b = d = 0.0
    for p in pts:
        dx = p[0] - mx; dy = p[1] - my
        a += dx * dx; b += dx * dy; d += dy * dy
    a /= n; b /= n; d /= n
    # largest-eigenvalue eigenvector of [[a,b],[b,d]]
    tr = a + d; det = a * d - b * b
    lam = tr / 2.0 + math.sqrt(max(0.0, (tr / 2.0) ** 2 - det))
    vx, vy = (b, lam - a) if abs(b) > 1e-9 else (1.0, 0.0)
    L = math.hypot(vx, vy) or 1.0
    vx /= L; vy /= L
    if vy < 0:
        vx, vy = -vx, -vy   # point toward the toe (+Y)
    return vx, vy

report = []
for sign, label in ((-1, "L"), (1, "R")):
    # one foot: verts below the ankle on this side
    idx = [i for i, c in enumerate(co)
           if c.z < ankle_z and ((c.x - cx) * sign) > 0]
    if not idx:
        continue
    sole = [(co[i].x, co[i].y) for i in idx if co[i].z < minz + 0.045 * H]
    if len(sole) < 8:
        sole = [(co[i].x, co[i].y) for i in idx]
    vx, vy = long_axis(sole)
    cur = math.atan2(vx, vy)             # signed angle of foot axis from +Y
    # ankle pivot: centroid of this foot's verts near the ankle band
    band = [co[i] for i in idx if co[i].z > ankle_z - 0.30 * (ankle_z - minz)]
    if not band:
        band = [co[i] for i in idx]
    px = sum(c.x for c in band) / len(band); py = sum(c.y for c in band) / len(band)
    report.append("foot %s toe-out=%.1f deg" % (label, math.degrees(cur)))
    for i in idx:
        c = co[i]
        w = max(0.0, min(1.0, (ankle_z - c.z) / max(1e-6, ankle_z - minz)))
        th = -cur * w
        ct = math.cos(th); st = math.sin(th)
        rx = c.x - px; ry = c.y - py
        nx = rx * ct - ry * st + px
        ny = rx * st + ry * ct + py
        lv = mw.inverted() @ mathutils.Vector((nx, ny, c.z))
        ob.data.vertices[i].co = lv
ob.data.update()
print("FIXFEET", "; ".join(report))

# ground feet to z=0, center x/y
co2 = [mw @ v.co for v in ob.data.vertices]
mn = mathutils.Vector((min(c.x for c in co2), min(c.y for c in co2), min(c.z for c in co2)))
mx = mathutils.Vector((max(c.x for c in co2), max(c.y for c in co2), max(c.z for c in co2)))
shift = mathutils.Vector((-0.5 * (mn.x + mx.x), -0.5 * (mn.y + mx.y), -mn.z))
ob.location += shift
bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)

# front verification render (workbench solid)
os.makedirs(rdir, exist_ok=True)
sc = bpy.context.scene
try: sc.render.engine = 'BLENDER_WORKBENCH'
except Exception: pass
sc.render.resolution_x = 520; sc.render.resolution_y = 900
sh = sc.display.shading; sh.light = 'STUDIO'; sh.show_cavity = True
sh.color_type = 'SINGLE'; sh.single_color = (0.55, 0.55, 0.58)
co3 = [ob.matrix_world @ v.co for v in ob.data.vertices]
ctr = mathutils.Vector((0, 0, 0.5 * (min(c.z for c in co3) + max(c.z for c in co3))))
size = max(max(c.z for c in co3) - min(c.z for c in co3), 0.5)
cd = bpy.data.cameras.new("c"); cam = bpy.data.objects.new("c", cd)
sc.collection.objects.link(cam); sc.camera = cam
cd.type = 'ORTHO'; cd.ortho_scale = size * 1.05
cam.location = ctr + mathutils.Vector((0, -size * 2.0, 0))
cam.rotation_euler = (ctr - cam.location).to_track_quat('-Z', 'Z').to_euler()
sc.render.filepath = os.path.join(rdir, "front_fixed.png")
bpy.ops.render.render(write_still=True)
print("RENDERED", sc.render.filepath)

# export FBX for Mixamo
bpy.ops.object.select_all(action='DESELECT')
ob.select_set(True)
bpy.context.view_layer.objects.active = ob
os.makedirs(os.path.dirname(out), exist_ok=True)
bpy.ops.export_scene.fbx(filepath=out, use_selection=True, object_types={'MESH'},
                         apply_unit_scale=True, axis_forward='-Z', axis_up='Y',
                         path_mode='COPY', embed_textures=True)
print("PREP_EXPORTED ->", out)
