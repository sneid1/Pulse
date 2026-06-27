# Render a labeled thumbnail for every Quaternius MegaKit building piece (Walls / Platforms /
# Columns / Props / Decals) into build/kit_thumbs/<Category>/<name>.png, for the asset catalog
# reference sheets. One EEVEE scene reused across pieces; a fixed 3/4 camera auto-fit per piece.
#   blender.exe --background --python tools/dev/render_kit_thumbs.py
import bpy, mathutils, math, os

ROOT = r"C:/Users/rq27/Pulse"
MK = ROOT + "/assets/quaternius/Modular SciFi MegaKit[Pro]/glTF"
OUT = ROOT + "/build/kit_thumbs"
CATS = ["Walls", "Platforms", "Columns", "Props", "Decals"]
RES = 224

bpy.ops.wm.read_factory_settings(use_empty=True)
scn = bpy.context.scene
# Cycles on CPU: EEVEE headless fails to upload GPU textures (-> magenta), Cycles CPU loads the
# kit's PBR textures correctly. Low samples + denoise keep 271 thumbnails fast enough.
scn.render.engine = "CYCLES"
scn.cycles.device = "CPU"
scn.cycles.samples = 24
scn.cycles.use_denoising = True
scn.render.resolution_x = RES; scn.render.resolution_y = RES
scn.render.film_transparent = True
scn.view_settings.view_transform = "Standard"
# world ambient so dark metals still read
world = bpy.data.worlds.new("w"); scn.world = world; world.use_nodes = True
bg = world.node_tree.nodes["Background"]
bg.inputs[0].default_value = (0.5, 0.52, 0.56, 1.0); bg.inputs[1].default_value = 1.0
# key + fill
for ang, en in [((math.radians(55), 0, math.radians(40)), 3.0), ((math.radians(60), 0, math.radians(-120)), 1.2)]:
    L = bpy.data.lights.new("L", "SUN"); L.energy = en
    o = bpy.data.objects.new("L", L); o.rotation_euler = ang
    bpy.context.collection.objects.link(o)
cam_data = bpy.data.cameras.new("cam"); cam = bpy.data.objects.new("cam", cam_data)
bpy.context.collection.objects.link(cam); scn.camera = cam
cam_data.lens = 60


def world_bbox(objs):
    lo = mathutils.Vector((1e30,) * 3); hi = mathutils.Vector((-1e30,) * 3)
    for o in objs:
        if o.type != "MESH":
            continue
        for c in o.bound_box:
            w = o.matrix_world @ mathutils.Vector(c)
            for i in range(3):
                lo[i] = min(lo[i], w[i]); hi[i] = max(hi[i], w[i])
    return lo, hi


total = 0
for cat in CATS:
    cdir = MK + "/" + cat
    odir = OUT + "/" + cat
    os.makedirs(odir, exist_ok=True)
    names = sorted(f[:-5] for f in os.listdir(cdir) if f.endswith(".gltf"))
    for nm in names:
        # clear previous import (keep cam + lights) + purge orphan data so images do not pile up
        bpy.ops.object.select_all(action="DESELECT")
        for o in list(bpy.context.scene.objects):
            if o.type in {"MESH", "EMPTY"}:
                o.select_set(True)
        bpy.ops.object.delete()
        for coll in (bpy.data.meshes, bpy.data.materials, bpy.data.images):
            for b in list(coll):
                if b.users == 0:
                    coll.remove(b)
        try:
            bpy.ops.import_scene.gltf(filepath=cdir + "/" + nm + ".gltf")
        except Exception as e:  # noqa: BLE001
            print("IMPORT_FAIL", nm, e); continue
        # The per-subfolder texture copies are INCOMPLETE; the full set lives at the glTF root.
        # Repoint every imported image there and reload so nothing renders as missing (magenta).
        for im in bpy.data.images:
            if im.source != "FILE":
                continue
            bn = os.path.basename(im.filepath.replace("\\", "/"))
            cand = MK + "/" + bn
            if bn and os.path.exists(cand):
                im.filepath = cand
                try:
                    im.reload()
                    im.pack()   # force pixels into memory; headless Cycles will not lazy-load them
                except Exception:  # noqa: BLE001
                    pass
        ms = [o for o in bpy.context.scene.objects if o.type == "MESH"]
        if not ms:
            continue
        lo, hi = world_bbox(ms); ctr = (lo + hi) * 0.5
        rad = max((hi - lo).length * 0.5, 0.5)
        d = rad / math.tan(math.radians(cam_data.angle * 0.5 * 57.2958 if False else 18.0))  # ~36deg fov
        dirv = mathutils.Vector((1.0, -1.1, 0.9)).normalized()
        cam.location = ctr + dirv * (rad * 3.0)
        # aim at centre
        fwd = (ctr - cam.location).normalized()
        cam.rotation_euler = fwd.to_track_quat("-Z", "Y").to_euler()
        cam_data.ortho_scale = rad * 2.2
        cam_data.type = "ORTHO"
        scn.render.filepath = odir + "/" + nm + ".png"
        bpy.ops.render.render(write_still=True)
        total += 1
    print("RENDERED_CATEGORY", cat, len(names))
print("THUMBS_DONE total=%d" % total)
