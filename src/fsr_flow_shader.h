// Motion vectors measured from the captured frames themselves, for the FSR3/4 output scaling (Upscale method 4).
// Improved with:
//   1) Hierarchical search up to +-32 pixels (handles fast camera pans without breaking)
//   2) Sub-pixel parabolic refinement + bilinear block blending (eliminates slow-pan stepping/judder)
//   3) Motion-based dynamic pseudo-depth (helps FSR3/4 & FG resolve foreground/HUD occlusion vs moving background)
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

// ---- pass 1: one thread per 4x4 block, 8x8 window, 3-step hierarchical search (+-32 range)
float BlockCost(float win[64], int2 org, int2 d)
{
    float s = 0.0;
    [unroll] for (int j = 0; j < 8; ++j)
        [unroll] for (int i = 0; i < 8; ++i)
            s += abs(win[j * 8 + i] - LumaPrev(org + int2(i, j) + d));
    // Gentle regularization to prefer zero/small motion on flat surfaces (sky, walls)
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
    float bestCost = BlockCost(win, org, best);

    // Early exit for completely static blocks (e.g. HUD elements)
    if (bestCost < 0.15)
    {
        uBlk[id.xy] = float2(0.0, 0.0);
        return;
    }

    // Step 1: Coarse search (+-24, step 4)
    for (int dy = -24; dy <= 24; dy += 4)
        for (int dx = -24; dx <= 24; dx += 4)
        {
            if (dx == 0 && dy == 0) continue;
            const float c = BlockCost(win, org, int2(dx, dy));
            if (c < bestCost) { bestCost = c; best = int2(dx, dy); }
        }

    // Step 2: Medium search around coarse best (+-4, step 2)
    int2 centre = best;
    [unroll] for (int my = -4; my <= 4; my += 2)
        [unroll] for (int mx = -4; mx <= 4; mx += 2)
        {
            if (mx == 0 && my == 0) continue;
            const int2 d = centre + int2(mx, my);
            const float c = BlockCost(win, org, d);
            if (c < bestCost) { bestCost = c; best = d; }
        }

    // Step 3: Fine search (+-1, step 1)
    centre = best;
    [unroll] for (int ry = -1; ry <= 1; ++ry)
        [unroll] for (int rx = -1; rx <= 1; ++rx)
        {
            if (rx == 0 && ry == 0) continue;
            const int2 d = centre + int2(rx, ry);
            const float c = BlockCost(win, org, d);
            if (c < bestCost) { bestCost = c; best = d; }
        }

    // Step 4: Sub-pixel parabolic refinement (1D along X and Y)
    float cxM = BlockCost(win, org, best + int2(-1, 0));
    float cxP = BlockCost(win, org, best + int2( 1, 0));
    float cyM = BlockCost(win, org, best + int2(0, -1));
    float cyP = BlockCost(win, org, best + int2(0,  1));

    float denomX = 2.0 * (cxM + cxP - 2.0 * bestCost);
    float denomY = 2.0 * (cyM + cyP - 2.0 * bestCost);
    float subX = (abs(denomX) > 1e-4) ? clamp((cxM - cxP) / denomX, -0.45, 0.45) : 0.0;
    float subY = (abs(denomY) > 1e-4) ? clamp((cyM - cyP) / denomY, -0.45, 0.45) : 0.0;

    uBlk[id.xy] = float2(best) + float2(subX, subY);
}

// ---- pass 2: one thread per pixel, sub-pixel candidate selection + dynamic pseudo-depth
float PixelCost(int2 p, float2 mv)
{
    const int2 d = int2(round(mv));
    float s = 0.0;
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i)
        {
            int2 pc = clamp(p + int2(i, j), int2(0, 0), int2(gW - 1, gH - 1));
            int2 pp = clamp(p + int2(i, j) + d, int2(0, 0), int2(gW - 1, gH - 1));
            float3 cCur = gCur.Load(int3(pc, 0)).rgb;
            float3 cPrev = gPrev.Load(int3(pp, 0)).rgb;
            s += dot(abs(cCur - cPrev), float3(0.299, 0.587, 0.114));
        }
    return s + 0.015 * length(mv);
}

[numthreads(8, 8, 1)]
void CSPixel(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    const int2 p = int2(id.xy);
    const float2 bf = (float2(p) - 2.0) / 4.0;
    const int2 b0 = int2(floor(bf));
    const float2 frac = saturate(bf - float2(b0));

    float2 best = float2(0.0, 0.0);
    float bestCost = PixelCost(p, best);

    float2 mv00 = gBlk.Load(int3(clamp(b0 + int2(0, 0), int2(0, 0), int2(gBW - 1, gBH - 1)), 0));
    float2 mv10 = gBlk.Load(int3(clamp(b0 + int2(1, 0), int2(0, 0), int2(gBW - 1, gBH - 1)), 0));
    float2 mv01 = gBlk.Load(int3(clamp(b0 + int2(0, 1), int2(0, 0), int2(gBW - 1, gBH - 1)), 0));
    float2 mv11 = gBlk.Load(int3(clamp(b0 + int2(1, 1), int2(0, 0), int2(gBW - 1, gBH - 1)), 0));

    // Check individual block vectors (preserves sharp object boundaries)
    float2 cands[5] = {
        mv00, mv10, mv01, mv11,
        // Bilinear blend candidate for smooth camera pans
        lerp(lerp(mv00, mv10, frac.x), lerp(mv01, mv11, frac.x), frac.y)
    };

    [unroll] for (int k = 0; k < 5; ++k)
    {
        const float c = PixelCost(p, cands[k]);
        if (c < bestCost)
        {
            bestCost = c;
            best = cands[k];
        }
    }

    // Output sub-pixel motion vector (NO round()!)
    uMv[id.xy] = best;

    // Dynamic Pseudo-Depth:
    // Static elements (HUD/crosshair, speed ~ 0) get closer depth (0.05) so FG never smears background over them.
    // Fast moving foreground objects get medium-near depth, background gets 0.5.
    float speed = length(best);
    float pseudoDepth = (speed < 0.25) ? 0.05 : clamp(0.5 - speed * 0.01, 0.1, 0.5);
    uDepth[id.xy] = pseudoDepth;
}
)HLSL";