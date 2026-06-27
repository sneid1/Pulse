// PULSE dist asset bake: shrink shipped *.glb to a normal game size.
//
// Every Meshy GLB ships as a raw generator dump - ~95% of each file is embedded
// 4K PNG textures (basecolor/normal/metallic-roughness/emissive). This pass runs
// each GLB through gltf-transform `optimize` with every loader-incompatible step
// turned OFF (the engine's glTF reader is stb_image PNG/JPEG only: no Draco,
// meshopt, KTX2/Basis, GPU instancing, or sparse accessors). What stays on:
// prune + dedup + texture downscale-to-cap + re-encode in the SAME format. Skins,
// animation clips, and mesh topology are preserved untouched.
//
// Usage:  node tools/assets/glb-bake/bake.mjs <root-dir>
//   walks <root-dir> for *.glb and optimizes each in place.
//
// No fallback: if gltf-transform is missing, or any file fails to optimize, the
// bake aborts non-zero. It never copies an un-optimized file through silently
// (matches the repo rule: fail loudly, never silently substitute).

import { promises as fs } from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';

const execFileP = promisify(execFile);

// Texture dimension caps (max px on the long edge) by path substring. First match
// wins; order matters. Bosses + first-person weapons are shown large/close, so they
// keep more detail than regular enemies and background environment props.
const CAPS = [
  { match: 'pulse_boss_concepts', cap: 2048 }, // boss models (large, showcased)
  { match: 'bumstrum',            cap: 2048 }, // FPS viewmodels (very close)
  { match: 'rigged_concepts',     cap: 1024 }, // regular enemies
  { match: 'pulse_enemy_concepts',cap: 1024 }, // regular enemies (raw-input variants still shipped)
];
const DEFAULT_CAP = 1024;                       // environment props, everything else

// gltf-transform passes that the engine's loader cannot consume are forced off.
const SAFE_FLAGS = [
  '--compress', 'false',          // no Draco / meshopt geometry compression
  '--simplify', 'false',          // keep original silhouettes
  '--weld', 'false',              // keep vertex topology
  '--flatten', 'false',           // keep node graph
  '--join', 'false',              // keep submeshes separate
  '--instance', 'false',          // no EXT_mesh_gpu_instancing
  '--palette', 'false',           // no palette texture material merge
  '--sparse', 'false',            // no sparse accessors
  '--resample', 'false',          // leave animation sampling untouched
  '--prune-solid-textures', 'false', // keep textures the materials reference
  '--texture-compress', 'auto',   // re-encode in the ORIGINAL format (PNG stays PNG)
];

const CONCURRENCY = Math.max(2, Math.min(8, os.cpus().length - 1));

function capFor(file) {
  const norm = file.replace(/\\/g, '/');
  for (const c of CAPS) if (norm.includes(c.match)) return c.cap;
  return DEFAULT_CAP;
}

async function walkGlb(dir, out) {
  const entries = await fs.readdir(dir, { withFileTypes: true });
  for (const e of entries) {
    const full = path.join(dir, e.name);
    if (e.isDirectory()) await walkGlb(full, out);
    else if (e.isFile() && e.name.toLowerCase().endsWith('.glb')
             && !e.name.toLowerCase().endsWith('.bake.glb')) out.push(full);
  }
}

async function sizeOf(file) {
  try { return (await fs.stat(file)).size; } catch { return 0; }
}

async function bakeOne(file) {
  const cap = capFor(file);
  // The temp MUST keep a .glb extension: gltf-transform selects the output container
  // by extension, and a non-glTF extension makes it emit a text .gltf stub with the
  // buffers/images spilled to sidecar files (a near-empty .glb is the symptom).
  const tmp = file.replace(/\.glb$/i, '') + '.bake.glb';
  const before = await sizeOf(file);
  // gltf-transform resolves on PATH; shell:true lets Windows find the .cmd shim.
  await execFileP('gltf-transform',
    ['optimize', file, tmp, ...SAFE_FLAGS, '--texture-size', String(cap)],
    { shell: true, maxBuffer: 64 * 1024 * 1024 });
  const after = await sizeOf(tmp);
  if (after <= 0) throw new Error(`optimize produced empty output for ${file}`);
  await fs.rename(tmp, file);
  return { file, cap, before, after };
}

async function main() {
  const root = process.argv[2];
  if (!root) { console.error('usage: node bake.mjs <root-dir>'); process.exit(2); }

  // Require the tool up front; fail loud if absent (no fallback path).
  try { await execFileP('gltf-transform', ['--version'], { shell: true }); }
  catch { console.error('[bake] gltf-transform not found. Run: npm install -g @gltf-transform/cli'); process.exit(2); }

  const files = [];
  await walkGlb(root, files);
  if (files.length === 0) { console.error(`[bake] no .glb under ${root}`); process.exit(2); }
  files.sort((a, b) => a.localeCompare(b));
  console.log(`[bake] ${files.length} GLB(s) under ${root}, concurrency ${CONCURRENCY}`);

  let totalBefore = 0, totalAfter = 0, done = 0, failed = 0;
  let next = 0;
  async function worker() {
    while (next < files.length) {
      const i = next++;
      const f = files[i];
      try {
        const r = await bakeOne(f);
        totalBefore += r.before; totalAfter += r.after; done++;
        const pct = r.before > 0 ? Math.round((1 - r.after / r.before) * 100) : 0;
        console.log(`  [${done + failed}/${files.length}] ${path.relative(root, f)}  `
          + `${(r.before / 1e6).toFixed(1)}MB -> ${(r.after / 1e6).toFixed(1)}MB (-${pct}%, ${r.cap}px)`);
      } catch (err) {
        failed++;
        console.error(`  [FAIL] ${path.relative(root, f)}: ${err.message || err}`);
      }
    }
  }
  await Promise.all(Array.from({ length: CONCURRENCY }, worker));

  console.log(`[bake] done: ${done} ok, ${failed} failed.  `
    + `${(totalBefore / 1e9).toFixed(2)}GB -> ${(totalAfter / 1e9).toFixed(2)}GB`);
  if (failed > 0) { console.error(`[bake] ${failed} file(s) failed - aborting (no fallback).`); process.exit(1); }
}

main().catch((e) => { console.error('[bake] fatal:', e); process.exit(1); });
