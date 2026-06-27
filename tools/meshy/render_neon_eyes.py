# render_neon_eyes.py - preview a KayKit skeleton with the body on its gradient atlas and the
# "eyes" submesh given an emissive-magenta material (mirrors the in-engine neon-eye pass), framed
# on the head, so the eye recolour can be confirmed without the in-game camera/flash. ASCII only.
# Usage: blender -b --python render_neon_eyes.py -- <model.gltf> <atlas.png> <out.png>
import bpy, sys, os, math, mathutils

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
model, atlas, out = argv[0], argv[1], argv[2]

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.import_scene.gltf(filepath=model)
meshes = [o for o in bpy.data.objects if o.type == 'MESH' and 'icosphere' not in o.name.lower()]

img = bpy.data.images.load(atlas)
body = bpy.data.materials.new("atlas"); body.use_nodes = True
bn = body.node_tree.nodes.get("Principled BSDF")
tn = body.node_tree.nodes.new("ShaderNodeTexImage"); tn.image = img
body.node_tree.links.new(tn.outputs["Color"], bn.inputs["Base Color"])
bn.inputs["Roughness"].default_value = 0.6

eye = bpy.data.materials.new("eye"); eye.use_nodes = True
en = eye.node_tree.nodes.get("Principled BSDF")
en.inputs["Emission Color"].default_value = (1.6, 0.10, 0.95, 1.0)
en.inputs["Emission Strength"].default_value = 6.0
en.inputs["Base Color"].default_value = (1.5, 0.1, 0.85, 1.0)

for o in meshes:
    o.data.materials.clear()
    o.data.materials.append(eye if 'eye' in o.name.lower() else body)

sc = bpy.context.scene
for e in ('BLENDER_EEVEE_NEXT', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'):
    try: sc.render.engine = e; break
    except Exception: pass
sc.render.resolution_x = 460; sc.render.resolution_y = 520
try: sc.view_settings.view_transform = 'Standard'; sc.view_settings.look = 'None'
except Exception: pass
if sc.world is None: sc.world = bpy.data.worlds.new("w")
sc.world.use_nodes = True
bg = sc.world.node_tree.nodes.get('Background')
if bg: bg.inputs[0].default_value = (0.03, 0.03, 0.05, 1.0); bg.inputs[1].default_value = 0.5
d = bpy.data.lights.new("k", 'SUN'); d.energy = 2.5; d.color = (0.8, 0.85, 1.0)
lo = bpy.data.objects.new("k", d); sc.collection.objects.link(lo); lo.rotation_euler = (math.radians(60), 0, math.radians(30))

# frame the UPPER body / head (top third of the model)
deps = bpy.context.evaluated_depsgraph_get()
mn = mathutils.Vector((1e9,)*3); mx = mathutils.Vector((-1e9,)*3)
for o in meshes:
    ev = o.evaluated_get(deps); me = ev.to_mesh()
    for v in me.vertices:
        w = o.matrix_world @ v.co
        for i in range(3): mn[i] = min(mn[i], w[i]); mx[i] = max(mx[i], w[i])
    ev.to_mesh_clear()
h = mx.z - mn.z
ctr = mathutils.Vector(((mn.x+mx.x)*0.5, (mn.y+mx.y)*0.5, mn.z + h*0.78))   # head height
cd = bpy.data.cameras.new("c"); cam = bpy.data.objects.new("c", cd); sc.collection.objects.link(cam); sc.camera = cam
cd.type = 'ORTHO'; cd.ortho_scale = h * 0.6
cam.location = ctr + mathutils.Vector((0, -h*2.0, 0))
cam.rotation_euler = (ctr - cam.location).to_track_quat('-Z', 'Z').to_euler()
sc.render.filepath = out
bpy.ops.render.render(write_still=True)
print("RENDERED", out)
