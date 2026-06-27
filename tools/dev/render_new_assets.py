# Render a quick reference of the newly-dropped Quaternius kits (mechs + a Cyberpunk sample) so we
# can judge what's worth including. blender --background --python tools/dev/render_new_assets.py
import bpy, mathutils, math, os

ROOT = r"C:/Users/rq27/Pulse"
Q = ROOT + "/assets/quaternius"
OUT = ROOT + "/build/new_thumbs"
os.makedirs(OUT, exist_ok=True)
RES = 256

# (label, gltf path)
mech = Q + "/Animated Mech Pack/Animated Mech Pack - March 2021/Textured/glTF"
cyber = Q + "/Cyberpunk Game Kit/Cyberpunk Game Kit - Quaternius"
ITEMS = [
    ("Mech_George", mech + "/George.gltf"), ("Mech_Leela", mech + "/Leela.gltf"),
    ("Mech_Mike", mech + "/Mike.gltf"), ("Mech_Stan", mech + "/Stan.gltf"),
    ("Cyber_Computer_Large", cyber + "/Platforms/Computer_Large.gltf"),
    ("Cyber_AC", cyber + "/Platforms/AC.gltf"), ("Cyber_TV_1", cyber + "/Platforms/TV_1.gltf"),
    ("Cyber_Light_Street_1", cyber + "/Platforms/Light_Street_1.gltf"),
    ("Cyber_Turret_Gun", cyber + "/Enemies/Turret_Gun.gltf"),
    ("Cyber_Enemy_2Legs", cyber + "/Enemies/Enemy_2Legs.gltf"),
    ("Cyber_Lootbox", cyber + "/Pickups and Objects/Lootbox.gltf"),
    ("Cyber_Platform_4x4_Empty", cyber + "/Platforms/Platform_4x4_Empty.gltf"),
]

bpy.ops.wm.read_factory_settings(use_empty=True)
scn = bpy.context.scene
scn.render.engine = "CYCLES"; scn.cycles.device = "CPU"; scn.cycles.samples = 24; scn.cycles.use_denoising = True
scn.render.resolution_x = RES; scn.render.resolution_y = RES; scn.render.film_transparent = True
scn.view_settings.view_transform = "Standard"
world = bpy.data.worlds.new("w"); scn.world = world; world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.5, 0.52, 0.56, 1); world.node_tree.nodes["Background"].inputs[1].default_value = 1.0
for ang, en in [((math.radians(55), 0, math.radians(40)), 3.0), ((math.radians(60), 0, math.radians(-120)), 1.2)]:
    L = bpy.data.lights.new("L", "SUN"); L.energy = en
    o = bpy.data.objects.new("L", L); o.rotation_euler = ang; bpy.context.collection.objects.link(o)
cam_data = bpy.data.cameras.new("c"); cam_data.type = "ORTHO"; cam = bpy.data.objects.new("c", cam_data)
bpy.context.collection.objects.link(cam); scn.camera = cam


def wb(objs):
    lo = mathutils.Vector((1e30,) * 3); hi = mathutils.Vector((-1e30,) * 3)
    for o in objs:
        if o.type != "MESH": continue
        for c in o.bound_box:
            w = o.matrix_world @ mathutils.Vector(c)
            for i in range(3): lo[i] = min(lo[i], w[i]); hi[i] = max(hi[i], w[i])
    return lo, hi


for label, path in ITEMS:
    for o in list(scn.objects):
        if o.type in {"MESH", "EMPTY", "ARMATURE"}: o.select_set(True)
        else: o.select_set(False)
    bpy.ops.object.delete()
    for coll in (bpy.data.meshes, bpy.data.materials, bpy.data.images):
        for b in list(coll):
            if b.users == 0: coll.remove(b)
    if not os.path.exists(path):
        print("MISS", label); continue
    bpy.ops.import_scene.gltf(filepath=path)
    for im in bpy.data.images:
        if im.source == "FILE":
            try: im.reload(); im.pack()
            except Exception: pass
    ms = [o for o in scn.objects if o.type == "MESH"]
    if not ms: continue
    lo, hi = wb(ms); ctr = (lo + hi) * 0.5; rad = max((hi - lo).length * 0.5, 0.5)
    cam_data.ortho_scale = rad * 2.3; cam_data.clip_start = 0.01; cam_data.clip_end = rad * 12
    cam.location = ctr + mathutils.Vector((1, -1.1, 0.9)).normalized() * rad * 3
    cam.rotation_euler = (ctr - cam.location).normalized().to_track_quat("-Z", "Y").to_euler()
    scn.render.filepath = OUT + "/" + label + ".png"
    bpy.ops.render.render(write_still=True)
    print("RENDERED", label)
print("NEW_DONE")
