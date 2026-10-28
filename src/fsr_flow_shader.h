// Motion vectors measured from the captured frames themselves, for the FSR3/4 output scaling (Upscale method 4).
// Two compute passes:
//   1) every 4x4 block finds where its 8x8 neighbourhood was in the frame before: coarse search (+-24, step 6), then step 3 and step 1
//      around the best, then a sub-pixel (parabola) refinement; blocks that did not change at all (a HUD) are not searched.
//   2) every pixel picks, from the four nearest blocks' vectors, their bilinear blend and "not moving", the one under which its 3x3
//      surroundings match best (sub-pixel vectors stay sub-pixel: smooth slow pans, no stepping). Also writes a flat depth.
// The vectors point from a pixel in this frame to where it was in the frame before, in pixels.
// (The search loops are real loops, not unrolled: the shader compiles in a moment and stays small.)
#pragma once
static const char kFsrFlowHlsl[] = R"HLSL(
Texture2D<float4> gCur : register(t0);
Texture2D<float4> gPrev : register(t1);
Texture2D<float2> gBlk : register(t2);
RWTexture2D<float2> uBlk : register(u0);
RWTexture2D<float2> uMv : register(u1);
RWTexture2D<float> uDepth : register(u2);
RWTexture2D<float> uReact : register(u3);
cbuffer C : register(b0) { uint gW; uint gH; uint gBW; uint gBH; };

float LumaCur(int2 p)
{
    p = clamp(p, int2(0, 0), int2(int(gW) - 1, int(gH) - 1));
    return dot(gCur.Load(int3(p, 0)).rgb, float3(0.299, 0.587, 0.114));
}
float LumaPrev(int2 p)
{
    p = clamp(p, int2(0, 0), int2(int(gW) - 1, int(gH) - 1));
    return dot(gPrev.Load(int3(p, 0)).rgb, float3(0.299, 0.587, 0.114));
}

// ---- pass 1: one thread per 4x4 block, 8x8 window
float BlockCost(float win[64], int2 org, int2 d)
{
    float s = 0.0;
    [unroll] for (int j = 0; j < 8; ++j)
        [unroll] for (int i = 0; i < 8; ++i)
            s += abs(win[j * 8 + i] - LumaPrev(org + int2(i, j) + d));
    // a small preference for little motion where the picture is flat (sky, walls)
    return s + 0.08 * length(float2(d));
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
    const float zeroCost = BlockCost(win, org, best);
    float bestCost = zeroCost;

    // a block that did not change at all (HUD, crosshair, a still scene) keeps a zero vector
    if (zeroCost < 0.15)
    {
        uBlk[id.xy] = float2(0.0, 0.0);
        return;
    }

    // coarse: +-24, step 6
    [loop] for (int dy = -24; dy <= 24; dy += 6)
    {
        [loop] for (int dx = -24; dx <= 24; dx += 6)
        {
            if (dx == 0 && dy == 0) continue;
            const float c = BlockCost(win, org, int2(dx, dy));
            if (c < bestCost) { bestCost = c; best = int2(dx, dy); }
        }
    }
    // medium: step 3 around the best, then step 1
    int2 centre = best;
    [loop] for (int my = -1; my <= 1; ++my)
    {
        [loop] for (int mx = -1; mx <= 1; ++mx)
        {
            if (mx == 0 && my == 0) continue;
            const int2 d = centre + int2(mx, my) * 3;
            const float c = BlockCost(win, org, d);
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
            const float c = BlockCost(win, org, d);
            if (c < bestCost) { bestCost = c; best = d; }
        }
    }

    // sub-pixel: a parabola through the cost at -1, 0, +1 along each axis
    const float cxM = BlockCost(win, org, best + int2(-1, 0));
    const float cxP = BlockCost(win, org, best + int2(1, 0));
    const float cyM = BlockCost(win, org, best + int2(0, -1));
    const float cyP = BlockCost(win, org, best + int2(0, 1));
    const float denomX = 2.0 * (cxM + cxP - 2.0 * bestCost);
    const float denomY = 2.0 * (cyM + cyP - 2.0 * bestCost);
    const float subX = (abs(denomX) > 1e-4) ? clamp((cxM - cxP) / denomX, -0.45, 0.45) : 0.0;
    const float subY = (abs(denomY) > 1e-4) ? clamp((cyM - cyP) / denomY, -0.45, 0.45) : 0.0;
    uBlk[id.xy] = float2(best) + float2(subX, subY);
}

// ---- pass 2: one thread per pixel
float PixelCost(int2 p, float2 mv)
{
    const int2 d = int2(round(mv));
    float s = 0.0;
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i)
            s += abs(LumaCur(p + int2(i, j)) - LumaPrev(p + int2(i, j) + d));
    return s + 0.015 * length(mv);
}

float2 BlockVector(int2 b)
{
    b = clamp(b, int2(0, 0), int2(int(gBW) - 1, int(gBH) - 1));
    return gBlk.Load(int3(b, 0));
}

[numthreads(8, 8, 1)]
void CSPixel(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    const int2 p = int2(id.xy);
    // the four blocks nearest to this pixel (block centres lie at 4 * b + 2)
    const float2 bf = (float2(p) - 2.0) / 4.0;
    const int2 b0 = int2(floor(bf));
    const float2 bt = saturate(bf - float2(b0));

    const float2 mv00 = BlockVector(b0);
    const float2 mv10 = BlockVector(b0 + int2(1, 0));
    const float2 mv01 = BlockVector(b0 + int2(0, 1));
    const float2 mv11 = BlockVector(b0 + int2(1, 1));
    const float2 blend = lerp(lerp(mv00, mv10, bt.x), lerp(mv01, mv11, bt.x), bt.y);

    float2 best = float2(0.0, 0.0);
    float bestCost = PixelCost(p, best);
    // single block vectors keep object edges sharp; the blend is for smooth pans
    float2 cands[5];
    cands[0] = mv00;
    cands[1] = mv10;
    cands[2] = mv01;
    cands[3] = mv11;
    cands[4] = blend;
    [unroll] for (int k = 0; k < 5; ++k)
    {
        const float c = PixelCost(p, cands[k]);
        if (c < bestCost) { bestCost = c; best = cands[k]; }
    }
    uMv[id.xy] = best; // sub-pixel, not rounded
    uDepth[id.xy] = 0.5; // the capture has no depth: flat, as before
    // Where even the best vector leaves the 3x3 surroundings unlike the frame before (uncovered background, effects, a wrong
    // estimate) the upscaler is told to lean on this frame instead of smearing its history (less ghosting and shimmer in a fast turn).
    const float sad = max(bestCost - 0.015 * length(best), 0.0);
    uReact[id.xy] = 0.8 * saturate((sad / 9.0 - 0.04) / 0.15);
}
)HLSL";
