# gen_obsidian_energy.py - generate a tileable "polished obsidian + hot-magenta energy veins"
# base-color texture for the Neon Ink Brutalism enemies. The body is near-black; the crack
# network is bright magenta so it GLOWS via the engine's emissive = albedo * emissive_scalar.
# Authored procedurally (toroidal Worley crack network) so it is seamless and controllable.
# ASCII only.  Usage: blender -b --python gen_obsidian_energy.py -- <out.png> [size]
import bpy, sys, numpy as np

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
out = argv[0]
N = int(argv[1]) if len(argv) > 1 else 1024

rng = np.random.default_rng(7)
gy, gx = np.mgrid[0:N, 0:N].astype(np.float32)
gx /= N; gy /= N

def worley(k, seed):
    # tileable Worley: returns (F1, F2) toroidal distance fields to k random feature points.
    r = np.random.default_rng(seed)
    pts = r.random((k, 2)).astype(np.float32)
    F1 = np.full((N, N), 9.0, np.float32)
    F2 = np.full((N, N), 9.0, np.float32)
    for i in range(k):
        dx = np.abs(gx - pts[i, 0]); dx = np.minimum(dx, 1.0 - dx)
        dy = np.abs(gy - pts[i, 1]); dy = np.minimum(dy, 1.0 - dy)
        d = np.sqrt(dx * dx + dy * dy)
        closer = d < F1
        F2 = np.where(closer, F1, np.minimum(F2, d))
        F1 = np.where(closer, d, F1)
    return F1, F2

def crack(k, seed, width):
    # cell-boundary mask: bright where equidistant to the two nearest points (F2-F1 ~ 0).
    F1, F2 = worley(k, seed)
    edge = (F2 - F1)
    return np.clip(1.0 - edge / width, 0.0, 1.0) ** 2.4

# Two scales of cracks: a few bold primary veins + sparse finer fractures. Big cells keep
# most of the body as dark obsidian; the cracks are thin seams of energy.
veins = np.maximum(crack(15, 1, 0.012), 0.5 * crack(38, 2, 0.006))
# A smooth low-frequency field to vary the body darkness (obsidian facets) + concentrate glow.
def smooth_noise(seed, oct):
    r = np.random.default_rng(seed)
    acc = np.zeros((N, N), np.float32); amp = 1.0; tot = 0.0
    for o in range(oct):
        f = 2 ** (o + 1)
        coarse = r.random((f, f)).astype(np.float32)
        # bilinear upsample (tileable wrap)
        yi = (gy * f); xi = (gx * f)
        y0 = np.floor(yi).astype(int) % f; x0 = np.floor(xi).astype(int) % f
        y1 = (y0 + 1) % f; x1 = (x0 + 1) % f
        ty = yi - np.floor(yi); tx = xi - np.floor(xi)
        v = (coarse[y0, x0] * (1 - tx) + coarse[y0, x1] * tx) * (1 - ty) + \
            (coarse[y1, x0] * (1 - tx) + coarse[y1, x1] * tx) * ty
        acc += v * amp; tot += amp; amp *= 0.5
    return acc / tot

facet = smooth_noise(5, 4)
glowfield = smooth_noise(9, 2)            # large soft regions that are more energised
# Gate the energy by a soft field so large stretches stay near-dark and the glow concentrates
# in pockets (reads as a body with a few hot seams, not a uniform glowing web).
veins = veins * np.clip((glowfield - 0.32) / 0.42, 0.06, 1.0)

# Body: deep obsidian, cool violet, faceted (subtle lightness variation + rare cool glints).
body = np.stack([
    0.045 + 0.05 * facet + 0.10 * (facet > 0.82),
    0.045 + 0.05 * facet + 0.11 * (facet > 0.82),
    0.075 + 0.07 * facet + 0.14 * (facet > 0.82),
], axis=-1).astype(np.float32)

# Hot magenta energy (bright so emissive*albedo blooms); whiter at the very core of a vein.
v = np.clip(veins, 0.0, 1.0)[..., None]
mag = np.array([1.0, 0.12, 0.72], np.float32)
hot = np.array([1.0, 0.55, 0.95], np.float32)            # white-hot vein cores
energy = mag * np.ones_like(body) * v + (hot - mag) * (np.clip(veins - 0.7, 0, 1)[..., None] / 0.3)
rgb = body * (1.0 - v) + energy
rgb = np.clip(rgb, 0.0, 1.0)

rgba = np.dstack([rgb, np.ones((N, N), np.float32)]).astype(np.float32)
rgba = rgba[::-1]   # Blender image rows are bottom-up

img = bpy.data.images.new("obsidian_energy", N, N, alpha=True)
img.pixels.foreach_set(rgba.ravel())
img.filepath_raw = out
img.file_format = 'PNG'
img.save()
print("GEN_OK", out, N, "vein_coverage=%.3f" % float((v > 0.25).mean()))
