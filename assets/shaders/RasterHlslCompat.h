#ifndef RASTERHLSLCOMPAT_H
#define RASTERHLSLCOMPAT_H

#ifdef HLSL
#include "HlslCompat.h"
#else
using namespace DirectX;
#endif

#include "CommonHlslCompat.h"

struct PerFrameConstantsRaster
{
    XMMATRIX WorldToViewMatrix;
    XMMATRIX WorldToViewClipMatrix;
};

#endif // RASTERHLSLCOMPAT_H
