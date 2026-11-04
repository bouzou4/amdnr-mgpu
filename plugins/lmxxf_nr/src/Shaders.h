// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
#pragma once

// Compiled at run time with the system d3dcompiler (cs_5_0)
inline constexpr const char kShaderSource[] = R"(
Texture2D<float4> Src : register(t0);
Texture2D<float4> Enh : register(t1);
Texture2D<float4> Fed : register(t2);
Texture2D<float4> Aux : register(t3); // motion vectors (Apply) or the HUD-less colour (ApplyTerminal)
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
    uint Encoding; // Src / Aux: 0 linear, 1 sRGB, 2 PQ (ST 2084, 80 nits = 1.0)
    uint Pad;
};

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
    Dst[id.xy] = float4(Finite(c * 0.25), 1);
}

float3 Edit(float2 uv)
{
    return Enh.SampleLevel(Lin, uv * EnhUvScale, 0).rgb - Fed.SampleLevel(Lin, uv * FedUvScale, 0).rgb;
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
    if (all(prev >= 0) && all(prev <= 1))
    {
        float3 e = Edit(prev);
        if (all(isfinite(e)) && all(isfinite(c.rgb)))
            o.rgb = FromLinear(max(ToLinear(c.rgb) + Strength * e, 0));
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
    float3 e = Edit(uv);
    float3 o = b.rgb;
    if (ui < 1 && all(isfinite(e)))
        o = lerp(FromLinear(max(ToLinear(b.rgb) + Strength * e, 0)), b.rgb, ui);
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
    uint32_t pad;
};
static_assert(sizeof(ShaderConstants) == 72);
