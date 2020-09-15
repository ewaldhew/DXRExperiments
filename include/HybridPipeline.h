#pragma once

#include "Helpers/DirectXRaytracingHelper.h"
#include "utils/DirectXHelper.h"
#include "RasterHlslCompat.h"
#include "RaytracingHlslCompat.h"
#include "RaytracingPipeline.h"
#include "RtBindings.h"
#include "RtContext.h"
#include "RtProgram.h"
#include "RtScene.h"
#include "RtState.h"
#include "Camera.h"
#include "DescriptorHeap.h"
#include "GeometricModel.h"
#include <vector>
#include <random>

namespace GBufferID { enum Value { Normal = 0, Albedo, Depth, Count }; };

class HybridPipeline : public RaytracingPipeline
{
public:
    using SharedPtr = std::shared_ptr<HybridPipeline>;

    static SharedPtr create(DXRFramework::RtContext::SharedPtr context, DXGI_FORMAT outputFormat) { return SharedPtr(new HybridPipeline(context, outputFormat)); }
    virtual ~HybridPipeline();

    virtual void userInterface() override;
    virtual void update(float elAapsedTime, UINT elapsedFrames, UINT prevFrameIndex, UINT frameIndex, UINT width, UINT height) override;
    virtual void render(ID3D12GraphicsCommandList *commandList, UINT frameIndex, UINT width, UINT height, UINT& pass) override;

    virtual void loadResources(ID3D12CommandQueue *uploadCommandQueue, UINT frameCount) override;
    virtual void createOutputResource(DXGI_FORMAT format, UINT width, UINT height) override;
    virtual void buildAccelerationStructures() override;

    virtual void addMaterial(Material material) override { mMaterials.push_back(material); }
    virtual void setCamera(std::shared_ptr<Math::Camera> camera) override { mCamera = camera; }
    virtual void setScene(DXRFramework::RtScene::SharedPtr scene) override;

    virtual int getNumOutputs() override { return 1; }
    virtual ID3D12Resource *getOutputResource(UINT id) override { return mOutput.Resource.Get(); }
    virtual D3D12_GPU_DESCRIPTOR_HANDLE getOutputUavHandle(UINT id) override { return mOutputUav.gpuHandle; }
    virtual D3D12_GPU_DESCRIPTOR_HANDLE getOutputSrvHandle(UINT id) override { return mOutput.Srv.gpuHandle; }

    virtual bool *isActive() override { return &mActive; }
    virtual const char *getName() override { return "Hybrid Pipeline"; }
private:
    HybridPipeline(DXRFramework::RtContext::SharedPtr context, DXGI_FORMAT outputFormat);

    void collectEmitters(UINT& numLights, UINT& maxSamples);
    void buildVolumePhotonAccelerationStructure(UINT buildStrategy);

    // Pipeline components
    struct RtPass
    {
        DXRFramework::RtProgram::SharedPtr mRtProgram;
        DXRFramework::RtBindings::SharedPtr mRtBindings;
        DXRFramework::RtState::SharedPtr mRtState;
    };
    struct RasterPass
    {
        ComPtr<ID3D12RootSignature> rootSignature;
        ComPtr<ID3D12PipelineState> stateObject;
    };

    DXRFramework::RtContext::SharedPtr mRtContext;
    RtPass mRtPhotonEmissionPass;
    RtPass mRtPhotonMappingPass;
    RtPass mRtPhotonSplattingVolumePass;
    RasterPass mPhotonSplattingPass;
    RasterPass mGBufferPass;
    RasterPass mCombinePass;

    ComPtr<ID3D12PipelineState> mGBufferVolumeStateObject;

    // Scene description
    DXRFramework::RtScene::SharedPtrMut mRtScene;
    std::vector<Material> mMaterials;
    std::shared_ptr<Math::Camera> mCamera;
    std::vector<DXTKExtend::GeometricModel::SharedPtr> mRasterScene;

    // Resources
    OutputResourceView mOutput;
    ResourceView mOutputUav;

    ConstantBuffer<PerFrameConstants> mConstantBuffer;
    StructuredBuffer<DirectionalLightParams> mDirLights;
    StructuredBuffer<PointLightParams> mPointLights;
    ConstantBuffer<PhotonMappingConstants> mPhotonMappingConstants;
    ConstantBuffer<PerFrameConstantsRaster> mRasterConstantsBuffer;

    ComPtr<ID3D12Resource> mMaterialParamsResource;
    StructuredBuffer<MaterialParams> mMaterialParamsBuffer;
    StructuredBuffer<MaterialTextureParams> mTextureParams;
    std::vector<ComPtr<ID3D12Resource>> mTextureResources;
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> mTextureSrvGpuHandles;

    StructuredBuffer<PhotonEmitter> mPhotonEmitters;
    StructuredBuffer<Photon> mPhotonUploadBuffer;
    OutputResourceView mPhotonSeed;

    OutputResourceView mPhotonMapCounters;
    ComPtr<ID3D12Resource> mPhotonMapCounterReadback;
    OutputResourceView mPhotonMap;
    OutputResourceView mVolumePhotonMap;

    OutputResourceView mVolumePhotonAabbs;
    OutputResourceView mVolumePhotonPositions;
    OutputResourceView mVolumePhotonPositionsObjSpace;
    ComPtr<ID3D12Resource> mVolumePhotonPositionsReadback;
    ComPtr<ID3D12Resource> mVolumePhotonBlas;
    ComPtr<ID3D12Resource> mVolumePhotonTlas;
    WRAPPED_GPU_POINTER mVolumePhotonTlasWrappedPtr;

    OutputResourceView mPhotonDensity;

    DXTKExtend::GeometricModel::SharedPtr mPhotonSplatKernelShape;
    OutputResourceView mPhotonSplat[2];
    ResourceView mPhotonSplatUav[2];

    OutputResourceView mPhotonSplatRtBuffer;

    const std::unordered_map<GBufferID::Value, DXGI_FORMAT> mGBufferFormats =
    {
        { GBufferID::Normal, DXGI_FORMAT_R32G32B32A32_FLOAT },
        { GBufferID::Albedo, DXGI_FORMAT_R32G32B32A32_FLOAT },
    };
    OutputResourceView mGBuffer[GBufferID::Count];

    std::unique_ptr<DirectX::DescriptorPile> mCpuOnlyDescriptorHeap; // for clearing photon map resources
    std::unique_ptr<DirectX::DescriptorPile> mRtvDescriptorHeap;
    std::unique_ptr<DirectX::DescriptorHeap> mDsvDescriptorHeap;

    // Rendering states
    bool mActive;
    bool mNeedPhotonMap;
    bool mNeedPhotonAS;
    UINT mSamplesCpu;
    UINT mSamplesGpu;
    UINT mNumPhotons; // after tracing
    UINT mPhotonMapCounts[PhotonMapID::Count];
    bool mAnimationPaused;
    HybridPipelineOptions mShaderOptions;

    Math::Matrix4 mLastCameraVPMatrix;

    std::mt19937 mRng;
    std::uniform_real_distribution<float> mRngDist;
};
