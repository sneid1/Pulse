# fbx_probe.py - print object types, action names, and bone info of an FBX (to confirm a
# Mixamo download is "With Skin" = has a mesh, and to read the bone naming). ASCII only.
# Usage: blender -b --python fbx_probe.py -- <a.fbx> [<b.fbx> ...]
import bpy, sys, os

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
for path in argv:
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for a in list(bpy.data.actions):
        bpy.data.actions.remove(a)
    bpy.ops.import_scene.fbx(filepath=path)
    meshes = [o for o in bpy.data.objects if o.type == 'MESH']
    arms = [o for o in bpy.data.objects if o.type == 'ARMATURE']
    nv = sum(len(m.data.vertices) for m in meshes)
    nb = sum(len(a.data.bones) for a in arms)
    bn = arms[0].data.bones[:6] if arms else []
    print("PROBE %-34s meshes=%d verts=%d armatures=%d bones=%d acts=%s firstbones=%s"
          % (os.path.basename(path), len(meshes), nv, len(arms), nb,
             [a.name for a in bpy.data.actions], [b.name for b in bn]))
