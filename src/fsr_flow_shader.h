// Motion vectors measured from the captured frames themselves, for the temporal FSR output scaling (Upscale method 4).
// Two compute passes: 1) every 4x4 block finds where its 8x8 neighbourhood was in the frame before (block matching on brightness),
// 2) every pixel picks, from the four nearest blocks' vectors and "not moving", the one under which its 3x3 surroundings match best,
// so motion follows object edges and a HUD that stays put stays put. Also writes a flat depth (FSR wants one; the capture has none).
// The vectors point from a pixel in this frame to where it was in the frame before, in pixels (FSR's convention, motionVectorScale 1).
#pragma once
static const char kFsrFlowHlsl[] = R"HLSL(
Texture2D<float4> gCur : register(t0);
Texture2D<float4> gPrev : register(t1);
Texture2D<float2> gBlk : register(t2);
RWTexture2D<float2> uBlk : register(u0);
RWTexture2D<float2> uMv : register(u1);
RWTexture2D<float> uDepth : register(u2);
cbuffer C : register(b0) { uint gW; uint gH; uint gBW; uint gBH; };

float LumaCur(int2 p)
{
    p = clamp(p, int2(0, 0), int2(gW - 1, gH - 1));
    return dot(gCur.Load(int3(p, 0)).rgb, float3(0.299, 0.587, 0.114));
}
float LumaPrev(int2 p)
{
    p = clamp(p, int2(0, 0), int2(gW - 1, gH - 1));
    return dot(gPrev.Load(int3(p, 0)).rgb, float3(0.299, 0.587, 0.114));
}

// ---- pass 1: one thread per 4x4 block, 8x8 window, coarse search (step 2, +-16) then +-1
float BlockCost(float win[64], int2 org, int2 d)
{
    float s = 0.0;
    [unroll] for (int j = 0; j < 8; ++j)
        [unroll] for (int i = 0; i < 8; ++i)
            s += abs(win[j * 8 + i] - LumaPrev(org + int2(i, j) + d));
    return s + 0.12 * length(float2(d));
}

[numthreads(8, 8, 1)]
void CSBlock(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gBW || id.y >= gBH) return;
    const int2 org = int2(id.xy) * 4 - 2;
    float win[64];
    [unroll] for (int j = 0; j < 8; ++j)
        [unroll] for (int i = 0; i < 8; ++i)
            win[j * 8 + i] = LumaCur(org + int2(i, j));

    int2 best = int2(0, 0);
    float bestCost = BlockCost(win, org, best);
    for (int dy = -16; dy <= 16; dy += 2)
        for (int dx = -16; dx <= 16; dx += 2)
        {
            if (dx == 0 && dy == 0) continue;
            const float c = BlockCost(win, org, int2(dx, dy));
            if (c < bestCost) { bestCost = c; best = int2(dx, dy); }
        }
    const int2 centre = best;
    [unroll] for (int ry = -1; ry <= 1; ++ry)
        [unroll] for (int rx = -1; rx <= 1; ++rx)
        {
            if (rx == 0 && ry == 0) continue;
            const int2 d = centre + int2(rx, ry);
            const float c = BlockCost(win, org, d);
            if (c < bestCost) { bestCost = c; best = d; }
        }
    uBlk[id.xy] = float2(best);
}

// ---- pass 2: one thread per pixel
float PixelCost(int2 p, float2 mv)
{
    const int2 d = int2(round(mv));
    float s = 0.0;
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i)
            s += abs(LumaCur(p + int2(i, j)) - LumaPrev(p + int2(i, j) + d));
    return s + 0.02 * length(mv);
}

[numthreads(8, 8, 1)]
void CSPixel(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    const int2 p = int2(id.xy);
    // the four blocks nearest to this pixel (block centres lie at 4 * b + 2)
    const int2 b0 = int2(floor((float2(p) - 2.0) / 4.0));
    float2 best = float2(0.0, 0.0);
    float bestCost = PixelCost(p, best);
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
        {
            const int2 b = clamp(b0 + int2(i, j), int2(0, 0), int2(gBW - 1, gBH - 1));
            const float2 mv = gBlk.Load(int3(b, 0));
            const float c = PixelCost(p, mv);
            if (c < bestCost) { bestCost = c; best = mv; }
        }
    uMv[id.xy] = round(best);
    uDepth[id.xy] = 0.5;
}
)HLSL";
