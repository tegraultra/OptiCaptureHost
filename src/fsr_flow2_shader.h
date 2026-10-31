// OPTIONAL experiment ("Motion 2", [Upscale] Motion2=1 in OptiCaptureHost.ini): a steadier second pass for the FSR3/4 / DLSS motion vectors.
// It replaces only the per-pixel pass (everything from "// ---- pass 2" in fsr_flow_shader.h); the block search is the same.
// Why: the temporal upscalers get no camera jitter from a capture, so wrong vectors on repeating textures (floor tiles, grass) are not hidden
// by anything: each block picks its own best match, neighbouring blocks disagree by a pixel or so, and the history shimmers.
//   - the vector median of the 3x3 blocks around the pixel is a candidate, and every candidate pays a small price for being far from it
//     (neighbouring pixels of one surface then agree);
//   - the match cost looks at a 5x5 neighbourhood instead of 3x3 (less fooled by repeating texture);
//   - where the surrounding blocks disagree a lot (the vector is a guess), the reactive mask is raised so the upscaler trusts this frame more.
// To remove it: delete this file, its #include and the two lines marked "Motion2" in main.cpp / settings.h.
#pragma once
static const char kFsrFlow2Pass2[] = R"HLSL(
// ---- pass 2 (Motion 2): one thread per pixel
float PixelCost(int2 p, float2 mv)
{
    const int2 d = int2(round(mv));
    float s = 0.0;
    [unroll] for (int j = -2; j <= 2; ++j)
        [unroll] for (int i = -2; i <= 2; ++i)
            s += abs(LumaCur(p + int2(i, j)) - LumaPrev(p + int2(i, j) + d));
    return s * (9.0 / 25.0); // the same scale as the 3x3 cost
}

float2 BlockVector(int2 b)
{
    b = clamp(b, int2(0, 0), int2(int(gBW) - 1, int(gBH) - 1));
    return gBlk.Load(int3(b, 0));
}

float Median9(float v[9])
{
    [unroll] for (int i = 1; i < 9; ++i)
    {
        const float key = v[i];
        int j = i - 1;
        [loop] while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; --j; }
        v[j + 1] = key;
    }
    return v[4];
}

[numthreads(8, 8, 1)]
void CSPixel(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    const int2 p = int2(id.xy);
    const float2 bf = (float2(p) - 2.0) / 4.0;
    const int2 b0 = int2(floor(bf));
    const float2 bt = saturate(bf - float2(b0));

    // the blocks around: the 3x3 centred on the nearest one
    const int2 bc = int2(round(bf));
    float mx[9], my[9];
    [unroll] for (int k = 0; k < 9; ++k)
    {
        const float2 v = BlockVector(bc + int2(k % 3 - 1, k / 3 - 1));
        mx[k] = v.x;
        my[k] = v.y;
    }
    float2 vmin = float2(mx[0], my[0]);
    float2 vmax = vmin;
    [unroll] for (int q = 1; q < 9; ++q)
    {
        vmin = min(vmin, float2(mx[q], my[q]));
        vmax = max(vmax, float2(mx[q], my[q]));
    }
    const float spread = length(vmax - vmin);   // how much the surrounding blocks disagree
    const float2 med = float2(Median9(mx), Median9(my));

    const float2 mv00 = BlockVector(b0);
    const float2 mv10 = BlockVector(b0 + int2(1, 0));
    const float2 mv01 = BlockVector(b0 + int2(0, 1));
    const float2 mv11 = BlockVector(b0 + int2(1, 1));
    const float2 blend = lerp(lerp(mv00, mv10, bt.x), lerp(mv01, mv11, bt.x), bt.y);

    float2 cands[6];
    cands[0] = mv00;
    cands[1] = mv10;
    cands[2] = mv01;
    cands[3] = mv11;
    cands[4] = blend;
    cands[5] = med;

    float2 best = float2(0.0, 0.0);
    float bestScore = PixelCost(p, best) + 0.10 * length(med);   // "not moving" also pays for being far from its neighbours
    float bestCost = PixelCost(p, best);
    [unroll] for (int c = 0; c < 6; ++c)
    {
        const float cost = PixelCost(p, cands[c]);
        const float score = cost + 0.10 * length(cands[c] - med) + 0.015 * length(cands[c]);
        if (score < bestScore) { bestScore = score; bestCost = cost; best = cands[c]; }
    }
    uMv[id.xy] = best;
    uDepth[id.xy] = 0.5;
    const float sad = max(bestCost, 0.0);
    float react = 0.8 * saturate((sad / 9.0 - 0.04) / 0.15);
    react = max(react, 0.5 * saturate((spread - 1.5) / 4.0)); // the vector is a guess: lean on this frame
#ifdef OC_MOTION_REACT
    // Motion3: the faster a pixel moves, the more the upscaler leans on this frame (FSR 3.1 smears its history over the whole picture in a turn)
    react = max(react, 0.95 * saturate((length(best) - 1.5) / 6.0));
#endif
    uReact[id.xy] = react;
}
)HLSL";
