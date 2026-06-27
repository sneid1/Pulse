// M1 clustered light culling. The view frustum is partitioned into a froxel grid
// (clusterDim X*Y screen tiles, Z log-distributed depth slices). One compute thread
// per cluster builds the cluster's world-space AABB by unprojecting its 8 NDC
// corners, tests every point light's sphere against it, and writes the overlapping
// light indices + a count. The deferred resolve then loops only the lights in the
// pixel's cluster instead of all of them (replacing the M1.0 brute force). Applies
// to both the raster and RT tiers.

struct LightData {
    float3 position; float intensity;
    float3 color;    float radius;
};

struct FrameCB {
    row_major float4x4 viewProj;
    row_major float4x4 viewProjCam;
    float3 sunDir;   float sunIntensity;
    float3 sunColor; float ambient;
    float  exposure; float3 clearColor;
    float3 fogColor; float fogDensity;
    float  nearZ;    float3 _pad;
    row_major float4x4 invViewProj;
    float3 cameraPos; uint lightCount;
    row_major float4x4 sunViewProj;
    row_major float4x4 viewProjNoJitter;
    row_major float4x4 prevViewProjNoJitter;
    row_major float4x4 viewProjCamNoJitter;
    row_major float4x4 prevViewProjCamNoJitter;
    uint clusterDimX, clusterDimY, clusterDimZ, clusterMaxPerCell;
    float zFar; float3 _cpad;
};

struct Push {
    uint frameCB;
    uint lightsIndex;
    uint gridCountUav;     // RWTexture2D<uint>  (cluster, 0) = light count
    uint gridIndicesUav;   // RWTexture2D<uint>  (cluster, i) = light index
};
ConstantBuffer<Push> pc : register(b0);

float3 unproject(float2 ndcXY, float ndcZ, row_major float4x4 invVP) {
    float4 wp = mul(float4(ndcXY, ndcZ, 1.0), invVP);
    return wp.xyz / max(wp.w, 1e-6);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    ConstantBuffer<FrameCB> f = ResourceDescriptorHeap[pc.frameCB];
    uint CX = f.clusterDimX, CY = f.clusterDimY, CZ = f.clusterDimZ;
    uint total = CX * CY * CZ;
    uint cluster = tid.x;
    if (cluster >= total) return;

    RWTexture2D<uint> gridCount   = ResourceDescriptorHeap[pc.gridCountUav];
    RWTexture2D<uint> gridIndices = ResourceDescriptorHeap[pc.gridIndicesUav];

    uint cz = cluster / (CX * CY);
    uint rem = cluster % (CX * CY);
    uint cy = rem / CX;
    uint cx = rem % CX;

    // Depth slice NDC planes (reverse-Z): view z grows log-distributed near->far.
    float zNearSlice = f.nearZ * pow(f.zFar / f.nearZ, float(cz)     / float(CZ));
    float zFarSlice  = f.nearZ * pow(f.zFar / f.nearZ, float(cz + 1) / float(CZ));
    float ndcZNear = f.nearZ / zNearSlice;   // closer slice -> larger ndcZ
    float ndcZFar  = f.nearZ / zFarSlice;

    // Tile NDC bounds.
    float2 ndcMin = float2(float(cx)     / CX, float(cy)     / CY) * 2.0 - 1.0;
    float2 ndcMax = float2(float(cx + 1) / CX, float(cy + 1) / CY) * 2.0 - 1.0;

    // Build the cluster's world-space AABB from its 8 unprojected corners.
    float3 bmin = float3(1e30, 1e30, 1e30);
    float3 bmax = float3(-1e30, -1e30, -1e30);
    [unroll] for (int i = 0; i < 8; ++i) {
        float2 cxy = float2((i & 1) ? ndcMax.x : ndcMin.x, (i & 2) ? ndcMax.y : ndcMin.y);
        float cz2 = (i & 4) ? ndcZFar : ndcZNear;
        float3 w = unproject(cxy, cz2, f.invViewProj);
        bmin = min(bmin, w); bmax = max(bmax, w);
    }

    uint count = 0;
    uint maxC = f.clusterMaxPerCell;
    StructuredBuffer<LightData> lights = ResourceDescriptorHeap[pc.lightsIndex];
    for (uint li = 0; li < f.lightCount && count < maxC; ++li) {
        LightData L = lights[li];
        float3 closest = clamp(L.position, bmin, bmax);
        float3 dv = L.position - closest;
        if (dot(dv, dv) <= L.radius * L.radius) {
            gridIndices[uint2(cluster, count)] = li;
            count++;
        }
    }
    gridCount[uint2(cluster, 0)] = count;
}
