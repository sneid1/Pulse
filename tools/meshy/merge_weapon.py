# merge_weapon.py - bake a Meshy weapon mesh into a bumstrum FPS arm rig, keeping the
# arms + their baked animation. Deletes the bumstrum weapon part-meshes, imports the Meshy
# weapon, fits it to the original weapon's bounding box, and parents it to the weapon-root
# node so it follows the hands during the animation. Re-exports a merged viewmodel GLB the
# existing engine pipeline loads unchanged. ASCII only.
#
# Usage: blender -b --python merge_weapon.py -- <rig.gltf> <meshy_weapon.glb> <out.glb>
#                                                [scaleMult rotX rotY rotZ offX offY offZ]
import bpy, sys, math, mathutils

a = sys.argv
a = a[a.index("--") + 1:] if "--" in a else []
rigPath, meshyPath, outPath = a[0], a[1], a[2]
scaleMult = float(a[3]) if len(a) > 3 else 1.0
rot = (float(a[4]) if len(a) > 4 else 0.0, float(a[5]) if len(a) > 5 else 0.0, float(a[6]) if len(a) > 6 else 0.0)
off = (float(a[7]) if len(a) > 7 else 0.0, float(a[8]) if len(a) > 8 else 0.0, float(a[9]) if len(a) > 9 else 0.0)

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.import_scene.gltf(filepath=rigPath)
scene = bpy.context.scene

# Arm mesh = the skinned mesh with the most vertex-group weight; everything else MESH
# (except tiny helpers) is the weapon.
meshes = [o for o in scene.objects if o.type == 'MESH']
def weightSum(o):
    s = 0.0
    for v in o.data.vertices:
        for g in v.groups: s += g.weight
    return s
armMesh = max(meshes, key=weightSum) if meshes else None
helpers = ('icosphere',)
weaponMeshes = [o for o in meshes if o is not armMesh and o.name.lower() not in helpers
                and weightSum(o) < 1.0]
print("ARM MESH:", armMesh.name if armMesh else None)
print("WEAPON MESHES:", [o.name for o in weaponMeshes])

# Weapon-root node: walk up from a weapon mesh to the highest non-scene empty (the node
# that carries the weapon's animated gross transform).
sceneRoots = ('sketchfab_model', 'rootnode', 'object_2')
def weaponRoot(o):
    node = o
    while node.parent is not None and node.parent.name.lower() not in sceneRoots \
          and node.parent.type != 'ARMATURE':
        node = node.parent
    return node
root = weaponRoot(weaponMeshes[0]) if weaponMeshes else None
if root:
    p = root.parent
    print("WEAPON ROOT:", root.name, "parent=", (p.name + "/" + p.type) if p else None,
          "parent_bone=", root.parent_bone if root.parent_type == 'BONE' else "(none)")

# Union world-space bbox of the original weapon at the rest pose.
import functools
lo = mathutils.Vector((1e9, 1e9, 1e9)); hi = mathutils.Vector((-1e9, -1e9, -1e9))
for o in weaponMeshes:
    for c in o.bound_box:
        w = o.matrix_world @ mathutils.Vector(c)
        for i in range(3):
            lo[i] = min(lo[i], w[i]); hi[i] = max(hi[i], w[i])
center = (lo + hi) * 0.5
size = hi - lo
origLongest = max(size.x, size.y, size.z)
print("ORIG WEAPON bbox center=(%.2f,%.2f,%.2f) size=(%.2f,%.2f,%.2f)" % (center.x, center.y, center.z, size.x, size.y, size.z))

# Import the Meshy weapon (its own objects).
before = set(bpy.data.objects)
bpy.ops.import_scene.gltf(filepath=meshyPath)
meshyObjs = [o for o in bpy.data.objects if o not in before]
meshyMeshes = [o for o in meshyObjs if o.type == 'MESH']
# Join the Meshy meshes into one + drop the imported empties.
bpy.ops.object.select_all(action='DESELECT')
for o in meshyMeshes: o.select_set(True)
bpy.context.view_layer.objects.active = meshyMeshes[0]
if len(meshyMeshes) > 1: bpy.ops.object.join()
gun = bpy.context.view_layer.objects.active
for o in meshyObjs:
    if o.type != 'MESH' and o.name in bpy.data.objects: bpy.data.objects.remove(o, do_unlink=True)

# Fit: scale longest dim to the original weapon's longest dim, recenter to its bbox center,
# apply the per-call orientation correction + offset.
bpy.context.view_layer.update()
glo = mathutils.Vector((1e9, 1e9, 1e9)); ghi = mathutils.Vector((-1e9, -1e9, -1e9))
for c in gun.bound_box:
    w = gun.matrix_world @ mathutils.Vector(c)
    for i in range(3):
        glo[i] = min(glo[i], w[i]); ghi[i] = max(ghi[i], w[i])
gsize = ghi - glo
gLongest = max(gsize.x, gsize.y, gsize.z, 1e-5)
s = (origLongest / gLongest) * scaleMult
rotM = mathutils.Euler((math.radians(rot[0]), math.radians(rot[1]), math.radians(rot[2])), 'XYZ').to_matrix().to_4x4()
gun.matrix_world = (mathutils.Matrix.Translation(center + mathutils.Vector(off))
                    @ rotM @ mathutils.Matrix.Scale(s, 4)
                    @ mathutils.Matrix.Translation(-(glo + ghi) * 0.5))
bpy.context.view_layer.update()

# Parent the Meshy gun to the weapon root (keep world transform) so it inherits the
# animated gross weapon motion; then delete the original weapon part-meshes.
if root:
    gun.parent = root
    gun.matrix_parent_inverse = root.matrix_world.inverted()
gun.name = "MeshyWeapon"
for o in list(weaponMeshes):
    if o.name in bpy.data.objects: bpy.data.objects.remove(o, do_unlink=True)

# Export the merged rig (armature + arms + Meshy weapon + animation).
bpy.ops.object.select_all(action='SELECT')
bpy.ops.export_scene.gltf(filepath=outPath, export_format='GLB',
                          export_animations=True, export_skins=True, use_selection=True)
print("MERGED ->", outPath)
