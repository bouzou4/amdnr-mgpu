// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
#pragma once

// Compiled at run time with the system d3dcompiler (cs_5_0)
inline constexpr const char kShaderSource[] = R"(
Texture2D<float4> Src : register(t0);
Texture2D<float4> Enh : register(t1);
Texture2D<float4> Fed : register(t2);
Texture2D<float4> Aux : register(t3); // motion vectors (Apply), HUD-less colour (ApplyTerminal), game exposure (Stats)
Texture2D<float4> Norm : register(t4); // Stats' output: x net units per scene unit, y share fed above the guard
RWTexture2D<float4> Dst : register(u0);
SamplerState Lin : register(s0);
SamplerState Pt : register(s1);

cbuffer C : register(b0)
{
    uint2 Size;        // threads: target size
    float2 SrcUvScale; // used rect / texture size of Src (Down)
    float2 MvUvScale;  // used rect / texture size of the motion vectors
    float2 MvToUv;     // raw motion vector -> uv offset to the previous frame
    float2 EnhUvScale;
    float2 FedUvScale;
    float Strength;
    uint UseMv;
    float UiLow;  // ApplyTerminal: back buffer - HUD-less difference where the UI mask starts / is full
    float UiHigh;
    uint Encoding; // Src / Aux: 0 linear, 1 sRGB, 2 PQ (ST 2084, 80 nits = 1.0), 3 scRGB display (linear, 80 nits = 1.0),
                   // 4 scene-linear HDR (exposure from Norm)
    float PaperWhite; // 2 and 3: linear value of paper white
    uint UseNorm;     // Norm bound (upscale placements): the guard can bypass Apply
    float ExpoMul;    // Stats: game exposure * ExpoMul = net units per scene unit (1 / pre-exposure); 0 = auto
    float ColorStrength; // 0: network luma on the game's chroma, 1: the network's colour edit as it is
};

static const float kGuard = 1.5; // fed median above this after mapping: bypass

float NetScale() { return Encoding == 4 ? Norm.Load(int3(0, 0, 0)).x : 1.0 / PaperWhite; }

float3 Finite(float3 c) { return all(isfinite(c)) ? max(c, 0) : 0; }

float3 ToLinear(float3 c)
{
    if (Encoding == 1)
        return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
    if (Encoding == 2)
    {
        float3 p = pow(saturate(c), 1.0 / 78.84375);
        return pow(max(p - 0.8359375, 0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125) * 125.0;
    }
    return c;
}

float3 FromLinear(float3 c)
{
    if (Encoding == 1)
        return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    if (Encoding == 2)
    {
        float3 y = pow(saturate(c / 125.0), 0.1593017578125);
        return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
    }
    return c;
}

// HDR (2, 3, 4): the network gets SDR-ish values, paper white (4: exposed 1.0) at 1, a C1 Reinhard shoulder above 0.8
float3 ToNet(float3 x)
{
    if (Encoding < 2)
        return x;
    float3 y = x * NetScale();
    float3 t = (y - 0.8) / 0.2;
    return y <= 0.8 ? y : 0.8 + 0.2 * t / (1 + t);
}

// Edit e (network space) on an encoded colour. HDR: additive in paper-white units up to the knee, the same relative
// change above it (the shoulder's inverse would blow highlights up); no edit, no change; negative scRGB kept
float3 AddEdit(float3 c, float3 e)
{
    float3 x = ToLinear(c);
    if (Encoding < 2)
        return FromLinear(max(x + e, 0));
    float3 xp = max(x, 0);
    float3 n = ToNet(xp);
    float3 y = n <= 0.8 ? max(xp + e / NetScale(), 0) : xp * max(1 + e / n, 0);
    return FromLinear(x + y - xp);
}
// Feed: box filter of Src's used rect into the network size, linear
[numthreads(8, 8, 1)]
void Down(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Size))
        return;
    float2 uv = (id.xy + 0.5) / Size * SrcUvScale;
    float2 q = SrcUvScale / Size * 0.25;
    float3 c = ToLinear(Src.SampleLevel(Lin, uv + float2(-q.x, -q.y), 0).rgb) +
               ToLinear(Src.SampleLevel(Lin, uv + float2(q.x, -q.y), 0).rgb) +
               ToLinear(Src.SampleLevel(Lin, uv + float2(-q.x, q.y), 0).rgb) +
               ToLinear(Src.SampleLevel(Lin, uv + float2(q.x, q.y), 0).rgb);
    Dst[id.xy] = float4(saturate(ToNet(Finite(c * 0.25))), 1);
}

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// One 16x16 group over a 64x64 grid of Src's used rect: the scene-to-net scale (game exposure or a smoothed
// auto-exposure, key 0.18) and the share of samples whose mapped luma is above kGuard, into Dst (1x1)
groupshared float gsLog[256];
groupshared float gsHigh[256];
groupshared float gsPrev;
[numthreads(16, 16, 1)]
void Stats(uint3 gt : SV_GroupThreadID)
{
    uint i = gt.y * 16 + gt.x;
    float l[16];
    float lsum = 0;
    for (uint k = 0; k < 16; k++)
    {
        float2 g = float2(gt.x * 4 + k % 4, gt.y * 4 + k / 4) + 0.5;
        l[k] = Luma(Finite(ToLinear(Src.SampleLevel(Pt, g / 64.0 * SrcUvScale, 0).rgb)));
        lsum += log(max(l[k], 1e-6));
    }
    gsLog[i] = lsum;
    if (i == 0)
        gsPrev = Dst[uint2(0, 0)].z;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128; s > 0; s >>= 1)
    {
        if (i < s)
            gsLog[i] += gsLog[i + s];
        GroupMemoryBarrierWithGroupSync();
    }
    float scale = 1, autoScale = 0;
    if (Encoding == 4)
    {
        autoScale = 0.18 / exp(gsLog[0] / 4096.0);
        if (gsPrev > 0 && isfinite(gsPrev))
            autoScale = exp(lerp(log(gsPrev), log(autoScale), 0.05));
        float game = Aux.Load(int3(0, 0, 0)).x * ExpoMul;
        scale = ExpoMul > 0 && isfinite(game) && game > 0 ? game : autoScale;
    }
    float high = 0;
    for (uint k2 = 0; k2 < 16; k2++)
        high += l[k2] * scale > kGuard ? 1 : 0;
    gsHigh[i] = high;
    GroupMemoryBarrierWithGroupSync();
    for (uint s2 = 128; s2 > 0; s2 >>= 1)
    {
        if (i < s2)
            gsHigh[i] += gsHigh[i + s2];
        GroupMemoryBarrierWithGroupSync();
    }
    if (i == 0)
        Dst[uint2(0, 0)] = float4(scale, gsHigh[0] / 4096.0, autoScale, 0);
}

// Network space edit at uv for colour c: below ColorStrength 1 part of it is the network's luma ratio on c's own chroma
float3 Edit(float2 uv, float3 c)
{
    float3 enh = Enh.SampleLevel(Lin, uv * EnhUvScale, 0).rgb, fed = Fed.SampleLevel(Lin, uv * FedUvScale, 0).rgb;
    float3 e = enh - fed;
    if (ColorStrength >= 1)
        return e;
    float r = clamp((Luma(enh) + 1e-3) / (Luma(fed) + 1e-3), 0, 4);
    return lerp(ToNet(max(ToLinear(c), 0)) * (r - 1), e, saturate(ColorStrength));
}

// The network's edit (Enh - Fed), moved by the motion vectors when it is a frame old, added to Src in linear
[numthreads(8, 8, 1)]
void Apply(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Size))
        return;
    float2 uv = (id.xy + 0.5) / Size;
    float4 c = Src[id.xy];
    float2 prev = uv;
    if (UseMv != 0)
        prev = uv + Aux.SampleLevel(Pt, uv * MvUvScale, 0).xy * MvToUv;
    float4 o = c;
    if (UseNorm != 0 && Norm.Load(int3(0, 0, 0)).y > 0.5)
        prev = -1; // guard: bypassed
    if (all(prev >= 0) && all(prev <= 1))
    {
        float3 e = Edit(prev, c.rgb);
        if (all(isfinite(e)) && all(isfinite(c.rgb)))
            o.rgb = AddEdit(c.rgb, Strength * e);
    }
    Dst[id.xy] = o;
}

// Terminal: the edit goes onto the back buffer where it matches the HUD-less colour; UI pixels stay as they are
[numthreads(8, 8, 1)]
void ApplyTerminal(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Size))
        return;
    float2 uv = (id.xy + 0.5) / Size;
    float4 b = Src[id.xy];
    float3 h = Aux[id.xy].rgb;
    float3 d = abs(b.rgb - h);
    float ui = smoothstep(UiLow, UiHigh, max(d.r, max(d.g, d.b)));
    float3 e = Edit(uv, b.rgb);
    float3 o = b.rgb;
    if (ui < 1 && all(isfinite(e)))
        o = lerp(AddEdit(b.rgb, Strength * e), b.rgb, ui);
    Dst[id.xy] = float4(o, b.a);
}
)";

struct ShaderConstants
{
    uint32_t sizeX, sizeY;
    float srcUvScaleX, srcUvScaleY;
    float mvUvScaleX, mvUvScaleY;
    float mvToUvX, mvToUvY;
    float enhUvScaleX, enhUvScaleY;
    float fedUvScaleX, fedUvScaleY;
    float strength;
    uint32_t useMv;
    float uiLow, uiHigh;
    uint32_t encoding;
    float paperWhite;
    uint32_t useNorm;
    float expoMul;
    float colorStrength;
};
static_assert(sizeof(ShaderConstants) == 84);

// Flicker meter, tools/seq_metrics.py on the GPU: per pixel of the feed, the change of the network's answer against
// the change of its input (display encoding), on still pixels and on visible moving pixels (warped with the vectors).
// Acc: still count, still |dO|, still |dI|, moving count, moving |dO|, moving |dI| (sums x kMeterScale, per group)
inline constexpr float kMeterScale = 1024.0f;
inline constexpr const char kMeterSource[] = R"(
Texture2D<float4> Fed : register(t0);
Texture2D<float4> Enh : register(t1);
Texture2D<float4> Prev : register(t2); // last pair: fed in [0, W), answer in [W, 2W)
Texture2D<float4> Mv : register(t3);
RWTexture2D<uint> Acc : register(u0);
cbuffer C : register(b0)
{
    uint2 Size;
    float2 MvExtent; // active size of the vectors
    float2 Unused0;
    float2 MvScale;  // raw vector -> feed pixels toward the previous frame
    float2 Unused1, Unused2;
    float Unused3;
    uint UseMv;
};
float3 E(float3 c) { return pow(saturate(c), 1.0 / 2.2); }
float Max3(float3 v) { return max(v.x, max(v.y, v.z)); }
float Mean3(float3 v) { return (v.x + v.y + v.z) / 3.0; }
float3 P(int2 p, uint side) { return E(Prev.Load(int3(p + int2(side * Size.x, 0), 0)).rgb); }
float3 Warp(float2 q, uint side)
{
    q = clamp(q, 0, float2(Size) - 1);
    int2 a = int2(floor(q));
    int2 b = min(a + 1, int2(Size) - 1);
    float2 f = q - a;
    return lerp(lerp(P(a, side), P(int2(b.x, a.y), side), f.x), lerp(P(int2(a.x, b.y), side), P(b, side), f.x), f.y);
}
groupshared float gs[6][64];
[numthreads(8, 8, 1)]
void Meter(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    float v[6] = { 0, 0, 0, 0, 0, 0 };
    if (id.x < Size.x && id.y < Size.y)
    {
        int2 p = int2(id.xy);
        float3 I = E(Fed.Load(int3(p, 0)).rgb), O = E(Enh.Load(int3(p, 0)).rgb);
        float3 Ip = P(p, 0), Op = P(p, 1);
        float2 d = 0;
        if (UseMv)
        {
            uint2 t = min(uint2((float2(p) + 0.5) * MvExtent / float2(Size)), uint2(MvExtent) - 1);
            d = Mv.Load(int3(t, 0)).xy * MvScale;
        }
        float mag = length(d);
        if (mag < 0.25 && Max3(abs(I - Ip)) < 0.06)
        {
            v[0] = 1; v[1] = Mean3(abs(O - Op)); v[2] = Mean3(abs(I - Ip));
        }
        float2 q = float2(p) + d;
        if (UseMv && mag >= 0.5 && all(q >= 0) && all(q <= float2(Size) - 1))
        {
            float3 wI = Warp(q, 0);
            if (Max3(abs(I - wI)) < 0.06)
            {
                v[3] = 1; v[4] = Mean3(abs(O - Warp(q, 1))); v[5] = Mean3(abs(I - wI));
            }
        }
    }
    [unroll] for (uint k = 0; k < 6; k++)
        gs[k][gi] = v[k];
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 32; s > 0; s >>= 1)
    {
        if (gi < s)
            [unroll] for (uint k = 0; k < 6; k++)
                gs[k][gi] += gs[k][gi + s];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi < 6)
    {
        uint o;
        InterlockedAdd(Acc[uint2(gi, 0)], uint(gs[gi][0] * (gi == 0 || gi == 3 ? 1.0 : 1024.0) + 0.5), o);
    }
}
)";
