#ifndef RASTERHLSLCOMPAT_H
#define RASTERHLSLCOMPAT_H

#ifdef HLSL
#include "HlslCompat.h"
#define RASTER_PIPELINE
#else
using namespace DirectX;
#endif

#include "CommonHlslCompat.h"

struct PerFrameConstantsRaster
{
    CameraParams cameraParams;
    XMMATRIX WorldToViewMatrix;
    XMMATRIX WorldToViewClipMatrix;
};

struct PerObjectConstants
{
    XMMATRIX worldMatrix;
    XMMATRIX invWorldMatrix;
    UINT isProcedural;
    XMFLOAT3 padding;
};

#endif // RASTERHLSLCOMPAT_H
