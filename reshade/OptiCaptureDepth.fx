// OptiCaptureDepth.fx  -  by MK-JH  -  https://t.me/MKJH_APPS
//
// Writes the game's depth, linearised by ReShade (0 = near, 1 = far), into the texture OC_LinearDepth.
// The add-on OptiCaptureDepth.addon64 shares that texture with OptiCaptureHost. It draws nothing on the screen.
//
// Put this file into ReShade's "Shaders" folder and tick "OptiCaptureDepth" on in ReShade's menu. Check ReShade's depth
// settings with its own "DisplayDepth" effect first (reversed / upside down / logarithmic), they apply here as well.

#include "ReShade.fxh"

texture OC_LinearDepth { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R32F; };

float PS_OptiCaptureDepth(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    return ReShade::GetLinearizedDepth(texcoord);
}

technique OptiCaptureDepth
<
    ui_tooltip = "Shares the game's depth with OptiCaptureHost (needs OptiCaptureDepth.addon64). Draws nothing.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_OptiCaptureDepth;
        RenderTarget = OC_LinearDepth;
    }
}
