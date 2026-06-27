# Headless preview render of the current .blend through its scene camera.
# Run: blender.exe -b <file.blend> --python tools/dev/render_preview.py -- <out.png>
import bpy, sys, math

argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
out = argv[0] if argv else 'C:/Users/rq27/Pulse/tools/dev/preview.png'

scene = bpy.context.scene
for eng in ('BLENDER_EEVEE_NEXT', 'BLENDER_EEVEE', 'CYCLES'):
    try:
        scene.render.engine = eng
        break
    except Exception:
        continue
scene.render.resolution_x = 720
scene.render.resolution_y = 720
scene.render.image_settings.file_format = 'PNG'
scene.render.filepath = out

if not any(o.type == 'LIGHT' for o in bpy.data.objects):
    ld = bpy.data.lights.new('sun', 'SUN')
    ld.energy = 4.0
    lo = bpy.data.objects.new('sun', ld)
    scene.collection.objects.link(lo)
    lo.rotation_euler = (math.radians(55), 0.0, math.radians(35))

cam = next((o for o in bpy.data.objects if o.type == 'CAMERA'), None)
if cam:
    scene.camera = cam
    print("CAM loc:", tuple(round(v, 3) for v in cam.location),
          "rot:", tuple(round(v, 3) for v in cam.rotation_euler),
          "lens:", round(cam.data.lens, 1))
bpy.ops.render.render(write_still=True)
print("RENDERED", out)
