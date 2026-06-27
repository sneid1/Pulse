# render_clips.py - render a multi-clip glTF at specific (action:frame) poses, 3/4 view, to
# verify the clips drive the mesh correctly (stance, feet, cast/walk read). ASCII only.
# Usage: blender -b --python render_clips.py -- <model.gltf> <out_dir> <action:frame> [...]
import bpy, sys, os, math, mathutils

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
model, out_dir = argv[0], argv[1]
shots = argv[2:]
os.makedirs(out_dir, exist_ok=True)

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.import_scene.gltf(filepath=model)
arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
meshes = [o for o in bpy.data.objects if o.type == 'MESH' and 'icosphere' not in o.name.lower()]
for o in [o for o in bpy.data.objects if o.type == 'MESH' and 'icosphere' in o.name.lower()]:
    o.hide_render = True

sc = bpy.context.scene
eng = None
for e in ('BLENDER_EEVEE_NEXT', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'):
    try: sc.render.engine = e; eng = e; break
    except Exception: pass
sc.render.resolution_x = 460; sc.render.resolution_y = 760
try:
    sc.view_settings.view_transform = 'Standard'; sc.view_settings.look = 'None'
except Exception: pass
if sc.world is None: sc.world = bpy.data.worlds.new("w")
sc.world.use_nodes = True
bg = sc.world.node_tree.nodes.get('Background')
if bg: bg.inputs[0].default_value = (0.30, 0.30, 0.34, 1.0); bg.inputs[1].default_value = 1.05
if eng == 'BLENDER_WORKBENCH':
    sh = sc.display.shading; sh.light = 'STUDIO'; sh.show_cavity = True; sh.color_type = 'SINGLE'
    sh.single_color = (0.55, 0.55, 0.58)

def add_sun(rx, rz, en, col):
    d = bpy.data.lights.new("s", 'SUN'); d.energy = en; d.color = col
    o = bpy.data.objects.new("s", d); sc.collection.objects.link(o)
    o.rotation_euler = (math.radians(rx), 0.0, math.radians(rz))
add_sun(55, 35, 3.0, (1, 0.98, 0.94)); add_sun(120, 205, 2.4, (0.84, 0.89, 1.0)); add_sun(70, -110, 0.9, (0.9, 0.92, 0.97))

cd = bpy.data.cameras.new("c"); cam = bpy.data.objects.new("c", cd)
sc.collection.objects.link(cam); sc.camera = cam; cd.type = 'ORTHO'

if arm and arm.animation_data is None:
    arm.animation_data_create()

def posed_bbox():
    deps = bpy.context.evaluated_depsgraph_get()
    mn = mathutils.Vector((1e9, 1e9, 1e9)); mx = mathutils.Vector((-1e9, -1e9, -1e9))
    for o in meshes:
        ev = o.evaluated_get(deps); me = ev.to_mesh()
        for v in me.vertices:
            w = o.matrix_world @ v.co
            for i in range(3):
                mn[i] = min(mn[i], w[i]); mx[i] = max(mx[i], w[i])
        ev.to_mesh_clear()
    return mn, mx

for shot in shots:
    name, fr = shot.split(":"); fr = int(fr)
    if name == "rest":
        arm.animation_data.action = None
    else:
        act = bpy.data.actions.get(name)
        if not act:
            print("MISS", name); continue
        arm.animation_data.action = act
    sc.frame_set(fr)
    bpy.context.view_layer.update()
    mn, mx = posed_bbox()
    ctr = (mn + mx) * 0.5
    cd.ortho_scale = max(mx.x - mn.x, mx.z - mn.z) * 1.15
    cam.location = ctr + mathutils.Vector((0.0, -max((mx - mn).length, 2.0) * 2.0, 0.0))
    cam.rotation_euler = (ctr - cam.location).to_track_quat('-Z', 'Z').to_euler()
    sc.render.filepath = os.path.join(out_dir, name + ".png")
    bpy.ops.render.render(write_still=True)
    print("RENDERED", name, "frame", fr, "h=%.2f" % (mx.z - mn.z))
