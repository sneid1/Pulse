# render_textured.py - apply a base-color texture to a glTF rig AS albedo + magenta emission
# (mimics the engine's emissive = albedo * scalar) and render a 3/4 view, to judge the enemy
# body look (obsidian + glowing veins) without the in-game gun/FX clutter. ASCII only.
# Usage: blender -b --python render_textured.py -- <model.gltf> <tex.png> <out.png> [action:frame]
import bpy, sys, os, math, mathutils

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
model, tex, out = argv[0], argv[1], argv[2]
shot = argv[3] if len(argv) > 3 else "idle:20"

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.import_scene.gltf(filepath=model)
arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
meshes = [o for o in bpy.data.objects if o.type == 'MESH' and 'icosphere' not in o.name.lower()]
for o in [o for o in bpy.data.objects if o.type == 'MESH' and 'icosphere' in o.name.lower()]:
    o.hide_render = True

img = bpy.data.images.load(tex)
mat = bpy.data.materials.new("obsidian_energy"); mat.use_nodes = True
nt = mat.node_tree
bsdf = nt.nodes.get("Principled BSDF")
texnode = nt.nodes.new("ShaderNodeTexImage"); texnode.image = img
nt.links.new(texnode.outputs["Color"], bsdf.inputs["Base Color"])
nt.links.new(texnode.outputs["Color"], bsdf.inputs["Emission Color"])
bsdf.inputs["Emission Strength"].default_value = 2.4
bsdf.inputs["Metallic"].default_value = 0.2
bsdf.inputs["Roughness"].default_value = 0.34
for ob in meshes:
    ob.data.materials.clear(); ob.data.materials.append(mat)

sc = bpy.context.scene
for e in ('BLENDER_EEVEE_NEXT', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'):
    try: sc.render.engine = e; break
    except Exception: pass
sc.render.resolution_x = 460; sc.render.resolution_y = 760
try:
    sc.view_settings.view_transform = 'Standard'; sc.view_settings.look = 'None'
except Exception: pass
if sc.world is None: sc.world = bpy.data.worlds.new("w")
sc.world.use_nodes = True
bg = sc.world.node_tree.nodes.get('Background')
if bg: bg.inputs[0].default_value = (0.02, 0.02, 0.03, 1.0); bg.inputs[1].default_value = 0.6

def add_sun(rx, rz, en, col):
    d = bpy.data.lights.new("s", 'SUN'); d.energy = en; d.color = col
    o = bpy.data.objects.new("s", d); sc.collection.objects.link(o)
    o.rotation_euler = (math.radians(rx), 0.0, math.radians(rz))
add_sun(55, 35, 2.0, (0.8, 0.85, 1.0)); add_sun(120, 205, 2.2, (1.0, 0.4, 0.9))  # cool key + magenta rim

if arm and arm.animation_data is None:
    arm.animation_data_create()
name, fr = shot.split(":")
act = bpy.data.actions.get(name)
if act and arm: arm.animation_data.action = act
sc.frame_set(int(fr)); bpy.context.view_layer.update()

deps = bpy.context.evaluated_depsgraph_get()
mn = mathutils.Vector((1e9, 1e9, 1e9)); mx = mathutils.Vector((-1e9, -1e9, -1e9))
for o in meshes:
    ev = o.evaluated_get(deps); me = ev.to_mesh()
    for v in me.vertices:
        w = o.matrix_world @ v.co
        for i in range(3):
            mn[i] = min(mn[i], w[i]); mx[i] = max(mx[i], w[i])
    ev.to_mesh_clear()
ctr = (mn + mx) * 0.5; size = max((mx - mn).length, 0.5)
cd = bpy.data.cameras.new("c"); cam = bpy.data.objects.new("c", cd)
sc.collection.objects.link(cam); sc.camera = cam; cd.lens = 50
cam.location = ctr + mathutils.Vector((size * 0.55, -size * 0.95, size * 0.10))
cam.rotation_euler = (ctr - cam.location).to_track_quat('-Z', 'Z').to_euler()
sc.render.filepath = out
bpy.ops.render.render(write_still=True)
print("RENDERED", out)
