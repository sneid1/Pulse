#!/usr/bin/env python3
# Analyze the single baked animation clip in each bumstrum enemy glTF and propose
# idle / walk / attack sub-ranges. Pure-data heuristic: sample the skeleton over
# the whole timeline, then use named joints (feet, wrists, hips) to score motion.
# ASCII-only output by design.
import json, os, struct, sys
import numpy as np

COMPONENT = {5120: ('b', 1), 5121: ('B', 1), 5122: ('h', 2),
             5123: ('H', 2), 5125: ('I', 4), 5126: ('f', 4)}
NUMC = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def load(path):
    g = json.load(open(path))
    base = os.path.dirname(path)
    buffers = []
    for b in g['buffers']:
        uri = b['uri']
        buffers.append(open(os.path.join(base, uri), 'rb').read())
    return g, buffers


def read_accessor(g, buffers, idx):
    a = g['accessors'][idx]
    bv = g['bufferViews'][a['bufferView']]
    buf = buffers[bv['buffer']]
    comp = a['componentType']
    fmt, size = COMPONENT[comp]
    ncomp = NUMC[a['type']]
    count = a['count']
    base = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    stride = bv.get('byteStride', 0) or (size * ncomp)
    out = np.empty((count, ncomp), dtype=np.float32)
    dt = np.dtype('<' + fmt)
    for i in range(count):
        off = base + i * stride
        vals = np.frombuffer(buf, dtype=dt, count=ncomp, offset=off).astype(np.float32)
        out[i] = vals
    return out if ncomp > 1 else out[:, 0]


def quat_to_mat(q):
    x, y, z, w = q
    n = (x * x + y * y + z * z + w * w) ** 0.5
    if n < 1e-9:
        return np.eye(4, dtype=np.float32)
    x, y, z, w = x / n, y / n, z / n, w / n
    m = np.eye(4, dtype=np.float32)
    m[0, 0] = 1 - 2 * (y * y + z * z); m[0, 1] = 2 * (x * y - w * z); m[0, 2] = 2 * (x * z + w * y)
    m[1, 0] = 2 * (x * y + w * z); m[1, 1] = 1 - 2 * (x * x + z * z); m[1, 2] = 2 * (y * z - w * x)
    m[2, 0] = 2 * (x * z - w * y); m[2, 1] = 2 * (y * z + w * x); m[2, 2] = 1 - 2 * (x * x + y * y)
    return m


def trs(t, r, s):
    T = np.eye(4, dtype=np.float32); T[:3, 3] = t
    R = quat_to_mat(r)
    S = np.eye(4, dtype=np.float32); S[0, 0], S[1, 1], S[2, 2] = s
    return T @ R @ S


def node_local_default(n):
    if 'matrix' in n:
        return np.array(n['matrix'], dtype=np.float32).reshape(4, 4).T  # glTF column-major
    t = np.array(n.get('translation', [0, 0, 0]), dtype=np.float32)
    r = np.array(n.get('rotation', [0, 0, 0, 1]), dtype=np.float32)
    s = np.array(n.get('scale', [1, 1, 1]), dtype=np.float32)
    return trs(t, r, s)


def build_samplers(g, buffers, anim):
    chans = {}  # node -> {path: (times, values, interp)}
    samplers = anim['samplers']
    for c in anim['channels']:
        s = samplers[c['sampler']]
        node = c['target']['node']; path = c['target']['path']
        times = read_accessor(g, buffers, s['input'])
        vals = read_accessor(g, buffers, s['output'])
        chans.setdefault(node, {})[path] = (np.asarray(times), np.asarray(vals), s.get('interpolation', 'LINEAR'))
    return chans


def sample_channel(times, vals, interp, t):
    if t <= times[0]:
        return vals[0]
    if t >= times[-1]:
        return vals[-1]
    k = np.searchsorted(times, t) - 1
    k = max(0, min(k, len(times) - 2))
    t0, t1 = times[k], times[k + 1]
    u = 0.0 if interp == 'STEP' else (t - t0) / max(1e-6, t1 - t0)
    if interp == 'CUBICSPLINE':
        # vals laid out [in,val,out] per key; take val only, lerp (approx good enough)
        a = vals[k * 3 + 1]; b = vals[(k + 1) * 3 + 1]
        return a + (b - a) * u
    a = vals[k]; b = vals[k + 1]
    return a + (b - a) * u


def main():
    models = {
        'ghoul': 'assets/bumstrum/ghoul/scene.gltf',
        'cultist': 'assets/bumstrum/cultist/scene.gltf',
        'skeleton_lord': 'assets/bumstrum/skeleton_lord/scene.gltf',
    }
    import re
    for name, path in models.items():
        g, buffers = load(path)
        nodes = g['nodes']
        N = len(nodes)
        parent = [-1] * N
        for i, n in enumerate(nodes):
            for c in n.get('children', []):
                parent[c] = i
        order = []  # topological: parents before children
        seen = [False] * N
        def visit(i):
            if seen[i]:
                return
            if parent[i] >= 0:
                visit(parent[i])
            seen[i] = True; order.append(i)
        for i in range(N):
            visit(i)

        skin = g['skins'][0]
        jnames = {nodes[j].get('name', ''): j for j in skin['joints']}
        def find(pat):
            for nm, idx in jnames.items():
                if re.search(pat, nm, re.I):
                    return idx
            return -1
        key = {
            'hips': find(r'hip'),
            'Lfoot': find(r'L_foot'), 'Rfoot': find(r'R_foot'),
            'Lwrist': find(r'L_wrist'), 'Rwrist': find(r'R_wrist'),
            'head': find(r'head'),
        }

        anim = g['animations'][0]
        chans = build_samplers(g, buffers, anim)
        dur = 0.0
        for c in anim['channels']:
            s = anim['samplers'][c['sampler']]
            t = read_accessor(g, buffers, s['input'])
            dur = max(dur, float(t[-1]))

        fps = 30.0
        nf = int(dur * fps) + 1
        defaults = [node_local_default(n) for n in nodes]

        # world position of each key joint per frame
        kp = {k: np.zeros((nf, 3), dtype=np.float32) for k in key}
        for f in range(nf):
            t = f / fps
            world = [None] * N
            for i in order:
                if i in chans and any(p in chans[i] for p in ('translation', 'rotation', 'scale')):
                    ch = chans[i]
                    if 'translation' in ch:
                        tr = sample_channel(*ch['translation'], t)
                    else:
                        tr = nodes[i].get('translation', [0, 0, 0])
                    if 'rotation' in ch:
                        ro = sample_channel(*ch['rotation'], t)
                    else:
                        ro = nodes[i].get('rotation', [0, 0, 0, 1])
                    if 'scale' in ch:
                        sc = sample_channel(*ch['scale'], t)
                    else:
                        sc = nodes[i].get('scale', [1, 1, 1])
                    local = trs(np.asarray(tr, np.float32), np.asarray(ro, np.float32), np.asarray(sc, np.float32))
                else:
                    local = defaults[i]
                world[i] = local if parent[i] < 0 else world[parent[i]] @ local
            for k, idx in key.items():
                if idx >= 0:
                    kp[k][f] = world[idx][:3, 3]

        # ---- signals ----
        def speed(arr):
            d = np.linalg.norm(np.diff(arr, axis=0), axis=1)
            return np.concatenate([[d[0]], d]) if len(d) else np.zeros(nf)
        # overall motion energy = sum of key joint speeds
        energy = np.zeros(nf)
        for k in key:
            if key[k] >= 0:
                energy += speed(kp[k])
        # normalize per scene scale using model height
        scale_ref = max(1e-3, float(np.percentile(kp['head'][:, 1] - kp['hips'][:, 1], 50)) if key['head'] >= 0 and key['hips'] >= 0 else 1.0)
        energyN = energy / scale_ref
        # smooth
        def smooth(x, w=5):
            k = np.ones(w) / w
            return np.convolve(x, k, mode='same')
        e = smooth(energyN, 7)

        # boundary detection: low-energy valleys
        thr = max(0.02, np.percentile(e, 20))
        low = e < thr
        # group contiguous frames; boundaries = midpoints of low runs
        bounds = [0]
        i = 0
        while i < nf:
            if low[i]:
                j = i
                while j < nf and low[j]:
                    j += 1
                mid = (i + j) // 2
                if mid - bounds[-1] > int(0.4 * fps):  # min segment 0.4s
                    bounds.append(mid)
                i = j
            else:
                i += 1
        if bounds[-1] < nf - 1:
            bounds.append(nf - 1)

        print('================= %s  dur=%.2fs frames=%d clip=%r =================' % (name, dur, nf, anim.get('name')))
        print('  height(head-hips)=%.1f  energy mean=%.3f thr=%.3f' % (scale_ref, e.mean(), thr))
        segs = []
        for s, en in zip(bounds[:-1], bounds[1:]):
            if en - s < int(0.4 * fps):
                continue
            seg = slice(s, en)
            me = float(e[seg].mean())
            # walk: alternating feet => foot height difference oscillates
            fy = kp['Lfoot'][seg, 1] - kp['Rfoot'][seg, 1]
            fy = fy - fy.mean()
            zc = int(((fy[:-1] * fy[1:]) < 0).sum())  # zero crossings -> step cadence
            foot_osc = float(np.std(fy) / scale_ref)
            # forward travel of hips (root locomotion)
            travel = float(np.linalg.norm(kp['hips'][en - 1] - kp['hips'][s]) / scale_ref)
            # attack: wrist reaches far from hips, transient
            lr = np.linalg.norm(kp['Lwrist'][seg] - kp['hips'][seg], axis=1) / scale_ref
            rr = np.linalg.norm(kp['Rwrist'][seg] - kp['hips'][seg], axis=1) / scale_ref
            reach = float(max(lr.max() if len(lr) else 0, rr.max() if len(rr) else 0))
            reach_var = float(max(lr.std() if len(lr) else 0, rr.std() if len(rr) else 0))
            hips_drop = float((kp['hips'][s, 1] - kp['hips'][en - 1, 1]) / scale_ref)
            segs.append(dict(s=s, e=en, t0=s / fps, t1=en / fps, energy=me, zc=zc,
                             foot_osc=foot_osc, travel=travel, reach=reach,
                             reach_var=reach_var, hips_drop=hips_drop))
        # classify
        for sg in segs:
            label = 'idle'
            if sg['energy'] < 0.04:
                label = 'idle'
            elif sg['foot_osc'] > 0.04 and sg['zc'] >= 3:
                label = 'walk/run'
            elif sg['reach_var'] > 0.06 or sg['reach'] > 0.9:
                label = 'attack'
            if sg['hips_drop'] > 0.5:
                label = 'death?'
            sg['label'] = label
        for sg in segs:
            print('  [%5.2f - %5.2f]s  %-9s energy=%.3f footOsc=%.3f zc=%2d travel=%.2f reach=%.2f reachVar=%.3f hipsDrop=%.2f'
                  % (sg['t0'], sg['t1'], sg['label'], sg['energy'], sg['foot_osc'], sg['zc'],
                     sg['travel'], sg['reach'], sg['reach_var'], sg['hips_drop']))


if __name__ == '__main__':
    main()
