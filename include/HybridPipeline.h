#pragma once

#include "Helpers/DirectXRaytracingHelper.h"
#include "RaytracingHlslCompat.h"
#include "RaytracingPipeline.h"
#include "RtBindings.h"
#include "RtContext.h"
#include "RtProgram.h"
#include "RtScene.h"
#include "RtState.h"
#include "Camera.h"
#include <vector>
#include <random>

class HybridPipeline : public RaytracingPipeline
{
public:
    using SharedPtr = std::shared_ptr<HybridPipeline>;

    static SharedPtr create(DXRFramework::RtContext::SharedPtr context) { return SharedPtr(new HybridPipeline(context)); }
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
    virtual ID3D12Resource *getOutputResource(UINT id) override { return mOutputResource.Get(); }
    virtual D3D12_GPU_DESCRIPTOR_HANDLE getOutputUavHandle(UINT id) override { return mOutputUavGpuHandle; }
    virtual D3D12_GPU_DESCRIPTOR_HANDLE getOutputSrvHandle(UINT id) override { return mOutputSrvGpuHandle; }

    virtual bool *isActive() override { return &mActive; }
    virtual const char *getName() override { return "Hybrid Pipeline"; }
private:
    HybridPipeline(DXRFramework::RtContext::SharedPtr context);

    void createClearableUav(ID3D12Resource* pResource, const D3D12_UNORDERED_ACCESS_VIEW_DESC * uavDesc, D3D12_GPU_DESCRIPTOR_HANDLE uavHandle);
    void clearUavs();
    void createPipelineStateObjects();
    void collectEmitters(UINT& numLights, UINT& maxSamples);

    // Pipeline components
    struct RtPass
    {
        DXRFramework::RtProgram::SharedPtr mRtProgram;
        DXRFramework::RtBindings::SharedPtr mRtBindings;
        DXRFramework::RtState::SharedPtr mRtState;
    };

    DXRFramework::RtContext::SharedPtr mRtContext;
    RtPass mRtPhotonEmissionPass;
    RtPass mRtPhotonMappingPass;

    // Scene description
    DXRFramework::RtScene::SharedPtrMut mRtScene;
    std::vector<Material> mMaterials;
    std::shared_ptr<Math::Camera> mCamera;

    // Resources
    ComPtr<ID3D12Resource> mOutputResource;
    UINT mOutputUavHeapIndex = UINT_MAX;
    UINT mOutputSrvHeapIndex = UINT_MAX;
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUavGpuHandle;
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputSrvGpuHandle;

    ConstantBuffer<PerFrameConstants> mConstantBuffer;
    StructuredBuffer<DirectionalLightParams> mDirLights;
    StructuredBuffer<PointLightParams> mPointLights;
    ConstantBuffer<PhotonMappingConstants> mPhotonMappingConstants;

    std::vector<ComPtr<ID3D12Resource>> mTextureResources;
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> mTextureSrvGpuHandles;

    StructuredBuffer<PhotonEmitter> mPhotonEmitters;
    StructuredBuffer<Photon> mPhotonUploadBuffer;
    ComPtr<ID3D12Resource> mPhotonSeedResource;
    UINT mPhotonSeedUavHeapIndex = UINT_MAX;
    UINT mPhotonSeedSrvHeapIndex = UINT_MAX;
    D3D12_GPU_DESCRIPTOR_HANDLE mPhotonSeedUavGpuHandle;
    D3D12_GPU_DESCRIPTOR_HANDLE mPhotonSeedSrvGpuHandle;

    ComPtr<ID3D12Resource> mPhotonMapResource;
    ComPtr<ID3D12Resource> mPhotonMapCounter;
    UINT mPhotonMapUavHeapIndex = UINT_MAX;
    D3D12_GPU_DESCRIPTOR_HANDLE mPhotonMapUavGpuHandle;

    ComPtr<ID3D12Resource> mPhotonDensityResource;
    UINT mPhotonDensityUavHeapIndex = UINT_MAX;
    D3D12_GPU_DESCRIPTOR_HANDLE mPhotonDensityUavGpuHandle;

    struct ClearableUAV
    {
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
        ID3D12Resource* pResource;
    };
    ComPtr<ID3D12DescriptorHeap> mCpuOnlyDescriptorHeap; // for clearing photon map resources
    UINT mDescriptorSize;
    std::vector<ClearableUAV> mClearableUavs;
    ComPtr<ID3D12Resource> zeroResource;

    // Rendering states
    bool mActive;
    UINT mAccumCount;
    bool mNeedPhotonMap;
    UINT mSamplesCpu;
    UINT mSamplesGpu;
    bool mFrameAccumulationEnabled;
    bool mAnimationPaused;
    DebugOptions mShaderDebugOptions;

    Math::Matrix4 mLastCameraVPMatrix;

    std::mt19937 mRng;
    std::uniform_real_distribution<float> mRngDist;
};
