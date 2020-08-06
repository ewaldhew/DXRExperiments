#include "pch.h"
#include "HybridPipeline.h"
#include "CompiledShaders/PhotonEmission.hlsl.h"
#include "CompiledShaders/PhotonTracing.hlsl.h"
#include "CompiledShaders/PhotonSplatting_vs.hlsl.h"
#include "CompiledShaders/PhotonSplatting_ps.hlsl.h"
#include "WICTextureLoader.h"
#include "DDSTextureLoader.h"
#include "ResourceUploadBatch.h"
#include "utils/DirectXHelper.h"
#include "DirectXHelpers.h"
#include "ImGuiRendererDX.h"
#include <chrono>

using namespace DXRFramework;

#define NUM_POINT_LIGHTS 1
#define NUM_DIR_LIGHTS 1
static XMFLOAT4 pointLightColor = XMFLOAT4(0.2f, 0.8f, 0.6f, 2.0f);
static XMFLOAT4 dirLightColor = XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f);

namespace GlobalRootSignatureParams
{
    enum Value
    {
        AccelerationStructureSlot = 0,
        OutputViewSlot,
        PerFrameConstantsSlot,
        PhotonSourcesSRVSlot,
        PhotonDensityOutputViewSlot,
        PhotonMappingConstantsSlot,
        MaterialTextureParamsSlot,
        MaterialTextureSrvSlot,
        Count
    };
}

namespace Pass
{
    enum Enum
    {
        Begin = 0,

        PhotonEmission = Begin,
        PhotonTracing,
        PhotonSplatting,
        Combine,

        End = 0,
    };
}

HybridPipeline::HybridPipeline(RtContext::SharedPtr context) :
    mRtContext(context),
    mFrameAccumulationEnabled(true),
    mAnimationPaused(true),
    mNeedPhotonMap(true),
    mActive(true)
{
    auto device = context->getDevice();

    /*******************************
     *  Photon Emission Pass
     *******************************/
    RtProgram::Desc photonEmit;
    {
        std::vector<std::wstring> libraryExports = {
            L"RayGen", L"ClosestHit", L"Miss",
        };
        photonEmit.addShaderLibrary(g_pPhotonEmission, ARRAYSIZE(g_pPhotonEmission), libraryExports);
        photonEmit.setRayGen("RayGen");
        photonEmit
            .addHitGroup(0, RtModel::GeometryType::Triangles, "ClosestHit", "")
            .addMiss(0, "Miss")
            .addDummyHitGroup(0, RtModel::GeometryType::AABB_Analytic)
            .addDummyHitGroup(0, RtModel::GeometryType::AABB_Volumetric)
            .addDummyHitGroup(0, RtModel::GeometryType::AABB_SignedDistance);
        photonEmit
            .addDummyHitGroup(1, RtModel::GeometryType::Triangles)
            .addDummyHitGroup(1, RtModel::GeometryType::AABB_Analytic)
            .addDummyHitGroup(1, RtModel::GeometryType::AABB_Volumetric)
            .addDummyHitGroup(1, RtModel::GeometryType::AABB_SignedDistance);
        photonEmit
            .addDummyHitGroup(2, RtModel::GeometryType::Triangles)
            .addDummyHitGroup(2, RtModel::GeometryType::AABB_Analytic)
            .addDummyHitGroup(2, RtModel::GeometryType::AABB_Volumetric)
            .addDummyHitGroup(2, RtModel::GeometryType::AABB_SignedDistance);

        photonEmit.configureGlobalRootSignature([](RootSignatureGenerator &config) {
            // GlobalRootSignatureParams::AccelerationStructureSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 0 /* t0 */);
            // GlobalRootSignatureParams::OutputViewSlot
            config.AddHeapRangesParameter({{0 /* u0 */, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0}});
            // GlobalRootSignatureParams::PerFrameConstantsSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 0 /* b0 */);
            // GlobalRootSignatureParams::PhotonSourcesSRVSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 3 /* t3 */);
        });
        photonEmit.configureHitGroupRootSignature([](RootSignatureGenerator &config) {
            config = {};
            config.AddHeapRangesParameter({{0 /* t0 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // vertices
            config.AddHeapRangesParameter({{1 /* t1 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // indices
            config.AddHeapRangesParameter({{2 /* t2 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // trianglecdf
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 0, 1, SizeOfInUint32(MaterialParams)); // space1 b0
        }, RtModel::GeometryType::Triangles);
    }
    mRtPhotonEmissionPass.mRtProgram = RtProgram::create(context, photonEmit);
    mRtPhotonEmissionPass.mRtState = RtState::create(context);
    mRtPhotonEmissionPass.mRtState->setProgram(mRtPhotonEmissionPass.mRtProgram);
    mRtPhotonEmissionPass.mRtState->setMaxTraceRecursionDepth(1);
    mRtPhotonEmissionPass.mRtState->setMaxAttributeSize(8);
    mRtPhotonEmissionPass.mRtState->setMaxPayloadSize(36);

    /*******************************
     *  Photon Tracing Pass
     *******************************/
    RtProgram::Desc photonTrace;
    {
        std::vector<std::wstring> libraryExports = {
            L"RayGen",
            L"ClosestHit", L"ClosestHit_AABB",
            L"Miss",
            L"VolumeClosestHit", L"VolumeClosestHit_AABB", L"VolumeMiss",
            L"StartingVolumeHit", L"StartingVolumeHit_AABB", L"StartingVolumeMiss",
            L"Intersection_AnalyticPrimitive", L"Intersection_VolumetricPrimitive", L"Intersection_SignedDistancePrimitive"
        };
        photonTrace.addShaderLibrary(g_pPhotonTracing, ARRAYSIZE(g_pPhotonTracing), libraryExports);
        photonTrace.setRayGen("RayGen");
        photonTrace
            .addHitGroup(0, RtModel::GeometryType::Triangles, "ClosestHit", "")
            .addHitGroup(0, RtModel::GeometryType::AABB_Analytic, "ClosestHit_AABB", "", "Intersection_AnalyticPrimitive")
            .addHitGroup(0, RtModel::GeometryType::AABB_Volumetric, "ClosestHit_AABB", "", "Intersection_VolumetricPrimitive")
            .addHitGroup(0, RtModel::GeometryType::AABB_SignedDistance, "ClosestHit_AABB", "", "Intersection_SignedDistancePrimitive")
            .addMiss(0, "Miss");
        photonTrace
            .addHitGroup(1, RtModel::GeometryType::Triangles, "VolumeClosestHit", "")
            .addHitGroup(1, RtModel::GeometryType::AABB_Analytic, "VolumeClosestHit_AABB", "", "Intersection_AnalyticPrimitive")
            .addHitGroup(1, RtModel::GeometryType::AABB_Volumetric, "VolumeClosestHit_AABB", "", "Intersection_VolumetricPrimitive")
            .addHitGroup(1, RtModel::GeometryType::AABB_SignedDistance, "VolumeClosestHit_AABB", "", "Intersection_SignedDistancePrimitive")
            .addMiss(1, "VolumeMiss");
        photonTrace
            .addHitGroup(2, RtModel::GeometryType::Triangles, "StartingVolumeHit", "")
            .addHitGroup(2, RtModel::GeometryType::AABB_Analytic, "StartingVolumeHit_AABB", "", "Intersection_AnalyticPrimitive")
            .addHitGroup(2, RtModel::GeometryType::AABB_Volumetric, "StartingVolumeHit_AABB", "", "Intersection_VolumetricPrimitive")
            .addHitGroup(2, RtModel::GeometryType::AABB_SignedDistance, "StartingVolumeHit_AABB", "", "Intersection_SignedDistancePrimitive")
            .addMiss(2, "StartingVolumeMiss");

        photonTrace.configureGlobalRootSignature([](RootSignatureGenerator &config) {
            // GlobalRootSignatureParams::AccelerationStructureSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 0 /* t0 */);
            // GlobalRootSignatureParams::OutputViewSlot
            config.AddHeapRangesParameter({{0 /* u0 */, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0}});
            // GlobalRootSignatureParams::PerFrameConstantsSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 0 /* b0 */);
            // GlobalRootSignatureParams::PhotonSourcesSRVSlot
            config.AddHeapRangesParameter({{3 /* t3 */, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}});

            D3D12_STATIC_SAMPLER_DESC cubeSampler = {};
            cubeSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            cubeSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            cubeSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            cubeSampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            cubeSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            cubeSampler.ShaderRegister = 0;
            config.AddStaticSampler(cubeSampler);

            // GlobalRootSignatureParams::PhotonDensityOutputViewSlot
            config.AddHeapRangesParameter({{1 /* u1 */, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0}});
            // GlobalRootSignatureParams::PhotonMappingConstantsSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 1 /* b1 */);
            //config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 1 /* b1 */, 0, SizeOfInUint32(PhotonMappingConstants));

            D3D12_STATIC_SAMPLER_DESC matTexSampler = {};
            matTexSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            matTexSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            matTexSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            matTexSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            matTexSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            matTexSampler.ShaderRegister = 1;
            config.AddStaticSampler(matTexSampler);

            // GlobalRootSignatureParams::MaterialTextureParamsSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 0 /* t0 */, 9); // space9 t0
            // GlobalRootSignatureParams::MaterialTextureSrvSlot
            config.AddHeapRangesParameter({ {1 /* t1 */, -1 /* unbounded */, 9 /* space9 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0} });
        });
        photonTrace.configureHitGroupRootSignature([] (RootSignatureGenerator &config) {
            config = {};
            config.AddHeapRangesParameter({{0 /* t0 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // vertices
            config.AddHeapRangesParameter({{1 /* t1 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // indices
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 0, 1, SizeOfInUint32(MaterialParams)); // space1 b0
        }, RtModel::GeometryType::Triangles);
        const auto aabbHitGroupConfigurator = [] (RootSignatureGenerator &config) {
            config = {};
            config.AddHeapRangesParameter({{1 /* b1 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 0}}); // attrs
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 0, 1, SizeOfInUint32(MaterialParams)); // space1 b0
        };
        photonTrace.configureHitGroupRootSignature(aabbHitGroupConfigurator, RtModel::GeometryType::AABB_Analytic);
        photonTrace.configureHitGroupRootSignature(aabbHitGroupConfigurator, RtModel::GeometryType::AABB_Volumetric);
        photonTrace.configureHitGroupRootSignature(aabbHitGroupConfigurator, RtModel::GeometryType::AABB_SignedDistance);
    }
    mRtPhotonMappingPass.mRtProgram = RtProgram::create(context, photonTrace);
    mRtPhotonMappingPass.mRtState = RtState::create(context);
    mRtPhotonMappingPass.mRtState->setProgram(mRtPhotonMappingPass.mRtProgram);
    mRtPhotonMappingPass.mRtState->setMaxTraceRecursionDepth(4);
    mRtPhotonMappingPass.mRtState->setMaxAttributeSize(sizeof(ProceduralPrimitiveAttributes));
    mRtPhotonMappingPass.mRtState->setMaxPayloadSize(64);

    /*******************************
     *  Photon Splatting Pass
     *******************************/
    {
        RootSignatureGenerator rsConfig;
        rsConfig.AddHeapRangesParameter({{0 /* t0 */, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // photons
        rsConfig.AddHeapRangesParameter({{1 /* t1 */, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // density
        rsConfig.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 0 /* b0 */, 0, 1);
        rsConfig.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 1 /* b1 */, 0, 1);

        mPhotonSplattingPass.rootSignature = rsConfig.Generate(device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

      /*D3D12_INPUT_ELEMENT_DESC inputDescs[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };*/

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = mPhotonSplattingPass.rootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(g_pPhotonSplatting_vs, ARRAYSIZE(g_pPhotonSplatting_vs));
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(g_pPhotonSplatting_ps, ARRAYSIZE(g_pPhotonSplatting_ps));
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.InputLayout = GeometricPrimitive::VertexType::InputLayout; //{ inputDescs, ARRAYSIZE(inputDescs) };
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 2;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        psoDesc.RTVFormats[1] = DXGI_FORMAT_R32G32_FLOAT;
        psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        psoDesc.SampleMask = 1;
        psoDesc.SampleDesc = { 1, 0 };

        ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPhotonSplattingPass.stateObject)));
        NAME_D3D12_OBJECT(mPhotonSplattingPass.stateObject);
    }

    mShaderDebugOptions.maxIterations = 1024;
    mShaderDebugOptions.cosineHemisphereSampling = true;
    mShaderDebugOptions.showIndirectDiffuseOnly = false;
    mShaderDebugOptions.showIndirectSpecularOnly = false;
    mShaderDebugOptions.showAmbientOcclusionOnly = false;
    mShaderDebugOptions.showGBufferAlbedoOnly = false;
    mShaderDebugOptions.showDirectLightingOnly = false;
    mShaderDebugOptions.showFresnelTerm = false;
    mShaderDebugOptions.noIndirectDiffuse = false;
    mShaderDebugOptions.environmentStrength = 1.0f;
    mShaderDebugOptions.debug = 0;

    auto now = std::chrono::high_resolution_clock::now();
    auto msTime = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    mRng = std::mt19937(uint32_t(msTime.time_since_epoch().count()));

    // Create descriptor heaps for UAV CPU descriptors and render target views
    {
        D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc = {};
        descriptorHeapDesc.NumDescriptors = 4;
        descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        mCpuOnlyDescriptorHeap = std::make_unique<DescriptorPile>(device, &descriptorHeapDesc);
        SetName(mCpuOnlyDescriptorHeap->Heap(), L"CPU-only descriptor heap");

        D3D12_DESCRIPTOR_HEAP_DESC rtvDescriptorHeapDesc = {};
        rtvDescriptorHeapDesc.NumDescriptors = 4;
        rtvDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        mRtvDescriptorHeap = std::make_unique<DescriptorPile>(device, &rtvDescriptorHeapDesc);
    }

    mPhotonSplatKernelShape = std::make_shared<DXTKExtend::GeometricModel>(device, [](auto& vertices, auto& indices) {
        GeometricPrimitive::CreateIcosahedron(vertices, indices);
    });
}

HybridPipeline::~HybridPipeline() = default;

void HybridPipeline::setScene(RtScene::SharedPtr scene)
{
    mRtScene = scene->copy();
    mRtPhotonEmissionPass.mRtBindings = RtBindings::create(mRtContext, mRtPhotonEmissionPass.mRtProgram, scene);
    mRtPhotonMappingPass.mRtBindings = RtBindings::create(mRtContext, mRtPhotonMappingPass.mRtProgram, scene);
}

void HybridPipeline::buildAccelerationStructures()
{
    mRtScene->build(mRtContext, mRtPhotonMappingPass.mRtProgram->getHitProgramCount());
}

void HybridPipeline::loadResources(ID3D12CommandQueue *uploadCommandQueue, UINT frameCount)
{
    auto device = mRtContext->getDevice();

    mTextureResources.resize(2);
    mTextureSrvGpuHandles.resize(3);

    mTextureParams.Create(device, mMaterials.size(), 1, L"MaterialTextureParams");

    // Create and upload global textures
    ResourceUploadBatch resourceUpload(device);
    resourceUpload.Begin();
    {
        ThrowIfFailed(CreateWICTextureFromFile(device, resourceUpload, L"..\\assets\\textures\\HdrStudioProductNightStyx001_JPG_8K.jpg", &mTextureResources[0], true));
        ThrowIfFailed(CreateDDSTextureFromFile(device, resourceUpload, L"..\\assets\\textures\\CathedralRadiance.dds", &mTextureResources[1]));

        UINT textureIndex = 0;
        UINT prevDescriptorHeapIndex = -1;
        for (auto & material : mMaterials)
        {
            if (material.params.type <= MaterialType::__UniformMaterials) continue;

            for (auto const& tex : material.textures)
            {
                ComPtr<ID3D12Resource> textureResource;
                CreateTextureResource(
                    device, resourceUpload,
                    tex.width, tex.height, tex.depth, tex.width * sizeof(XMFLOAT4), tex.height,
                    DXGI_FORMAT_R32G32B32A32_FLOAT, tex.data.data(), textureResource);
                mTextureResources.push_back(textureResource);
                mTextureParams[textureIndex] = tex.params;

                D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle;
                auto descriptorIndex = mRtContext->allocateDescriptor(&srvCpuHandle);
                auto texSrvGpuHandle = mRtContext->createTextureSRVHandle(textureResource.Get(), false, descriptorIndex);

                if (prevDescriptorHeapIndex == -1) {
                    mTextureSrvGpuHandles[2] = texSrvGpuHandle;
                }
                else {
                    ThrowIfFalse(descriptorIndex == prevDescriptorHeapIndex + 1, L"Material texture descriptor indices not contiguous");
                }

                prevDescriptorHeapIndex = descriptorIndex;
                material.params.*tex.targetParam = XMFLOAT4(static_cast<float>(textureIndex++), 0, 0, -1.0f);
            }
        }
    }
    auto uploadResourcesFinished = resourceUpload.End(uploadCommandQueue);
    uploadResourcesFinished.wait();

    mTextureSrvGpuHandles[0] = mRtContext->createTextureSRVHandle(mTextureResources[0].Get());
    mTextureSrvGpuHandles[1] = mRtContext->createTextureSRVHandle(mTextureResources[1].Get(), true);

    if (!mTextureSrvGpuHandles[2].ptr) {
        mTextureSrvGpuHandles[2] = mTextureSrvGpuHandles[1];
    }

    // Create per-frame constant buffer
    mConstantBuffer.Create(device, frameCount, L"PerFrameConstantBuffer");
    mRasterConstantsBuffer.Create(device, frameCount, L"PerFrameConstantBufferR");

    mDirLights.Create(device, NUM_DIR_LIGHTS, frameCount, L"DirectionalLightBuffer");
    mPointLights.Create(device, NUM_POINT_LIGHTS, frameCount, L"PointLightBuffer");

    mPhotonMappingConstants.Create(device, 1, L"PhotonMappingConstantBuffer");

    UINT zero = 0;
    AllocateUploadBuffer(device, &zero, 4, zeroResource.GetAddressOf());
}

void HybridPipeline::createOutputResource(DXGI_FORMAT format, UINT width, UINT height)
{
    auto device = mRtContext->getDevice();

    mClearableUavs.clear();
    mCpuOnlyDescriptorHeap = std::make_unique<DescriptorPile>(mCpuOnlyDescriptorHeap->Heap());
    mRtvDescriptorHeap = std::make_unique<DescriptorPile>(mRtvDescriptorHeap->Heap());

    // Final output resource

    AllocateRTVTexture(device, format, width, height, mOutputResource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Final output resource", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
        mOutputUavHeapIndex = mRtContext->allocateDescriptor(&uavCpuHandle, mOutputUavHeapIndex);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(mOutputResource.Get(), nullptr, &uavDesc, uavCpuHandle);

        mOutputUavGpuHandle = mRtContext->getDescriptorGPUHandle(mOutputUavHeapIndex);
        createClearableUav(mOutputResource.Get(), &uavDesc, mOutputUavGpuHandle);
    }

    {
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle;
        mOutputSrvHeapIndex = mRtContext->allocateDescriptor(&srvCpuHandle, mOutputSrvHeapIndex);
        mOutputSrvGpuHandle = mRtContext->createTextureSRVHandle(mOutputResource.Get(), false, mOutputSrvHeapIndex);
    }

    {
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        size_t rtvHeapIndex = mRtvDescriptorHeap->Allocate();
        mOutputRtvCpuHandle = mRtvDescriptorHeap->GetCpuHandle(rtvHeapIndex);
        device->CreateRenderTargetView(mOutputResource.Get(), &rtvDesc, mOutputRtvCpuHandle);
    }

    // Intermediate output resources

    mPhotonEmitters.Create(device, mRtScene->getNumInstances(), 1, L"PhotonEmitters");
    mPhotonUploadBuffer.Create(device, MAX_PHOTON_SEED_SAMPLES, 1, L"PhotonSeed");
    AllocateUAVBuffer(device, MAX_PHOTON_SEED_SAMPLES * sizeof(Photon), mPhotonSeedResource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
        mPhotonSeedUavHeapIndex = mRtContext->allocateDescriptor(&uavCpuHandle, mPhotonSeedUavHeapIndex);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.Buffer.StructureByteStride = sizeof(Photon);
        uavDesc.Buffer.NumElements = MAX_PHOTON_SEED_SAMPLES;
        device->CreateUnorderedAccessView(mPhotonSeedResource.Get(), nullptr, &uavDesc, uavCpuHandle);

        mPhotonSeedUavGpuHandle = mRtContext->getDescriptorGPUHandle(mPhotonSeedUavHeapIndex);
    }

    {
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle;
        mPhotonSeedSrvHeapIndex = mRtContext->allocateDescriptor(&srvCpuHandle, mPhotonSeedSrvHeapIndex);
        mPhotonSeedSrvGpuHandle = mRtContext->createBufferSRVHandle(mPhotonSeedResource.Get(), false, sizeof(Photon), mPhotonSeedSrvHeapIndex);
    }

    // TODO: merge resources and use counterOffset
    AllocateUAVBuffer(device, MAX_PHOTONS * sizeof(Photon), mPhotonMapResource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    AllocateUAVBuffer(device, 4, mPhotonMapCounter.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    AllocateReadbackBuffer(device, 4, mPhotonMapCounterReadback.ReleaseAndGetAddressOf(), L"photon map counter readback");

    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
        mPhotonMapUavHeapIndex = mRtContext->allocateDescriptor(&uavCpuHandle, mPhotonMapUavHeapIndex);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.Buffer.StructureByteStride = sizeof(Photon);
        uavDesc.Buffer.NumElements = MAX_PHOTONS;
        //uavDesc.Buffer.CounterOffsetInBytes = MAX_PHOTONS * sizeof(Photon);
        device->CreateUnorderedAccessView(mPhotonMapResource.Get(), mPhotonMapCounter.Get(), &uavDesc, uavCpuHandle);

        mPhotonMapUavGpuHandle = mRtContext->getDescriptorGPUHandle(mPhotonMapUavHeapIndex);
    }

    {
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle;
        mPhotonMapSrvHeapIndex = mRtContext->allocateDescriptor(&srvCpuHandle, mPhotonMapSrvHeapIndex);
        mPhotonMapSrvGpuHandle = mRtContext->createBufferSRVHandle(mPhotonMapResource.Get(), false, sizeof(Photon), mPhotonMapSrvHeapIndex);
    }

    const double preferredTileSizeInPixels = 8.0;
    UINT numTilesX = static_cast<UINT>(ceil(width / preferredTileSizeInPixels));
    UINT numTilesY = static_cast<UINT>(ceil(height / preferredTileSizeInPixels));
    mPhotonMappingConstants->numTiles = XMUINT2(numTilesX, numTilesY);
    mPhotonMappingConstants.CopyStagingToGpu();
    AllocateUAVTexture(device, DXGI_FORMAT_R32_UINT, numTilesX, numTilesY, mPhotonDensityResource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
        mPhotonDensityUavHeapIndex = mRtContext->allocateDescriptor(&uavCpuHandle, mPhotonDensityUavHeapIndex);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(mPhotonDensityResource.Get(), nullptr, &uavDesc, uavCpuHandle);

        mPhotonDensityUavGpuHandle = mRtContext->getDescriptorGPUHandle(mPhotonDensityUavHeapIndex);
        createClearableUav(mPhotonDensityResource.Get(), &uavDesc, mPhotonDensityUavGpuHandle);
    }

    {
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle;
        mPhotonDensitySrvHeapIndex = mRtContext->allocateDescriptor(&srvCpuHandle, mPhotonDensitySrvHeapIndex);
        mPhotonDensitySrvGpuHandle = mRtContext->createTextureSRVHandle(mPhotonDensityResource.Get(), false, mPhotonDensitySrvHeapIndex);
    }

    AllocateRTVTexture(device, DXGI_FORMAT_R32G32B32A32_FLOAT, width, height, mPhotonSplatTargetResource[0].ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"PhotonSplatResultColorDirX");

    {
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        size_t rtvHeapIndex = mRtvDescriptorHeap->Allocate();
        mPhotonSplatRtvCpuHandle[0] = mRtvDescriptorHeap->GetCpuHandle(rtvHeapIndex);
        device->CreateRenderTargetView(mPhotonSplatTargetResource[0].Get(), &rtvDesc, mPhotonSplatRtvCpuHandle[0]);
    }

    AllocateRTVTexture(device, DXGI_FORMAT_R32G32_FLOAT, width, height, mPhotonSplatTargetResource[1].ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"PhotonSplatResultDirYZ");

    {
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        size_t rtvHeapIndex = mRtvDescriptorHeap->Allocate();
        mPhotonSplatRtvCpuHandle[1] = mRtvDescriptorHeap->GetCpuHandle(rtvHeapIndex);
        device->CreateRenderTargetView(mPhotonSplatTargetResource[1].Get(), &rtvDesc, mPhotonSplatRtvCpuHandle[1]);
    }
}

void HybridPipeline::createClearableUav(ID3D12Resource* pResource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* uavDesc, D3D12_GPU_DESCRIPTOR_HANDLE uavHandle)
{
    auto device = mRtContext->getDevice();

    auto uavCpuHandle = mCpuOnlyDescriptorHeap->GetCpuHandle(mCpuOnlyDescriptorHeap->Allocate());
    device->CreateUnorderedAccessView(pResource, nullptr, uavDesc, uavCpuHandle);

    mClearableUavs.push_back({ uavHandle, uavCpuHandle, pResource });
}

void HybridPipeline::clearUavs()
{
    auto commandList = mRtContext->getCommandList();

    const UINT clear[4] = { 0 };
    for (auto const& uav : mClearableUavs) {
        commandList->ClearUnorderedAccessViewUint(uav.gpuHandle, uav.cpuHandle, uav.pResource, clear, 0, nullptr);
    }
}

inline void clearRtv(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    commandList->ClearRenderTargetView(handle, clearZero, 0, nullptr);
}

inline void calculateCameraVariables(Math::Camera &camera, float aspectRatio, XMFLOAT4 *U, XMFLOAT4 *V, XMFLOAT4 *W)
{
    float ulen, vlen, wlen;
    XMVECTOR w = camera.GetForwardVec(); // Do not normalize W -- it implies focal length

    wlen = XMVectorGetX(XMVector3Length(w));
    XMVECTOR u = XMVector3Normalize(XMVector3Cross(w, camera.GetUpVec()));
    XMVECTOR v = XMVector3Normalize(XMVector3Cross(u, w));

    vlen = wlen * tanf(0.5f * camera.GetFOV());
    ulen = vlen / aspectRatio;
    u = XMVectorScale(u, ulen);
    v = XMVectorScale(v, vlen);

    XMStoreFloat4(U, u);
    XMStoreFloat4(V, v);
    XMStoreFloat4(W, w);
}

inline void calculateCameraFrustum(Math::Camera &camera, XMFLOAT2 *NH, XMFLOAT2 *NV, XMFLOAT2 *clipZ)
{
    auto vsFrustum = camera.GetViewSpaceFrustum();
    auto nh = vsFrustum.GetFrustumPlane(vsFrustum.kLeftPlane).GetNormal();
    auto nv = vsFrustum.GetFrustumPlane(vsFrustum.kBottomPlane).GetNormal();
    auto nhn = Math::Vector3(XMVector3Normalize(nh));
    auto nvn = Math::Vector3(XMVector3Normalize(nv));
    *NH = XMFLOAT2(nhn.GetX(), nhn.GetZ());
    *NV = XMFLOAT2(nvn.GetY(), nvn.GetZ());
    float nearZ = vsFrustum.GetFrustumCorner(vsFrustum.kNearLowerLeft).GetZ();
    float farZ = vsFrustum.GetFrustumCorner(vsFrustum.kFarLowerLeft).GetZ();
    *clipZ = XMFLOAT2(nearZ, farZ);
}

inline bool hasCameraMoved(Math::Camera &camera, Math::Matrix4 &lastVPMatrix)
{
    const Math::Matrix4 &currentMatrix = camera.GetViewProjMatrix();
    return !(XMVector4Equal(lastVPMatrix.GetX(), currentMatrix.GetX()) && XMVector4Equal(lastVPMatrix.GetY(), currentMatrix.GetY()) &&
             XMVector4Equal(lastVPMatrix.GetZ(), currentMatrix.GetZ()) && XMVector4Equal(lastVPMatrix.GetW(), currentMatrix.GetW()));
}

void HybridPipeline::update(float elapsedTime, UINT elapsedFrames, UINT prevFrameIndex, UINT frameIndex, UINT width, UINT height)
{
    if (mAnimationPaused) {
        elapsedTime = 142.0f;
    }

    if (hasCameraMoved(*mCamera, mLastCameraVPMatrix) || !mFrameAccumulationEnabled) {
        mAccumCount = 0;
        mLastCameraVPMatrix = mCamera->GetViewProjMatrix();
        mNeedPhotonMap = true;
    }

    CameraParams &cameraParams = mConstantBuffer->cameraParams;
    XMStoreFloat4(&cameraParams.worldEyePos, mCamera->GetPosition());
    calculateCameraVariables(*mCamera, mCamera->GetAspectRatio(), &cameraParams.U, &cameraParams.V, &cameraParams.W);
    calculateCameraFrustum(*mCamera, &cameraParams.frustumNH, &cameraParams.frustumNV, &cameraParams.frustumNearFar);
    float xJitter = (mRngDist(mRng) - 0.5f) / float(width);
    float yJitter = (mRngDist(mRng) - 0.5f) / float(height);
    cameraParams.jitters = XMFLOAT2(xJitter, yJitter);
    cameraParams.frameCount = elapsedFrames;
    cameraParams.accumCount = mAccumCount++;

    mConstantBuffer->options = mShaderDebugOptions;
    mConstantBuffer.CopyStagingToGpu(frameIndex);

    XMVECTOR dirLightVector = XMVectorSet(0.3f, -0.2f, -1.0f, 0.0f);
    XMMATRIX rotation = XMMatrixRotationY(sin(elapsedTime * 0.2f) * 3.14f * 0.5f);
    dirLightVector = XMVector4Transform(dirLightVector, rotation);
    XMStoreFloat4(&mDirLights[0].forwardDir, dirLightVector);
    mDirLights[0].color = dirLightColor;
    mDirLights.CopyStagingToGpu(frameIndex);

    // XMVECTOR pointLightPos = XMVectorSet(sin(elapsedTime * 0.97f), sin(elapsedTime * 0.45f), sin(elapsedTime * 0.32f), 1.0f);
    // pointLightPos = XMVectorAdd(pointLightPos, XMVectorSet(0.0f, 0.5f, 1.0f, 0.0f));
    // pointLightPos = XMVectorMultiply(pointLightPos, XMVectorSet(0.221f, 0.049f, 0.221f, 1.0f));
    XMVECTOR pointLightPos = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    XMStoreFloat4(&mPointLights[0].worldPos, pointLightPos);
    mPointLights[0].color = pointLightColor;
    mPointLights.CopyStagingToGpu(frameIndex);

    mTextureParams.CopyStagingToGpu();

    float tileAreaFactor = 1.0f / (mPhotonMappingConstants->numTiles.x * mPhotonMappingConstants->numTiles.y);
    float vsHeight = tanf(0.5f * mCamera->GetFOV());
    float vsArea = vsHeight * vsHeight / mCamera->GetAspectRatio();
    mPhotonMappingConstants->tileAreaConstant = tileAreaFactor * vsArea;
    mPhotonMappingConstants->maxRayLength = 2.0f * float(mRtScene->getBoundingBox().toBoundingSphere().GetRadius());
    mPhotonMappingConstants.CopyStagingToGpu();

    mRasterConstantsBuffer->WorldToViewMatrix = mCamera->GetViewMatrix();
    mRasterConstantsBuffer->WorldToViewClipMatrix = mCamera->GetViewProjMatrix();
    mRasterConstantsBuffer.CopyStagingToGpu(frameIndex);
}

void HybridPipeline::collectEmitters(UINT& numLights, UINT& maxSamples)
{
    // collect total intensity
    auto totalRadiance = 0.0f;
    auto getLightMagnitude = [](XMFLOAT4 radiance) {
        float result = 0.0f;
        result += radiance.x * radiance.w;
        result += radiance.y * radiance.w;
        result += radiance.z * radiance.w;
        return result;
    };
    std::vector<UINT> complexLights;

    for (UINT i = 0; i < NUM_DIR_LIGHTS; i++) {
        totalRadiance += getLightMagnitude(mDirLights[i].color);
    }
    for (UINT i = 0; i < NUM_POINT_LIGHTS; i++) {
        totalRadiance += getLightMagnitude(mPointLights[i].color);
    }
    for (UINT i = 0; i < mRtScene->getNumInstances(); i++) {
        auto model = mRtScene->getModel(i);
        XMFLOAT4 emission = mMaterials[model->mMaterialIndex].params.emissive;
        if (emission.w > 0.0f && (emission.x > 0.0f || emission.y > 0.0f || emission.z > 0.0f)) {
            totalRadiance += getLightMagnitude(emission);
            complexLights.push_back(i);
        }
    }

    mSamplesCpu = 0;
    mSamplesGpu = 0;
    maxSamples = 0;
    numLights = static_cast<UINT>(complexLights.size());

    std::uniform_int_distribution<UINT> rand;
    auto getScaledSamplePower = [&](XMFLOAT4 radiance, float& proportion, XMVECTOR& lightRadiance) {
        lightRadiance = XMLoadFloat4(&radiance);
        proportion = getLightMagnitude(radiance) / totalRadiance;
        lightRadiance = XMVectorScale(lightRadiance, radiance.w * proportion);
    };
    auto storePhoton = [&](XMVECTOR power, XMVECTOR position, XMVECTOR direction) {
        Photon photon = {};
        photon.randSeed = rand(mRng);
        XMStoreFloat3(&photon.power, power);
        XMStoreFloat3(&photon.position, position);
        XMStoreFloat3(&photon.direction, direction);

        mPhotonUploadBuffer[mSamplesCpu++] = photon;
    };

    // for each light, take samples proportional to light intensity
    for (UINT i = 0; i < NUM_DIR_LIGHTS; i++) {

    }
    for (UINT i = 0; i < NUM_POINT_LIGHTS; i++) {
        XMVECTOR lightPos = XMLoadFloat4(&mPointLights[i].worldPos);
        XMVECTOR lightRadiance;
        float proportion;
        getScaledSamplePower(mPointLights[i].color, proportion, lightRadiance);

        for (UINT j = 0; j < proportion * MAX_PHOTON_SEED_SAMPLES; j++) {
            XMVECTOR photonDir = XMVectorSet(1.0, 0.0, 0.0, 0.0);
            XMMATRIX rotation = XMMatrixRotationRollPitchYaw(mRngDist(mRng) * XM_2PI, mRngDist(mRng) * XM_2PI, mRngDist(mRng) * XM_2PI);
            photonDir = XMVector4Transform(photonDir, rotation);
            storePhoton(lightRadiance, lightPos, photonDir);
        }
    }
    for (UINT i = 0; i < complexLights.size(); i++) {
        auto lightIndex = complexLights[i];
        auto model = mRtScene->getModel(lightIndex);

        if (model->getGeometryType() != RtModel::GeometryType::Triangles) continue;
        auto light = toRtMesh(model);
        auto transform = mRtScene->getTransform(lightIndex);

        XMVECTOR lightRadiance;
        float proportion;
        getScaledSamplePower(mMaterials[light->mMaterialIndex].params.emissive, proportion, lightRadiance);

        UINT numSamples = static_cast<UINT>(proportion * MAX_PHOTON_SEED_SAMPLES);
        mPhotonEmitters[i].samplesToTake = numSamples;
        mPhotonEmitters[i].sampleStartIndex = mSamplesGpu;

        auto bbox = Math::Matrix4(transform) * light->getBoundingBox();
        auto bSphere = bbox.toBoundingSphere();
        XMStoreFloat3(&mPhotonEmitters[i].direction, XMVector3Normalize(bbox.GetPrimaryVector()));
        XMStoreFloat3(&mPhotonEmitters[i].center, bSphere.GetCenter());
        mPhotonEmitters[i].radius = bSphere.GetRadius();

        mSamplesGpu += numSamples;
        maxSamples = max(maxSamples, numSamples);
    }

    mPhotonUploadBuffer.CopyStagingToGpu();
    mPhotonEmitters.CopyStagingToGpu();
}

void HybridPipeline::render(ID3D12GraphicsCommandList *commandList, UINT frameIndex, UINT width, UINT height, UINT& pass)
{
    if (!mNeedPhotonMap && pass < Pass::PhotonSplatting)
    {
        pass = Pass::PhotonSplatting;
    }

    // generate photon seed
    if (pass == Pass::PhotonEmission)
    {
        clearUavs();

        UINT numLights, maxSamples;
        collectEmitters(numLights, maxSamples);

        // Update shader table root arguments
        auto mRtBindings = mRtPhotonEmissionPass.mRtBindings;
        auto mRtState = mRtPhotonEmissionPass.mRtState;
        auto program = mRtBindings->getProgram();

        for (UINT rayType = 0; rayType < program->getHitProgramCount(); ++rayType) {
            for (UINT instance = 0; instance < mRtScene->getNumInstances(); ++instance) {
                auto &hitVars = mRtBindings->getHitVars(rayType, instance);
                switch (mRtScene->getModel(instance)->getGeometryType())
                {
                case RtModel::GeometryType::Triangles: {
                    auto model = toRtMesh(mRtScene->getModel(instance));
                    hitVars->appendHeapRanges(model->getVertexBufferSrvHandle().ptr);
                    hitVars->appendHeapRanges(model->getIndexBufferSrvHandle().ptr);
                    hitVars->appendHeapRanges(model->getTriangleCdfSrvHandle().ptr);
                    hitVars->append32BitConstants((void*)&mMaterials[model->mMaterialIndex].params, SizeOfInUint32(MaterialParams));
                    break;
                }
                default: {
                    //TODO
                    // no-op shader
                    break;
                }
                }
            }
        }

        mRtBindings->apply(mRtContext, mRtState);

        // Set global root arguments
        mRtContext->bindDescriptorHeap();
        commandList->SetComputeRootSignature(program->getGlobalRootSignature());
        commandList->SetComputeRootConstantBufferView(GlobalRootSignatureParams::PerFrameConstantsSlot, mConstantBuffer.GpuVirtualAddress(frameIndex));
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::OutputViewSlot, mPhotonSeedUavGpuHandle);
        commandList->SetComputeRootShaderResourceView(GlobalRootSignatureParams::PhotonSourcesSRVSlot, mPhotonEmitters.GpuVirtualAddress());
        mRtContext->getFallbackCommandList()->SetTopLevelAccelerationStructure(GlobalRootSignatureParams::AccelerationStructureSlot, mRtScene->getTlasWrappedPtr());

        mRtContext->raytrace(mRtBindings, mRtState, maxSamples, numLights, 1);

        mRtContext->insertUAVBarrier(mPhotonSeedResource.Get());

        // copy photons emitted on CPU to the buffer
        mRtContext->transitionResource(mPhotonSeedResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(mPhotonSeedResource.Get(), mSamplesGpu * sizeof(Photon), mPhotonUploadBuffer.GetResource().Get(), 0, mSamplesCpu * sizeof(Photon));

        pass = Pass::PhotonTracing;
        return;
    }

    // generate photon map
    if (pass == Pass::PhotonTracing)
    {
        // Update shader table root arguments
        auto mRtBindings = mRtPhotonMappingPass.mRtBindings;
        auto mRtState = mRtPhotonMappingPass.mRtState;
        auto program = mRtBindings->getProgram();

        for (UINT rayType = 0; rayType < program->getHitProgramCount(); ++rayType) {
            for (UINT instance = 0; instance < mRtScene->getNumInstances(); ++instance) {
                auto &hitVars = mRtBindings->getHitVars(rayType, instance);
                switch (mRtScene->getModel(instance)->getGeometryType())
                {
                case RtModel::GeometryType::Triangles: {
                    auto model = toRtMesh(mRtScene->getModel(instance));
                    hitVars->appendHeapRanges(model->getVertexBufferSrvHandle().ptr);
                    hitVars->appendHeapRanges(model->getIndexBufferSrvHandle().ptr);
                    hitVars->append32BitConstants((void*)&mMaterials[model->mMaterialIndex].params, SizeOfInUint32(MaterialParams));
                    break;
                }
                default: {
                    auto model = toRtProcedural(mRtScene->getModel(instance));
                    hitVars->appendHeapRanges(model->getPrimitiveConstantsCbvHandle().ptr);
                    hitVars->append32BitConstants((void*)&mMaterials[model->mMaterialIndex].params, SizeOfInUint32(MaterialParams));
                    break;
                }
                }
            }
        }

        mRtContext->transitionResource(mPhotonSeedResource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        mRtBindings->apply(mRtContext, mRtState);

        // Set global root arguments
        mRtContext->bindDescriptorHeap();
        commandList->SetComputeRootSignature(program->getGlobalRootSignature());
        commandList->SetComputeRootConstantBufferView(GlobalRootSignatureParams::PerFrameConstantsSlot, mConstantBuffer.GpuVirtualAddress(frameIndex));
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::OutputViewSlot, mPhotonMapUavGpuHandle);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::PhotonDensityOutputViewSlot, mPhotonDensityUavGpuHandle);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::MaterialTextureSrvSlot, mTextureSrvGpuHandles[2]);
        commandList->SetComputeRootShaderResourceView(GlobalRootSignatureParams::MaterialTextureParamsSlot, mTextureParams.GpuVirtualAddress());
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::PhotonSourcesSRVSlot, mPhotonSeedSrvGpuHandle);
        commandList->SetComputeRootConstantBufferView(GlobalRootSignatureParams::PhotonMappingConstantsSlot, mPhotonMappingConstants.GpuVirtualAddress());
        mRtContext->getFallbackCommandList()->SetTopLevelAccelerationStructure(GlobalRootSignatureParams::AccelerationStructureSlot, mRtScene->getTlasWrappedPtr());

        mRtContext->raytrace(mRtBindings, mRtState, mSamplesCpu + mSamplesGpu, 1, 1);

        mRtContext->insertUAVBarrier(mPhotonMapResource.Get());
        mRtContext->transitionResource(mPhotonSeedResource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        mRtContext->transitionResource(mPhotonMapCounter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->CopyResource(mPhotonMapCounterReadback.Get(), mPhotonMapCounter.Get());
        mRtContext->transitionResource(mPhotonMapCounter.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyResource(mPhotonMapCounter.Get(), zeroResource.Get());
        mRtContext->transitionResource(mPhotonMapCounter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        mNumPhotons = 0;
        mNeedPhotonMap = false;
        pass = Pass::PhotonSplatting;
        return;
    }

    if (pass == Pass::PhotonSplatting)
    {
        auto viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        auto scissorRect = CD3DX12_RECT(0, 0, (width), static_cast<LONG>(height));

        if (mNumPhotons == 0) {
            UINT* pReadbackBufferData;
            ThrowIfFailed(mPhotonMapCounterReadback->Map(0, nullptr, reinterpret_cast<void**>(&pReadbackBufferData)));
            mNumPhotons = pReadbackBufferData[0];
            mPhotonMapCounterReadback->Unmap(0, &CD3DX12_RANGE(0, 0));
        }

        std::initializer_list<D3D12_RESOURCE_BARRIER> barriers =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(mPhotonMapResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mPhotonDensityResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mPhotonSplatTargetResource[0].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(mPhotonSplatTargetResource[1].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        auto transitions = ScopedBarrier(commandList, barriers);

        // Set necessary state.
        mRtContext->bindDescriptorHeap();
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissorRect);
        commandList->SetGraphicsRootSignature(mPhotonSplattingPass.rootSignature.Get());
        commandList->SetGraphicsRootDescriptorTable(0, mPhotonMapSrvGpuHandle);
        commandList->SetGraphicsRootDescriptorTable(1, mPhotonDensitySrvGpuHandle);
        commandList->SetGraphicsRootConstantBufferView(2, mRasterConstantsBuffer.GpuVirtualAddress(frameIndex));
        commandList->SetGraphicsRootConstantBufferView(3, mPhotonMappingConstants.GpuVirtualAddress());
        commandList->OMSetRenderTargets(ARRAYSIZE(mPhotonSplatRtvCpuHandle), mPhotonSplatRtvCpuHandle, FALSE, nullptr);

        clearRtv(commandList, mPhotonSplatRtvCpuHandle[0]);
        clearRtv(commandList, mPhotonSplatRtvCpuHandle[1]);

        commandList->SetPipelineState(mPhotonSplattingPass.stateObject.Get());

        mPhotonSplatKernelShape->Draw(commandList, mNumPhotons);

        pass = Pass::Combine;
    }

    // final render pass
    if (pass == Pass::Combine)
    {
        pass = Pass::End;
    }
}

void HybridPipeline::userInterface()
{
    bool frameDirty = false;

    ui::Begin("Lighting");
    {
        frameDirty |= ui::ColorPicker4("Point Light", (float*)&pointLightColor);
        frameDirty |= ui::ColorPicker4("Directional Light", (float*)&dirLightColor);
    }
    ui::End();

    ui::Begin("Material");
    {
        frameDirty |= ui::SliderFloat3("Albedo", &mMaterials[0].params.albedo.x, 0.0f, 1.0f);
        frameDirty |= ui::SliderFloat3("Specular", &mMaterials[0].params.specular.x, 0.0f, 1.0f);
        frameDirty |= ui::SliderFloat("Reflectivity", &mMaterials[0].params.reflectivity, 0.0f, 1.0f);
        frameDirty |= ui::SliderFloat("Roughness", &mMaterials[0].params.roughness, 0.0f, 1.0f);
    }
    ui::End();

    ui::Begin("Hybrid Raytracing");
    {
        frameDirty |= ui::Checkbox("Pause Animation", &mAnimationPaused);

        ui::Separator();

        if (ui::Checkbox("Frame Accumulation", &mFrameAccumulationEnabled)) {
            mAnimationPaused = true;
            frameDirty = true;
        }

        if (mFrameAccumulationEnabled) {
            UINT currentIterations = min(mAccumCount, mShaderDebugOptions.maxIterations);
            UINT oldMaxIterations = mShaderDebugOptions.maxIterations;
            if (ui::SliderInt("Max Iterations", (int*)&mShaderDebugOptions.maxIterations, 1, 2048)) {
                frameDirty |= (mShaderDebugOptions.maxIterations < mAccumCount);
                mAccumCount = min(mAccumCount, oldMaxIterations);
            }
            ui::ProgressBar(float(currentIterations) / float(mShaderDebugOptions.maxIterations), ImVec2(), std::to_string(currentIterations).c_str());
        }

        ui::Separator();

        frameDirty |= ui::Checkbox("Cosine Hemisphere Sampling", (bool*)&mShaderDebugOptions.cosineHemisphereSampling);
        frameDirty |= ui::Checkbox("Indirect Diffuse Only", (bool*)&mShaderDebugOptions.showIndirectDiffuseOnly);
        frameDirty |= ui::Checkbox("Indirect Specular Only", (bool*)&mShaderDebugOptions.showIndirectSpecularOnly);
        frameDirty |= ui::Checkbox("Ambient Occlusion Only", (bool*)&mShaderDebugOptions.showAmbientOcclusionOnly);
        frameDirty |= ui::Checkbox("GBuffer Albedo Only", (bool*)&mShaderDebugOptions.showGBufferAlbedoOnly);
        frameDirty |= ui::Checkbox("Direct Lighting Only", (bool*)&mShaderDebugOptions.showDirectLightingOnly);
        frameDirty |= ui::Checkbox("Fresnel Term Only", (bool*)&mShaderDebugOptions.showFresnelTerm);
        frameDirty |= ui::Checkbox("No Indirect Diffuse", (bool*)&mShaderDebugOptions.noIndirectDiffuse);
        frameDirty |= ui::SliderFloat("Environment Strength", &mShaderDebugOptions.environmentStrength, 0.0f, 10.0f);
        frameDirty |= ui::SliderInt("Debug", (int*)&mShaderDebugOptions.debug, 0, 2);

        ui::Separator();

        ui::Text("Press space to toggle first person camera");
    }
    ui::End();

    if (frameDirty) {
        mLastCameraVPMatrix = Math::Matrix4();
    }
}
