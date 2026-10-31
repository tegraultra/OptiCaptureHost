// OPTIONAL experiment ("Motion 3", [Upscale] Motion3=1 in OptiCaptureHost.ini): multi-scale (coarse-to-fine) motion vectors for the
// FSR3/4 / DLSS output scaling. It replaces only the per-block search of fsr_flow_shader.h; the per-pixel pass is Motion 2's.
// Why: the old search looks at an 8x8 window and tries only a coarse grid of shifts (every 6th pixel); on repeating texture (floor tiles, grass)
// the best match is often a neighbouring repeat, and every block picks its own, so the vectors flicker from frame to frame and the temporal
// upscaler (which gets no camera jitter from a capture to hide it) shimmers. Here:
//   1) a brightness pyramid: half size (L1) and quarter size (L2), made every frame for the new frame (the old frame's is kept);
//   2) L2: every 4x4 block (a 32x32 area of the real picture, enough context to tell one tile repeat from the next) tries EVERY shift within
//      +-8 (+-32 real pixels);
//   3) L1: each block starts from the vectors of the coarser blocks around it (their blend, their 3x3 median) and refines by +-2 with a
//      small price for leaving the surroundings' guess (neighbours agree);
//   4) the real picture: the same again with the final 8x8 windows, then a sub-pixel (parabola) step. Static blocks (HUD) stay zero.
// To remove it: delete this file, its #include, and everything marked "Motion3" in main.cpp / settings.h.
#pragma once
static const char kFsrFlow3Hlsl[] = R"HLSL(
cbuffer C : register(b0) { uint2 outSize; uint2 srcSize; uint2 parSize; uint2 padC; };

#if defined(PASS_LUMA1) || defined(PASS_BLOCK)
Texture2D<float4> tCur : register(t0);
Texture2D<float4> tPrev : register(t1);
float Cur(int2 p) { p = clamp(p, int2(0, 0), int2(srcSize) - 1); return dot(tCur.Load(int3(p, 0)).rgb, float3(0.299, 0.587, 0.114)); }
float Prev(int2 p) { p = clamp(p, int2(0, 0), int2(srcSize) - 1); return dot(tPrev.Load(int3(p, 0)).rgb, float3(0.299, 0.587, 0.114)); }
#else
Texture2D<float> tCur : register(t0);
Texture2D<float> tPrev : register(t1);
float Cur(int2 p) { p = clamp(p, int2(0, 0), int2(srcSize) - 1); return tCur.Load(int3(p, 0)); }
float Prev(int2 p) { p = clamp(p, int2(0, 0), int2(srcSize) - 1); return tPrev.Load(int3(p, 0)); }
#endif
Texture2D<float2> tPar : register(t2);

#if defined(PASS_LUMA1) || defined(PASS_LUMA2)
RWTexture2D<float> uL : register(u0);
[numthreads(8, 8, 1)]
void CSLuma(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    const int2 p = int2(id.xy) * 2;
    uL[id.xy] = 0.25 * (Cur(p) + Cur(p + int2(1, 0)) + Cur(p + int2(0, 1)) + Cur(p + int2(1, 1)));
}
#else
RWTexture2D<float2> uOut : register(u0);

// the mean difference of an 8x8 window (brightness 0..1) against the frame before, shifted by d
float Cost(float win[64], int2 org, int2 d)
{
    // a real loop over the rows (unrolled everywhere it is used, the shader took very long to compile)
    float s = 0.0;
    [loop] for (int j = 0; j < 8; ++j)
    {
        [unroll] for (int i = 0; i < 8; ++i)
            s += abs(win[j * 8 + i] - Prev(org + int2(i, j) + d));
    }
    return s * (1.0 / 64.0);
}
static const float kLambda = 0.0012; // price per pixel of straying from the surroundings' guess
static const float kStatic = 0.003;  // a block this unchanged is not searched (HUD, a still scene)

#if defined(PASS_SEARCH2)
// the coarsest size: every shift within +-8
[numthreads(8, 8, 1)]
void CSSearch2(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    const int2 org = int2(id.xy) * 4 - 2;
    float win[64];
    [loop] for (int j = 0; j < 8; ++j)
    {
        [unroll] for (int i = 0; i < 8; ++i)
            win[j * 8 + i] = Cur(org + int2(i, j));
    }
    const float zeroCost = Cost(win, org, int2(0, 0));
    if (zeroCost < kStatic) { uOut[id.xy] = float2(0.0, 0.0); return; }
    int2 best = int2(0, 0);
    float bestCost = zeroCost;
    [loop] for (int dy = -8; dy <= 8; ++dy)
    {
        [loop] for (int dx = -8; dx <= 8; ++dx)
        {
            if (dx == 0 && dy == 0) continue;
            const float c = Cost(win, org, int2(dx, dy)) + kLambda * length(float2(dx, dy));
            if (c < bestCost) { bestCost = c; best = int2(dx, dy); }
        }
    }
    uOut[id.xy] = float2(best);
}
#else
float2 Par(int2 b)
{
    b = clamp(b, int2(0, 0), int2(parSize) - 1);
    return tPar.Load(int3(b, 0));
}
float Med9(float a[9])
{
    [unroll] for (int i = 1; i < 9; ++i)
    {
        const float v = a[i];
        int j = i - 1;
        [loop] while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; --j; }
        a[j + 1] = v;
    }
    return a[4];
}

// L1 and the real picture: start from the coarser blocks' vectors (doubled), refine by +-2
[numthreads(8, 8, 1)]
void CSRefine(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    const int2 org = int2(id.xy) * 4 - 2;
    float win[64];
    [loop] for (int j = 0; j < 8; ++j)
    {
        [unroll] for (int i = 0; i < 8; ++i)
            win[j * 8 + i] = Cur(org + int2(i, j));
    }
    const float zeroCost = Cost(win, org, int2(0, 0));
    if (zeroCost < kStatic) { uOut[id.xy] = float2(0.0, 0.0); return; }

    // this block's centre lies at 4 * b + 2; the coarser block centres at 4 * B + 2 in half the pixels
    const float2 pf = (float2(id.xy) * 2.0 - 1.0) / 4.0;
    const int2 p0 = int2(floor(pf));
    const float2 pt = saturate(pf - float2(p0));
    const float2 v00 = Par(p0) * 2.0, v10 = Par(p0 + int2(1, 0)) * 2.0, v01 = Par(p0 + int2(0, 1)) * 2.0, v11 = Par(p0 + int2(1, 1)) * 2.0;
    const float2 blend = lerp(lerp(v00, v10, pt.x), lerp(v01, v11, pt.x), pt.y);
    // the vector median of the 3x3 coarse blocks around the nearest one
    const int2 pn = p0 + int2(round(pt));
    float mx[9], my[9];
    [loop] for (int k = 0; k < 9; ++k)
    {
        const float2 v = Par(pn + int2(k % 3 - 1, k / 3 - 1)) * 2.0;
        mx[k] = v.x; my[k] = v.y;
    }
    const float2 med = float2(Med9(mx), Med9(my));

    float2 cands[7];
    cands[0] = float2(0.0, 0.0);
    cands[1] = v00; cands[2] = v10; cands[3] = v01; cands[4] = v11;
    cands[5] = blend; cands[6] = med;
    int2 best = int2(0, 0);
    float bestScore = 1e9;
    [loop] for (int c = 0; c < 7; ++c)
    {
        const int2 d = int2(round(cands[c]));
        const float s = Cost(win, org, d) + kLambda * length(float2(d) - blend);
        if (s < bestScore) { bestScore = s; best = d; }
    }
    // two rounds of the eight neighbours: reaches +-2 around the best start
    [loop] for (int round2 = 0; round2 < 2; ++round2)
    {
        const int2 centre = best;
        [loop] for (int ry = -1; ry <= 1; ++ry)
        {
            [loop] for (int rx = -1; rx <= 1; ++rx)
            {
                if (rx == 0 && ry == 0) continue;
                const int2 d = centre + int2(rx, ry);
                const float s = Cost(win, org, d) + kLambda * length(float2(d) - blend);
                if (s < bestScore) { bestScore = s; best = d; }
            }
        }
    }
#if defined(PASS_BLOCK)
    // sub-pixel: a parabola through the cost at -1, 0, +1 along each axis (without the price)
    const float c0 = Cost(win, org, best);
    const float cxM = Cost(win, org, best + int2(-1, 0));
    const float cxP = Cost(win, org, best + int2(1, 0));
    const float cyM = Cost(win, org, best + int2(0, -1));
    const float cyP = Cost(win, org, best + int2(0, 1));
    const float denomX = 2.0 * (cxM + cxP - 2.0 * c0);
    const float denomY = 2.0 * (cyM + cyP - 2.0 * c0);
    const float subX = (abs(denomX) > 1e-5) ? clamp((cxM - cxP) / denomX, -0.45, 0.45) : 0.0;
    const float subY = (abs(denomY) > 1e-5) ? clamp((cyM - cyP) / denomY, -0.45, 0.45) : 0.0;
    uOut[id.xy] = float2(best) + float2(subX, subY);
#else
    uOut[id.xy] = float2(best);
#endif
}
#endif
#endif
)HLSL";
