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
//   4) the real picture: the OLD block search unchanged (fsr_flow_shader.h); the coarse vectors are only extra candidates that must win by 35%.
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
static const float kLambda = 0.0012; // price per pixel of straying from the surroundings' guess (the coarse search)
static const float kLambdaR = 0.0006; // the same, finer sizes: a different surface may move differently (grass against a rock)
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
    // a surface at another depth moves differently from the coarser blocks' guess (the ground against an object): a wider look around the
    // guess, every 4th pixel, up to +-12; it wins only where it matches clearly better than staying close to the guess
    {
        const int2 gc = int2(round(blend));
        [loop] for (int wy = -3; wy <= 3; ++wy)
        {
            [loop] for (int wx = -3; wx <= 3; ++wx)
            {
                if (wx == 0 && wy == 0) continue;
                const int2 d = gc + int2(wx, wy) * 4;
                const float s = Cost(win, org, d) + kLambdaR * length(float2(d) - blend);
                if (s < bestScore) { bestScore = s; best = d; }
            }
        }
    }
    // three rounds of the eight neighbours: reaches +-3 around the best start
    [loop] for (int round2 = 0; round2 < 3; ++round2)
    {
        const int2 centre = best;
        [loop] for (int ry = -1; ry <= 1; ++ry)
        {
            [loop] for (int rx = -1; rx <= 1; ++rx)
            {
                if (rx == 0 && ry == 0) continue;
                const int2 d = centre + int2(rx, ry);
                const float s = Cost(win, org, d) + kLambdaR * length(float2(d) - blend);
                if (s < bestScore) { bestScore = s; best = d; }
            }
        }
    }
#if defined(PASS_BLOCK)
    // sub-pixel: a parabola through the cost at -1, 0, +1 along each axis (without the price)
    float cs[5];
    [loop] for (int k2 = 0; k2 < 5; ++k2)
    {
        const int2 o = (k2 == 0) ? int2(0, 0) : (k2 == 1) ? int2(-1, 0) : (k2 == 2) ? int2(1, 0) : (k2 == 3) ? int2(0, -1) : int2(0, 1);
        cs[k2] = Cost(win, org, best + o);
    }
    const float c0 = cs[0], cxM = cs[1], cxP = cs[2], cyM = cs[3], cyP = cs[4];
    const float denomX = 2.0 * (cxM + cxP - 2.0 * c0);
    const float denomY = 2.0 * (cyM + cyP - 2.0 * c0);
    const float subX = (abs(denomX) > 1e-5) ? clamp((cxM - cxP) / denomX, -0.45, 0.45) : 0.0;
    const float subY = (abs(denomY) > 1e-5) ? clamp((cyM - cyP) / denomY, -0.45, 0.45) : 0.0;
    uOut[id.xy] = float2(best) + float2(subX, subY);
#else
    uOut[id.xy] = float2(best);
#endif
}

#if defined(PASS_BLOCK)
// The real picture: EXACTLY the search of fsr_flow_shader.h (coarse +-24 step 6, then 3, then 1, then sub-pixel; same cost and the same
// small price for motion), so it is as good as that one everywhere it was good. The coarse-size vectors (L1/L2 pyramid) are only extra
// candidates, and one replaces the old search's result only if it matches at least 35% better (repeating texture, a fast turn the +-24
// reach misses).
float BlockCostV1(float win[64], int2 org, int2 d) { return Cost(win, org, d) * 64.0 + 0.08 * length(float2(d)); }

[numthreads(8, 8, 1)]
void CSBlock3(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    const int2 org = int2(id.xy) * 4 - 2;
    float win[64];
    [loop] for (int j = 0; j < 8; ++j)
    {
        [unroll] for (int i = 0; i < 8; ++i)
            win[j * 8 + i] = Cur(org + int2(i, j));
    }
    int2 best = int2(0, 0);
    const float zeroCost = BlockCostV1(win, org, best);
    float bestCost = zeroCost;
    if (zeroCost < 0.15) { uOut[id.xy] = float2(0.0, 0.0); return; }

    [loop] for (int dy = -24; dy <= 24; dy += 6)
    {
        [loop] for (int dx = -24; dx <= 24; dx += 6)
        {
            if (dx == 0 && dy == 0) continue;
            const float c = BlockCostV1(win, org, int2(dx, dy));
            if (c < bestCost) { bestCost = c; best = int2(dx, dy); }
        }
    }
    int2 centre = best;
    [loop] for (int my = -1; my <= 1; ++my)
    {
        [loop] for (int mx = -1; mx <= 1; ++mx)
        {
            if (mx == 0 && my == 0) continue;
            const int2 d = centre + int2(mx, my) * 3;
            const float c = BlockCostV1(win, org, d);
            if (c < bestCost) { bestCost = c; best = d; }
        }
    }
    centre = best;
    [loop] for (int ry = -1; ry <= 1; ++ry)
    {
        [loop] for (int rx = -1; rx <= 1; ++rx)
        {
            if (rx == 0 && ry == 0) continue;
            const int2 d = centre + int2(rx, ry);
            const float c = BlockCostV1(win, org, d);
            if (c < bestCost) { bestCost = c; best = d; }
        }
    }

    // the coarse-size candidates (their vectors doubled): the four nearest blocks, their blend and their 3x3 median
    const float2 pf = (float2(id.xy) * 2.0 - 1.0) / 4.0;
    const int2 p0 = int2(floor(pf));
    const float2 pt = saturate(pf - float2(p0));
    const float2 v00 = Par(p0) * 2.0, v10 = Par(p0 + int2(1, 0)) * 2.0, v01 = Par(p0 + int2(0, 1)) * 2.0, v11 = Par(p0 + int2(1, 1)) * 2.0;
    const float2 blend = lerp(lerp(v00, v10, pt.x), lerp(v01, v11, pt.x), pt.y);
    const int2 pn = p0 + int2(round(pt));
    float mxs[9], mys[9];
    [loop] for (int k = 0; k < 9; ++k)
    {
        const float2 v = Par(pn + int2(k % 3 - 1, k / 3 - 1)) * 2.0;
        mxs[k] = v.x; mys[k] = v.y;
    }
    float2 cands[6];
    cands[0] = v00; cands[1] = v10; cands[2] = v01; cands[3] = v11; cands[4] = blend; cands[5] = float2(Med9(mxs), Med9(mys));
    const float oldBest = bestCost;
    int2 pick = best;
    float pickCost = bestCost;
    [loop] for (int c = 0; c < 6; ++c)
    {
        const int2 d = int2(round(cands[c]));
        const float cc = BlockCostV1(win, org, d);
        if (cc < pickCost) { pickCost = cc; pick = d; }
    }
    if (pickCost < 0.65 * oldBest)
    {
        // refine the winner by one pixel, then it is used
        best = pick;
        bestCost = pickCost;
        [loop] for (int qy = -1; qy <= 1; ++qy)
        {
            [loop] for (int qx = -1; qx <= 1; ++qx)
            {
                if (qx == 0 && qy == 0) continue;
                const int2 d = pick + int2(qx, qy);
                const float cc = BlockCostV1(win, org, d);
                if (cc < bestCost) { bestCost = cc; best = d; }
            }
        }
    }

    float cs[5];
    [loop] for (int k2 = 0; k2 < 5; ++k2)
    {
        const int2 o = (k2 == 0) ? int2(0, 0) : (k2 == 1) ? int2(-1, 0) : (k2 == 2) ? int2(1, 0) : (k2 == 3) ? int2(0, -1) : int2(0, 1);
        cs[k2] = BlockCostV1(win, org, best + o);
    }
    const float denomX = 2.0 * (cs[1] + cs[2] - 2.0 * cs[0]);
    const float denomY = 2.0 * (cs[3] + cs[4] - 2.0 * cs[0]);
    const float subX = (abs(denomX) > 1e-4) ? clamp((cs[1] - cs[2]) / denomX, -0.45, 0.45) : 0.0;
    const float subY = (abs(denomY) > 1e-4) ? clamp((cs[3] - cs[4]) / denomY, -0.45, 0.45) : 0.0;
    uOut[id.xy] = float2(best) + float2(subX, subY);
}
#endif
#endif
#endif
)HLSL";
