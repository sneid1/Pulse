# Frame all mesh objects (posed/evaluated) and render a 3/4 view so we can see
# the actual rest pose. Run:
#   blender.exe -b <file.blend> --python tools/dev/render_arms_framed.py -- <out_prefix>
import bpy, math, sys, mathutils

argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
prefix = argv[0] if argv else 'C:/Users/rq27/Pulse/tools/dev/arms'

deps = bpy.context.evaluated_depsgraph_get()
mins = [1e9] * 3
maxs = [-1e9] * 3
for o in [m for m in bpy.data.objects if m.type == 'MESH']:
    ev = o.evaluated_get(deps)
    me = ev.to_mesh()
    for v in me.vertices:
        w = o.matrix_world @ v.co
        for i in range(3):
            mins[i] = min(mins[i], w[i])
            maxs[i] = max(maxs[i], w[i])
    ev.to_mesh_clear()
center = mathutils.Vector([(mins[i] + maxs[i]) * 0.5 for i in range(3)])
size = max(maxs[i] - mins[i] for i in range(3))
print("BOUNDS min", [round(x, 3) for x in mins], "max", [round(x, 3) for x in maxs])
print("CENTER", [round(x, 3) for x in center], "SIZE", round(size, 3))

scene = bpy.context.scene
for eng in ('BLENDER_EEVEE_NEXT', 'BLENDER_EEVEE', 'CYCLES'):
    try:
        scene.render.engine = eng
        break
    except Exception:
        continue
scene.render.resolution_x = 800
scene.render.resolution_y = 800
scene.render.image_settings.file_format = 'PNG'

if not any(o.type == 'LIGHT' for o in bpy.data.objects):
    ld = bpy.data.lights.new('sun', 'SUN'); ld.energy = 4.0
    lo = bpy.data.objects.new('sun', ld); scene.collection.objects.link(lo)
    lo.rotation_euler = (math.radians(55), 0.0, math.radians(35))

cam_data = bpy.data.cameras.new('pc'); cam_data.lens = 35
cam = bpy.data.objects.new('pc', cam_data); scene.collection.objects.link(cam)
scene.camera = cam

views = {
    'front':  mathutils.Vector(( 0.2,  1.0,  0.25)),
    'back':   mathutils.Vector(( 0.2, -1.0,  0.25)),
    'side':   mathutils.Vector(( 1.0,  0.15, 0.25)),
}
for name, d in views.items():
    eye = center + d.normalized() * size * 1.7
    cam.location = eye
    cam.rotation_euler = (center - eye).to_track_quat('-Z', 'Y').to_euler()
    scene.render.filepath = f"{prefix}_{name}.png"
    bpy.ops.render.render(write_still=True)
    print("RENDERED", scene.render.filepath)
