# Render the prepped tileset tiles (floor / wall_plain / wall_traced) in EEVEE with the
# cyan emissive boosted, for a quick visual check.
#   blender.exe --background --python tools/dev/render_tiles.py
import bpy
import mathutils
import math

ROOT = r"C:/Users/rq27/Pulse"
KIT = ROOT + "/assets/external/sketchfab_scifi/scifi_tileset"
TILES = ["floor", "wall_plain", "wall_traced"]


def wbbox(o):
    lo = mathutils.Vector((1e30,) * 3); hi = mathutils.Vector((-1e30,) * 3)
    for c in o.bound_box:
        w = o.matrix_world @ mathutils.Vector(c)
        for i in range(3):
            lo[i] = min(lo[i], w[i]); hi[i] = max(hi[i], w[i])
    return lo, hi


for name in TILES:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=KIT + "/" + name + "/scene.gltf")
    # the geometry-only tile references the shared atlas via ../textures - rebind images
    for img in bpy.data.images:
        if img.source == "FILE" and not img.has_data:
            base = img.filepath.split("/")[-1].split("\\")[-1]
            img.filepath = KIT + "/textures/" + base
            try:
                img.reload()
            except Exception:
                pass
    for m in bpy.data.materials:
        if m.use_nodes:
            for n in m.node_tree.nodes:
                if n.type == "BSDF_PRINCIPLED":
                    e = n.inputs.get("Emission Strength")
                    if e:
                        e.default_value = max(e.default_value, 5.0)
    ms = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    lo = mathutils.Vector((1e30,) * 3); hi = mathutils.Vector((-1e30,) * 3)
    for o in ms:
        l, h = wbbox(o)
        for i in range(3):
            lo[i] = min(lo[i], l[i]); hi[i] = max(hi[i], h[i])
    ctr = (lo + hi) * 0.5; big = max((hi - lo)) or 100.0
    scn = bpy.context.scene
    scn.render.engine = "BLENDER_EEVEE"
    scn.view_settings.exposure = 1.2
    scn.render.resolution_x = 640; scn.render.resolution_y = 640
    world = bpy.data.worlds.new("w"); scn.world = world; world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.06, 0.07, 0.10, 1)
    world.node_tree.nodes["Background"].inputs[1].default_value = 0.7
    light = bpy.data.lights.new("s", "SUN"); light.energy = 4.0
    lobj = bpy.data.objects.new("s", light); lobj.rotation_euler = (math.radians(55), 0, math.radians(35))
    bpy.context.collection.objects.link(lobj)
    cd = bpy.data.cameras.new("c"); cd.type = "ORTHO"
    cd.ortho_scale = big * 1.35; cd.clip_start = 0.1; cd.clip_end = big * 12
    cam = bpy.data.objects.new("c", cd)
    cam.location = mathutils.Vector((ctr.x + big, ctr.y - big, ctr.z + big * 0.65))
    cam.rotation_euler = (math.radians(60), 0, math.radians(45))
    bpy.context.collection.objects.link(cam); scn.camera = cam
    scn.render.filepath = ROOT + "/build/tile_" + name + ".png"
    bpy.ops.render.render(write_still=True)
    print("RENDERED tile_" + name)

print("RENDER_TILES_DONE")
