# Headless Blender inspection: dump objects, meshes, rig, dimensions, textures.
# Run: blender.exe -b <file.blend> --python tools/dev/inspect_blend.py
import bpy

def r(vals, n=3):
    return tuple(round(float(v), n) for v in vals)

print("=== OBJECTS ===")
for obj in bpy.data.objects:
    print("OBJ:", obj.name, "| type:", obj.type, "| parent:", obj.parent.name if obj.parent else None)
    print("    loc:", r(obj.location), "rot_euler:", r(obj.rotation_euler), "scale:", r(obj.scale))
    if obj.type == 'MESH':
        me = obj.data
        print("    verts:", len(me.vertices), "polys:", len(me.polygons),
              "uv_layers:", [l.name for l in me.uv_layers])
        print("    dims(world):", r(obj.dimensions))
        print("    materials:", [m.name if m else None for m in me.materials])
        # modifiers (armature/mirror etc.)
        if obj.modifiers:
            print("    modifiers:", [(m.name, m.type) for m in obj.modifiers])
    if obj.type == 'ARMATURE':
        bones = obj.data.bones
        print("    bones:", len(bones), "->", [b.name for b in bones][:40])

print("=== MATERIALS ===")
for m in bpy.data.materials:
    nodes = []
    if m.use_nodes and m.node_tree:
        nodes = [n.type for n in m.node_tree.nodes]
    print("MAT:", m.name, "| nodes:", nodes)

print("=== IMAGES ===")
for img in bpy.data.images:
    print("IMG:", img.name, "| size:", tuple(img.size), "| file:", img.filepath)

print("=== SCENE UNITS ===")
us = bpy.context.scene.unit_settings
print("system:", us.system, "scale_length:", us.scale_length)
print("=== DONE ===")
