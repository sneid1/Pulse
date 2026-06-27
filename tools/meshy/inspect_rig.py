# inspect_rig.py - dump a bumstrum FPS rig's structure to plan the weapon merge. ASCII only.
# Usage: blender --background --python inspect_rig.py -- <rig.gltf>
import bpy, sys

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
path = argv[0]
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.import_scene.gltf(filepath=path)
scene = bpy.context.scene

print("OBJECTS:")
for o in scene.objects:
    info = "  " + o.name + " / " + o.type
    if o.type == 'MESH':
        d = o.dimensions
        info += " dims=(%.3f,%.3f,%.3f)" % (d.x, d.y, d.z)
        wc = o.matrix_world.translation
        info += " worldpos=(%.2f,%.2f,%.2f)" % (wc.x, wc.y, wc.z)
        vg = {}
        for v in o.data.vertices:
            for g in v.groups:
                vg[g.group] = vg.get(g.group, 0.0) + g.weight
        top = sorted(vg.items(), key=lambda kv: -kv[1])[:4]
        names = []
        for gi, w in top:
            if gi < len(o.vertex_groups):
                names.append(o.vertex_groups[gi].name + ":%.0f" % w)
        info += " topbones=[" + ", ".join(names) + "]"
    print(info)

print("ARMATURES:")
for o in scene.objects:
    if o.type == 'ARMATURE':
        print("  " + o.name + " (" + str(len(o.data.bones)) + " bones):")
        print("    " + ", ".join(b.name for b in o.data.bones))

print("ACTIONS:", [a.name + "(" + str(int(a.frame_range[0])) + "-" + str(int(a.frame_range[1])) + ")" for a in bpy.data.actions])
