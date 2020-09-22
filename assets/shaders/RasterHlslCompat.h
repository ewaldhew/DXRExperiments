#ifndef RASTERHLSLCOMPAT_H
#define RASTERHLSLCOMPAT_H

#ifdef HLSL
#include "HlslCompat.h"
#define RASTER_PIPELINE
#else
using namespace DirectX;
#endif

#include "CommonHlslCompat.h"

namespace SplatMethod {
    static const UINT
        Raster = 0,
        Raytrace = 1,
        Voxels = 2,
        COUNT = Voxels + 1;
}

struct HybridPipelineOptions
{
    UINT volumeSplattingMethod;
    UINT skipPhotonTracing;
    UINT showRawSplattingResult;
    UINT showVolumePhotonsOnly;
};

struct PerFrameConstantsRaster
{
    CameraParams cameraParams;
    HybridPipelineOptions options;
    XMMATRIX WorldToViewMatrix;
    XMMATRIX WorldToViewClipMatrix;
};

struct PerObjectConstants
{
    XMMATRIX worldMatrix;
    XMMATRIX invWorldMatrix;
    UINT isProcedural;
    UINT isVolume;
    XMFLOAT2 padding;
};

#endif // RASTERHLSLCOMPAT_H
