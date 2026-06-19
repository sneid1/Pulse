"""Author the three PULSE enemy drones in Blender and export them as OBJ+MTL
into assets/models/. Mirrors the conventions of build_pulse_weapon.py so the
in-engine OBJ loader (Kd -> colour, Ke -> emissive) picks them up directly.

Authoring convention (matches the weapon):
  +Z = up,  -X = forward / the face that looks at the player,  +/-Y = sideways.
Each drone is authored centred on the origin, roughly within a 1-unit radius so
the engine's per-kind scale lands it at the right size.
"""

import math
from pathlib import Path

import bpy
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = ROOT / "assets" / "blender"
MODEL_DIR = ROOT / "assets" / "models"
RENDER_DIR = ROOT / "build" / "blender"
for d in (OUT_DIR, MODEL_DIR, RENDER_DIR):
    d.mkdir(parents=True, exist_ok=True)


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    for block in (bpy.data.meshes, bpy.data.materials):
        for item in list(block):
            if item.users == 0:
                block.remove(item)


def material(name, color, metallic=0.4, roughness=0.5, emission=None, strength=0.0):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    if bsdf:
        bsdf.inputs["Base Color"].default_value = (color[0], color[1], color[2], 1.0)
        bsdf.inputs["Metallic"].default_value = metallic
        bsdf.inputs["Roughness"].default_value = roughness
        if emission and "Emission Color" in bsdf.inputs:
            bsdf.inputs["Emission Color"].default_value = (emission[0], emission[1], emission[2], 1.0)
            bsdf.inputs["Emission Strength"].default_value = strength
    return mat


def assign(obj, mat):
    obj.data.materials.append(mat)
    return obj


def bevel(obj, amount=0.02, segments=2):
    mod = obj.modifiers.new("bevel", "BEVEL")
    mod.width = amount
    mod.segments = segments
    mod.affect = "EDGES"
    obj.modifiers.new("wn", "WEIGHTED_NORMAL")


def subsurf(obj, levels=1):
    mod = obj.modifiers.new("subsurf", "SUBSURF")
    mod.levels = levels
    mod.render_levels = levels
    bpy.ops.object.shade_smooth()


def cube(name, loc, scale, mat, bevel_w=0.03, segs=2, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_cube_add(size=1, location=loc, rotation=rot)
    o = bpy.context.object
    o.name = name
    o.dimensions = scale
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    assign(o, mat)
    bevel(o, bevel_w, segs)
    return o


def sphere(name, loc, scale, mat, smooth=True, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=28, ring_count=16, radius=1, location=loc, rotation=rot)
    o = bpy.context.object
    o.name = name
    o.scale = scale
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    assign(o, mat)
    if smooth:
        bpy.ops.object.shade_smooth()
        o.modifiers.new("wn", "WEIGHTED_NORMAL")
    return o


def cone(name, loc, r1, r2, depth, mat, rot=(0, 0, 0), verts=20):
    bpy.ops.mesh.primitive_cone_add(vertices=verts, radius1=r1, radius2=r2, depth=depth, location=loc, rotation=rot)
    o = bpy.context.object
    o.name = name
    assign(o, mat)
    bpy.ops.object.shade_smooth()
    o.modifiers.new("wn", "WEIGHTED_NORMAL")
    return o


def cyl_between(name, a, b, radius, mat, verts=18):
    a = Vector(a)
    b = Vector(b)
    mid = (a + b) * 0.5
    d = b - a
    length = max(d.length, 0.001)
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts, radius=radius, depth=length, location=mid)
    o = bpy.context.object
    o.name = name
    o.rotation_euler = d.to_track_quat("Z", "Y").to_euler()
    assign(o, mat)
    bpy.ops.object.shade_smooth()
    o.modifiers.new("wn", "WEIGHTED_NORMAL")
    return o


# ----------------------------------------------------------------------------
# Rusher: a low, wide crimson manta. Forward (toward player) is -X.
# ----------------------------------------------------------------------------
def build_rusher():
    armor = material("rusher_armor", (0.62, 0.10, 0.08), 0.55, 0.42)
    dark = material("rusher_plate", (0.34, 0.05, 0.045), 0.5, 0.5)
    eye = material("rusher_eye", (1.0, 0.45, 0.12), 0.0, 0.3, (1.0, 0.42, 0.10), 6.0)
    glow = material("rusher_exhaust", (1.0, 0.5, 0.15), 0.0, 0.3, (1.0, 0.45, 0.12), 4.0)

    sphere("rusher body", (0.05, 0, 0), (0.95, 0.6, 0.2), armor)
    cone("rusher fang", (-0.95, 0, 0.0), 0.16, 0.0, 0.55, dark, rot=(0, math.radians(-90), 0))
    sphere("rusher eye", (-0.5, 0, 0.06), (0.16, 0.18, 0.12), eye)
    # Swept-back delta wings.
    for side in (-1, 1):
        cube("rusher wing %d" % side, (0.18, side * 0.55, 0.02), (0.95, 0.7, 0.06), armor,
             bevel_w=0.02, segs=1, rot=(0, math.radians(6 * side), math.radians(side * -26)))
    # Raised tail fin.
    cube("rusher tail", (0.78, 0, 0.26), (0.34, 0.06, 0.5), dark, bevel_w=0.02, segs=1, rot=(0, math.radians(28), 0))
    # Twin engine glows at the back.
    for side in (-1, 1):
        sphere("rusher exhaust %d" % side, (0.92, side * 0.2, 0.0), (0.07, 0.07, 0.07), glow)


# ----------------------------------------------------------------------------
# Ranged: a tall purple sentry on three splayed legs. Forward is -X.
# ----------------------------------------------------------------------------
def build_ranged():
    armor = material("ranged_armor", (0.45, 0.24, 0.78), 0.5, 0.42)
    dark = material("ranged_strut", (0.18, 0.10, 0.34), 0.6, 0.5)
    eye = material("ranged_eye", (0.45, 0.7, 1.0), 0.0, 0.2, (0.35, 0.75, 1.0), 7.0)

    sphere("ranged pod", (0, 0, 0.1), (0.52, 0.52, 0.78), armor)
    sphere("ranged crest", (0.05, 0, 0.72), (0.4, 0.4, 0.26), dark)
    # Big forward lens eye (faces -X).
    sphere("ranged eye", (-0.48, 0, 0.2), (0.12, 0.34, 0.34), eye)
    cube("ranged eye ring", (-0.44, 0, 0.2), (0.1, 0.46, 0.46), dark, bevel_w=0.03, segs=2)
    # Three splayed legs.
    feet = [(-0.55, 0.0, -1.0), (0.45, 0.52, -1.0), (0.45, -0.52, -1.0)]
    for i, f in enumerate(feet):
        cyl_between("ranged leg %d" % i, (f[0] * 0.2, f[1] * 0.25, -0.3), f, 0.07, dark)
        sphere("ranged foot %d" % i, f, (0.1, 0.1, 0.06), dark)
    # Antenna with an emissive tip.
    cyl_between("ranged antenna", (0.1, 0, 0.85), (0.1, 0, 1.25), 0.025, dark)
    sphere("ranged antenna tip", (0.1, 0, 1.28), (0.06, 0.06, 0.06), eye)


# ----------------------------------------------------------------------------
# Tank: a hulking amber-bronze brute with a sloped glacis and a red visor.
# Forward (the sloped, visored face) is -X.
# ----------------------------------------------------------------------------
def build_tank():
    armor = material("tank_armor", (0.70, 0.44, 0.16), 0.7, 0.4)
    dark = material("tank_plate", (0.34, 0.21, 0.08), 0.6, 0.5)
    tread = material("tank_tread", (0.07, 0.06, 0.05), 0.2, 0.7)
    visor = material("tank_visor", (1.0, 0.32, 0.18), 0.0, 0.3, (1.0, 0.28, 0.14), 6.5)

    cube("tank chassis", (0.1, 0, 0.05), (0.95, 0.98, 0.72), armor, bevel_w=0.05, segs=2)
    # Sloped front glacis (a wedge angled forward-down).
    cube("tank glacis", (-0.55, 0, 0.06), (0.45, 0.92, 0.66), armor, bevel_w=0.04, segs=2,
         rot=(0, math.radians(34), 0))
    # Wide glowing visor slit across the glacis.
    cube("tank visor", (-0.78, 0, 0.16), (0.08, 0.74, 0.16), visor, bevel_w=0.02, segs=1,
         rot=(0, math.radians(34), 0))
    # Shoulder pauldrons.
    for side in (-1, 1):
        cube("tank pauldron %d" % side, (0.12, side * 0.56, 0.42), (0.6, 0.36, 0.34), dark,
             bevel_w=0.04, segs=2, rot=(math.radians(side * -8), 0, 0))
    # Heavy treads/legs along each side.
    for side in (-1, 1):
        cube("tank tread %d" % side, (0.05, side * 0.6, -0.42), (1.15, 0.26, 0.36), tread, bevel_w=0.04, segs=2)
    # Top hatch + exhaust stacks.
    cube("tank hatch", (0.3, 0, 0.46), (0.4, 0.5, 0.16), dark, bevel_w=0.03, segs=2)
    for side in (-1, 1):
        cyl_between("tank stack %d" % side, (0.5, side * 0.22, 0.42), (0.5, side * 0.22, 0.7), 0.06, tread)


BUILDERS = {
    "rusher": build_rusher,
    "ranged": build_ranged,
    "tank": build_tank,
}


def setup_scene():
    try:
        bpy.context.scene.render.engine = "CYCLES"
        bpy.context.scene.cycles.samples = 64
        bpy.context.scene.cycles.use_denoising = True
    except Exception:
        pass
    bpy.context.scene.render.resolution_x = 900
    bpy.context.scene.render.resolution_y = 900
    bpy.context.scene.view_settings.view_transform = "Filmic"
    bpy.context.scene.view_settings.look = "Medium High Contrast"

    bpy.ops.object.light_add(type="AREA", location=(-3.0, -3.5, 3.5))
    k = bpy.context.object
    k.data.energy = 500
    k.data.size = 5.0
    bpy.ops.object.light_add(type="AREA", location=(3.0, 2.5, 2.0))
    r = bpy.context.object
    r.data.energy = 160
    r.data.color = (0.6, 0.75, 1.0)
    r.data.size = 3.0

    # Camera front-left-high so the -X face (toward the player) is visible.
    bpy.ops.object.camera_add(location=(-3.4, -3.2, 2.2))
    cam = bpy.context.object
    bpy.context.scene.camera = cam
    d = Vector((0, 0, 0.05)) - cam.location
    cam.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    cam.data.lens = 50


def export_kind(kind):
    obj_path = MODEL_DIR / ("pulse_enemy_%s.obj" % kind)
    bpy.ops.object.select_all(action="DESELECT")
    for o in bpy.context.scene.objects:
        if o.type == "MESH":
            o.select_set(True)
    try:
        bpy.ops.wm.obj_export(filepath=str(obj_path), export_selected_objects=True,
                              export_materials=True, export_triangulated_mesh=True)
    except Exception:
        bpy.ops.export_scene.obj(filepath=str(obj_path), use_selection=True, use_materials=True)
    print("PULSE_ENEMY_OBJ_%s=%s" % (kind.upper(), obj_path))


def render_kind(kind):
    path = RENDER_DIR / ("pulse_enemy_%s.png" % kind)
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    print("PULSE_ENEMY_RENDER_%s=%s" % (kind.upper(), path))


def main():
    for kind, builder in BUILDERS.items():
        clear_scene()
        builder()
        setup_scene()
        export_kind(kind)
        render_kind(kind)


if __name__ == "__main__":
    main()
