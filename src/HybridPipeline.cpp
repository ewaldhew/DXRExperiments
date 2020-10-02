#include "pch.h"
#include "HybridPipeline.h"
#include "CompiledShaders/PhotonEmission.hlsl.h"
#include "CompiledShaders/PhotonTracing.hlsl.h"
#include "CompiledShaders/PhotonSplatting_vs.hlsl.h"
#include "CompiledShaders/PhotonSplatting_ps.hlsl.h"
#include "CompiledShaders/GBuffer_ps.hlsl.h"
#include "CompiledShaders/GBuffer_vs.hlsl.h"
#include "CompiledShaders/PhotonLightingCombine_vs.hlsl.h"
#include "CompiledShaders/PhotonLightingCombine_ps.hlsl.h"
#include "CompiledShaders/PhotonSplattingVolume.hlsl.h"
#include "CompiledShaders/PhotonSplattingVoxels.hlsl.h"
#include "WICTextureLoader.h"
#include "DDSTextureLoader.h"
#include "ResourceUploadBatch.h"
#define _PIX_H_ // prevent name conflict in older version of PIX
#include "DirectXHelpers.h"
#include "ImGuiRendererDX.h"
#include "Helpers/BottomLevelASGenerator.h"
#include "Helpers/TopLevelASGenerator.h"
#include <chrono>

using namespace DXRFramework;

#define NUM_POINT_LIGHTS 1
#define NUM_DIR_LIGHTS 1
static XMFLOAT4 pointLightColor = XMFLOAT4(0.2f, 0.8f, 0.6f, 0.0f);
static XMFLOAT4 dirLightColor = XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f);

static const D3D12_STATIC_SAMPLER_DESC linearMipPointSampler = CD3DX12_STATIC_SAMPLER_DESC(0,
    D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT
);
static const D3D12_STATIC_SAMPLER_DESC pointClampSampler = CD3DX12_STATIC_SAMPLER_DESC(1,
    D3D12_FILTER_MIN_MAG_MIP_POINT,
    D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
    D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
    D3D12_TEXTURE_ADDRESS_MODE_CLAMP
);
static const D3D12_STATIC_SAMPLER_DESC anisotropicSampler = CD3DX12_STATIC_SAMPLER_DESC(2,
    D3D12_FILTER_ANISOTROPIC,
    D3D12_TEXTURE_ADDRESS_MODE_WRAP,
    D3D12_TEXTURE_ADDRESS_MODE_WRAP,
    D3D12_TEXTURE_ADDRESS_MODE_WRAP,
    0.0f, 16U,
    D3D12_COMPARISON_FUNC_NEVER
);
static const D3D12_STATIC_SAMPLER_DESC linearSampler = CD3DX12_STATIC_SAMPLER_DESC(3,
    D3D12_FILTER_MIN_MAG_MIP_LINEAR,
    D3D12_TEXTURE_ADDRESS_MODE_BORDER,
    D3D12_TEXTURE_ADDRESS_MODE_BORDER,
    D3D12_TEXTURE_ADDRESS_MODE_BORDER,
    0.0f, 0U,
    D3D12_COMPARISON_FUNC_NEVER,
    D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK
);

namespace GlobalRootSignatureParams
{
    enum Value
    {
        AccelerationStructureSlot = 0,
        OutputViewSlot,
        PerFrameConstantsSlot,
        PhotonSourcesSRVSlot,
        PhotonDensityOutputViewSlot,
        PhotonGeometryOutputViewSlot,
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

        GBuffer,
        PhotonEmission,
        PhotonTracing,
        PhotonSplatting,
        PhotonSplattingRaster,
        PhotonSplattingVolume,
        PhotonSplattingVoxels,
        Combine,

        End = 0,
    };
}

HybridPipeline::HybridPipeline(RtContext::SharedPtr context, DXGI_FORMAT outputFormat) :
    mRtContext(context),
    mAnimationPaused(true),
    mNeedPhotonMap(true),
    mIsTracingFrame(true),
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
            config.AddHeapRangesParameter({{0 /* u0 */, PhotonMapID::Count + 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0}});
            // GlobalRootSignatureParams::PerFrameConstantsSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 0 /* b0 */);
            // GlobalRootSignatureParams::PhotonSourcesSRVSlot
            config.AddHeapRangesParameter({{3 /* t3 */, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}});

            D3D12_STATIC_SAMPLER_DESC cubeSampler = linearMipPointSampler;
            cubeSampler.ShaderRegister = 0;
            config.AddStaticSampler(cubeSampler);

            // GlobalRootSignatureParams::PhotonDensityOutputViewSlot
            config.AddHeapRangesParameter({{PhotonMapID::Count + 1, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0}});
            // GlobalRootSignatureParams::PhotonGeometryOutputViewSlot
            config.AddHeapRangesParameter({{PhotonMapID::Count + 2, 3, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0}});
            // GlobalRootSignatureParams::PhotonMappingConstantsSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 1 /* b1 */);

            D3D12_STATIC_SAMPLER_DESC matTexSampler = pointClampSampler;
            matTexSampler.ShaderRegister = 1;
            config.AddStaticSampler(matTexSampler);

            // GlobalRootSignatureParams::MaterialTextureParamsSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 1 /* t1 */, 9); // space9 t1
            // GlobalRootSignatureParams::MaterialTextureSrvSlot
            config.AddHeapRangesParameter({{2 /* t2 */, -1 /* unbounded */, 9 /* space9 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}});
        });
        photonTrace.configureHitGroupRootSignature([] (RootSignatureGenerator &config) {
            config = {};
            config.AddHeapRangesParameter({{0 /* t0 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // vertices
            config.AddHeapRangesParameter({{1 /* t1 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // indices
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 0, 1, SizeOfInUint32(MaterialParams)); // space1 b0
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 2, 1, SizeOfInUint32(UINT)); // space1 b1
        }, RtModel::GeometryType::Triangles);
        const auto aabbHitGroupConfigurator = [] (RootSignatureGenerator &config) {
            config = {};
            config.AddHeapRangesParameter({{1 /* b1 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 0}}); // attrs
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 0, 1, SizeOfInUint32(MaterialParams)); // space1 b0
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 2, 1, SizeOfInUint32(UINT)); // space1 b1
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
        rsConfig.AddHeapRangesParameter({{0 /* t0 */, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // density
        rsConfig.AddHeapRangesParameter({{1 /* t1 */, PhotonMapID::Count, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // photons
        rsConfig.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 0 /* b0 */, 0, 1);
        rsConfig.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 1 /* b1 */, 0, 1);
        rsConfig.AddHeapRangesParameter({{0 /* t0 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // gbuffer depth

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
        psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
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

    RtProgram::Desc photonSplatVolume;
    {
        std::vector<std::wstring> libraryExports = {
            L"RayGen", L"ParticleIntersect", L"AnyHit", L"ClosestHit", L"Miss"
        };
        photonSplatVolume.addShaderLibrary(g_pPhotonSplattingVolume, ARRAYSIZE(g_pPhotonSplattingVolume), libraryExports);
        photonSplatVolume.setRayGen("RayGen");
        photonSplatVolume
            .addHitGroup(0, 0, "ClosestHit", "AnyHit", "ParticleIntersect")
            .addMiss(0, "Miss");

        photonSplatVolume.configureGlobalRootSignature([](RootSignatureGenerator &config) {
            // GlobalRootSignatureParams::AccelerationStructureSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 0 /* t0 */);
            // GlobalRootSignatureParams::OutputViewSlot
            config.AddHeapRangesParameter({{0 /* u0 */, 2 /* ~u1 */, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0}}); // 2 slots: color and dir
            // GlobalRootSignatureParams::PerFrameConstantsSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 0 /* b0 */);

            D3D12_STATIC_SAMPLER_DESC matTexSampler = pointClampSampler;
            config.AddStaticSampler(matTexSampler);
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 0 /* t0 */, 9 /* space9 */); // material params
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 1 /* t1 */, 9 /* space9 */); // material texture params
            config.AddHeapRangesParameter({{2 /* t2 */, -1 /* unbounded */, 9 /* space9 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // material textures

            config.AddHeapRangesParameter({{0 /* t0 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // density
            config.AddHeapRangesParameter({{1 /* t1 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // photons
            config.AddHeapRangesParameter({{2 /* t2 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // photon object space positions
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 1 /* b1 */, 0, 1); // mapping consts

            config.AddHeapRangesParameter({{0 /* u0 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0}}); // rt buffer

            config.AddHeapRangesParameter({{0 /* t0 */, 1, 2 /* space2 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // gbuffer depth
        });
    }
    mRtPhotonSplattingVolumePass.mRtProgram = RtProgram::create(context, photonSplatVolume);
    mRtPhotonSplattingVolumePass.mRtState = RtState::create(context);
    mRtPhotonSplattingVolumePass.mRtState->setProgram(mRtPhotonSplattingVolumePass.mRtProgram);
    mRtPhotonSplattingVolumePass.mRtState->setMaxTraceRecursionDepth(1);
    mRtPhotonSplattingVolumePass.mRtState->setMaxAttributeSize(4);
    mRtPhotonSplattingVolumePass.mRtState->setMaxPayloadSize(sizeof(PhotonSplatPayload));

    {
        RootSignatureGenerator rsConfig;
        rsConfig.AddHeapRangesParameter({{0 /* t0 */, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // density
        rsConfig.AddHeapRangesParameter({{1 /* t1 */, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // photons
        rsConfig.AddHeapRangesParameter({{2 /* t2 */, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // photon object space positions
        rsConfig.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 0 /* b0 */, 0, 1);
        rsConfig.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 1 /* b1 */, 0, 1);
        rsConfig.AddHeapRangesParameter({{0 /* u0 */, 2 /* ~u1 */, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0}}); // voxel maps x2

        mPhotonSplattingVoxelPass.rootSignature = rsConfig.Generate(device, false);

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = mPhotonSplattingVoxelPass.rootSignature.Get();
        psoDesc.CS = CD3DX12_SHADER_BYTECODE(g_pPhotonSplattingVoxels, ARRAYSIZE(g_pPhotonSplattingVoxels));

        ThrowIfFailed(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mPhotonSplattingVoxelPass.stateObject)));
        NAME_D3D12_OBJECT(mPhotonSplattingVoxelPass.stateObject);
    }

    /*******************************
     *  G-Buffer Pass
     *******************************/
    {
        D3D12_STATIC_SAMPLER_DESC albedoSampler = pointClampSampler;
        albedoSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        albedoSampler.ShaderRegister = 1;
        albedoSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        RootSignatureGenerator rsConfig;
        rsConfig.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 0 /* b0 */); // per frame constants
        rsConfig.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 1 /* b1 */, 0, SizeOfInUint32(PerObjectConstants)); // object constants
        rsConfig.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 0 /* b0 */, 1 /* space1 */, SizeOfInUint32(MaterialParams)); // material
        rsConfig.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 1 /* t1 */, 9 /* space9 */); // material texture params
        rsConfig.AddHeapRangesParameter({{2 /* t2 */, -1 /* unbounded */, 9 /* space9 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // material textures
        rsConfig.AddHeapRangesParameter({{1 /* b1 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 0}}); // procedural aabb attrs
        rsConfig.AddStaticSampler(albedoSampler);

        mGBufferPass.rootSignature = rsConfig.Generate(device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        // DirectX::VertexPositionNormal::InputLayout
        D3D12_INPUT_ELEMENT_DESC inputDescs[] =
        {
            { "SV_POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",      0, DXGI_FORMAT_R32G32B32_FLOAT,    0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = mGBufferPass.rootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(g_pGBuffer_vs, ARRAYSIZE(g_pGBuffer_vs));
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(g_pGBuffer_ps, ARRAYSIZE(g_pGBuffer_ps));
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
        psoDesc.InputLayout = { inputDescs, ARRAYSIZE(inputDescs) };
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 3; //GBufferID::Count - 1; // exclude depth buffer from count
        psoDesc.RTVFormats[0] = mGBufferFormats.at(GBufferID::Normal);
        psoDesc.RTVFormats[1] = mGBufferFormats.at(GBufferID::VolMask);
        psoDesc.RTVFormats[2] = mGBufferFormats.at(GBufferID::LinDepth);
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleMask = 1;
        psoDesc.SampleDesc = { 1, 0 };

        ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mGBufferPass.stateObject)));
        NAME_D3D12_OBJECT(mGBufferPass.stateObject);

        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mGBufferVolumeStateObject)));
        NAME_D3D12_OBJECT(mGBufferVolumeStateObject);
    }

    /*******************************
     *  Combine Pass
     *******************************/
    {
        D3D12_STATIC_SAMPLER_DESC materialSampler = pointClampSampler;
        materialSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        materialSampler.ShaderRegister = 1;
        materialSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC lightSampler = anisotropicSampler;
        lightSampler.ShaderRegister = 0;
        lightSampler.RegisterSpace = 1;
        lightSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC voxelSampler = linearSampler;
        voxelSampler.ShaderRegister = 1;
        voxelSampler.RegisterSpace = 1;
        voxelSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        RootSignatureGenerator rsConfig;
        rsConfig.AddHeapRangesParameter({{0 /* t0 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // normals
        rsConfig.AddHeapRangesParameter({{1 /* t1 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // splat0
        rsConfig.AddHeapRangesParameter({{2 /* t2 */, 1, 1 /* space1 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // splat1

        rsConfig.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 0 /* b0 */); // per frame constants
        rsConfig.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 1 /* b1 */); // photon map constants

        rsConfig.AddHeapRangesParameter({{0 /* t0 */, 1, 2 /* space2 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // depth
        rsConfig.AddHeapRangesParameter({{1 /* t1 */, 1, 2 /* space2 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // volmask
        rsConfig.AddHeapRangesParameter({{2 /* t2 */, 2 /* ~t3 */, 2 /* space2 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // voxel maps x2

        rsConfig.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 0 /* t0 */, 9 /* space9 */); // material params
        rsConfig.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 1 /* t1 */, 9 /* space9 */); // material texture params
        rsConfig.AddHeapRangesParameter({{2 /* t2 */, -1 /* unbounded */, 9 /* space9 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}}); // material textures

        rsConfig.AddStaticSampler(materialSampler);
        rsConfig.AddStaticSampler(lightSampler);
        rsConfig.AddStaticSampler(voxelSampler);

        mCombinePass.rootSignature = rsConfig.Generate(device, false);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = mCombinePass.rootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(g_pPhotonLightingCombine_vs, ARRAYSIZE(g_pPhotonLightingCombine_vs));
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(g_pPhotonLightingCombine_ps, ARRAYSIZE(g_pPhotonLightingCombine_ps));
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = outputFormat;
        psoDesc.SampleMask = 1;
        psoDesc.SampleDesc = { 1, 0 };

        ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mCombinePass.stateObject)));
        NAME_D3D12_OBJECT(mCombinePass.stateObject);
    }

    mShaderOptions.showVolumePhotonsOnly = false;
    mShaderOptions.showRawSplattingResult = false;
    mShaderOptions.skipPhotonTracing = false;
    mShaderOptions.volumeSplattingMethod = SplatMethod::Raster;

    auto now = std::chrono::high_resolution_clock::now();
    auto msTime = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    mRng = std::mt19937(uint32_t(msTime.time_since_epoch().count()));

    // Create descriptor heaps for UAV CPU descriptors and render target views
    {
        D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc = {};
        descriptorHeapDesc.NumDescriptors = 8;
        descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        mCpuOnlyDescriptorHeap = std::make_unique<DescriptorPile>(device, &descriptorHeapDesc);
        SetName(mCpuOnlyDescriptorHeap->Heap(), L"CPU-only descriptor heap");

        D3D12_DESCRIPTOR_HEAP_DESC rtvDescriptorHeapDesc = {};
        rtvDescriptorHeapDesc.NumDescriptors = 8;
        rtvDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        mRtvDescriptorHeap = std::make_unique<DescriptorPile>(device, &rtvDescriptorHeapDesc);

        D3D12_DESCRIPTOR_HEAP_DESC dsvDescriptorHeapDesc = {};
        dsvDescriptorHeapDesc.NumDescriptors = 1;
        dsvDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        mDsvDescriptorHeap = std::make_unique<DescriptorHeap>(device, &dsvDescriptorHeapDesc);
    }

    mPhotonSplatKernelShape = std::make_shared<DXTKExtend::GeometricModel>(device, [](auto& vertices, auto& indices) {
        GeometricPrimitive::CreateIcosahedron(vertices, indices);
    });

    if (DXGIGetDebugInterface1(0, IID_PPV_ARGS(&ga))) {
        ga = nullptr;
    }
}

HybridPipeline::~HybridPipeline() = default;

void HybridPipeline::setScene(RtScene::SharedPtr scene)
{
    mRtScene = scene->copy();
    mRtPhotonEmissionPass.mRtBindings = RtBindings::create(mRtContext, mRtPhotonEmissionPass.mRtProgram, scene);
    mRtPhotonMappingPass.mRtBindings = RtBindings::create(mRtContext, mRtPhotonMappingPass.mRtProgram, scene);
    mRtPhotonSplattingVolumePass.mRtBindings = RtBindings::create(mRtContext, mRtPhotonSplattingVolumePass.mRtProgram, RtScene::create());

    mRasterScene.resize(mRtScene->getNumInstances());
    for (UINT i = 0; i < mRtScene->getNumInstances(); i++) {
        auto model = mRtScene->getModel(i);
        switch (model->getGeometryType())
        {
        case RtModel::GeometryType::Triangles: {
            auto obj = toRtMesh(model);
            mRasterScene[i] = std::make_shared<DXTKExtend::GeometricModel>(
                obj->getVertexBuffer(), sizeof(Vertex),
                obj->getIndexBuffer(), DXGI_FORMAT_R32_UINT);
            break;
        }
        default: {
            auto obj = toRtProcedural(model);
            auto bb = obj->getBoundingBox();
            XMVECTOR size = bb.GetBoxMax() - bb.GetBoxMin();
            XMFLOAT3 boxSize;
            XMStoreFloat3(&boxSize, size);
            mRasterScene[i] = std::make_shared<DXTKExtend::GeometricModel>(mRtContext->getDevice(), [&](auto& vertices, auto& indices) {
                GeometricPrimitive::CreateBox(vertices, indices, boxSize, false);
                auto halfSize = size * 0.5;
                for (DXTKExtend::GeometricModel::VertexType &vtx : vertices) {
                    XMVECTOR position = XMLoadFloat3(&vtx.position);
                    position = position + halfSize + bb.GetBoxMin();
                    XMStoreFloat3(&vtx.position, position);
                }
            });
            break;
        }
        }
    }

    mNeedPhotonMap = true;
}

void HybridPipeline::buildAccelerationStructures()
{
    mRtScene->build(mRtContext, mRtPhotonMappingPass.mRtProgram->getHitProgramCount());

    // NOTE: abusing this call to initialize other resources
    mRtContext->getCommandList()->CopyResource(mMaterialParamsResource.Get(), mMaterialParamsBuffer.GetResource().Get());
    mRtContext->transitionResource(mMaterialParamsResource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void HybridPipeline::loadResources(ID3D12CommandQueue *uploadCommandQueue, UINT frameCount)
{
    auto device = mRtContext->getDevice();

    mTextureResources.resize(2);
    mTextureSrvGpuHandles.resize(3);

    mMaterialParamsBuffer.Create(device, mMaterials.size(), 1, L"MaterialParams");
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

    for (int i = 0; i < mMaterials.size(); i++) {
        mMaterialParamsBuffer[i] = mMaterials[i].params;
    }
    mMaterialParamsBuffer.CopyStagingToGpu();
    *mMaterialParamsResource.ReleaseAndGetAddressOf() = CreateBuffer(device, mMaterialParamsBuffer.InstanceSize(), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, kDefaultHeapProps);
    NAME_D3D12_OBJECT(mMaterialParamsResource);

    // Create per-frame constant buffer
    mConstantBuffer.Create(device, frameCount, L"PerFrameConstantBuffer");
    mRasterConstantsBuffer.Create(device, frameCount, L"PerFrameConstantBufferR");

    mDirLights.Create(device, NUM_DIR_LIGHTS, frameCount, L"DirectionalLightBuffer");
    mPointLights.Create(device, NUM_POINT_LIGHTS, frameCount, L"PointLightBuffer");

    mPhotonMappingConstants.Create(device, 1, L"PhotonMappingConstantBuffer");

    mPhotonMappingConstants->kernelScaleMin = 0.01f;
    mPhotonMappingConstants->kernelScaleMax = 1.0f;
    mPhotonMappingConstants->uniformScaleStrength = 3.2f;
    mPhotonMappingConstants->maxLightShapingScale = 5.0f;
    mPhotonMappingConstants->kernelCompressFactor = 0.8f;
    mPhotonMappingConstants->volumeSplatPhotonSize = .01f;
    mPhotonMappingConstants->particlesPerSlab = 16;
    mPhotonMappingConstants->photonGeometryBuildStrategy = 0;
}

inline void CreateClearableUAV(ID3D12Device* device, DescriptorPile* heap, OutputResourceView& view, D3D12_UNORDERED_ACCESS_VIEW_DESC const& uavDesc)
{
    D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
    uavCpuHandle = heap->GetCpuHandle(heap->Allocate());
    device->CreateUnorderedAccessView(view.Resource.Get(), nullptr, &uavDesc, uavCpuHandle);
    view.Uav.cpuHandle = uavCpuHandle;
}

inline void CreateTextureRTV(ID3D12Device* device, DescriptorPile* heap, OutputResourceView& view)
{
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    view.Rtv.heapIndex = view.Rtv.heapIndex == UINT_MAX ? heap->Allocate() : view.Rtv.heapIndex;
    view.Rtv.cpuHandle = heap->GetCpuHandle(view.Rtv.heapIndex);
    device->CreateRenderTargetView(view.Resource.Get(), &rtvDesc, view.Rtv.cpuHandle);
}

inline void CreateTextureSRV(const RtContext::SharedPtr pRtContext, OutputResourceView& view)
{
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle;
    view.Srv.heapIndex = pRtContext->allocateDescriptor(&srvCpuHandle, view.Srv.heapIndex);
    view.Srv.gpuHandle = pRtContext->createTextureSRVHandle(view.Resource.Get(), false, view.Srv.heapIndex);
    view.Srv.cpuHandle = srvCpuHandle;
}

inline void CreateBufferSRV(const RtContext::SharedPtr pRtContext, OutputResourceView& view, UINT structureStride = 4)
{
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle;
    view.Srv.heapIndex = pRtContext->allocateDescriptor(&srvCpuHandle, view.Srv.heapIndex);
    view.Srv.gpuHandle = pRtContext->createBufferSRVHandle(view.Resource.Get(), false, structureStride, view.Srv.heapIndex);
    view.Srv.cpuHandle = srvCpuHandle;
}

inline void CreateStructuredBufferUAV(ID3D12Device* device, const RtContext::SharedPtr pRtContext, OutputResourceView& view, UINT structureStride, UINT numElements)
{
    D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
    view.Uav.heapIndex = pRtContext->allocateDescriptor(&uavCpuHandle, view.Uav.heapIndex);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.Buffer.StructureByteStride = structureStride;
    uavDesc.Buffer.NumElements = numElements;
    device->CreateUnorderedAccessView(view.Resource.Get(), nullptr, &uavDesc, uavCpuHandle);

    view.Uav.gpuHandle = pRtContext->getDescriptorGPUHandle(view.Uav.heapIndex);
    view.Uav.cpuHandle = uavCpuHandle;
}

void HybridPipeline::createOutputResource(DXGI_FORMAT format, UINT width, UINT height)
{
    auto device = mRtContext->getDevice();

    mRtvDescriptorHeap = std::make_unique<DescriptorPile>(mRtvDescriptorHeap->Heap());
    mCpuOnlyDescriptorHeap = std::make_unique<DescriptorPile>(mCpuOnlyDescriptorHeap->Heap());

    mNeedPhotonMap = true;

    // Final output resource

    AllocateRTVTexture(device, format, width, height, mOutput.Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Final output resource", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CreateTextureRTV(device, mRtvDescriptorHeap.get(), mOutput);
    CreateTextureSRV(mRtContext, mOutput);

    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
        mOutputUav.heapIndex = mRtContext->allocateDescriptor(&uavCpuHandle, mOutputUav.heapIndex);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(mOutput.Resource.Get(), nullptr, &uavDesc, uavCpuHandle);

        mOutputUav.gpuHandle = mRtContext->getDescriptorGPUHandle(mOutputUav.heapIndex);
        mOutputUav.cpuHandle = uavCpuHandle;
    }

    // Intermediate output resources

    mPhotonEmitters.Create(device, mRtScene->getNumInstances(), 1, L"PhotonEmitters");
    mPhotonUploadBuffer.Create(device, MAX_PHOTON_SEED_SAMPLES, 1, L"PhotonSeed");
    AllocateUAVBuffer(device, MAX_PHOTON_SEED_SAMPLES * sizeof(Photon), mPhotonSeed.Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CreateBufferSRV(mRtContext, mPhotonSeed, sizeof(Photon));
    CreateStructuredBufferUAV(device, mRtContext, mPhotonSeed, sizeof(Photon), MAX_PHOTON_SEED_SAMPLES);

    AllocateUAVBuffer(device, PhotonMapID::Count * 4, mPhotonMapCounters.Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    AllocateReadbackBuffer(device, PhotonMapID::Count * 4, mPhotonMapCounterReadback.ReleaseAndGetAddressOf(), L"photon map counter readback");

    AllocateUAVBuffer(device, MAX_PHOTONS * sizeof(Photon), mPhotonMap.Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    AllocateUAVBuffer(device, MAX_PHOTONS * sizeof(Photon), mVolumePhotonMap.Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CreateBufferSRV(mRtContext, mPhotonMap, sizeof(Photon));
    CreateBufferSRV(mRtContext, mVolumePhotonMap, sizeof(Photon));

    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
        mPhotonMapCounters.Uav.heapIndex = mRtContext->allocateDescriptor(&uavCpuHandle, mPhotonMapCounters.Uav.heapIndex);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Format = DXGI_FORMAT_R32_UINT;
        uavDesc.Buffer.NumElements = PhotonMapID::Count;
        device->CreateUnorderedAccessView(mPhotonMapCounters.Resource.Get(), nullptr, &uavDesc, uavCpuHandle);

        mPhotonMapCounters.Uav.gpuHandle = mRtContext->getDescriptorGPUHandle(mPhotonMapCounters.Uav.heapIndex);
        CreateClearableUAV(device, mCpuOnlyDescriptorHeap.get(), mPhotonMapCounters, uavDesc);
    }

    CreateStructuredBufferUAV(device, mRtContext, mPhotonMap, sizeof(Photon), MAX_PHOTONS);
    CreateStructuredBufferUAV(device, mRtContext, mVolumePhotonMap, sizeof(Photon), MAX_PHOTONS);
    ThrowIfFalse(mPhotonMapCounters.Uav.heapIndex + 1 == mPhotonMap.Uav.heapIndex);
    ThrowIfFalse(mPhotonMapCounters.Uav.heapIndex + 2 == mVolumePhotonMap.Uav.heapIndex);

    AllocateUAVBuffer(device, MAX_PHOTONS * sizeof(PhotonAABB), mVolumePhotonAabbs.Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    AllocateUAVBuffer(device, MAX_PHOTONS * sizeof(XMVECTOR), mVolumePhotonPositions.Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    AllocateUAVBuffer(device, MAX_PHOTONS * sizeof(XMVECTOR), mVolumePhotonPositionsObjSpace.Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    AllocateReadbackBuffer(device, MAX_PHOTONS * sizeof(XMVECTOR), mVolumePhotonPositionsReadback.ReleaseAndGetAddressOf());
    CreateBufferSRV(mRtContext, mVolumePhotonPositionsObjSpace, sizeof(XMVECTOR));
    CreateStructuredBufferUAV(device, mRtContext, mVolumePhotonAabbs, sizeof(PhotonAABB), MAX_PHOTONS);

    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
        mVolumePhotonPositions.Uav.heapIndex = mRtContext->allocateDescriptor(&uavCpuHandle, mVolumePhotonPositions.Uav.heapIndex);
        ThrowIfFalse(mVolumePhotonAabbs.Uav.heapIndex + 1 == mVolumePhotonPositions.Uav.heapIndex);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        uavDesc.Buffer.NumElements = MAX_PHOTONS;
        device->CreateUnorderedAccessView(mVolumePhotonPositions.Resource.Get(), nullptr, &uavDesc, uavCpuHandle);

        mVolumePhotonPositions.Uav.gpuHandle = mRtContext->getDescriptorGPUHandle(mVolumePhotonPositions.Uav.heapIndex);
    }

    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
        mVolumePhotonPositionsObjSpace.Uav.heapIndex = mRtContext->allocateDescriptor(&uavCpuHandle, mVolumePhotonPositionsObjSpace.Uav.heapIndex);
        ThrowIfFalse(mVolumePhotonAabbs.Uav.heapIndex + 2 == mVolumePhotonPositionsObjSpace.Uav.heapIndex);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        uavDesc.Buffer.NumElements = MAX_PHOTONS;
        device->CreateUnorderedAccessView(mVolumePhotonPositionsObjSpace.Resource.Get(), nullptr, &uavDesc, uavCpuHandle);

        mVolumePhotonPositionsObjSpace.Uav.gpuHandle = mRtContext->getDescriptorGPUHandle(mVolumePhotonPositionsObjSpace.Uav.heapIndex);
    }

    const double preferredTileSizeInPixels = 8.0;
    UINT numTilesX = static_cast<UINT>(ceil(width / preferredTileSizeInPixels));
    UINT numTilesY = static_cast<UINT>(ceil(height / preferredTileSizeInPixels));
    mPhotonMappingConstants->numTiles = XMUINT2(numTilesX, numTilesY);
    mPhotonMappingConstants.CopyStagingToGpu();
    AllocateUAVTexture(device, DXGI_FORMAT_R32_UINT, numTilesX, numTilesY, mPhotonDensity.Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CreateTextureSRV(mRtContext, mPhotonDensity);

    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
        mPhotonDensity.Uav.heapIndex = mRtContext->allocateDescriptor(&uavCpuHandle, mPhotonDensity.Uav.heapIndex);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(mPhotonDensity.Resource.Get(), nullptr, &uavDesc, uavCpuHandle);

        mPhotonDensity.Uav.gpuHandle = mRtContext->getDescriptorGPUHandle(mPhotonDensity.Uav.heapIndex);
        CreateClearableUAV(device, mCpuOnlyDescriptorHeap.get(), mPhotonDensity, uavDesc);
    }

    AllocateRTVTexture(device, DXGI_FORMAT_R32G32B32A32_FLOAT, width, height, mPhotonSplat[0].Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"PhotonSplatResultColorDirX", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CreateTextureRTV(device, mRtvDescriptorHeap.get(), mPhotonSplat[0]);
    CreateTextureSRV(mRtContext, mPhotonSplat[0]);

    AllocateRTVTexture(device, DXGI_FORMAT_R32G32_FLOAT, width, height, mPhotonSplat[1].Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"PhotonSplatResultDirYZ", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CreateTextureRTV(device, mRtvDescriptorHeap.get(), mPhotonSplat[1]);
    CreateTextureSRV(mRtContext, mPhotonSplat[1]);

    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
        mPhotonSplatUav[0].heapIndex = mRtContext->allocateDescriptor(&uavCpuHandle, mPhotonSplatUav[0].heapIndex);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(mPhotonSplat[0].Resource.Get(), nullptr, &uavDesc, uavCpuHandle);

        mPhotonSplatUav[0].gpuHandle = mRtContext->getDescriptorGPUHandle(mPhotonSplatUav[0].heapIndex);
        mPhotonSplatUav[0].cpuHandle = uavCpuHandle;
    }

    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
        mPhotonSplatUav[1].heapIndex = mRtContext->allocateDescriptor(&uavCpuHandle, mPhotonSplatUav[1].heapIndex);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(mPhotonSplat[1].Resource.Get(), nullptr, &uavDesc, uavCpuHandle);

        mPhotonSplatUav[1].gpuHandle = mRtContext->getDescriptorGPUHandle(mPhotonSplatUav[1].heapIndex);
        mPhotonSplatUav[1].cpuHandle = uavCpuHandle;
    }

    ThrowIfFalse(mPhotonSplatUav[0].heapIndex + 1 == mPhotonSplatUav[1].heapIndex);

    AllocateUAVTexture3D(device, DXGI_FORMAT_R32G32_FLOAT, width, height, PARTICLE_BUFFER_SIZE, mPhotonSplatRtBuffer.Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"payload buffer for RT volsplat");

    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
        mPhotonSplatRtBuffer.Uav.heapIndex = mRtContext->allocateDescriptor(&uavCpuHandle, mPhotonSplatRtBuffer.Uav.heapIndex);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uavDesc.Texture3D.WSize = -1;
        device->CreateUnorderedAccessView(mPhotonSplatRtBuffer.Resource.Get(), nullptr, &uavDesc, uavCpuHandle);

        mPhotonSplatRtBuffer.Uav.gpuHandle = mRtContext->getDescriptorGPUHandle(mPhotonSplatRtBuffer.Uav.heapIndex);
    }

    const UINT VOXEL_GRID_DIMS = 128;
    AllocateUAVTexture3D(device, DXGI_FORMAT_R32G32B32A32_FLOAT, VOXEL_GRID_DIMS, VOXEL_GRID_DIMS, VOXEL_GRID_DIMS, mPhotonSplatVoxels[0].Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"Voxel map for volume photons 0");
    CreateTextureSRV(mRtContext, mPhotonSplatVoxels[0]);
    AllocateUAVTexture3D(device, DXGI_FORMAT_R32G32B32A32_FLOAT, VOXEL_GRID_DIMS, VOXEL_GRID_DIMS, VOXEL_GRID_DIMS, mPhotonSplatVoxels[1].Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"Voxel map for volume photons 1");
    CreateTextureSRV(mRtContext, mPhotonSplatVoxels[1]);

    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
        mPhotonSplatVoxels[0].Uav.heapIndex = mRtContext->allocateDescriptor(&uavCpuHandle, mPhotonSplatVoxels[0].Uav.heapIndex);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uavDesc.Texture3D.WSize = -1;
        device->CreateUnorderedAccessView(mPhotonSplatVoxels[0].Resource.Get(), nullptr, &uavDesc, uavCpuHandle);

        mPhotonSplatVoxels[0].Uav.gpuHandle = mRtContext->getDescriptorGPUHandle(mPhotonSplatVoxels[0].Uav.heapIndex);
        mPhotonSplatVoxels[0].Uav.cpuHandle = uavCpuHandle;
        CreateClearableUAV(device, mCpuOnlyDescriptorHeap.get(), mPhotonSplatVoxels[0], uavDesc);
    }

    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
        mPhotonSplatVoxels[1].Uav.heapIndex = mRtContext->allocateDescriptor(&uavCpuHandle, mPhotonSplatVoxels[1].Uav.heapIndex);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uavDesc.Texture3D.WSize = -1;
        device->CreateUnorderedAccessView(mPhotonSplatVoxels[1].Resource.Get(), nullptr, &uavDesc, uavCpuHandle);

        mPhotonSplatVoxels[1].Uav.gpuHandle = mRtContext->getDescriptorGPUHandle(mPhotonSplatVoxels[1].Uav.heapIndex);
        mPhotonSplatVoxels[1].Uav.cpuHandle = uavCpuHandle;
        CreateClearableUAV(device, mCpuOnlyDescriptorHeap.get(), mPhotonSplatVoxels[1], uavDesc);
    }

    AllocateRTVTexture(device, mGBufferFormats.at(GBufferID::Normal), width, height, mGBuffer[GBufferID::Normal].Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"G buffer normals");
    CreateTextureRTV(device, mRtvDescriptorHeap.get(), mGBuffer[GBufferID::Normal]);
    CreateTextureSRV(mRtContext, mGBuffer[GBufferID::Normal]);

    //AllocateRTVTexture(device, mGBufferFormats.at(GBufferID::Albedo), width, height, mGBuffer[GBufferID::Albedo].Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"G buffer flux");
    //CreateTextureRTV(device, mRtvDescriptorHeap.get(), mGBuffer[GBufferID::Albedo]);
    //CreateTextureSRV(mRtContext, mGBuffer[GBufferID::Albedo]);

    AllocateRTVTexture(device, mGBufferFormats.at(GBufferID::VolMask), width, height, mGBuffer[GBufferID::VolMask].Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"G buffer volume mask");
    CreateTextureRTV(device, mRtvDescriptorHeap.get(), mGBuffer[GBufferID::VolMask]);
    CreateTextureSRV(mRtContext, mGBuffer[GBufferID::VolMask]);

    AllocateRTVTexture(device, mGBufferFormats.at(GBufferID::LinDepth), width, height, mGBuffer[GBufferID::LinDepth].Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"G buffer linear depth", D3D12_RESOURCE_FLAG_NONE, { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX });
    CreateTextureRTV(device, mRtvDescriptorHeap.get(), mGBuffer[GBufferID::LinDepth]);
    CreateTextureSRV(mRtContext, mGBuffer[GBufferID::LinDepth]);

    AllocateDepthTexture(device, width, height, mGBuffer[GBufferID::Depth].Resource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"G buffer depth");

    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

        mGBuffer[GBufferID::Depth].Dsv.cpuHandle = mDsvDescriptorHeap->GetCpuHandle(0);
        device->CreateDepthStencilView(mGBuffer[GBufferID::Depth].Resource.Get(), &dsvDesc, mGBuffer[GBufferID::Depth].Dsv.cpuHandle);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = -1;

        D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle;
        mGBuffer[GBufferID::Depth].Srv.heapIndex = mRtContext->allocateDescriptor(&srvCpuHandle, mGBuffer[GBufferID::Depth].Srv.heapIndex);
        mGBuffer[GBufferID::Depth].Srv.gpuHandle = mRtContext->getDescriptorGPUHandle(mGBuffer[GBufferID::Depth].Srv.heapIndex);
        mGBuffer[GBufferID::Depth].Srv.cpuHandle = srvCpuHandle;
        device->CreateShaderResourceView(mGBuffer[GBufferID::Depth].Resource.Get(), &srvDesc, srvCpuHandle);
    }
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

    if (hasCameraMoved(*mCamera, mLastCameraVPMatrix)) {
        mLastCameraVPMatrix = mCamera->GetViewProjMatrix();
        mNumPhotons = 0;
    }

    CameraParams cameraParams = {};
    XMStoreFloat4(&cameraParams.worldEyePos, mCamera->GetPosition());
    calculateCameraVariables(*mCamera, mCamera->GetAspectRatio(), &cameraParams.U, &cameraParams.V, &cameraParams.W);
    calculateCameraFrustum(*mCamera, &cameraParams.frustumNH, &cameraParams.frustumNV, &cameraParams.frustumNearFar);
    float xJitter = (mRngDist(mRng) - 0.5f) / float(width);
    float yJitter = (mRngDist(mRng) - 0.5f) / float(height);
    cameraParams.jitters = XMFLOAT2(xJitter, yJitter);
    cameraParams.frameCount = elapsedFrames;

    mConstantBuffer->cameraParams = cameraParams;
    mConstantBuffer->options = {};
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
    XMStoreFloat3(&mPhotonMappingConstants->volumeBboxMin, mRtScene->getBoundingBox().GetBoxMin());
    XMStoreFloat3(&mPhotonMappingConstants->volumeBboxMax, mRtScene->getBoundingBox().GetBoxMax());
    mPhotonMappingConstants.CopyStagingToGpu();

    mRasterConstantsBuffer->cameraParams = cameraParams;
    mRasterConstantsBuffer->options = mShaderOptions;
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

void HybridPipeline::buildVolumePhotonAccelerationStructure(UINT buildStrategy)
{
    static ComPtr<ID3D12Resource> mAabbBuffer;
    static ComPtr<ID3D12Resource> blasScratch;
    static ComPtr<ID3D12Resource> tlasScratch;
    static ComPtr<ID3D12Resource> tlasInstanceDescs;

    auto context = mRtContext;
    auto device = context->getDevice();
    auto commandList = context->getCommandList();
    auto fallbackDevice = context->getFallbackDevice();
    auto fallbackCommandList = context->getFallbackCommandList();

    const UINT numPhotons = mPhotonMapCounts[PhotonMapID::Volume];

    switch (buildStrategy) {
    case 0: {
        // N AABBs at photon positions in 1 BLAS, 1 instance at origin in TLAS
        // Needs: AABBs array

        // Build BLAS
        {
            auto transitions = ScopedBarrier(commandList, { CD3DX12_RESOURCE_BARRIER::Transition(
                mVolumePhotonAabbs.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            });

            D3D12_RAYTRACING_GEOMETRY_DESC descriptor = {};
            descriptor.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
            descriptor.AABBs.AABBCount = numPhotons;
            descriptor.AABBs.AABBs.StartAddress = mVolumePhotonAabbs.Resource->GetGPUVirtualAddress();
            descriptor.AABBs.AABBs.StrideInBytes = sizeof(PhotonAABB);
            descriptor.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS buildInputs = {};
            buildInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            buildInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            buildInputs.NumDescs = 1;
            buildInputs.pGeometryDescs = &descriptor;
            buildInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
            fallbackDevice->GetRaytracingAccelerationStructurePrebuildInfo(&buildInputs, &info);
            UINT64 scratchSizeInBytes = ROUND_UP(info.ScratchDataSizeInBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            UINT64 resultSizeInBytes = ROUND_UP(info.ResultDataMaxSizeInBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

            D3D12_RESOURCE_STATES initialResourceState = fallbackDevice->GetAccelerationStructureResourceState();
            AllocateUAVBuffer(device, resultSizeInBytes, mVolumePhotonBlas.ReleaseAndGetAddressOf(), initialResourceState);
            AllocateUAVBuffer(device, scratchSizeInBytes, blasScratch.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
            buildDesc.Inputs = buildInputs;
            buildDesc.DestAccelerationStructureData = mVolumePhotonBlas->GetGPUVirtualAddress();
            buildDesc.ScratchAccelerationStructureData = blasScratch->GetGPUVirtualAddress();

            // Build the AS
            fallbackCommandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
            context->insertUAVBarrier(mVolumePhotonBlas.Get());
        }
        // Build TLAS
        {
            nv_helpers_dx12::TopLevelASGenerator tlasGenerator;
            tlasGenerator.AddInstance(mVolumePhotonBlas.Get(), XMMatrixIdentity(), 0, 0, 0xFF);

            UINT64 scratchSizeInBytes = 0;
            UINT64 resultSizeInBytes = 0;
            UINT64 instanceDescsSize = 0;
            tlasGenerator.ComputeASBufferSizes(fallbackDevice, false, &scratchSizeInBytes, &resultSizeInBytes, &instanceDescsSize);

            D3D12_RESOURCE_STATES initialResourceState = fallbackDevice->GetAccelerationStructureResourceState();
            AllocateUAVBuffer(device, resultSizeInBytes, mVolumePhotonTlas.ReleaseAndGetAddressOf(), initialResourceState);
            AllocateUAVBuffer(device, scratchSizeInBytes, tlasScratch.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            *tlasInstanceDescs.ReleaseAndGetAddressOf() = CreateBuffer(device, instanceDescsSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, kUploadHeapProps);

            context->bindDescriptorHeap();

            tlasGenerator.Generate(commandList, fallbackCommandList, tlasScratch.Get(), mVolumePhotonTlas.Get(), tlasInstanceDescs.Get(),
                [&](ID3D12Resource *resource) -> WRAPPED_GPU_POINTER { return context->createBufferUAVWrappedPointer(resource); });

            mVolumePhotonTlasWrappedPtr = context->createBufferUAVWrappedPointer(mVolumePhotonTlas.Get());
        }
    } break;

    case 1: {
        // 1 AABB at origin in 1 BLAS, N instances at photon positions in TLAS
        // Needs: Photon positions array

        // Build BLAS
        {
            auto photonRadius = mPhotonMappingConstants->volumeSplatPhotonSize;
            auto mAabb = D3D12_RAYTRACING_AABB{
                -photonRadius, -photonRadius, -photonRadius,
                photonRadius, photonRadius, photonRadius,
            };
            AllocateUploadBuffer(device, &mAabb, sizeof(mAabb), mAabbBuffer.ReleaseAndGetAddressOf());

            nv_helpers_dx12::BottomLevelASGenerator blasGenerator;
            blasGenerator.AddAabbBuffer(mAabbBuffer.Get(), false);

            UINT64 scratchSizeInBytes = 0;
            UINT64 resultSizeInBytes = 0;
            blasGenerator.ComputeASBufferSizes(fallbackDevice, false, &scratchSizeInBytes, &resultSizeInBytes);

            D3D12_RESOURCE_STATES initialResourceState = fallbackDevice->GetAccelerationStructureResourceState();
            AllocateUAVBuffer(device, resultSizeInBytes, mVolumePhotonBlas.ReleaseAndGetAddressOf(), initialResourceState);
            AllocateUAVBuffer(device, scratchSizeInBytes, blasScratch.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            blasGenerator.Generate(commandList, fallbackCommandList, blasScratch.Get(), mVolumePhotonBlas.Get());
        }
        // Build TLAS
        {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS buildInputs = {};
            buildInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            buildInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            buildInputs.NumDescs = numPhotons;
            buildInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
            fallbackDevice->GetRaytracingAccelerationStructurePrebuildInfo(&buildInputs, &info);

            UINT64 resultSizeInBytes = ROUND_UP(info.ResultDataMaxSizeInBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            UINT64 scratchSizeInBytes = ROUND_UP(info.ScratchDataSizeInBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            UINT64 instanceDescsSize = ROUND_UP(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * static_cast<UINT64>(numPhotons), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

            D3D12_RESOURCE_STATES initialResourceState = fallbackDevice->GetAccelerationStructureResourceState();
            AllocateUAVBuffer(device, resultSizeInBytes, mVolumePhotonTlas.ReleaseAndGetAddressOf(), initialResourceState);
            AllocateUAVBuffer(device, scratchSizeInBytes, tlasScratch.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            *tlasInstanceDescs.ReleaseAndGetAddressOf() = CreateBuffer(device, instanceDescsSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, kUploadHeapProps);

            context->bindDescriptorHeap();

            // Copy the descriptors into the target descriptor buffer
            D3D12_RAYTRACING_FALLBACK_INSTANCE_DESC* instanceDescs;
            ThrowIfFailed(tlasInstanceDescs->Map(0, nullptr, reinterpret_cast<void**>(&instanceDescs)));

            // Read photon positions
            XMVECTOR* photonPositions;
            ThrowIfFailed(mVolumePhotonPositionsReadback->Map(0, nullptr, reinterpret_cast<void**>(&photonPositions)));

            for (uint32_t i = 0; i < buildInputs.NumDescs; i++)
            {
                instanceDescs[i] = {};
                instanceDescs[i].InstanceID = i;
                instanceDescs[i].InstanceContributionToHitGroupIndex = 0;
                instanceDescs[i].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
                instanceDescs[i].AccelerationStructure = context->createBufferUAVWrappedPointer(mVolumePhotonBlas.Get());
                instanceDescs[i].InstanceMask = 0xFF;
                // GLM is column major, the INSTANCE_DESC is row major
                XMMATRIX m = XMMatrixTranspose(XMMatrixTranslationFromVector(photonPositions[i]));
                memcpy(instanceDescs[i].Transform, &m, sizeof(instanceDescs[i].Transform));
            }

            mVolumePhotonPositionsReadback->Unmap(0, &CD3DX12_RANGE(0, 0));
            tlasInstanceDescs->Unmap(0, nullptr);

            buildInputs.InstanceDescs = tlasInstanceDescs->GetGPUVirtualAddress();

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
            buildDesc.Inputs = buildInputs;
            buildDesc.DestAccelerationStructureData = mVolumePhotonTlas->GetGPUVirtualAddress();
            buildDesc.ScratchAccelerationStructureData = tlasScratch->GetGPUVirtualAddress();

            // Build the top-level AS
            fallbackCommandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
            context->insertUAVBarrier(mVolumePhotonTlas.Get());

            mVolumePhotonTlasWrappedPtr = context->createBufferUAVWrappedPointer(mVolumePhotonTlas.Get());
        }
    } break;
    }
}

void HybridPipeline::render(ID3D12GraphicsCommandList *commandList, UINT frameIndex, UINT width, UINT height, UINT& pass)
{
    auto viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    auto scissorRect = CD3DX12_RECT(0, 0, (width), static_cast<LONG>(height));

    if (pass == Pass::Begin)
    {
        PIXEndEvent(commandList);
        if (ga) {
            ga->EndCapture();
            if (mNeedPhotonMap) ga->BeginCapture();
        }

        pass = Pass::GBuffer;
    }

    if (pass == Pass::GBuffer)
    {
        PIXBeginEvent(commandList, 0, L"GBuffer");

        std::initializer_list<D3D12_RESOURCE_BARRIER> barriers =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(mGBuffer[GBufferID::Normal].Resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(mGBuffer[GBufferID::VolMask].Resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(mGBuffer[GBufferID::LinDepth].Resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(mGBuffer[GBufferID::Depth].Resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE),
        };
        auto transitions = ScopedBarrier(commandList, barriers);

        D3D12_CPU_DESCRIPTOR_HANDLE targets[] =
        {
            mGBuffer[GBufferID::Normal].Rtv.cpuHandle,
            mGBuffer[GBufferID::VolMask].Rtv.cpuHandle,
            mGBuffer[GBufferID::LinDepth].Rtv.cpuHandle, // must be last
        };

        clearRtv(commandList, mGBuffer[GBufferID::Normal]);
        clearRtv(commandList, mGBuffer[GBufferID::VolMask]);
        //clearRtv(commandList, mGBufferTargetCpuHandle[GBufferID::Albedo]);
        const float clear[4] = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
        commandList->ClearRenderTargetView(mGBuffer[GBufferID::LinDepth].Rtv.cpuHandle, clear, 0, nullptr);
        commandList->ClearDepthStencilView(mGBuffer[GBufferID::Depth].Dsv.cpuHandle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);

        // Set necessary state.
        mRtContext->bindDescriptorHeap();
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissorRect);

        commandList->SetGraphicsRootSignature(mGBufferPass.rootSignature.Get());
        commandList->SetGraphicsRootConstantBufferView(0, mRasterConstantsBuffer.GpuVirtualAddress(frameIndex));
        commandList->SetGraphicsRootShaderResourceView(3, mTextureParams.GpuVirtualAddress());
        commandList->SetGraphicsRootDescriptorTable(4, mTextureSrvGpuHandles[2]);

        for (UINT instance = 0; instance < mRtScene->getNumInstances(); ++instance) {
            PerObjectConstants constants = {};
            constants.worldMatrix = mRtScene->getTransform(instance);
            constants.invWorldMatrix = XMMatrixInverse(nullptr, constants.worldMatrix);

            auto model = mRtScene->getModel(instance);
            if (mMaterials[model->mMaterialIndex].params.type == MaterialType::ParticipatingMedia) {
                commandList->OMSetRenderTargets(ARRAYSIZE(targets) - 1, targets, FALSE, &mGBuffer[GBufferID::Depth].Dsv.cpuHandle);
                commandList->SetPipelineState(mGBufferVolumeStateObject.Get());
                constants.isVolume = TRUE;
            } else {
                commandList->OMSetRenderTargets(ARRAYSIZE(targets), targets, FALSE, &mGBuffer[GBufferID::Depth].Dsv.cpuHandle);
                commandList->SetPipelineState(mGBufferPass.stateObject.Get());
            }

            if (model->getGeometryType() != RtModel::GeometryType::Triangles) {
                constants.isProcedural = TRUE;
                commandList->SetGraphicsRootDescriptorTable(5, toRtProcedural(model)->getPrimitiveConstantsCbvHandle());
            } else {
                constants.isProcedural = FALSE;
            }

            commandList->SetGraphicsRoot32BitConstants(1, SizeOfInUint32(PerObjectConstants), &constants, 0);
            commandList->SetGraphicsRoot32BitConstants(2, SizeOfInUint32(MaterialParams), &mMaterials[model->mMaterialIndex].params, 0);
            mRasterScene[instance]->Draw(commandList);
        }

        if (!mNeedPhotonMap && pass < Pass::PhotonSplatting)
        {
            pass = Pass::PhotonSplatting;
            return;
        }
        else {
            pass = Pass::PhotonEmission;
        }
    }

    // generate photon seed
    if (pass == Pass::PhotonEmission)
    {
        PIXEndEvent(commandList);
        PIXBeginEvent(commandList, 0, L"PhotonEmission");

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
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::OutputViewSlot, mPhotonSeed.Uav.gpuHandle);
        commandList->SetComputeRootShaderResourceView(GlobalRootSignatureParams::PhotonSourcesSRVSlot, mPhotonEmitters.GpuVirtualAddress());
        mRtContext->getFallbackCommandList()->SetTopLevelAccelerationStructure(GlobalRootSignatureParams::AccelerationStructureSlot, mRtScene->getTlasWrappedPtr());

        mRtContext->raytrace(mRtBindings, mRtState, maxSamples, numLights, 1);

        mRtContext->insertUAVBarrier(mPhotonSeed.Resource.Get());

        // copy photons emitted on CPU to the buffer
        mRtContext->transitionResource(mPhotonSeed.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(mPhotonSeed.Resource.Get(), mSamplesGpu * sizeof(Photon), mPhotonUploadBuffer.GetResource().Get(), 0, mSamplesCpu * sizeof(Photon));

        pass = Pass::PhotonTracing;
        return;
    }

    // generate photon map
    if (pass == Pass::PhotonTracing)
    {
        PIXEndEvent(commandList);
        PIXBeginEvent(commandList, 0, L"PhotonTracing");

        // Update shader table root arguments
        auto mRtBindings = mRtPhotonMappingPass.mRtBindings;
        auto mRtState = mRtPhotonMappingPass.mRtState;
        auto program = mRtBindings->getProgram();

        for (UINT rayType = 0; rayType < program->getHitProgramCount(); ++rayType) {
            for (UINT instance = 0; instance < mRtScene->getNumInstances(); ++instance) {
                auto &hitVars = mRtBindings->getHitVars(rayType, instance);
                auto rtModel = mRtScene->getModel(instance);
                switch (rtModel->getGeometryType())
                {
                case RtModel::GeometryType::Triangles: {
                    auto model = toRtMesh(rtModel);
                    hitVars->appendHeapRanges(model->getVertexBufferSrvHandle().ptr);
                    hitVars->appendHeapRanges(model->getIndexBufferSrvHandle().ptr);
                    break;
                }
                default: {
                    auto model = toRtProcedural(rtModel);
                    hitVars->appendHeapRanges(model->getPrimitiveConstantsCbvHandle().ptr);
                    break;
                }
                }
                hitVars->append32BitConstants((void*)&mMaterials[rtModel->mMaterialIndex].params, SizeOfInUint32(MaterialParams));
                hitVars->append32BitConstants((void*)&rtModel->mMaterialIndex, 1);
            }
        }

        mRtContext->transitionResource(mPhotonSeed.Resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        mRtBindings->apply(mRtContext, mRtState);

        clearUav(commandList, mPhotonDensity);

        // Set global root arguments
        mRtContext->bindDescriptorHeap();
        commandList->SetComputeRootSignature(program->getGlobalRootSignature());
        commandList->SetComputeRootConstantBufferView(GlobalRootSignatureParams::PerFrameConstantsSlot, mConstantBuffer.GpuVirtualAddress(frameIndex));
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::OutputViewSlot, mPhotonMapCounters.Uav.gpuHandle);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::PhotonDensityOutputViewSlot, mPhotonDensity.Uav.gpuHandle);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::PhotonGeometryOutputViewSlot, mVolumePhotonAabbs.Uav.gpuHandle);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::MaterialTextureSrvSlot, mTextureSrvGpuHandles[2]);
        commandList->SetComputeRootShaderResourceView(GlobalRootSignatureParams::MaterialTextureParamsSlot, mTextureParams.GpuVirtualAddress());
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::PhotonSourcesSRVSlot, mPhotonSeed.Srv.gpuHandle);
        commandList->SetComputeRootConstantBufferView(GlobalRootSignatureParams::PhotonMappingConstantsSlot, mPhotonMappingConstants.GpuVirtualAddress());
        mRtContext->getFallbackCommandList()->SetTopLevelAccelerationStructure(GlobalRootSignatureParams::AccelerationStructureSlot, mRtScene->getTlasWrappedPtr());

        mRtContext->raytrace(mRtBindings, mRtState, mSamplesCpu + mSamplesGpu, 1, 1);

        mRtContext->insertUAVBarrier(mPhotonMap.Resource.Get());
        mRtContext->transitionResource(mPhotonSeed.Resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        mRtContext->transitionResource(mPhotonMapCounters.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->CopyResource(mPhotonMapCounterReadback.Get(), mPhotonMapCounters.Resource.Get());
        mRtContext->transitionResource(mPhotonMapCounters.Resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        clearUav(commandList, mPhotonMapCounters);

        // photon positions for AS build
        if (mShaderOptions.volumeSplattingMethod == SplatMethod::Raytrace) {
            mRtContext->transitionResource(mVolumePhotonPositions.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            commandList->CopyResource(mVolumePhotonPositionsReadback.Get(), mVolumePhotonPositions.Resource.Get());
            mRtContext->transitionResource(mVolumePhotonPositions.Resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        mNumPhotons = 0;
        mNeedPhotonMap = false;
        mIsTracingFrame = true;
        pass = Pass::PhotonSplatting;
        return;
    }

    if (pass == Pass::PhotonSplatting)
    {
        PIXEndEvent(commandList);
        PIXBeginEvent(commandList, 0, L"PhotonSplatting");

        if (mIsTracingFrame) {
            if (mNumPhotons == 0) {
                UINT* pReadbackBufferData;
                ThrowIfFailed(mPhotonMapCounterReadback->Map(0, nullptr, reinterpret_cast<void**>(&pReadbackBufferData)));
                for (UINT i = 0; i < PhotonMapID::Count; i++) {
                    mNumPhotons += pReadbackBufferData[i];
                    mPhotonMapCounts[i] = pReadbackBufferData[i];
                    mPhotonMappingConstants->counts[i].x = mNumPhotons;
                }
                mPhotonMappingConstants.CopyStagingToGpu();
                mPhotonMapCounterReadback->Unmap(0, &CD3DX12_RANGE(0, 0));
            }

            if (mShaderOptions.volumeSplattingMethod == SplatMethod::Raytrace) {
                buildVolumePhotonAccelerationStructure(mPhotonMappingConstants->photonGeometryBuildStrategy);
            }

        }

        pass = Pass::PhotonSplattingRaster;
    }

    if (pass == Pass::PhotonSplattingRaster)
    {
        PIXEndEvent(commandList);
        PIXBeginEvent(commandList, 0, L"PhotonSplattingRaster");

        UINT splatUntil = PhotonMapID::Count - 1 - static_cast<UINT>(mShaderOptions.volumeSplattingMethod != SplatMethod::Raster);
        mNumPhotons = mPhotonMappingConstants->counts[splatUntil].x;

        std::initializer_list<D3D12_RESOURCE_BARRIER> barriers =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(mPhotonMap.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mVolumePhotonMap.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mPhotonDensity.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mPhotonSplat[0].Resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(mPhotonSplat[1].Resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        auto transitions = ScopedBarrier(commandList, barriers);

        // Set necessary state.
        mRtContext->bindDescriptorHeap();
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissorRect);

        commandList->SetGraphicsRootSignature(mPhotonSplattingPass.rootSignature.Get());
        commandList->SetGraphicsRootDescriptorTable(0, mPhotonDensity.Srv.gpuHandle); // XXX: not redrawn after camera move!
        commandList->SetGraphicsRootDescriptorTable(1, mPhotonMap.Srv.gpuHandle);
        commandList->SetGraphicsRootConstantBufferView(2, mRasterConstantsBuffer.GpuVirtualAddress(frameIndex));
        commandList->SetGraphicsRootConstantBufferView(3, mPhotonMappingConstants.GpuVirtualAddress());
        commandList->SetGraphicsRootDescriptorTable(4, mGBuffer[GBufferID::LinDepth].Srv.gpuHandle);

        D3D12_CPU_DESCRIPTOR_HANDLE targets[] =
        {
            mPhotonSplat[0].Rtv.cpuHandle,
            mPhotonSplat[1].Rtv.cpuHandle,
        };

        commandList->OMSetRenderTargets(ARRAYSIZE(targets), targets, FALSE, nullptr);

        clearRtv(commandList, mPhotonSplat[0]);
        clearRtv(commandList, mPhotonSplat[1]);

        commandList->SetPipelineState(mPhotonSplattingPass.stateObject.Get());

        mPhotonSplatKernelShape->Draw(commandList, mNumPhotons);

        switch (mShaderOptions.volumeSplattingMethod) {
        case SplatMethod::Raytrace: {
            pass = Pass::PhotonSplattingVolume;
            break;
        }
        case SplatMethod::Voxels: {
            if (mIsTracingFrame) {
                pass = Pass::PhotonSplattingVoxels;
                break;
            } else {
                // fallthrough
            }
        }
        default: {
            pass = Pass::Combine;
            break;
        }
        }

        return;
    }

    if (pass == Pass::PhotonSplattingVolume)
    {
        PIXEndEvent(commandList);
        PIXBeginEvent(commandList, 0, L"PhotonSplattingVolume");

        auto mRtBindings = mRtPhotonSplattingVolumePass.mRtBindings;
        auto mRtState = mRtPhotonSplattingVolumePass.mRtState;
        auto program = mRtBindings->getProgram();

        std::initializer_list<D3D12_RESOURCE_BARRIER> barriers =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(mVolumePhotonMap.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mVolumePhotonPositionsObjSpace.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mPhotonDensity.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mPhotonSplat[0].Resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(mPhotonSplat[1].Resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        auto transitions = ScopedBarrier(commandList, barriers);

        mRtBindings->apply(mRtContext, mRtState);

        // Set global root arguments
        mRtContext->bindDescriptorHeap();
        commandList->SetComputeRootSignature(program->getGlobalRootSignature());
        commandList->SetComputeRootConstantBufferView(GlobalRootSignatureParams::PerFrameConstantsSlot, mConstantBuffer.GpuVirtualAddress(frameIndex));
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::OutputViewSlot, mPhotonSplatUav[0].gpuHandle);
        mRtContext->getFallbackCommandList()->SetTopLevelAccelerationStructure(GlobalRootSignatureParams::AccelerationStructureSlot, mVolumePhotonTlasWrappedPtr);

        commandList->SetComputeRootShaderResourceView(3, mMaterialParamsBuffer.GpuVirtualAddress());
        commandList->SetComputeRootShaderResourceView(4, mTextureParams.GpuVirtualAddress());
        commandList->SetComputeRootDescriptorTable(5, mTextureSrvGpuHandles[2]);

        commandList->SetComputeRootDescriptorTable(6, mPhotonDensity.Srv.gpuHandle);
        commandList->SetComputeRootDescriptorTable(7, mVolumePhotonMap.Srv.gpuHandle);
        commandList->SetComputeRootDescriptorTable(8, mVolumePhotonPositionsObjSpace.Srv.gpuHandle);
        commandList->SetComputeRootConstantBufferView(9, mPhotonMappingConstants.GpuVirtualAddress());

        commandList->SetComputeRootDescriptorTable(10, mPhotonSplatRtBuffer.Uav.gpuHandle);

        commandList->SetComputeRootDescriptorTable(11, mGBuffer[GBufferID::LinDepth].Srv.gpuHandle);

        mRtContext->raytrace(mRtBindings, mRtState, width, height, 1);

        mRtContext->insertUAVBarrier(mPhotonSplat[0].Resource.Get());
        mRtContext->insertUAVBarrier(mPhotonSplat[1].Resource.Get());

        pass = Pass::Combine;
        return;
    }

    if (pass == Pass::PhotonSplattingVoxels)
    {
        PIXEndEvent(commandList);
        PIXBeginEvent(commandList, 0, L"PhotonSplattingVoxels");

        UINT voxelGridSizeX = mPhotonSplatVoxels[0].Resource->GetDesc().Width;
        UINT voxelGridSizeY = mPhotonSplatVoxels[0].Resource->GetDesc().Height;
        UINT voxelGridSizeZ = mPhotonSplatVoxels[0].Resource->GetDesc().DepthOrArraySize;

        std::initializer_list<D3D12_RESOURCE_BARRIER> barriers =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(mVolumePhotonMap.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mVolumePhotonPositionsObjSpace.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mPhotonDensity.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mPhotonSplatVoxels[0].Resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(mPhotonSplatVoxels[1].Resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        auto transitions = ScopedBarrier(commandList, barriers);

        clearUav(commandList, mPhotonSplatVoxels[0]);
        clearUav(commandList, mPhotonSplatVoxels[1]);

        // Set global root arguments
        mRtContext->bindDescriptorHeap();
        commandList->SetPipelineState(mPhotonSplattingVoxelPass.stateObject.Get());
        commandList->SetComputeRootSignature(mPhotonSplattingVoxelPass.rootSignature.Get());

        commandList->SetComputeRootDescriptorTable(0, mPhotonDensity.Srv.gpuHandle);
        commandList->SetComputeRootDescriptorTable(1, mVolumePhotonMap.Srv.gpuHandle);
        commandList->SetComputeRootDescriptorTable(2, mVolumePhotonPositionsObjSpace.Srv.gpuHandle);

        commandList->SetComputeRootConstantBufferView(3, mConstantBuffer.GpuVirtualAddress(frameIndex));
        commandList->SetComputeRootConstantBufferView(4, mPhotonMappingConstants.GpuVirtualAddress());

        commandList->SetComputeRootDescriptorTable(5, mPhotonSplatVoxels[0].Uav.gpuHandle);

        commandList->Dispatch(voxelGridSizeX, voxelGridSizeY, voxelGridSizeZ);

        mRtContext->insertUAVBarrier(mPhotonSplatVoxels[0].Resource.Get());
        mRtContext->insertUAVBarrier(mPhotonSplatVoxels[1].Resource.Get());

        pass = Pass::Combine;
        return;
    }

    // final render pass
    if (pass == Pass::Combine)
    {
        PIXEndEvent(commandList);
        PIXBeginEvent(commandList, 0, L"Combine");

        for (UINT instance = 0; instance < mRtScene->getNumInstances(); ++instance) {
            if (mMaterials[mRtScene->getModel(instance)->mMaterialIndex].params.type == MaterialType::ParticipatingMedia) {
                mPhotonMappingConstants->worldToObjMatrix = XMMatrixInverse(nullptr, mRtScene->getTransform(instance));
                mPhotonMappingConstants.CopyStagingToGpu();
                break;
            }
        }

        std::initializer_list<D3D12_RESOURCE_BARRIER> barriers =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(mOutput.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        auto transitions = ScopedBarrier(commandList, barriers);

        mRtContext->bindDescriptorHeap();
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissorRect);

        commandList->SetGraphicsRootSignature(mCombinePass.rootSignature.Get());
        commandList->SetGraphicsRootDescriptorTable(0, mGBuffer[GBufferID::Normal].Srv.gpuHandle);
        commandList->SetGraphicsRootDescriptorTable(1, mPhotonSplat[0].Srv.gpuHandle);
        commandList->SetGraphicsRootDescriptorTable(2, mPhotonSplat[1].Srv.gpuHandle);

        commandList->SetGraphicsRootConstantBufferView(3, mRasterConstantsBuffer.GpuVirtualAddress(frameIndex));
        commandList->SetGraphicsRootConstantBufferView(4, mPhotonMappingConstants.GpuVirtualAddress());

        commandList->SetGraphicsRootDescriptorTable(5, mGBuffer[GBufferID::LinDepth].Srv.gpuHandle);
        commandList->SetGraphicsRootDescriptorTable(6, mGBuffer[GBufferID::VolMask].Srv.gpuHandle);
        commandList->SetGraphicsRootDescriptorTable(7, mPhotonSplatVoxels[0].Srv.gpuHandle);

        commandList->SetGraphicsRootShaderResourceView(8, mMaterialParamsBuffer.GpuVirtualAddress());
        commandList->SetGraphicsRootShaderResourceView(9, mTextureParams.GpuVirtualAddress());
        commandList->SetGraphicsRootDescriptorTable(10, mTextureSrvGpuHandles[2]);

        commandList->OMSetRenderTargets(1, &mOutput.Rtv.cpuHandle, FALSE, nullptr);

        clearRtv(commandList, mOutput);

        commandList->SetPipelineState(mCombinePass.stateObject.Get());

        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->DrawInstanced(3, 1, 0, 0);

        if (mIsTracingFrame) mIsTracingFrame = false;
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
        static int matIdx = 0;
        ui::SliderInt("Material Index", &matIdx, 0, mMaterials.size() - 1);
        frameDirty |= ui::SliderFloat3("Albedo", &mMaterials[matIdx].params.albedo.x, 0.0f, 1.0f);
        frameDirty |= ui::SliderFloat("Reflectivity", &mMaterials[matIdx].params.reflectivity, 0.0f, 1.0f);
        frameDirty |= ui::SliderFloat("Roughness", &mMaterials[matIdx].params.roughness, 0.0f, 1.0f);
        frameDirty |= ui::SliderInt("Type", (int*)&mMaterials[matIdx].params.type, 0, MaterialType::Count - 1);
    }
    ui::End();

    ui::Begin("Hybrid Raytracing");
    {
        frameDirty |= ui::Checkbox("Pause Animation", &mAnimationPaused);

        ui::Separator();

        mNeedPhotonMap |= ui::Checkbox("Skip Tracing", (bool*)&mShaderOptions.skipPhotonTracing);
        frameDirty |= ui::Checkbox("Show Volume Photons Only", (bool*)&mShaderOptions.showVolumePhotonsOnly);
        frameDirty |= ui::Checkbox("Show Splatting Result Only", (bool*)&mShaderOptions.showRawSplattingResult);
        mNeedPhotonMap |= ui::SliderInt("Splatting Method", (int*)&mShaderOptions.volumeSplattingMethod, 0, SplatMethod::COUNT - 1);

        ui::Separator();
        ui::Text("Splatting Parameters");

        frameDirty |= ui::DragFloatRange2("0: Uniform Scale Min/Max", &mPhotonMappingConstants->kernelScaleMin, &mPhotonMappingConstants->kernelScaleMax, 0.01f, 0.01f, 2.0f);
        frameDirty |= ui::SliderFloat("0: Uniform Scale Strength", &mPhotonMappingConstants->uniformScaleStrength, 0.5f, 100.0f);
        frameDirty |= ui::SliderFloat("0: Light Shaping Max", &mPhotonMappingConstants->maxLightShapingScale, 1.0f, 10.0f);
        frameDirty |= ui::SliderFloat("0: Kernel Compress", &mPhotonMappingConstants->kernelCompressFactor, 0.3f, 1.0f);
        frameDirty |= ui::SliderInt("1: Volume Splatting Slab Size", (int*)&mPhotonMappingConstants->particlesPerSlab, 16, 64);
        mNeedPhotonMap |= ui::SliderInt("1: Photon Geom Strategy", (int*)&mPhotonMappingConstants->photonGeometryBuildStrategy, 0, 1);
        mNeedPhotonMap |= ui::SliderFloat("1: Volume Splatting Radius", &mPhotonMappingConstants->volumeSplatPhotonSize, 0.01f, .1f);

        ui::Separator();

        ui::Text("Press F to toggle first person camera");
    }
    ui::End();

    if (frameDirty || mNeedPhotonMap) {
        mLastCameraVPMatrix = Math::Matrix4();
    }
}
