import json
import math
import struct
import sys
from pathlib import Path

import bpy
from mathutils import Vector


ROOT = Path(__file__).resolve().parents[2]
SOURCE_DIR = ROOT / "assets" / "bumstrum"
OUT_DIR = ROOT / "assets" / "models"


ASSETS = [
    {
        "name": "ak47",
        "src": SOURCE_DIR / "fps_ak_animated" / "scene.gltf",
        "out": OUT_DIR / "bumstrum_ak47_viewmodel.glb",
        "frame": 0.0,
        "target_depth": 1.55,
        "brightness": 1.45,
        "lift": 0.045,
        "preserve_origin": True,
    },
    {
        "name": "carbine",
        "src": SOURCE_DIR / "fps_animated_carbine" / "scene.gltf",
        "out": OUT_DIR / "bumstrum_carbine_viewmodel.glb",
        "frame": 0.0,
        "target_depth": 1.35,
        "brightness": 1.45,
        "lift": 0.045,
        "preserve_origin": True,
    },
    {
        "name": "pistol",
        "src": SOURCE_DIR / "fps_pistol_animated" / "scene.gltf",
        "out": OUT_DIR / "bumstrum_pistol_viewmodel.glb",
        "frame": 0.0,
        "target_depth": 0.95,
        "brightness": 1.55,
        "lift": 0.050,
        "preserve_origin": True,
    },
]


def pad4(data, fill=b"\x00"):
    pad = (4 - (len(data) & 3)) & 3
    return data + fill * pad


def srgb_lift(c, brightness, lift):
    return max(0.0, min(1.0, c * brightness + lift))


def find_base_image(mat):
    if not mat or not mat.use_nodes or not mat.node_tree:
        return None
    for node in mat.node_tree.nodes:
        if node.type != "BSDF_PRINCIPLED":
            continue
        socket = node.inputs.get("Base Color")
        if not socket or not socket.is_linked:
            continue
        upstream = socket.links[0].from_node
        if upstream.type == "TEX_IMAGE" and upstream.image:
            return upstream.image
        for link in upstream.inputs:
            if not link.is_linked:
                continue
            src = link.links[0].from_node
            if src.type == "TEX_IMAGE" and src.image:
                return src.image
    for node in mat.node_tree.nodes:
        if node.type == "TEX_IMAGE" and node.image:
            return node.image
    return None


def make_sampler(mat, image_cache, brightness, lift):
    base = tuple(mat.diffuse_color) if mat else (0.65, 0.65, 0.65, 1.0)
    image = find_base_image(mat)
    if not image:
        return lambda uv: (
            srgb_lift(base[0], brightness, lift),
            srgb_lift(base[1], brightness, lift),
            srgb_lift(base[2], brightness, lift),
            base[3] if len(base) > 3 else 1.0,
        )

    if image.name not in image_cache:
        pixels = list(image.pixels[:])
        image_cache[image.name] = (image.size[0], image.size[1], pixels)
    width, height, pixels = image_cache[image.name]

    def sample(uv):
        u = uv.x % 1.0
        v = uv.y % 1.0
        x = min(width - 1, max(0, int(u * width)))
        y = min(height - 1, max(0, int(v * height)))
        i = (y * width + x) * 4
        return (
            srgb_lift(pixels[i + 0], brightness, lift),
            srgb_lift(pixels[i + 1], brightness, lift),
            srgb_lift(pixels[i + 2], brightness, lift),
            pixels[i + 3],
        )

    return sample


def convert_pos(p):
    # Bumstrum FPS assets are authored X=side, Y=length, Z=up. PULSE camera-space
    # viewmodels use X=right, Y=up, Z=forward, so the muzzle is +Z.
    return Vector((p.x, p.z, -p.y))


def convert_nrm(n):
    out = Vector((n.x, n.z, -n.y))
    if out.length_squared > 1.0e-10:
        out.normalize()
    else:
        out = Vector((0.0, 1.0, 0.0))
    return out


def write_glb(path, positions, normals, colors, indices):
    def pack_floats(values, width):
        blob = bytearray()
        for v in values:
            for i in range(width):
                blob += struct.pack("<f", float(v[i]))
        return bytes(blob)

    pos_blob = pack_floats(positions, 3)
    nrm_blob = pack_floats(normals, 3)
    col_blob = pack_floats(colors, 4)
    idx_blob = b"".join(struct.pack("<I", int(i)) for i in indices)

    chunks = []
    offset = 0
    for blob, target in ((pos_blob, 34962), (nrm_blob, 34962), (col_blob, 34962), (idx_blob, 34963)):
        padded = pad4(blob)
        chunks.append((offset, len(blob), target, padded))
        offset += len(padded)
    bin_blob = b"".join(c[3] for c in chunks)

    mins = [min(p[i] for p in positions) for i in range(3)]
    maxs = [max(p[i] for p in positions) for i in range(3)]
    doc = {
        "asset": {"version": "2.0", "generator": "PULSE Bumstrum viewmodel baker"},
        "buffers": [{"byteLength": len(bin_blob)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": chunks[0][0], "byteLength": chunks[0][1], "target": chunks[0][2]},
            {"buffer": 0, "byteOffset": chunks[1][0], "byteLength": chunks[1][1], "target": chunks[1][2]},
            {"buffer": 0, "byteOffset": chunks[2][0], "byteLength": chunks[2][1], "target": chunks[2][2]},
            {"buffer": 0, "byteOffset": chunks[3][0], "byteLength": chunks[3][1], "target": chunks[3][2]},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3", "min": mins, "max": maxs},
            {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": len(colors), "type": "VEC4"},
            {"bufferView": 3, "componentType": 5125, "count": len(indices), "type": "SCALAR"},
        ],
        "meshes": [{
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1, "COLOR_0": 2},
                "indices": 3,
                "mode": 4,
            }]
        }],
        "nodes": [{"mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }

    json_blob = pad4(json.dumps(doc, separators=(",", ":")).encode("utf-8"), b" ")
    bin_chunk = pad4(bin_blob)
    total = 12 + 8 + len(json_blob) + 8 + len(bin_chunk)
    with path.open("wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(json_blob), 0x4E4F534A))
        f.write(json_blob)
        f.write(struct.pack("<II", len(bin_chunk), 0x004E4942))
        f.write(bin_chunk)


def bake_asset(spec):
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    bpy.ops.import_scene.gltf(filepath=str(spec["src"]))
    bpy.context.scene.frame_set(int(spec["frame"]))
    deps = bpy.context.evaluated_depsgraph_get()

    raw = []
    image_cache = {}
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH" or obj.name.lower().startswith("icosphere"):
            continue
        eval_obj = obj.evaluated_get(deps)
        mesh = eval_obj.to_mesh(preserve_all_data_layers=True, depsgraph=deps)
        mesh.calc_loop_triangles()
        uv_layer = mesh.uv_layers.active
        samplers = []
        for slot in obj.material_slots:
            samplers.append(make_sampler(slot.material, image_cache, spec["brightness"], spec["lift"]))
        if not samplers:
            samplers.append(make_sampler(None, image_cache, spec["brightness"], spec["lift"]))

        normal_matrix = obj.matrix_world.to_3x3()
        for tri in mesh.loop_triangles:
            sampler = samplers[min(tri.material_index, len(samplers) - 1)]
            for loop_index in tri.loops:
                loop = mesh.loops[loop_index]
                vert = mesh.vertices[loop.vertex_index]
                pos = convert_pos(obj.matrix_world @ vert.co)
                nrm = convert_nrm(normal_matrix @ loop.normal)
                uv = uv_layer.data[loop_index].uv if uv_layer else Vector((0.0, 0.0))
                col = sampler(uv)
                raw.append((pos, nrm, col))
        eval_obj.to_mesh_clear()

    if not raw:
        raise RuntimeError(f"No triangles baked from {spec['src']}")

    mn = Vector((min(v[0].x for v in raw), min(v[0].y for v in raw), min(v[0].z for v in raw)))
    mx = Vector((max(v[0].x for v in raw), max(v[0].y for v in raw), max(v[0].z for v in raw)))
    center = (mn + mx) * 0.5
    depth = max(0.001, mx.z - mn.z)
    scale = spec["target_depth"] / depth

    positions = []
    normals = []
    colors = []
    indices = []
    for i, (pos, nrm, col) in enumerate(raw):
        p = pos * scale if spec.get("preserve_origin", False) else (pos - center) * scale
        positions.append((p.x, p.y, p.z))
        normals.append((nrm.x, nrm.y, nrm.z))
        colors.append(col)
        indices.append(i)

    spec["out"].parent.mkdir(parents=True, exist_ok=True)
    write_glb(spec["out"], positions, normals, colors, indices)
    print(f"{spec['name']}: wrote {spec['out']} ({len(positions)} verts, {len(indices)//3} tris, scale={scale:.5f})")


def parse_args():
    args = sys.argv
    if "--" in args:
        args = args[args.index("--") + 1:]
    else:
        args = []
    only = None
    frame = None
    i = 0
    while i < len(args):
        if args[i] == "--only" and i + 1 < len(args):
            only = args[i + 1]
            i += 2
        elif args[i] == "--frame" and i + 1 < len(args):
            frame = int(float(args[i + 1]))
            i += 2
        else:
            i += 1
    return only, frame


def main():
    only, frame = parse_args()
    for base in ASSETS:
        if only and base["name"] != only:
            continue
        spec = dict(base)
        if frame is not None:
            spec["frame"] = frame
        bake_asset(spec)


if __name__ == "__main__":
    main()
