// GPU HUD. Screen-space textured quads + glyph runs, drawn onto the tonemapped
// LDR target with alpha blending. Vertices are pulled from a bindless structured
// buffer; solid primitives point their UV at a white texel in the font atlas, so
// text and solid shapes share one pass. Colours are display-space (drawn after
// the AgX tonemap).

struct UiVertex {
    float2 pos;     // screen pixels
    float2 uv;      // font atlas
    uint   rgba;    // R | G<<8 | B<<16 | A<<24
};

struct Push {
    uint  vbIndex;
    uint  atlasIndex;
    float invW;
    float invH;
};
ConstantBuffer<Push> pc : register(b0);

SamplerState sampLinear : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    StructuredBuffer<UiVertex> verts = ResourceDescriptorHeap[pc.vbIndex];
    UiVertex v = verts[vid];

    VSOut o;
    o.pos = float4(v.pos.x * pc.invW * 2.0 - 1.0, 1.0 - v.pos.y * pc.invH * 2.0, 0.0, 1.0);
    o.uv = v.uv;
    o.col = float4((v.rgba & 0xFF) / 255.0,
                   ((v.rgba >> 8) & 0xFF) / 255.0,
                   ((v.rgba >> 16) & 0xFF) / 255.0,
                   ((v.rgba >> 24) & 0xFF) / 255.0);
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    Texture2D<float4> atlas = ResourceDescriptorHeap[pc.atlasIndex];
    // UI/font atlas: sampled from a no-mip, high-resolution bake. A mild contrast
    // curve removes the gray fringe that made screenshots look like upscaled bitmap
    // text while preserving the anti-aliased edge.
    float coverage = atlas.Sample(sampLinear, i.uv).r;
    coverage = saturate((coverage - 0.5) * 1.28 + 0.5);
    return float4(i.col.rgb, i.col.a * coverage);
}
