# unpack_glb.py - re-export a Meshy .glb (embedded textures) as .gltf + .bin + external
# texture PNGs, so the engine's AnimatedGltf loader (which only resolves external image
# URIs) can bind the textures. Preserves the skin + animation. ASCII only.
# Usage: blender -b --python unpack_glb.py -- <in.glb> <out.gltf>
import bpy, sys, os

a = sys.argv
a = a[a.index("--") + 1:] if "--" in a else []
src, out = a[0], a[1]

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.import_scene.gltf(filepath=src)
os.makedirs(os.path.dirname(out), exist_ok=True)
bpy.ops.export_scene.gltf(
    filepath=out,
    export_format='GLTF_SEPARATE',   # .gltf + .bin + external image files
    export_animations=True,
    export_skins=True,
    export_yup=True,
)
print("EXPORTED ->", out)
