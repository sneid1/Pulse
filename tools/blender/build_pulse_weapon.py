import math
import os
from pathlib import Path

import bpy
from mathutils import Vector


ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = ROOT / "assets" / "blender"
MODEL_DIR = ROOT / "assets" / "models"
RENDER_DIR = ROOT / "build" / "blender"
OUT_DIR.mkdir(parents=True, exist_ok=True)
MODEL_DIR.mkdir(parents=True, exist_ok=True)
RENDER_DIR.mkdir(parents=True, exist_ok=True)


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def material(name, color, metallic=0.0, roughness=0.55, emission=None, strength=0.0):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    if bsdf:
        bsdf.inputs["Base Color"].default_value = color
        bsdf.inputs["Metallic"].default_value = metallic
        bsdf.inputs["Roughness"].default_value = roughness
        if emission and "Emission Color" in bsdf.inputs:
            bsdf.inputs["Emission Color"].default_value = emission
            bsdf.inputs["Emission Strength"].default_value = strength
    return mat


MAT_GUNMETAL = material("brushed gunmetal", (0.28, 0.30, 0.31, 1), 0.65, 0.36)
MAT_DARK = material("matte black polymer", (0.020, 0.023, 0.026, 1), 0.0, 0.78)
MAT_DARK2 = material("soft charcoal panels", (0.055, 0.060, 0.066, 1), 0.15, 0.66)
MAT_EDGE = material("worn bevel highlights", (0.64, 0.66, 0.64, 1), 0.72, 0.31)
MAT_GLASS = material("smoked cyan lens", (0.08, 0.30, 0.36, 0.72), 0.0, 0.10, (0.02, 0.55, 0.74, 1), 0.45)
MAT_EMIT = material("cyan weapon electronics", (0.25, 0.95, 1.0, 1), 0.0, 0.22, (0.10, 0.95, 1.0, 1), 1.8)
MAT_MARK = material("painted white markings", (0.88, 0.91, 0.90, 1), 0.0, 0.42)
MAT_GLOVE = material("blue grey tactical gloves", (0.320, 0.380, 0.510, 1), 0.0, 0.70)
MAT_GLOVE_PAD = material("dark rubber glove pads", (0.115, 0.135, 0.175, 1), 0.0, 0.66)
MAT_SLEEVE = material("dark operator sleeves", (0.070, 0.092, 0.135, 1), 0.0, 0.74)
MAT_WOOD = material("ak wood furniture", (0.34, 0.165, 0.065, 1), 0.0, 0.5)
MAT_WOOD_DARK = material("ak dark wood", (0.20, 0.092, 0.04, 1), 0.0, 0.55)
MAT_BLUED = material("blued gun steel", (0.05, 0.055, 0.062, 1), 0.85, 0.33)
MAT_MAG = material("magazine steel", (0.085, 0.092, 0.105, 1), 0.7, 0.44)


def assign(obj, mat):
    obj.data.materials.append(mat)
    return obj


def bevel(obj, amount=0.025, segments=2, weighted=True):
    mod = obj.modifiers.new("wide bevels", "BEVEL")
    mod.width = amount
    mod.segments = segments
    mod.affect = "EDGES"
    if weighted:
        obj.modifiers.new("weighted normals", "WEIGHTED_NORMAL")
    return obj


def cube_obj(name, loc, scale, mat, bevel_width=0.02, bevel_segments=2, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_cube_add(size=1, location=loc, rotation=rot)
    obj = bpy.context.object
    obj.name = name
    obj.dimensions = scale
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    assign(obj, mat)
    bevel(obj, bevel_width, bevel_segments)
    return obj


def cyl_obj(name, loc, radius, depth, mat, vertices=48, rot=(0, math.pi / 2, 0), bevel_width=0.0):
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius, depth=depth, location=loc, rotation=rot)
    obj = bpy.context.object
    obj.name = name
    assign(obj, mat)
    bpy.ops.object.shade_smooth()
    obj.modifiers.new("weighted normals", "WEIGHTED_NORMAL")
    if bevel_width > 0:
        bevel(obj, bevel_width, 2)
    return obj


def ellipsoid_obj(name, loc, scale, mat, segments=32, rings=12, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=segments, ring_count=rings, radius=1, location=loc, rotation=rot)
    obj = bpy.context.object
    obj.name = name
    obj.scale = scale
    assign(obj, mat)
    bpy.ops.object.shade_smooth()
    obj.modifiers.new("weighted normals", "WEIGHTED_NORMAL")
    return obj


def cyl_between(name, a, b, radius, mat, vertices=24, bevel_width=0.0):
    a = Vector(a)
    b = Vector(b)
    mid = (a + b) * 0.5
    direction = b - a
    length = max(direction.length, 0.001)
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius, depth=length, location=mid)
    obj = bpy.context.object
    obj.name = name
    obj.rotation_euler = direction.to_track_quat("Z", "Y").to_euler()
    assign(obj, mat)
    bpy.ops.object.shade_smooth()
    obj.modifiers.new("weighted normals", "WEIGHTED_NORMAL")
    if bevel_width > 0:
        bevel(obj, bevel_width, 1)
    return obj


def bar_between(name, a, b, thickness, mat, bevel_width=0.01):
    a = Vector(a)
    b = Vector(b)
    mid = (a + b) * 0.5
    direction = b - a
    length = direction.length
    obj = cube_obj(name, mid, (length, thickness, thickness), mat, bevel_width, 1)
    quat = direction.to_track_quat("X", "Z")
    obj.rotation_euler = quat.to_euler()
    return obj


def trapezoid_prism(name, x0, x1, y_half0, y_half1, z0, z1, mat):
    verts = [
        (x0, -y_half0, z0), (x0, y_half0, z0), (x0, y_half0, z1), (x0, -y_half0, z1),
        (x1, -y_half1, z0), (x1, y_half1, z0), (x1, y_half1, z1), (x1, -y_half1, z1),
    ]
    faces = [
        (0, 1, 2, 3), (4, 7, 6, 5), (0, 4, 5, 1), (3, 2, 6, 7),
        (1, 5, 6, 2), (0, 3, 7, 4),
    ]
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    assign(obj, mat)
    bevel(obj, 0.025, 2)
    return obj


def pistol_grip_prism(name, y_half, mat):
    # Side profile in the weapon's X/Z plane. Positive X is stockward, so the
    # grip rakes back as it descends like an FPS carbine grip instead of reading
    # as a second magazine.
    verts = [
        (0.50, -y_half, -0.22), (0.78, -y_half, -0.22), (0.94, -y_half, -0.87), (0.66, -y_half, -0.92),
        (0.50, y_half, -0.22), (0.78, y_half, -0.22), (0.94, y_half, -0.87), (0.66, y_half, -0.92),
    ]
    faces = [
        (0, 1, 2, 3), (4, 7, 6, 5), (0, 4, 5, 1), (3, 2, 6, 7),
        (1, 5, 6, 2), (0, 3, 7, 4),
    ]
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    assign(obj, mat)
    bevel(obj, 0.040, 5)
    return obj


def add_text(name, text, loc, size, mat, rot=(math.radians(80), 0, math.radians(90))):
    bpy.ops.object.text_add(location=loc, rotation=rot)
    obj = bpy.context.object
    obj.name = name
    obj.data.body = text
    obj.data.align_x = "CENTER"
    obj.data.align_y = "CENTER"
    obj.data.size = size
    obj.data.extrude = 0.002
    assign(obj, mat)
    return obj


def look_at(obj, target):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def build_weapon():
    # Coordinate system: X is muzzle-to-stock, Y is width, Z is vertical.
    cube_obj("monolithic lower receiver", (0.06, 0, 0.05), (1.18, 0.22, 0.34), MAT_GUNMETAL, 0.035, 3)
    cube_obj("stepped upper receiver", (-0.08, 0, 0.29), (1.38, 0.20, 0.20), MAT_GUNMETAL, 0.025, 3)
    cube_obj("ejection port black inset", (-0.12, -0.111, 0.28), (0.34, 0.012, 0.115), MAT_DARK, 0.006, 1)
    cube_obj("bolt carrier highlight", (-0.17, -0.120, 0.31), (0.28, 0.010, 0.065), MAT_EDGE, 0.004, 1)

    cube_obj("angled magwell", (0.16, 0, -0.19), (0.34, 0.23, 0.34), MAT_GUNMETAL, 0.035, 3, rot=(0, math.radians(0), math.radians(-3)))
    mag = trapezoid_prism("curved magazine body", 0.02, 0.34, 0.105, 0.135, -0.93, -0.20, MAT_DARK2)
    mag.rotation_euler[1] = math.radians(-7)
    mag.location.x += 0.05

    cube_obj("trigger well shadow", (0.47, -0.137, -0.255), (0.34, 0.018, 0.185), MAT_DARK, 0.010, 1)
    cube_obj("trigger top housing", (0.51, -0.136, -0.180), (0.34, 0.030, 0.055), MAT_GUNMETAL, 0.012, 1)
    for y in (-0.153, 0.153):
        bar_between(f"trigger guard front side {y}", (0.315, y, -0.185), (0.300, y, -0.355), 0.030, MAT_GUNMETAL, 0.007)
        bar_between(f"trigger guard lower side {y}", (0.300, y, -0.355), (0.590, y, -0.360), 0.030, MAT_GUNMETAL, 0.007)
        bar_between(f"trigger guard rear side {y}", (0.590, y, -0.360), (0.625, y, -0.215), 0.030, MAT_GUNMETAL, 0.007)
    bar_between("trigger guard front bridge", (0.315, -0.153, -0.185), (0.315, 0.153, -0.185), 0.020, MAT_GUNMETAL, 0.005)
    bar_between("trigger guard lower bridge", (0.455, -0.153, -0.360), (0.455, 0.153, -0.360), 0.020, MAT_GUNMETAL, 0.005)
    cyl_between("curved trigger blade upper", (0.465, -0.174, -0.215), (0.430, -0.174, -0.300), 0.020, MAT_DARK, 20, 0.004)
    cyl_between("curved trigger blade lower", (0.430, -0.174, -0.300), (0.455, -0.174, -0.365), 0.017, MAT_DARK, 20, 0.004)
    # Presentation-side guard: the first-person hand sits on this side too, so
    # the trigger silhouette needs a raised outer rail instead of hiding in the
    # middle of the weapon.
    y_vis = -0.515
    bar_between("visible trigger guard front rail", (0.320, y_vis, -0.178), (0.300, y_vis, -0.365), 0.034, MAT_EDGE, 0.008)
    bar_between("visible trigger guard lower rail", (0.300, y_vis, -0.365), (0.600, y_vis, -0.370), 0.034, MAT_EDGE, 0.008)
    bar_between("visible trigger guard rear rail", (0.600, y_vis, -0.370), (0.635, y_vis, -0.212), 0.034, MAT_EDGE, 0.008)
    cyl_between("visible trigger blade upper", (0.470, y_vis - 0.012, -0.210), (0.430, y_vis - 0.012, -0.300), 0.022, MAT_DARK, 24, 0.004)
    cyl_between("visible trigger blade lower", (0.430, y_vis - 0.012, -0.300), (0.458, y_vis - 0.012, -0.365), 0.018, MAT_DARK, 24, 0.004)

    pistol_grip_prism("raked rubber pistol grip", 0.105, MAT_DARK)
    cube_obj("grip rear backstrap", (0.875, -0.123, -0.565), (0.040, 0.026, 0.575), MAT_DARK2, 0.012, 1, rot=(0, math.radians(-14), 0))
    cube_obj("grip front groove", (0.595, -0.124, -0.535), (0.036, 0.026, 0.470), MAT_GUNMETAL, 0.006, 1, rot=(0, math.radians(-14), 0))
    for i in range(5):
        cube_obj(f"grip rib {i}", (0.635 + i * 0.046, -0.132, -0.585 - i * 0.030), (0.026, 0.022, 0.315), MAT_GUNMETAL, 0.005, 1, rot=(0, math.radians(-14), 0))

    # Handguard and rails.
    cube_obj("faceted freefloat handguard", (-1.02, 0, 0.19), (1.18, 0.28, 0.30), MAT_DARK2, 0.040, 4)
    cube_obj("handguard lower bevel plane", (-1.02, 0, 0.02), (1.08, 0.24, 0.08), MAT_DARK, 0.025, 2)
    cube_obj("continuous top picatinny rail", (-0.70, 0, 0.49), (1.72, 0.16, 0.07), MAT_DARK, 0.012, 1)
    for i in range(16):
        cube_obj(f"rail tooth {i:02d}", (-1.50 + i * 0.105, 0, 0.56), (0.050, 0.19, 0.055), MAT_DARK, 0.006, 1)
    for side in (-1, 1):
        for i in range(7):
            cube_obj(f"handguard vent {side} {i}", (-1.40 + i * 0.16, side * 0.146, 0.20), (0.085, 0.014, 0.13), MAT_EDGE, 0.006, 1, rot=(0, 0, math.radians(12)))
        cube_obj(f"side accessory rail {side}", (-0.95, side * 0.165, 0.33), (0.88, 0.035, 0.055), MAT_DARK, 0.008, 1)
    cube_obj("underbarrel support rail", (-0.97, 0, -0.02), (0.80, 0.14, 0.055), MAT_DARK, 0.010, 1)

    # Barrel group.
    cyl_obj("inner barrel", (-1.74, 0, 0.19), 0.045, 1.18, MAT_GUNMETAL, 48, bevel_width=0.004)
    cyl_obj("gas block", (-1.34, 0, 0.19), 0.082, 0.15, MAT_DARK2, 48, bevel_width=0.012)
    cyl_obj("muzzle brake body", (-2.34, 0, 0.19), 0.080, 0.24, MAT_DARK, 48, bevel_width=0.012)
    for i in range(3):
        cube_obj(f"muzzle side port L{i}", (-2.36 + i * 0.055, -0.083, 0.214), (0.030, 0.010, 0.040), MAT_EDGE, 0.002, 1)
        cube_obj(f"muzzle side port R{i}", (-2.36 + i * 0.055, 0.083, 0.214), (0.030, 0.010, 0.040), MAT_EDGE, 0.002, 1)

    # Sights and optic.
    cube_obj("folding front sight base", (-1.55, 0, 0.66), (0.10, 0.14, 0.17), MAT_GUNMETAL, 0.012, 2)
    cube_obj("front sight post", (-1.58, 0, 0.80), (0.025, 0.035, 0.18), MAT_EDGE, 0.004, 1)
    cube_obj("rear sight block", (0.23, 0, 0.67), (0.18, 0.15, 0.14), MAT_GUNMETAL, 0.014, 2)
    cube_obj("holographic sight hood", (-0.28, 0, 0.78), (0.36, 0.24, 0.22), MAT_DARK, 0.025, 3)
    cube_obj("holo glass front", (-0.42, -0.002, 0.80), (0.026, 0.205, 0.145), MAT_GLASS, 0.008, 2)
    cube_obj("holo glass rear", (-0.16, -0.002, 0.80), (0.026, 0.205, 0.145), MAT_GLASS, 0.008, 2)
    cube_obj("optic cyan reticle strip", (-0.43, -0.109, 0.80), (0.020, 0.010, 0.08), MAT_EMIT, 0.003, 1)

    # Stock.
    cyl_obj("buffer tube", (1.18, 0, 0.26), 0.062, 0.95, MAT_DARK, 48, bevel_width=0.004)
    cube_obj("stock cheek rest", (1.70, 0, 0.39), (0.88, 0.23, 0.22), MAT_DARK, 0.045, 5)
    bar_between("stock upper strut", (1.30, -0.095, 0.22), (2.00, -0.095, -0.20), 0.055, MAT_DARK2, 0.012)
    bar_between("stock lower strut", (1.33, 0.095, 0.17), (2.00, 0.095, -0.23), 0.055, MAT_DARK2, 0.012)
    cube_obj("stock shoulder pad", (2.15, 0, 0.10), (0.18, 0.30, 0.82), MAT_DARK2, 0.065, 5)
    cube_obj("rubber butt pad inset", (2.23, 0, 0.10), (0.045, 0.32, 0.72), MAT_DARK, 0.045, 4)

    # Small authored details that sell scale.
    for x in (-0.28, -0.18, -0.08):
        cyl_obj(f"receiver pin {x}", (x, -0.116, 0.08), 0.018, 0.011, MAT_EDGE, 24, rot=(math.pi / 2, 0, 0), bevel_width=0.002)
    cube_obj("selector switch", (0.37, -0.124, 0.11), (0.10, 0.018, 0.032), MAT_EDGE, 0.006, 1, rot=(0, 0, math.radians(23)))
    cube_obj("cyan side power cell", (-0.07, -0.126, 0.15), (0.32, 0.016, 0.060), MAT_EMIT, 0.007, 1)
    cube_obj("white chamber marking", (-0.20, -0.129, 0.25), (0.22, 0.012, 0.045), MAT_MARK, 0.004, 1)
    add_text("serial text", "PULSE-01", (0.18, -0.131, -0.03), 0.075, MAT_MARK)


def build_hands():
    # Authored in the same coordinate system as the gun: X muzzle-to-stock,
    # Y width, Z vertical. Negative Y is the camera/near side after OBJ export,
    # so the grip geometry must sit there instead of being buried inside the gun.
    # Right/fire hand: palm wraps the near face of the pistol grip, with an
    # obvious trigger finger and a short sleeve entering from the bottom-right.
    ellipsoid_obj("right sleeve cuff", (0.92, -0.42, -0.80), (0.170, 0.105, 0.120), MAT_SLEEVE, rot=(math.radians(5), math.radians(-24), math.radians(10)))
    ellipsoid_obj("right wrist cuff", (0.82, -0.40, -0.68), (0.145, 0.088, 0.115), MAT_SLEEVE, rot=(math.radians(4), math.radians(-20), math.radians(8)))
    cyl_between("right glove wrist bridge", (0.82, -0.395, -0.67), (0.67, -0.370, -0.54), 0.072, MAT_GLOVE, 24)
    ellipsoid_obj("right glove palm", (0.65, -0.360, -0.47), (0.175, 0.105, 0.215), MAT_GLOVE, rot=(math.radians(4), math.radians(-14), math.radians(-6)))
    cube_obj("right glove back armor", (0.64, -0.455, -0.47), (0.235, 0.030, 0.175), MAT_GLOVE_PAD, 0.010, 1, rot=(0, math.radians(-12), 0))
    ellipsoid_obj("right thumb pad", (0.50, -0.455, -0.38), (0.052, 0.033, 0.125), MAT_GLOVE_PAD, 20, 8, rot=(math.radians(12), math.radians(-28), math.radians(-28)))
    cyl_between("right thumb wrap", (0.54, -0.445, -0.39), (0.47, -0.430, -0.27), 0.024, MAT_GLOVE, 16)
    for i, x in enumerate((0.55, 0.61, 0.67, 0.73)):
        z0 = -0.31 - i * 0.006
        z1 = -0.61 - i * 0.006
        cyl_between(f"right curled finger {i}", (x, -0.468, z0), (x + 0.014, -0.438, z1), 0.024, MAT_GLOVE, 18)
        ellipsoid_obj(f"right knuckle pad {i}", (x, -0.492, z0 + 0.012), (0.038, 0.020, 0.027), MAT_GLOVE_PAD, 16, 6)
    cyl_between("right trigger finger base", (0.49, -0.468, -0.28), (0.39, -0.452, -0.245), 0.023, MAT_GLOVE, 18)
    cyl_between("right trigger finger tip", (0.39, -0.452, -0.245), (0.42, -0.438, -0.345), 0.020, MAT_GLOVE, 18)

    # Left/support hand: visible under the freefloat handguard, fingers curled up
    # around the near lower edge instead of hiding behind the rail.
    ellipsoid_obj("left sleeve cuff", (-0.50, -0.43, -0.43), (0.155, 0.100, 0.110), MAT_SLEEVE, rot=(math.radians(-4), math.radians(18), math.radians(-12)))
    ellipsoid_obj("left wrist cuff", (-0.62, -0.405, -0.31), (0.135, 0.086, 0.108), MAT_SLEEVE, rot=(math.radians(-4), math.radians(16), math.radians(-10)))
    cyl_between("left glove wrist bridge", (-0.62, -0.397, -0.30), (-0.78, -0.372, -0.18), 0.066, MAT_GLOVE, 24)
    ellipsoid_obj("left glove palm", (-0.92, -0.365, -0.13), (0.195, 0.105, 0.120), MAT_GLOVE, rot=(math.radians(3), math.radians(10), math.radians(5)))
    cube_obj("left glove back armor", (-0.92, -0.458, -0.13), (0.265, 0.030, 0.105), MAT_GLOVE_PAD, 0.010, 1, rot=(0, math.radians(8), 0))
    ellipsoid_obj("left thumb brace", (-0.72, -0.455, -0.005), (0.046, 0.030, 0.118), MAT_GLOVE_PAD, 20, 8, rot=(math.radians(18), math.radians(62), math.radians(0)))
    for i, x in enumerate((-1.04, -0.95, -0.86, -0.77)):
        cyl_between(f"left support finger {i}", (x, -0.472, -0.12), (x + 0.060, -0.440, 0.020), 0.024, MAT_GLOVE, 18)
        ellipsoid_obj(f"left knuckle pad {i}", (x + 0.010, -0.496, -0.064), (0.038, 0.020, 0.027), MAT_GLOVE_PAD, 16, 6)


def build_weapon_overhaul():
    # New first-person carbine silhouette. It is deliberately built around a
    # side-readable lower receiver: open trigger guard, clear trigger blade,
    # raked pistol grip, and a separate magazine in front of the grip.
    cube_obj("v2 upper receiver slab", (0.18, 0, 0.27), (1.18, 0.29, 0.24), MAT_GUNMETAL, 0.035, 4)
    cube_obj("v2 lower receiver body", (0.28, 0, 0.02), (0.84, 0.31, 0.33), MAT_GUNMETAL, 0.034, 4)
    cube_obj("v2 rear receiver shoulder", (0.83, 0, 0.12), (0.20, 0.32, 0.45), MAT_GUNMETAL, 0.032, 4)
    cube_obj("v2 ejection port dark recess", (0.12, -0.166, 0.28), (0.34, 0.018, 0.105), MAT_DARK, 0.006, 1)
    cube_obj("v2 bolt highlight", (0.10, -0.180, 0.31), (0.26, 0.010, 0.048), MAT_EDGE, 0.004, 1)
    cube_obj("v2 mag release paddle", (0.45, -0.176, 0.03), (0.095, 0.014, 0.052), MAT_EDGE, 0.004, 1)
    cube_obj("v2 selector switch", (0.62, -0.178, 0.13), (0.110, 0.016, 0.034), MAT_EDGE, 0.005, 1, rot=(0, 0, math.radians(24)))

    # Forward assembly.
    cube_obj("v2 octagonal handguard core", (-0.82, 0, 0.14), (1.18, 0.34, 0.34), MAT_DARK2, 0.050, 5)
    cube_obj("v2 handguard lower flat", (-0.82, 0, -0.07), (1.05, 0.25, 0.090), MAT_DARK, 0.020, 2)
    cube_obj("v2 continuous top rail", (-0.36, 0, 0.52), (1.74, 0.18, 0.070), MAT_DARK, 0.011, 1)
    for i in range(17):
        cube_obj(f"v2 rail tooth {i:02d}", (-1.18 + i * 0.100, 0, 0.59), (0.048, 0.215, 0.052), MAT_DARK, 0.005, 1)
    for i in range(7):
        cube_obj(f"v2 near handguard vent {i}", (-1.20 + i * 0.150, -0.182, 0.13), (0.075, 0.020, 0.145), MAT_EDGE, 0.006, 1, rot=(0, 0, math.radians(-10)))
        cube_obj(f"v2 far handguard vent {i}", (-1.20 + i * 0.150, 0.182, 0.13), (0.075, 0.020, 0.145), MAT_EDGE, 0.006, 1, rot=(0, 0, math.radians(-10)))
    cube_obj("v2 angled foregrip stub", (-0.62, -0.02, -0.28), (0.18, 0.18, 0.34), MAT_DARK, 0.030, 3, rot=(0, math.radians(-11), 0))

    cyl_obj("v2 barrel", (-1.72, 0, 0.14), 0.047, 1.26, MAT_GUNMETAL, 48, bevel_width=0.004)
    cyl_obj("v2 suppressor collar", (-1.32, 0, 0.14), 0.076, 0.16, MAT_DARK2, 48, bevel_width=0.010)
    cyl_obj("v2 muzzle brake", (-2.38, 0, 0.14), 0.084, 0.24, MAT_DARK, 48, bevel_width=0.012)
    for i in range(3):
        cube_obj(f"v2 muzzle port near {i}", (-2.41 + i * 0.055, -0.088, 0.165), (0.030, 0.010, 0.036), MAT_EDGE, 0.002, 1)

    # Optic and sights.
    cube_obj("v2 holo base", (-0.05, 0, 0.67), (0.42, 0.23, 0.085), MAT_DARK, 0.015, 2)
    cube_obj("v2 holo hood", (-0.05, 0, 0.82), (0.36, 0.28, 0.24), MAT_DARK, 0.030, 4)
    cube_obj("v2 holo front glass", (-0.19, -0.003, 0.83), (0.026, 0.235, 0.160), MAT_GLASS, 0.008, 2)
    cube_obj("v2 cyan reticle slash", (-0.205, -0.123, 0.83), (0.018, 0.010, 0.090), MAT_EMIT, 0.003, 1)
    cube_obj("v2 front sight block", (-1.43, 0, 0.66), (0.11, 0.16, 0.16), MAT_GUNMETAL, 0.012, 2)
    cube_obj("v2 front sight post", (-1.45, 0, 0.80), (0.025, 0.035, 0.17), MAT_EDGE, 0.004, 1)

    # Magazine is forward of the trigger and distinct from the pistol grip.
    mag = trapezoid_prism("v2 curved magazine", 0.00, 0.34, 0.120, 0.150, -0.84, -0.20, MAT_DARK2)
    mag.rotation_euler[1] = math.radians(-8)
    mag.location.x -= 0.03
    cube_obj("v2 magazine floorplate", (0.19, 0, -0.86), (0.35, 0.33, 0.070), MAT_DARK, 0.014, 2)
    cube_obj("v2 magwell bevel", (0.20, 0, -0.19), (0.42, 0.33, 0.18), MAT_GUNMETAL, 0.025, 3, rot=(0, math.radians(-3), 0))

    # Open, side-readable trigger system.
    cube_obj("v2 trigger pocket shadow", (0.54, -0.182, -0.235), (0.33, 0.020, 0.205), MAT_DARK, 0.009, 1)
    for y in (-0.205, 0.205):
        bar_between(f"v2 trigger guard front {y}", (0.405, y, -0.120), (0.375, y, -0.365), 0.032, MAT_GUNMETAL, 0.008)
        bar_between(f"v2 trigger guard bottom {y}", (0.375, y, -0.365), (0.680, y, -0.368), 0.032, MAT_GUNMETAL, 0.008)
        bar_between(f"v2 trigger guard rear {y}", (0.680, y, -0.368), (0.715, y, -0.155), 0.032, MAT_GUNMETAL, 0.008)
    y_vis = -0.430
    bar_between("v2 raised visible trigger guard front", (0.405, y_vis, -0.120), (0.375, y_vis, -0.370), 0.034, MAT_EDGE, 0.008)
    bar_between("v2 raised visible trigger guard lower", (0.375, y_vis, -0.370), (0.680, y_vis, -0.372), 0.034, MAT_EDGE, 0.008)
    bar_between("v2 raised visible trigger guard rear", (0.680, y_vis, -0.372), (0.715, y_vis, -0.155), 0.034, MAT_EDGE, 0.008)
    cyl_between("v2 black trigger blade upper", (0.545, y_vis - 0.020, -0.175), (0.500, y_vis - 0.020, -0.275), 0.022, MAT_DARK, 28, 0.004)
    cyl_between("v2 black trigger blade lower", (0.500, y_vis - 0.020, -0.275), (0.525, y_vis - 0.020, -0.350), 0.018, MAT_DARK, 28, 0.004)

    pistol_grip_prism("v2 raked pistol grip", 0.115, MAT_DARK)
    cube_obj("v2 grip near side panel", (0.790, -0.128, -0.585), (0.245, 0.024, 0.520), MAT_DARK2, 0.018, 2, rot=(0, math.radians(-14), 0))
    cube_obj("v2 grip rear backstrap", (0.910, -0.132, -0.585), (0.045, 0.026, 0.600), MAT_EDGE, 0.010, 1, rot=(0, math.radians(-14), 0))
    for i in range(5):
        cube_obj(f"v2 grip horizontal rib {i}", (0.735 + i * 0.030, -0.144, -0.455 - i * 0.078), (0.145, 0.020, 0.026), MAT_EDGE, 0.004, 1, rot=(0, math.radians(-14), 0))

    # Stock group.
    cyl_obj("v2 buffer tube", (1.18, 0, 0.23), 0.060, 0.86, MAT_DARK, 48, bevel_width=0.004)
    cube_obj("v2 stock body", (1.72, 0, 0.24), (0.78, 0.28, 0.26), MAT_DARK, 0.050, 5)
    cube_obj("v2 stock buttpad", (2.18, 0, 0.04), (0.16, 0.34, 0.70), MAT_DARK2, 0.055, 5)
    bar_between("v2 stock lower strut near", (1.32, -0.115, 0.11), (2.03, -0.115, -0.25), 0.050, MAT_DARK2, 0.010)
    bar_between("v2 stock lower strut far", (1.32, 0.115, 0.11), (2.03, 0.115, -0.25), 0.050, MAT_DARK2, 0.010)

    add_text("v2 receiver mark", "PULSE", (0.57, -0.187, -0.02), 0.070, MAT_MARK)
    cube_obj("v2 cyan power cell", (0.05, -0.184, 0.055), (0.30, 0.015, 0.058), MAT_EMIT, 0.006, 1)


def build_hands_overhaul():
    # Right hand visibly holds the grip, with the index finger separated into
    # the trigger guard. Keep the palm near the grip instead of hiding the whole
    # trigger group.
    cyl_between("right sleeve forearm", (1.13, -0.48, -0.95), (0.91, -0.40, -0.76), 0.105, MAT_SLEEVE, 24)
    ellipsoid_obj("right wrist cuff", (0.88, -0.39, -0.72), (0.145, 0.085, 0.115), MAT_SLEEVE, rot=(math.radians(4), math.radians(-18), math.radians(8)))
    ellipsoid_obj("right glove palm", (0.80, -0.345, -0.60), (0.165, 0.090, 0.185), MAT_GLOVE, rot=(math.radians(4), math.radians(-15), math.radians(-8)))
    cube_obj("right glove back plate", (0.78, -0.424, -0.60), (0.220, 0.026, 0.150), MAT_GLOVE_PAD, 0.010, 1, rot=(0, math.radians(-13), 0))
    for i, z in enumerate((-0.55, -0.62, -0.69)):
        x = 0.72 + i * 0.025
        cyl_between(f"right gripping finger {i}", (x, -0.430, z), (x + 0.090, -0.388, z - 0.055), 0.022, MAT_GLOVE, 18)
        ellipsoid_obj(f"right knuckle {i}", (x, -0.455, z + 0.012), (0.036, 0.018, 0.026), MAT_GLOVE_PAD, 16, 6)
    cyl_between("right index finger trigger reach", (0.62, -0.438, -0.34), (0.52, -0.456, -0.285), 0.021, MAT_GLOVE, 18)
    cyl_between("right index finger trigger curl", (0.52, -0.456, -0.285), (0.535, -0.465, -0.355), 0.018, MAT_GLOVE, 18)
    cyl_between("right thumb over grip", (0.66, -0.418, -0.47), (0.55, -0.405, -0.36), 0.025, MAT_GLOVE, 18)

    # Left support hand sits under and slightly around the handguard, not as a
    # floating cluster below it.
    cyl_between("left sleeve forearm", (-0.38, -0.50, -0.62), (-0.61, -0.40, -0.32), 0.095, MAT_SLEEVE, 24)
    ellipsoid_obj("left wrist cuff", (-0.63, -0.39, -0.30), (0.130, 0.080, 0.105), MAT_SLEEVE, rot=(math.radians(-4), math.radians(16), math.radians(-8)))
    ellipsoid_obj("left glove palm", (-0.83, -0.350, -0.120), (0.185, 0.095, 0.115), MAT_GLOVE, rot=(math.radians(2), math.radians(8), math.radians(5)))
    cube_obj("left glove back plate", (-0.84, -0.430, -0.115), (0.245, 0.026, 0.105), MAT_GLOVE_PAD, 0.010, 1, rot=(0, math.radians(8), 0))
    for i, x in enumerate((-0.98, -0.90, -0.82, -0.74)):
        cyl_between(f"left curled support finger {i}", (x, -0.438, -0.120), (x + 0.070, -0.392, 0.030), 0.022, MAT_GLOVE, 18)
        ellipsoid_obj(f"left support knuckle {i}", (x + 0.010, -0.462, -0.058), (0.036, 0.018, 0.026), MAT_GLOVE_PAD, 16, 6)
    cyl_between("left thumb along handguard", (-0.70, -0.425, -0.050), (-0.60, -0.388, 0.060), 0.024, MAT_GLOVE, 18)


def build_viewmodel_rig():
    bpy.ops.object.armature_add(location=(0, 0, 0))
    rig = bpy.context.object
    rig.name = "PULSE_viewmodel_armature"
    rig.data.name = "PULSE_viewmodel_skeleton"
    bpy.ops.object.mode_set(mode="EDIT")
    bones = rig.data.edit_bones
    default = bones.get("Bone")
    if default:
        bones.remove(default)

    def bone(name, head, tail, parent=None):
        b = bones.new(name)
        b.head = head
        b.tail = tail
        if parent:
            b.parent = bones[parent]
        return b

    bone("vm_root", (0.0, 0.62, -0.76), (0.0, 0.62, -0.22))
    bone("weapon_root", (-0.10, 0.0, -0.18), (-0.10, 0.0, 0.42), "vm_root")
    bone("right_forearm", (0.88, 0.78, -0.82), (0.76, 0.20, -0.66), "vm_root")
    bone("right_hand", (0.76, 0.20, -0.66), (0.68, -0.12, -0.51), "right_forearm")
    bone("right_thumb", (0.76, -0.07, -0.36), (0.64, -0.03, -0.24), "right_hand")
    for i, z in enumerate((-0.315, -0.405, -0.495, -0.585)):
        bone(f"right_finger_{i}", (0.56, 0.22, z), (0.45, -0.04, z - 0.02), "right_hand")
    bone("right_trigger_finger", (0.56, 0.21, -0.30), (0.41, 0.02, -0.30), "right_hand")
    bone("left_forearm", (-1.34, 0.92, -0.52), (-1.18, 0.48, -0.34), "vm_root")
    bone("left_hand", (-1.18, 0.48, -0.34), (-1.38, 0.45, -0.08), "left_forearm")
    bone("left_thumb", (-1.15, 0.38, -0.02), (-0.98, 0.43, 0.03), "left_hand")
    for i, x in enumerate((-1.57, -1.43, -1.29, -1.15)):
        bone(f"left_finger_{i}", (x, 0.59, -0.01), (x, 0.35, 0.08), "left_hand")
    for i, (x, z) in enumerate(((-1.12, -0.06), (-1.02, -0.03), (-0.92, 0.00), (-0.82, 0.04))):
        bone(f"left_visible_finger_{i}", (x, 0.72, z), (x + 0.06, 0.36, z + 0.10), "left_hand")

    bpy.ops.object.mode_set(mode="OBJECT")
    rig.show_in_front = True
    for obj in bpy.context.scene.objects:
        lower = obj.name.lower()
        if lower.startswith("right "):
            obj["viewmodel_bone"] = "right_hand"
        elif lower.startswith("left "):
            obj["viewmodel_bone"] = "left_hand"
        elif obj.type in {"MESH", "FONT"} and obj.name not in {"matte display plinth", "rear shadow card"}:
            obj["viewmodel_bone"] = "weapon_root"
    return rig


def setup_scene():
    try:
        bpy.context.scene.render.engine = "CYCLES"
        bpy.context.scene.cycles.samples = 96
        bpy.context.scene.cycles.use_denoising = True
    except Exception:
        pass

    bpy.context.scene.render.resolution_x = 1600
    bpy.context.scene.render.resolution_y = 900
    bpy.context.scene.view_settings.view_transform = "Filmic"
    bpy.context.scene.view_settings.look = "Medium High Contrast"
    bpy.context.scene.view_settings.exposure = -0.25
    bpy.context.scene.view_settings.gamma = 1.0

    # Matte ground and a few vertical reference planes, kept subtle so the model stays the subject.
    cube_obj("matte display plinth", (0.0, 0.0, -1.10), (5.4, 1.8, 0.055), MAT_DARK, 0.025, 2)
    cube_obj("rear shadow card", (0.05, -0.76, 0.25), (5.4, 0.045, 2.1), MAT_DARK, 0.020, 1)

    bpy.ops.object.light_add(type="AREA", location=(-2.8, 3.8, 4.0))
    key = bpy.context.object
    key.name = "large softbox key"
    key.data.energy = 560
    key.data.size = 4.6

    bpy.ops.object.light_add(type="AREA", location=(2.6, 2.2, 2.0))
    rim = bpy.context.object
    rim.name = "cool rim strip"
    rim.data.energy = 185
    rim.data.color = (0.55, 0.82, 1.0)
    rim.data.size = 2.5

    bpy.ops.object.camera_add(location=(5.80, 8.20, 1.55))
    camera = bpy.context.object
    bpy.context.scene.camera = camera
    look_at(camera, (-0.35, 0.36, -0.08))
    camera.data.lens = 38
    camera.data.dof.use_dof = True
    camera.data.dof.focus_distance = 7.7
    camera.data.dof.aperture_fstop = 8.0


def export_assets():
    blend_path = OUT_DIR / "pulse_carbine_viewmodel.blend"
    render_path = RENDER_DIR / "pulse_carbine_full.png"
    viewmodel_path = RENDER_DIR / "pulse_carbine_viewmodel.png"
    obj_path = MODEL_DIR / "pulse_carbine_viewmodel.obj"
    left_hand_path = MODEL_DIR / "pulse_left_hand_viewmodel.obj"
    right_hand_path = MODEL_DIR / "pulse_right_hand_viewmodel.obj"

    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path))

    export_excludes = {"matte display plinth", "rear shadow card"}

    def is_exportable(obj):
        return obj.type in {"MESH", "FONT"} and obj.name not in export_excludes

    def is_left_hand(obj):
        return obj.name.lower().startswith("left ")

    def is_right_hand(obj):
        return obj.name.lower().startswith("right ")

    def export_group(path, predicate):
        bpy.ops.object.select_all(action="DESELECT")
        for obj in bpy.context.scene.objects:
            if is_exportable(obj) and predicate(obj):
                obj.select_set(True)
        try:
            bpy.ops.wm.obj_export(filepath=str(path), export_selected_objects=True, export_materials=True)
        except Exception:
            bpy.ops.export_scene.obj(filepath=str(path), use_selection=True, use_materials=True)

    export_group(obj_path, lambda obj: not is_left_hand(obj) and not is_right_hand(obj))
    export_group(left_hand_path, is_left_hand)
    export_group(right_hand_path, is_right_hand)

    if os.environ.get("PULSE_FAST_EXPORT") == "1":
        print(f"PULSE_WEAPON_BLEND={blend_path}")
        print(f"PULSE_WEAPON_OBJ={obj_path}")
        print(f"PULSE_LEFT_HAND_OBJ={left_hand_path}")
        print(f"PULSE_RIGHT_HAND_OBJ={right_hand_path}")
        return

    bpy.context.scene.render.filepath = str(render_path)
    bpy.ops.render.render(write_still=True)

    camera = bpy.context.scene.camera
    camera.location = (5.15, 7.15, 1.08)
    look_at(camera, (-0.42, 0.38, -0.08))
    camera.data.lens = 42
    camera.data.dof.focus_distance = 8.7
    camera.data.dof.aperture_fstop = 7.5
    bpy.context.scene.render.filepath = str(viewmodel_path)
    bpy.ops.render.render(write_still=True)

    print(f"PULSE_WEAPON_BLEND={blend_path}")
    print(f"PULSE_WEAPON_FULL_RENDER={render_path}")
    print(f"PULSE_WEAPON_VIEWMODEL_RENDER={viewmodel_path}")
    print(f"PULSE_WEAPON_OBJ={obj_path}")
    print(f"PULSE_LEFT_HAND_OBJ={left_hand_path}")
    print(f"PULSE_RIGHT_HAND_OBJ={right_hand_path}")


def build_ak47():
    # Realistic AK-47. Convention: muzzle at -X, up is +Z, -Y faces the camera.
    # Blued steel receiver/barrel, wood furniture, curved steel banana magazine,
    # and a fixed wood stock.
    bz = 0.17  # bore line height

    # Barrel + muzzle device.
    cyl_obj("barrel", (-1.45, 0, bz), 0.048, 1.5, MAT_BLUED, 32)
    cyl_obj("front sight ferrule", (-1.9, 0, bz), 0.07, 0.14, MAT_BLUED, 24)
    cyl_obj("muzzle nut", (-2.12, 0, bz), 0.066, 0.14, MAT_BLUED, 24)
    cyl_obj("slant brake", (-2.26, 0, bz + 0.02), 0.072, 0.20, MAT_BLUED, 24, rot=(0, math.radians(76), 0))
    # Hooded front sight post.
    cube_obj("front sight ear near", (-1.9, -0.055, bz + 0.15), (0.05, 0.022, 0.13), MAT_BLUED, 0.004, 1)
    cube_obj("front sight ear far", (-1.9, 0.055, bz + 0.15), (0.05, 0.022, 0.13), MAT_BLUED, 0.004, 1)
    cube_obj("front sight post", (-1.9, 0, bz + 0.12), (0.02, 0.02, 0.10), MAT_EDGE, 0.003, 1)
    # Gas block + gas tube.
    cube_obj("gas block", (-1.6, 0, bz + 0.05), (0.14, 0.15, 0.20), MAT_BLUED, 0.02, 2)
    cyl_obj("gas tube", (-1.12, 0, bz + 0.16), 0.05, 0.78, MAT_BLUED, 24)

    # Wood furniture.
    cube_obj("lower handguard", (-1.16, 0, bz - 0.10), (0.72, 0.20, 0.20), MAT_WOOD, 0.05, 4)
    cube_obj("lower handguard palmswell", (-1.16, 0, bz - 0.18), (0.50, 0.24, 0.10), MAT_WOOD, 0.07, 4)
    cube_obj("handguard retainer", (-0.80, 0, bz - 0.02), (0.08, 0.18, 0.24), MAT_BLUED, 0.015, 2)
    cube_obj("upper handguard", (-1.12, 0, bz + 0.18), (0.62, 0.15, 0.13), MAT_WOOD, 0.045, 3)
    cube_obj("gas tube front cap", (-1.5, 0, bz + 0.16), (0.09, 0.13, 0.15), MAT_BLUED, 0.015, 2)

    # Stamped steel receiver.
    cube_obj("receiver body", (-0.12, 0, 0.13), (1.18, 0.21, 0.30), MAT_BLUED, 0.03, 3)
    cube_obj("dust cover", (-0.14, 0, 0.31), (1.04, 0.20, 0.09), MAT_BLUED, 0.03, 3)
    cube_obj("rear sight block", (-0.62, 0, 0.30), (0.12, 0.16, 0.10), MAT_BLUED, 0.012, 2)
    cube_obj("rear sight leaf", (-0.55, 0, 0.34), (0.05, 0.13, 0.05), MAT_EDGE, 0.005, 1)
    cube_obj("magwell front lip", (0.16, 0, -0.06), (0.10, 0.20, 0.20), MAT_BLUED, 0.015, 2)

    # Near-side controls (so they read on the visible -Y face).
    cube_obj("charging handle rail", (-0.46, -0.112, 0.20), (0.36, 0.012, 0.05), MAT_DARK, 0.004, 1)
    cyl_obj("charging handle knob", (-0.30, -0.155, 0.20), 0.028, 0.10, MAT_EDGE, 16, rot=(math.radians(90), 0, 0))
    cube_obj("selector lever", (0.18, -0.116, 0.08), (0.40, 0.018, 0.045), MAT_BLUED, 0.005, 1, rot=(0, 0, math.radians(-16)))

    # Curved banana magazine: segments lean further forward as they descend.
    segs = [(0.06, -0.16, 0.24, 6), (0.02, -0.39, 0.24, 14), (-0.07, -0.62, 0.24, 22), (-0.19, -0.84, 0.22, 30)]
    for i, (mx, mz, ml, lean) in enumerate(segs):
        cube_obj("magazine seg %d" % i, (mx, 0, mz), (0.19, 0.19, ml), MAT_MAG, 0.02, 2, rot=(0, math.radians(lean), 0))
        cube_obj("magazine rib %d" % i, (mx, -0.1, mz), (0.12, 0.006, 0.02), MAT_DARK, 0.002, 1, rot=(0, math.radians(lean), 0))
    cube_obj("magazine floorplate", (-0.25, 0, -0.95), (0.20, 0.20, 0.06), MAT_MAG, 0.015, 2, rot=(0, math.radians(32), 0))

    # Trigger group.
    bar_between("trigger guard front", (0.30, 0, -0.13), (0.30, 0, -0.32), 0.026, MAT_BLUED, 0.006)
    bar_between("trigger guard bottom", (0.30, 0, -0.32), (0.60, 0, -0.33), 0.026, MAT_BLUED, 0.006)
    bar_between("trigger guard rear", (0.60, 0, -0.33), (0.62, 0, -0.15), 0.026, MAT_BLUED, 0.006)
    cyl_between("trigger blade", (0.42, 0, -0.14), (0.39, 0, -0.27), 0.017, MAT_EDGE, 16)

    # Wood pistol grip, raked back.
    cube_obj("pistol grip", (0.60, 0, -0.42), (0.15, 0.16, 0.55), MAT_WOOD, 0.05, 4, rot=(0, math.radians(-17), 0))
    cube_obj("grip cap", (0.72, 0, -0.66), (0.16, 0.17, 0.07), MAT_DARK, 0.02, 2, rot=(0, math.radians(-17), 0))

    # Fixed wood stock.
    cube_obj("stock wrist", (0.85, 0, 0.10), (0.34, 0.12, 0.18), MAT_WOOD, 0.04, 3)
    cube_obj("stock body", (1.40, 0, 0.07), (0.95, 0.115, 0.18), MAT_WOOD, 0.05, 4)
    cube_obj("stock comb", (1.28, 0, 0.19), (0.55, 0.115, 0.07), MAT_WOOD, 0.05, 3)
    cube_obj("butt plate", (1.90, 0, 0.05), (0.06, 0.13, 0.26), MAT_BLUED, 0.02, 2)
    cube_obj("stock sling loop", (1.20, -0.063, 0.05), (0.10, 0.012, 0.06), MAT_BLUED, 0.004, 1)


def build_ak_hands():
    # First-person convention for this authored AK: muzzle is -X, stock is +X.
    # Blender OBJ export maps +Y into the player-facing side used by the engine
    # viewmodel transform, so the gloves sit on +Y and the sleeves run mostly
    # back toward +Y into the camera, with only a small downward drop.
    rgr = math.radians(17)

    # Right/fire hand: on the right-side pistol grip, with knuckles on the
    # player-facing side and the index finger reaching into the trigger guard.
    cyl_between("right forearm upper sleeve", (0.88, 0.78, -0.82), (0.80, 0.28, -0.68), 0.072, MAT_SLEEVE, 24)
    cyl_between("right forearm lower sleeve", (0.72, 0.74, -0.90), (0.70, 0.22, -0.72), 0.058, MAT_SLEEVE, 20)
    ellipsoid_obj("right wrist cuff", (0.76, 0.20, -0.66), (0.124, 0.094, 0.110), MAT_SLEEVE,
                  rot=(math.radians(-12), math.radians(17), math.radians(-10)))
    ellipsoid_obj("right palm hidden on grip", (0.68, -0.120, -0.51), (0.074, 0.052, 0.170), MAT_GLOVE,
                  rot=(math.radians(-4), rgr, math.radians(-8)))
    ellipsoid_obj("right back of hand mass", (0.60, 0.185, -0.50), (0.130, 0.070, 0.220), MAT_GLOVE_PAD,
                  rot=(math.radians(-2), rgr, math.radians(-6)))
    cube_obj("right back of hand armor", (0.58, 0.250, -0.49), (0.220, 0.046, 0.300), MAT_GLOVE_PAD,
             0.018, 2, rot=(math.radians(0), rgr, math.radians(-4)))
    for i, z in enumerate((-0.315, -0.405, -0.495, -0.585)):
        cyl_between("right finger curling inward %d" % i, (0.555, 0.220, z), (0.445, -0.035, z - 0.020),
                    0.026, MAT_GLOVE, 18)
        ellipsoid_obj("right outside knuckle %d" % i, (0.570, 0.255, z + 0.014), (0.040, 0.023, 0.027),
                      MAT_GLOVE_PAD, 16, 6, rot=(0, rgr, 0))
    cyl_between("right thumb wrapping inside grip", (0.76, -0.065, -0.36), (0.64, -0.030, -0.245), 0.026,
                MAT_GLOVE, 18)
    cyl_between("right trigger finger base", (0.56, 0.205, -0.300), (0.43, 0.105, -0.220), 0.024, MAT_GLOVE, 18)
    cyl_between("right trigger finger tip", (0.43, 0.105, -0.220), (0.405, 0.020, -0.300), 0.020, MAT_GLOVE, 18)

    # Left/support hand: visibly on the left face of the wooden fore-stock.
    # Fingers sit in front of the handguard from the gameplay camera, and the
    # sleeve starts lower-left/near-camera so the arm extends toward the player.
    cyl_between("left forearm upper sleeve", (-1.34, 0.92, -0.52), (-1.20, 0.62, -0.35), 0.070, MAT_SLEEVE, 24)
    cyl_between("left forearm lower sleeve", (-1.16, 0.88, -0.62), (-1.10, 0.56, -0.40), 0.056, MAT_SLEEVE, 20)
    ellipsoid_obj("left wrist cuff", (-1.18, 0.48, -0.34), (0.150, 0.108, 0.125), MAT_SLEEVE,
                  rot=(math.radians(-11), math.radians(-18), math.radians(8)))
    ellipsoid_obj("left palm on fore stock", (-1.38, 0.45, -0.080), (0.315, 0.145, 0.190), MAT_GLOVE,
                  rot=(math.radians(4), math.radians(-7), math.radians(2)))
    cube_obj("left glove knuckle plate", (-1.46, 0.640, -0.025), (0.345, 0.052, 0.122), MAT_GLOVE_PAD,
             0.018, 2, rot=(0, math.radians(-6), 0))
    cube_obj("left curled fingers over fore stock", (-1.37, 0.350, 0.055), (0.420, 0.210, 0.160), MAT_GLOVE,
             0.070, 5, rot=(0, math.radians(-6), 0))
    for i, (x, z) in enumerate(((-1.12, -0.060), (-1.02, -0.028), (-0.92, 0.004), (-0.82, 0.036))):
        cyl_between("left visible support finger %d" % i, (x, 0.735, z), (x + 0.060, 0.365, z + 0.105),
                    0.030, MAT_GLOVE, 18)
        ellipsoid_obj("left visible support knuckle %d" % i, (x - 0.012, 0.755, z + 0.020),
                      (0.046, 0.026, 0.030), MAT_GLOVE_PAD, 16, 6, rot=(0, math.radians(-6), 0))
    for i, x in enumerate((-1.57, -1.43, -1.29, -1.15)):
        cube_obj("left finger groove %d" % i, (x, 0.660, 0.080), (0.026, 0.027, 0.138), MAT_GLOVE_PAD,
                 0.004, 1, rot=(0, math.radians(-6), 0))
        ellipsoid_obj("left raised knuckle %d" % i, (x + 0.012, 0.692, -0.012), (0.042, 0.024, 0.027),
                      MAT_GLOVE_PAD, 16, 6, rot=(0, math.radians(-6), 0))
    cube_obj("left thumb clamped on fore stock", (-1.15, 0.382, -0.020), (0.245, 0.112, 0.090), MAT_GLOVE,
             0.045, 3, rot=(0, math.radians(-12), math.radians(6)))


def build_fps_carbine():
    # Purpose-built first-person viewmodel, not a side-on world rifle. Keep the
    # rear short so the closest geometry is the player's hands, while the barrel
    # reads forward toward the crosshair.
    bz = 0.18

    # Compact receiver and ejection-side detail.
    cube_obj("fps upper receiver", (-0.06, 0.02, 0.22), (0.92, 0.27, 0.24), MAT_GUNMETAL, 0.035, 4)
    cube_obj("fps lower receiver", (0.14, 0.02, -0.03), (0.56, 0.29, 0.30), MAT_GUNMETAL, 0.032, 4)
    cube_obj("fps rear buffer socket", (0.54, 0.02, 0.11), (0.18, 0.26, 0.32), MAT_DARK2, 0.025, 3)
    cube_obj("fps ejection port recess", (-0.10, -0.168, 0.24), (0.32, 0.018, 0.092), MAT_DARK, 0.005, 1)
    cube_obj("fps bolt glint", (-0.12, -0.184, 0.27), (0.22, 0.010, 0.042), MAT_EDGE, 0.004, 1)
    cube_obj("fps charging handle nub", (0.22, -0.176, 0.30), (0.13, 0.028, 0.046), MAT_EDGE, 0.005, 1)
    cube_obj("fps selector switch", (0.38, -0.178, 0.06), (0.105, 0.018, 0.035), MAT_EDGE, 0.005, 1,
             rot=(0, 0, math.radians(23)))

    # Handguard: wide and faceted so the support hand has something believable
    # to clamp around in the lower-left of the screen.
    cube_obj("fps faceted handguard", (-0.86, 0.00, 0.13), (1.05, 0.34, 0.32), MAT_DARK2, 0.045, 5)
    cube_obj("fps lower handguard plane", (-0.86, 0.00, -0.075), (0.96, 0.25, 0.070), MAT_DARK, 0.018, 2)
    cube_obj("fps top rail spine", (-0.44, 0.00, 0.45), (1.56, 0.17, 0.064), MAT_DARK, 0.010, 1)
    for i in range(14):
        cube_obj(f"fps rail tooth {i:02d}", (-1.08 + i * 0.105, 0.00, 0.515), (0.046, 0.205, 0.050),
                 MAT_DARK, 0.005, 1)
    for i in range(6):
        x = -1.22 + i * 0.16
        cube_obj(f"fps near mlok slot {i}", (x, 0.182, 0.10), (0.075, 0.020, 0.125), MAT_EDGE, 0.005, 1,
                 rot=(0, 0, math.radians(-8)))
        cube_obj(f"fps far mlok slot {i}", (x, -0.182, 0.10), (0.075, 0.020, 0.125), MAT_EDGE, 0.005, 1,
                 rot=(0, 0, math.radians(-8)))
    cube_obj("fps angled hand stop", (-0.78, 0.03, -0.255), (0.16, 0.19, 0.28), MAT_DARK, 0.028, 3,
             rot=(0, math.radians(-12), 0))

    # Barrel group. The muzzle is deliberately clean and forward-readable.
    cyl_obj("fps inner barrel", (-1.62, 0.00, bz), 0.045, 1.12, MAT_GUNMETAL, 48, bevel_width=0.004)
    cyl_obj("fps gas block", (-1.28, 0.00, bz), 0.075, 0.15, MAT_DARK2, 40, bevel_width=0.010)
    cyl_obj("fps muzzle brake", (-2.17, 0.00, bz), 0.078, 0.24, MAT_DARK, 48, bevel_width=0.012)
    for i in range(3):
        cube_obj(f"fps muzzle side port {i}", (-2.20 + i * 0.052, -0.086, bz + 0.026), (0.030, 0.010, 0.036),
                 MAT_EDGE, 0.002, 1)

    # Sight line: low enough that it reads without blocking the crosshair.
    cube_obj("fps micro sight base", (-0.18, 0.00, 0.58), (0.36, 0.22, 0.074), MAT_DARK, 0.014, 2)
    cube_obj("fps micro sight hood", (-0.18, 0.00, 0.72), (0.31, 0.25, 0.19), MAT_DARK, 0.026, 4)
    cube_obj("fps blue lens", (-0.29, 0.00, 0.73), (0.022, 0.205, 0.128), MAT_GLASS, 0.006, 2)
    cube_obj("fps cyan reticle mark", (-0.303, -0.112, 0.73), (0.016, 0.010, 0.070), MAT_EMIT, 0.003, 1)
    cube_obj("fps front sight nub", (-1.38, 0.00, 0.55), (0.092, 0.13, 0.13), MAT_GUNMETAL, 0.010, 2)
    cube_obj("fps front sight post", (-1.40, 0.00, 0.66), (0.020, 0.030, 0.13), MAT_EDGE, 0.003, 1)

    # Magazine and controls. The mag is in front of the grip, not mistaken for
    # the grip itself in the gameplay frame.
    mag = trapezoid_prism("fps curved magazine", -0.03, 0.28, 0.112, 0.140, -0.78, -0.18, MAT_MAG)
    mag.rotation_euler[1] = math.radians(12)
    cube_obj("fps magazine floor plate", (0.19, 0.00, -0.83), (0.23, 0.22, 0.055), MAT_DARK2, 0.012, 2,
             rot=(0, math.radians(12), 0))
    cube_obj("fps magwell lip", (0.11, 0.00, -0.16), (0.25, 0.27, 0.12), MAT_GUNMETAL, 0.018, 2)

    # Open trigger and grip, authored for the visible right side (+Y).
    for y in (-0.155, 0.155):
        bar_between(f"fps trigger guard front {y}", (0.30, y, -0.13), (0.29, y, -0.34), 0.030, MAT_GUNMETAL, 0.007)
        bar_between(f"fps trigger guard bottom {y}", (0.29, y, -0.34), (0.56, y, -0.35), 0.030, MAT_GUNMETAL, 0.007)
        bar_between(f"fps trigger guard rear {y}", (0.56, y, -0.35), (0.60, y, -0.16), 0.030, MAT_GUNMETAL, 0.007)
    bar_between("fps visible trigger guard front", (0.30, -0.215, -0.13), (0.29, -0.215, -0.35), 0.033, MAT_EDGE, 0.008)
    bar_between("fps visible trigger guard bottom", (0.29, -0.215, -0.35), (0.57, -0.215, -0.35), 0.033, MAT_EDGE, 0.008)
    bar_between("fps visible trigger guard rear", (0.57, -0.215, -0.35), (0.61, -0.215, -0.16), 0.033, MAT_EDGE, 0.008)
    cyl_between("fps trigger blade", (0.43, -0.230, -0.17), (0.405, -0.225, -0.305), 0.018, MAT_DARK, 20, 0.004)

    pistol_grip_prism("fps raked pistol grip", 0.105, MAT_DARK)
    cube_obj("fps grip visible side pad", (0.70, -0.128, -0.55), (0.225, 0.024, 0.500), MAT_DARK2, 0.016, 2,
             rot=(0, math.radians(-14), 0))
    cube_obj("fps grip backstrap highlight", (0.84, -0.135, -0.55), (0.040, 0.026, 0.560), MAT_EDGE, 0.010, 1,
             rot=(0, math.radians(-14), 0))
    for i in range(4):
        cube_obj(f"fps grip rib {i}", (0.66 + i * 0.034, -0.150, -0.43 - i * 0.085), (0.135, 0.018, 0.022),
                 MAT_EDGE, 0.004, 1, rot=(0, math.radians(-14), 0))

    # Short stock impression only. A full buttstock near the camera was the main
    # reason the old model read like a side-profile rifle pasted into view.
    cyl_obj("fps short buffer tube", (0.82, 0.00, 0.16), 0.055, 0.44, MAT_DARK, 32, bevel_width=0.004)
    cube_obj("fps cropped cheek rest", (1.05, 0.00, 0.24), (0.36, 0.20, 0.16), MAT_DARK, 0.030, 3)

    cube_obj("fps cyan side cell", (0.02, -0.180, 0.045), (0.26, 0.014, 0.050), MAT_EMIT, 0.006, 1)
    add_text("fps receiver mark", "PULSE", (0.35, -0.186, -0.02), 0.062, MAT_MARK,
             rot=(math.radians(90), 0, 0))


def build_fps_hands():
    # Hands are built as chunky tactical gloves with short, connected fingers.
    # The aim is silhouette correctness from gameplay distance, not anatomical
    # complexity that turns into floating rods after projection.
    rgr = math.radians(-13)

    # Right/fire arm enters from the bottom-right and terminates on the pistol
    # grip. Blender OBJ export flips the side axis here, so negative Y becomes
    # screen-right after the engine's OBJ remap.
    cyl_between("right sleeve upper", (1.10, -0.82, -1.02), (0.82, -0.42, -0.72), 0.098, MAT_SLEEVE, 28)
    cyl_between("right sleeve lower", (0.94, -0.92, -1.10), (0.72, -0.44, -0.78), 0.074, MAT_SLEEVE, 24)
    ellipsoid_obj("right wrist cuff", (0.72, -0.39, -0.70), (0.142, 0.105, 0.120), MAT_SLEEVE,
                  rot=(math.radians(-8), rgr, math.radians(-8)))
    ellipsoid_obj("right palm on grip", (0.64, -0.245, -0.52), (0.150, 0.086, 0.205), MAT_GLOVE,
                  rot=(math.radians(-3), rgr, math.radians(-8)))
    cube_obj("right glove back plate", (0.60, -0.325, -0.50), (0.230, 0.040, 0.235), MAT_GLOVE_PAD,
             0.017, 2, rot=(0, rgr, math.radians(-5)))
    cube_obj("right knuckle bar", (0.54, -0.360, -0.39), (0.205, 0.036, 0.060), MAT_GLOVE_PAD,
             0.015, 2, rot=(0, rgr, math.radians(-6)))
    for i, z in enumerate((-0.36, -0.45, -0.54, -0.63)):
        cyl_between(f"right grip finger {i}", (0.55 + i * 0.012, -0.345, z), (0.50 + i * 0.010, -0.115, z - 0.035),
                    0.026, MAT_GLOVE, 18)
        ellipsoid_obj(f"right finger pad {i}", (0.56 + i * 0.012, -0.370, z + 0.014), (0.041, 0.024, 0.030),
                      MAT_GLOVE_PAD, 16, 6, rot=(0, rgr, 0))
    cyl_between("right thumb over backstrap", (0.75, -0.215, -0.40), (0.59, -0.110, -0.32), 0.028, MAT_GLOVE, 18)
    cyl_between("right index trigger reach", (0.49, -0.360, -0.30), (0.39, -0.245, -0.235), 0.023, MAT_GLOVE, 18)
    cyl_between("right index trigger curl", (0.39, -0.245, -0.235), (0.415, -0.218, -0.325), 0.019, MAT_GLOVE, 18)

    # Left/support side: exaggerated first-person glove volumes. The hand must
    # survive the game camera and dark arena grid, so the palm, cuff, and each
    # finger get separate readable masses instead of one merged blue strip.
    cyl_between("left short forearm stub", (-0.66, 0.410, -0.410), (-0.81, 0.285, -0.245),
                0.080, MAT_SLEEVE, 28)
    cube_obj("left angled wrist cuff", (-0.80, 0.288, -0.230), (0.300, 0.138, 0.185), MAT_SLEEVE,
             0.040, 4, rot=(math.radians(-6), math.radians(10), math.radians(-8)))
    cube_obj("left cuff shadow gap", (-0.775, 0.192, -0.165), (0.255, 0.034, 0.070), MAT_GLOVE_PAD,
             0.012, 1, rot=(math.radians(-4), math.radians(9), math.radians(-6)))
    ellipsoid_obj("left wrist joint", (-0.835, 0.230, -0.175), (0.130, 0.095, 0.090), MAT_GLOVE,
                  rot=(math.radians(-6), math.radians(10), math.radians(4)))
    ellipsoid_obj("left palm under handguard", (-0.930, 0.195, -0.125), (0.335, 0.180, 0.205), MAT_GLOVE,
                  rot=(math.radians(2), math.radians(8), math.radians(4)))
    cube_obj("left glove back plate", (-0.955, 0.330, -0.082), (0.430, 0.078, 0.178), MAT_GLOVE_PAD,
             0.022, 2, rot=(math.radians(-3), math.radians(7), math.radians(-2)))
    cube_obj("left palm heel pad", (-0.865, 0.060, -0.220), (0.355, 0.105, 0.135), MAT_GLOVE_PAD,
             0.030, 3, rot=(0, math.radians(6), 0))
    for i, x in enumerate((-1.105, -0.995, -0.885, -0.775)):
        cyl_between(f"left curled finger body {i}", (x, 0.350, -0.050), (x + 0.058, 0.135, 0.052),
                    0.033, MAT_GLOVE, 20)
        ellipsoid_obj(f"left raised knuckle pad {i}", (x + 0.010, 0.398, -0.030), (0.070, 0.039, 0.046),
                      MAT_GLOVE_PAD, 18, 7, rot=(0, math.radians(6), 0))
        cube_obj(f"left squared fingertip pad {i}", (x + 0.062, 0.118, 0.052), (0.070, 0.052, 0.050),
                 MAT_GLOVE_PAD, 0.013, 2, rot=(math.radians(-8), math.radians(8), math.radians(-14)))
        if i < 3:
            cube_obj(f"left dark finger split {i}", (x + 0.058, 0.285, -0.002), (0.018, 0.145, 0.100),
                     MAT_GLOVE_PAD, 0.006, 1, rot=(math.radians(-8), math.radians(8), math.radians(-17)))
    cyl_between("left thumb locked forward", (-0.700, 0.332, -0.080), (-0.535, 0.082, 0.014),
                0.046, MAT_GLOVE, 20)
    ellipsoid_obj("left thumb knuckle pad", (-0.640, 0.245, -0.026), (0.062, 0.038, 0.050),
                  MAT_GLOVE_PAD, 18, 7, rot=(math.radians(8), math.radians(34), math.radians(-6)))


def scale_hand_group(prefix, anchor, scale):
    anchor = Vector(anchor)
    for obj in bpy.context.scene.objects:
        if not obj.name.lower().startswith(prefix):
            continue
        offset = obj.location - anchor
        obj.location = anchor + offset * scale
        obj.scale = (obj.scale.x * scale, obj.scale.y * scale, obj.scale.z * scale)


def translate_hand_group(prefix, delta):
    delta = Vector(delta)
    for obj in bpy.context.scene.objects:
        if obj.name.lower().startswith(prefix):
            obj.location += delta


def main():
    clear_scene()
    build_fps_carbine()
    build_fps_hands()
    scale_hand_group("left ", (-0.90, 0.205, -0.155), 1.30)
    scale_hand_group("right ", (0.64, -0.245, -0.52), 1.25)
    translate_hand_group("left ", (0.12, -0.035, -0.125))
    setup_scene()
    export_assets()


if __name__ == "__main__":
    main()
