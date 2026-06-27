# gen_obsidian_pbr.py - generate a full PBR "polished obsidian + hot-magenta energy" texture SET
# (base color + normal + ORM) for the Neon Ink Brutalism enemies. A faceted obsidian surface with
# real relief (normal map) so it catches light and reads as a textured material, not flat colour;
# crack-veins carry bright magenta energy that blooms via the engine's emissive = albedo*scalar.
# ASCII only.  Usage: blender -b --python gen_obsidian_pbr.py -- <out_base.png> <out_n.png> <out_orm.png> [size]
import bpy, sys, numpy as np

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
out_base, out_n, out_orm = argv[0], argv[1], argv[2]
N = int(argv[3]) if len(argv) > 3 else 1024

gy, gx = np.mgrid[0:N, 0:N].astype(np.float32)
gx /= N; gy /= N

def worley(k, seed):
    r = np.random.default_rng(seed)
    pts = r.random((k, 2)).astype(np.float32)
    ids = r.random(k).astype(np.float32)
    F1 = np.full((N, N), 9.0, np.float32); F2 = np.full((N, N), 9.0, np.float32)
    CID = np.zeros((N, N), np.float32)
    for i in range(k):
        dx = np.abs(gx - pts[i, 0]); dx = np.minimum(dx, 1.0 - dx)
        dy = np.abs(gy - pts[i, 1]); dy = np.minimum(dy, 1.0 - dy)
        d = np.sqrt(dx * dx + dy * dy)
        closer = d < F1
        F2 = np.where(closer, F1, np.minimum(F2, d))
        CID = np.where(closer, ids[i], CID)
        F1 = np.where(closer, d, F1)
    return F1, F2, CID

def fbm(seed, oct):
    r = np.random.default_rng(seed)
    acc = np.zeros((N, N), np.float32); amp = 1.0; tot = 0.0
    for o in range(oct):
        f = 2 ** (o + 2)
        c = r.random((f, f)).astype(np.float32)
        yi = gy * f; xi = gx * f
        y0 = np.floor(yi).astype(int) % f; x0 = np.floor(xi).astype(int) % f
        y1 = (y0 + 1) % f; x1 = (x0 + 1) % f
        ty = yi - np.floor(yi); tx = xi - np.floor(xi)
        ty = ty * ty * (3 - 2 * ty); tx = tx * tx * (3 - 2 * tx)
        v = (c[y0, x0] * (1 - tx) + c[y0, x1] * tx) * (1 - ty) + \
            (c[y1, x0] * (1 - tx) + c[y1, x1] * tx) * ty
        acc += v * amp; tot += amp; amp *= 0.5
    return acc / tot

# --- structure: big facets (Voronoi) + finer fracture + rock grain ---
F1, F2, CID = worley(26, 1)          # primary facets
f1b, f2b, _ = worley(70, 2)          # secondary fractures
grain = fbm(5, 5)                    # rock micro-bump
crack = np.maximum(np.clip(1 - (F2 - F1) / 0.014, 0, 1) ** 2.2,
                   0.55 * np.clip(1 - (f2b - f1b) / 0.008, 0, 1) ** 2.2)

# Height field: facets sit slightly proud + domed, cracks carve deep grooves, grain adds tooth.
facet_dome = np.clip(1.0 - F1 / 0.16, 0.0, 1.0)
height = (0.30 + 0.30 * CID) + 0.22 * facet_dome + 0.30 * grain - 0.85 * crack
height = np.clip(height, 0.0, 1.0)

# --- normal map from the height gradient (tileable via np.roll), OpenGL +Y ---
strength = 2.6
hx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * 0.5
hy = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * 0.5
nx = -hx * strength * N / 256.0
ny = -hy * strength * N / 256.0
nz = np.ones_like(nx)
ln = np.sqrt(nx * nx + ny * ny + nz * nz)
normal = np.dstack([(nx / ln) * 0.5 + 0.5, (ny / ln) * 0.5 + 0.5, (nz / ln) * 0.5 + 0.5]).astype(np.float32)

# --- base colour: cool charcoal-violet obsidian, per-facet shade + grain + mineral glints,
#     AO-darkened in the grooves; bright magenta along the energised cracks ---
shade = 0.12 + 0.14 * CID + 0.12 * (grain - 0.5)          # facet-to-facet value variation
glint = np.clip(grain - 0.84, 0, 1) * 4.5                  # rare specular speckle
ao = np.clip(1.0 - 1.2 * crack - 0.3 * (1 - facet_dome), 0.30, 1.0)
base_v = np.clip((shade + 0.22 * glint) * ao, 0.04, 0.62)
body = np.stack([base_v * 0.86, base_v * 0.86, base_v * 1.20], -1)   # violet-cool tint

glowfield = fbm(9, 2)
energy = np.clip(crack * np.clip((glowfield - 0.30) / 0.42, 0.05, 1.0), 0, 1)
mag = np.array([1.0, 0.10, 0.70], np.float32)
hot = np.array([1.0, 0.55, 0.95], np.float32)
ec = mag + (hot - mag) * np.clip((energy - 0.7) / 0.3, 0, 1)[..., None]
e3 = energy[..., None]
base = np.clip(body * (1 - e3) + ec * e3, 0, 1).astype(np.float32)

# --- ORM: R=AO  G=roughness (glossy facets, rough grooves, glossy veins)  B=metallic ---
rough = np.clip(0.30 + 0.45 * crack + 0.12 * (1 - facet_dome) - 0.10 * glint, 0.12, 0.85)
rough = rough * (1 - 0.6 * energy) + 0.18 * energy
metal = np.clip(0.10 + 0.25 * facet_dome, 0.0, 0.45) * (1 - energy)
orm = np.dstack([ao, rough, metal]).astype(np.float32)

import zlib, struct

def write_png(path, arr3):
    # arr3: (N,N,3) float 0..1 -> 8-bit RGBA PNG, written by hand (no bpy colorspace surprises).
    a = np.clip(arr3, 0.0, 1.0)
    rgba = np.dstack([a, np.ones((N, N), np.float32)])
    u8 = (rgba * 255.0 + 0.5).astype(np.uint8)
    raw = bytearray()
    row_bytes = u8.reshape(N, N * 4)
    for y in range(N):
        raw.append(0)                      # filter type 0 (none)
        raw.extend(row_bytes[y].tobytes())
    comp = zlib.compress(bytes(raw), 6)
    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data +
                struct.pack(">I", zlib.crc32(typ + data) & 0xffffffff))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", comp) + chunk(b"IEND", b""))
    with open(path, "wb") as fp:
        fp.write(png)
    print("SAVED", path, "mean=%.3f" % float(a.mean()))

write_png(out_base, base)
write_png(out_n, normal)
write_png(out_orm, orm)
print("GEN_PBR_OK vein=%.3f base_mean=%.3f" % (float((energy > 0.25).mean()), float(base_v.mean())))
