// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "shader_common.hlsl"

cbuffer ConstBuffer : register(b0)
{
    float2 positionScale;
    // Smooth scrolling: every quad except the full-screen background is drawn shifted
    // by this many pixels. y is negative (content moves up) and always a whole pixel,
    // so glyphs keep landing on the same subpixel grid as before.
    float2 positionOffset;
}

// clang-format off
PSData main(VSData data)
// clang-format on
{
    PSData output;
    output.color = data.color;
    output.shadingType = data.shadingType;
    output.renditionScale = data.renditionScale;
    // positionScale is expected to be float2(2.0f / sizeInPixel.x, -2.0f / sizeInPixel.y). Together with the
    // addition below this will transform our "position" from pixel into normalized device coordinate (NDC) space.
    // The background quad covers the entire target and derives its cell from the screen
    // position, so it must not move; the pixel shader offsets its lookup instead.
    const float2 offset = data.shadingType == SHADING_TYPE_TEXT_BACKGROUND ? float2(0.0f, 0.0f) : positionOffset;
    output.position.xy = (data.position + offset + data.vertex.xy * data.size) * positionScale + float2(-1.0f, 1.0f);
    output.position.zw = float2(0, 1);
    output.texcoord = data.texcoord + data.vertex.xy * data.size;
    return output;
}
