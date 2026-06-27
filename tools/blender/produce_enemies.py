#!/usr/bin/env python3
# Produce game-ready enemy glb from Meshy OBJ exports, reusing existing animation where possible.
#   rig:<obj>    -> decimate, fit + bind to the George RobotArmature, export glb WITH its 20 actions
#   hover:<obj>  -> decimate, give a 1-bone armature + subtle Idle bob (loader needs >=1 clip), export glb
#   static:<obj> -> decimate, export glb (no rig)
# Texture = <obj-stem>.png next to the obj (the Meshy .mtl link is stale, so assign the PNG directly).
#   blender -b --python tools/blender/produce_enemies.py -- <outdir> <rig.gltf> rig:<a.obj> hover:<b.obj> ...
import bpy, sys, os
import numpy as np
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
outdir, rig_src, entries = argv[0], argv[1], argv[2:]
os.makedirs(outdir, exist_ok=True)
TARGET = 40000


def bbox(objs):
    mn = Vector((1e9, 1e9, 1e9)); mx = Vector((-1e9, -1e9, -1e9))
    for o in objs:
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            for i in range(3):
                mn[i] = min(mn[i], w[i]); mx[i] = max(mx[i], w[i])
    return mn, mx


def tri_count(m):
    return sum(len(p.vertices) - 2 for p in m.data.polygons)


def derive_normal_image(base_img, name):
    # Meshy exports only a base-colour image, so derive a tangent-space NORMAL map from the painted
    # detail (height = luminance -> gradient) to give the flat mesh real surface relief under the key
    # light. OpenGL +Y convention to match the engine's normal sampling.
    w, h = base_img.size
    src = np.empty(w * h * 4, dtype=np.float32)
    base_img.pixels.foreach_get(src)
    src = src.reshape(h, w, 4)
    lum = 0.299 * src[:, :, 0] + 0.587 * src[:, :, 1] + 0.114 * src[:, :, 2]
    k = 5.0
    gx = (np.roll(lum, -1, 1) - np.roll(lum, 1, 1)) * k
    gy = (np.roll(lum, -1, 0) - np.roll(lum, 1, 0)) * k
    inv = 1.0 / np.sqrt(gx * gx + gy * gy + 1.0)
    out = np.empty((h, w, 4), dtype=np.float32)
    out[:, :, 0] = (-gx * inv) * 0.5 + 0.5
    out[:, :, 1] = (gy * inv) * 0.5 + 0.5
    out[:, :, 2] = inv * 0.5 + 0.5
    out[:, :, 3] = 1.0
    nimg = bpy.data.images.new(name + "_n", w, h, alpha=True)
    nimg.colorspace_settings.name = 'Non-Color'
    nimg.pixels.foreach_set(out.ravel())
    nimg.pack()
    return nimg


def assign_texture(mesh, png):
    mat = bpy.data.materials.new(mesh.name + "_mat"); mat.use_nodes = True
    nt = mat.node_tree; bsdf = nt.nodes.get("Principled BSDF")
    img = bpy.data.images.load(png)
    tex = nt.nodes.new("ShaderNodeTexImage"); tex.image = img
    nt.links.new(bsdf.inputs["Base Color"], tex.outputs["Color"])
    # Derived normal map -> Normal Map node -> Principled (relief from the painted detail).
    nimg = derive_normal_image(img, mesh.name)
    ntex = nt.nodes.new("ShaderNodeTexImage"); ntex.image = nimg
    ntex.image.colorspace_settings.name = 'Non-Color'
    nmap = nt.nodes.new("ShaderNodeNormalMap"); nmap.inputs["Strength"].default_value = 1.0
    nt.links.new(nmap.inputs["Color"], ntex.outputs["Color"])
    nt.links.new(bsdf.inputs["Normal"], nmap.outputs["Normal"])
    # A touch of metal + mid roughness so the relief catches the key light (matte industrial).
    bsdf.inputs["Roughness"].default_value = 0.62
    bsdf.inputs["Metallic"].default_value = 0.18
    mesh.data.materials.clear(); mesh.data.materials.append(mat)


def decimate(mesh, target):
    before = tri_count(mesh)
    r = min(1.0, float(target) / max(before, 1))
    m = mesh.modifiers.new("dec", 'DECIMATE'); m.decimate_type = 'COLLAPSE'; m.ratio = r
    bpy.context.view_layer.objects.active = mesh
    bpy.ops.object.modifier_apply(modifier=m.name)
    return before, tri_count(mesh)


def export_glb(out, objs, anim):
    bpy.ops.object.select_all(action='DESELECT')
    for o in objs:
        o.select_set(True)
    kw = dict(filepath=out, export_format='GLB', use_selection=True, export_apply=False)
    if anim:
        kw.update(export_animations=True, export_animation_mode='ACTIONS')
    bpy.ops.export_scene.gltf(**kw)


for e in entries:
    mode, _, src = e.partition(":")
    name = os.path.splitext(os.path.basename(src))[0]
    png = os.path.splitext(src)[0] + ".png"
    out = os.path.join(outdir, name + ".glb")
    bpy.ops.wm.read_homefile(use_empty=True)

    if mode == "rig":
        bpy.ops.import_scene.gltf(filepath=rig_src)
        arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
        rig_meshes = [o for o in bpy.data.objects if o.type == 'MESH']
        arm.data.pose_position = 'REST'; bpy.context.view_layer.update()
        rmn, rmx = bbox(rig_meshes); rdim = rmx - rmn

    before_objs = set(bpy.data.objects)
    bpy.ops.wm.obj_import(filepath=src)
    nm = next(o for o in bpy.data.objects if o.type == 'MESH' and o not in before_objs)
    assign_texture(nm, png)
    b, a = decimate(nm, TARGET)

    if mode == "rig":
        nmn, nmx = bbox([nm]); ndim = nmx - nmn
        s = rdim.z / max(ndim.z, 1e-4)
        nm.scale = (nm.scale.x * s, nm.scale.y * s, nm.scale.z * s)
        bpy.context.view_layer.update()
        nmn, nmx = bbox([nm]); rc = (rmn + rmx) * 0.5; nc = (nmn + nmx) * 0.5
        nm.location += Vector((rc.x - nc.x, rc.y - nc.y, rmn.z - nmn.z))
        bpy.ops.object.select_all(action='DESELECT'); nm.select_set(True)
        bpy.context.view_layer.objects.active = nm
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        bpy.ops.object.select_all(action='DESELECT')
        nm.select_set(True); arm.select_set(True); bpy.context.view_layer.objects.active = arm
        try:
            bpy.ops.object.parent_set(type='ARMATURE_AUTO')
        except Exception:
            bpy.ops.object.parent_set(type='ARMATURE_ENVELOPE')
        for o in rig_meshes:
            bpy.data.objects.remove(o, do_unlink=True)
        arm.data.pose_position = 'POSE'
        export_glb(out, [arm, nm], anim=True)
        print("RIG    %-40s tris %d->%d clips=%d -> %s" % (name, b, a, len(bpy.data.actions), os.path.basename(out)))

    elif mode == "hover":
        # 1-bone armature + rigid bind + a subtle Idle bob, so the loader (needs >=1 clip) accepts it
        # and the engine hovers it procedurally (flyer behaviour).
        nmn, nmx = bbox([nm]); h = max(nmx.z - nmn.z, 0.5)
        cx = (nmn.x + nmx.x) * 0.5; cy = (nmn.y + nmx.y) * 0.5
        adata = bpy.data.armatures.new("HoverArm"); aobj = bpy.data.objects.new("HoverArm", adata)
        bpy.context.scene.collection.objects.link(aobj)
        bpy.context.view_layer.objects.active = aobj; aobj.select_set(True)
        bpy.ops.object.mode_set(mode='EDIT')
        bone = adata.edit_bones.new("Root"); bone.head = (cx, cy, nmn.z); bone.tail = (cx, cy, nmn.z + h)
        bpy.ops.object.mode_set(mode='OBJECT')
        bpy.ops.object.select_all(action='DESELECT')
        nm.select_set(True); aobj.select_set(True); bpy.context.view_layer.objects.active = aobj
        bpy.ops.object.parent_set(type='ARMATURE_AUTO')   # 1 bone -> rigid bind
        aobj.animation_data_create(); act = bpy.data.actions.new("Idle"); aobj.animation_data.action = act
        pb = aobj.pose.bones["Root"]
        for fr, off in [(1, 0.0), (30, 0.06), (60, 0.0)]:
            pb.location = (0.0, off, 0.0)   # +Y is along the bone (up)
            pb.keyframe_insert("location", frame=fr)
        export_glb(out, [aobj, nm], anim=True)
        print("HOVER  %-40s tris %d->%d -> %s" % (name, b, a, os.path.basename(out)))

    else:
        export_glb(out, [nm], anim=False)
        print("STATIC %-40s tris %d->%d -> %s" % (name, b, a, os.path.basename(out)))
print("DONE")
