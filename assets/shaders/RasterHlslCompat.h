#ifndef RASTERHLSLCOMPAT_H
#define RASTERHLSLCOMPAT_H

#ifdef HLSL
#include "HlslCompat.h"
#define RASTER_PIPELINE
#else
using namespace DirectX;
#endif

#include "CommonHlslCompat.h"

struct PhotonSplattingOptions
{
    float kernelScaleMin;
    float kernelScaleMax;
    float uniformScaleStrength;
    float maxLightShapingScale;
    float kernelCompressFactor;
    float volumeSplatPhotonSize;
};

struct HybridPipelineOptions
{
    PhotonSplattingOptions photonSplat;
    UINT useRaytracedVolumeSplatting;
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
    XMFLOAT3 padding;
};

#endif // RASTERHLSLCOMPAT_H
