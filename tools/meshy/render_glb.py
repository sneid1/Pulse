# render_glb.py - headless Blender preview renderer for Meshy GLBs. ASCII only.
# Usage: blender --background --python render_glb.py -- <out_dir> <glb1> [<glb2> ...]
# EEVEE render with the imported PBR materials, hard-surface (auto-smooth) shading and
# key/rim/fill lighting so metal catches crisp specular highlights instead of reading like
# matte clay. Frames each GLB with a 3/4 camera. Falls back to Workbench if EEVEE is absent.

import bpy, sys, os, math, mathutils

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
out_dir = argv[0]
glbs = argv[1:]
os.makedirs(out_dir, exist_ok=True)

scene = bpy.context.scene
engine = None
for eng in ('BLENDER_EEVEE_NEXT', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'):
    try:
        scene.render.engine = eng
        engine = eng
        break
    except Exception:
        pass
scene.render.resolution_x = 900
scene.render.resolution_y = 720
try:
    scene.view_settings.view_transform = 'Standard'   # true colours for an asset preview (not AgX)
    scene.view_settings.look = 'None'
except Exception:
    pass

if scene.world is None:
    scene.world = bpy.data.worlds.new("w")
scene.world.use_nodes = True
bg = scene.world.node_tree.nodes.get('Background')
if bg:
    # Neutral, fairly bright studio environment so METAL has something to reflect (a dark
    # world makes PBR metal read as broken black patches). This is the reflection source.
    bg.inputs[0].default_value = (0.32, 0.32, 0.35, 1.0)
    bg.inputs[1].default_value = 1.1
try:
    scene.eevee.use_raytracing = True   # EEVEE Next: real specular reflections on metal
except Exception:
    pass
if engine == 'BLENDER_WORKBENCH':
    sh = scene.display.shading
    sh.light = 'STUDIO'; sh.show_cavity = False; sh.color_type = 'TEXTURE'


def clear():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)


def add_sun(name, rx, rz, energy, color):
    d = bpy.data.lights.new(name, 'SUN')
    d.energy = energy
    d.color = color
    try:
        d.angle = 0.06   # tight angle -> crisp specular streaks
    except Exception:
        pass
    o = bpy.data.objects.new(name, d)
    scene.collection.objects.link(o)
    o.rotation_euler = (math.radians(rx), 0.0, math.radians(rz))
    return o


def shade_sharp(meshes):
    try:
        for o in meshes:
            bpy.context.view_layer.objects.active = o
            o.select_set(True)
        bpy.ops.object.shade_auto_smooth(angle=math.radians(32))
    except Exception:
        for o in meshes:
            for p in o.data.polygons:
                p.use_smooth = False   # flat fallback (still crisp, just faceted)


def frame_and_render(glb, out):
    clear()
    bpy.ops.import_scene.gltf(filepath=glb)
    meshes = [o for o in scene.objects if o.type == 'MESH']
    if not meshes:
        return False
    shade_sharp(meshes)
    mn = mathutils.Vector((1e9, 1e9, 1e9))
    mx = mathutils.Vector((-1e9, -1e9, -1e9))
    for o in meshes:
        for c in o.bound_box:
            w = o.matrix_world @ mathutils.Vector(c)
            for i in range(3):
                mn[i] = min(mn[i], w[i]); mx[i] = max(mx[i], w[i])
    center = (mn + mx) * 0.5
    size = max((mx - mn).length, 0.25)
    cd = bpy.data.cameras.new("cam")
    cam = bpy.data.objects.new("cam", cd)
    scene.collection.objects.link(cam)
    scene.camera = cam
    cd.lens = 60
    d = size * 1.15
    cam.location = center + mathutils.Vector((d * 0.85, -d * 1.0, d * 0.5))
    direction = center - cam.location
    cam.rotation_euler = direction.to_track_quat('-Z', 'Y').to_euler()
    add_sun("key", 55, 35, 3.2, (1.0, 0.98, 0.94))    # near-neutral key, upper front-right
    add_sun("rim", 120, 205, 2.6, (0.84, 0.89, 1.0))  # gentle cool rim from behind-left
    add_sun("fill", 70, -110, 0.9, (0.9, 0.92, 0.97)) # soft neutral fill
    scene.render.filepath = out
    bpy.ops.render.render(write_still=True)
    return True


for glb in glbs:
    stem = os.path.splitext(os.path.basename(glb))[0]
    out = os.path.join(out_dir, stem + ".png")
    try:
        ok = frame_and_render(glb, out)
        print(("RENDERED " if ok else "EMPTY ") + out)
    except Exception as e:
        print("FAIL %s: %s" % (glb, e))
