import os

import bpy
import numpy as np


ROOT = r"C:\Users\rq27\Pulse"
ASSETS = [
    ("pistol", "assets/bumstrum/fps_pistol_animated/scene.gltf", 7.4667),
    ("ak47", "assets/bumstrum/fps_ak_animated/scene.gltf", 0.0),
    ("smg", "assets/bumstrum/fps_smg9_animated/scene.gltf", 6.82),
    ("carbine", "assets/bumstrum/fps_animated_carbine/scene.gltf", 6.92),
]


def is_weapon_mesh(obj):
    if obj.type != "MESH":
        return False
    name = (obj.name + " " + (obj.data.name if obj.data else "")).lower()
    return "arm" not in name and "hand" not in name and "lens" not in name


def pca(points):
    arr = np.array([[p.x, p.y, p.z] for p in points], dtype=np.float64)
    centered = arr - arr.mean(axis=0)
    cov = centered.T @ centered / max(1, len(arr) - 1)
    vals, vecs = np.linalg.eigh(cov)
    order = vals.argsort()[::-1]
    return vals[order], vecs[:, order[0]], arr


for name, rel, time_s in ASSETS:
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()

    bpy.ops.import_scene.gltf(filepath=os.path.join(ROOT, rel))
    bpy.context.scene.frame_set(round(time_s * 30.0))
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()

    points = []
    objects = []
    for obj in bpy.context.scene.objects:
        if not is_weapon_mesh(obj):
            continue
        evaluated = obj.evaluated_get(depsgraph)
        mesh = evaluated.to_mesh()
        if mesh:
            points.extend(evaluated.matrix_world @ v.co for v in mesh.vertices)
            objects.append(obj.name)
            evaluated.to_mesh_clear()

    if not points:
        print(f"== {name} == no weapon points")
        continue

    vals, axis, arr = pca(points)
    mins = arr.min(axis=0)
    maxs = arr.max(axis=0)
    dims = maxs - mins
    runtime_axis = np.array([-axis[0], axis[1], axis[2]])

    print(f"== {name} time={time_s:.4f} ==")
    print("objects:", ", ".join(objects[:16]))
    print("bounds:", [round(float(v), 3) for v in mins], [round(float(v), 3) for v in maxs])
    print("dims:", [round(float(v), 3) for v in dims])
    print("raw_major_axis:", [round(float(v), 3) for v in axis])
    print("runtime_pre_profile_axis:", [round(float(v), 3) for v in runtime_axis],
          "abs=", [round(float(abs(v)), 3) for v in runtime_axis])
    print("eigen:", [round(float(v), 3) for v in vals])
