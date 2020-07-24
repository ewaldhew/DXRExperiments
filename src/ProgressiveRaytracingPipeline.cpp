#include "pch.h"
#include "ProgressiveRaytracingPipeline.h"
#include "CompiledShaders/ProgressivePathtracing.hlsl.h"
#include "WICTextureLoader.h"
#include "DDSTextureLoader.h"
#include "ResourceUploadBatch.h"
#include "utils/DirectXHelper.h"
#include "Helpers/DirectXRaytracingHelper.h"
#include "ImGuiRendererDX.h"
#include <chrono>

using namespace DXRFramework;

static XMFLOAT4 pointLightColor = XMFLOAT4(0.2f, 0.8f, 0.6f, 2.0f);
static XMFLOAT4 dirLightColor = XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f);

namespace GlobalRootSignatureParams
{
    enum Value
    {
        AccelerationStructureSlot = 0,
        OutputViewSlot,
        PerFrameConstantsSlot,
        DirectionalLightBufferSlot,
        PointLightBufferSlot,
        MaterialTextureParamsSlot,
        MaterialTextureSrvSlot,
        Count
    };
}

ProgressiveRaytracingPipeline::ProgressiveRaytracingPipeline(RtContext::SharedPtr context) :
    mRtContext(context),
    mFrameAccumulationEnabled(true),
    mAnimationPaused(true),
    mActive(true)
{
    RtProgram::Desc programDesc;
    {
        std::vector<std::wstring> libraryExports = {
            L"RayGen",
            L"PrimaryClosestHit", L"PrimaryClosestHit_AABB", L"PrimaryMiss",
            L"ShadowClosestHit", L"ShadowAnyHit", L"ShadowMiss",
            L"VolumeClosestHit", L"VolumeClosestHit_AABB", L"VolumeMiss",
            L"Intersection_AnalyticPrimitive", L"Intersection_VolumetricPrimitive", L"Intersection_SignedDistancePrimitive"
        };
        programDesc.addShaderLibrary(g_pProgressivePathtracing, ARRAYSIZE(g_pProgressivePathtracing), libraryExports);
        programDesc.setRayGen("RayGen");
        programDesc
            .addHitGroup(0, RtModel::GeometryType::Triangles, "PrimaryClosestHit", "")
            .addHitGroup(0, RtModel::GeometryType::AABB_Analytic, "PrimaryClosestHit_AABB", "", "Intersection_AnalyticPrimitive")
            .addHitGroup(0, RtModel::GeometryType::AABB_Volumetric, "PrimaryClosestHit_AABB", "", "Intersection_VolumetricPrimitive")
            .addHitGroup(0, RtModel::GeometryType::AABB_SignedDistance, "PrimaryClosestHit_AABB", "", "Intersection_SignedDistancePrimitive")
            .addMiss(0, "PrimaryMiss");
        programDesc
            .addHitGroup(1, RtModel::GeometryType::Triangles, "ShadowClosestHit", "ShadowAnyHit")
            .addHitGroup(1, RtModel::GeometryType::AABB_Analytic, "", "", "Intersection_AnalyticPrimitive")
            .addHitGroup(1, RtModel::GeometryType::AABB_Volumetric, "", "", "Intersection_VolumetricPrimitive")
            .addHitGroup(1, RtModel::GeometryType::AABB_SignedDistance, "", "", "Intersection_SignedDistancePrimitive")
            .addMiss(1, "ShadowMiss");
        programDesc
            .addHitGroup(2, RtModel::GeometryType::Triangles, "VolumeClosestHit", "")
            .addHitGroup(2, RtModel::GeometryType::AABB_Analytic, "VolumeClosestHit_AABB", "", "Intersection_AnalyticPrimitive")
            .addHitGroup(2, RtModel::GeometryType::AABB_Volumetric, "VolumeClosestHit_AABB", "", "Intersection_VolumetricPrimitive")
            .addHitGroup(2, RtModel::GeometryType::AABB_SignedDistance, "VolumeClosestHit_AABB", "", "Intersection_SignedDistancePrimitive")
            .addMiss(2, "VolumeMiss");

        programDesc.configureGlobalRootSignature([] (RootSignatureGenerator &config) {
            // GlobalRootSignatureParams::AccelerationStructureSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 0 /* t0 */);
            // GlobalRootSignatureParams::OutputViewSlot
            config.AddHeapRangesParameter({{0 /* u0 */, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0}});
            // GlobalRootSignatureParams::PerFrameConstantsSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 0 /* b0 */);
            // GlobalRootSignatureParams::DirectionalLightBufferSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 1 /* t1 */);
            // GlobalRootSignatureParams::PointLightBufferSlot
            config.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 2 /* t2 */);

            D3D12_STATIC_SAMPLER_DESC cubeSampler = {};
            cubeSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            cubeSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            cubeSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            cubeSampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            cubeSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            cubeSampler.ShaderRegister = 0;
            config.AddStaticSampler(cubeSampler);

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
            config.AddHeapRangesParameter({{1 /* t1 */, -1 /* unbounded */, 9 /* space9 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}});
        });
        programDesc.configureHitGroupRootSignature([] (RootSignatureGenerator &config) {
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
        programDesc.configureHitGroupRootSignature(aabbHitGroupConfigurator, RtModel::GeometryType::AABB_Analytic);
        programDesc.configureHitGroupRootSignature(aabbHitGroupConfigurator, RtModel::GeometryType::AABB_Volumetric);
        programDesc.configureHitGroupRootSignature(aabbHitGroupConfigurator, RtModel::GeometryType::AABB_SignedDistance);
        programDesc.configureMissRootSignature([] (RootSignatureGenerator &config) {
            config.AddHeapRangesParameter({{0 /* t0 */, 1, 2 /* space2 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}});
            config.AddHeapRangesParameter({{1 /* t1 */, 1, 2 /* space2 */, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0}});
        });
    }
    mRtProgram = RtProgram::create(context, programDesc);
    mRtState = RtState::create(context);
    mRtState->setProgram(mRtProgram);
    mRtState->setMaxTraceRecursionDepth(8);
    mRtState->setMaxAttributeSize(sizeof(ProceduralPrimitiveAttributes));
    mRtState->setMaxPayloadSize(20);

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
}

ProgressiveRaytracingPipeline::~ProgressiveRaytracingPipeline() = default;

void ProgressiveRaytracingPipeline::setScene(RtScene::SharedPtr scene)
{
    mRtScene = scene->copy();
    mRtBindings = RtBindings::create(mRtContext, mRtProgram, scene);
}

void ProgressiveRaytracingPipeline::buildAccelerationStructures()
{
    mRtScene->build(mRtContext, mRtProgram->getHitProgramCount());
}

void ProgressiveRaytracingPipeline::loadResources(ID3D12CommandQueue *uploadCommandQueue, UINT frameCount)
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
                material.params.*tex.targetParam = XMFLOAT4(textureIndex++, 0, 0, -1.0f);
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

    mDirLights.Create(device, 1, frameCount, L"DirectionalLightBuffer");
    mPointLights.Create(device, 1, frameCount, L"PointLightBuffer");
}

void ProgressiveRaytracingPipeline::createOutputResource(DXGI_FORMAT format, UINT width, UINT height)
{
    auto device = mRtContext->getDevice();

    AllocateUAVTexture(device, format, width, height, mOutputResource.ReleaseAndGetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle;
        mOutputUavHeapIndex = mRtContext->allocateDescriptor(&uavCpuHandle, mOutputUavHeapIndex);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(mOutputResource.Get(), nullptr, &uavDesc, uavCpuHandle);

        mOutputUavGpuHandle = mRtContext->getDescriptorGPUHandle(mOutputUavHeapIndex);
    }

    {
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle;
        mOutputSrvHeapIndex = mRtContext->allocateDescriptor(&srvCpuHandle, mOutputSrvHeapIndex);
        mOutputSrvGpuHandle = mRtContext->createTextureSRVHandle(mOutputResource.Get(), false, mOutputSrvHeapIndex);
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
    ulen = vlen * aspectRatio;
    u = XMVectorScale(u, ulen);
    v = XMVectorScale(v, vlen);

    XMStoreFloat4(U, u);
    XMStoreFloat4(V, v);
    XMStoreFloat4(W, w);
}

inline bool hasCameraMoved(Math::Camera &camera, Math::Matrix4 &lastVPMatrix)
{
    const Math::Matrix4 &currentMatrix = camera.GetViewProjMatrix();
    return !(XMVector4Equal(lastVPMatrix.GetX(), currentMatrix.GetX()) && XMVector4Equal(lastVPMatrix.GetY(), currentMatrix.GetY()) &&
             XMVector4Equal(lastVPMatrix.GetZ(), currentMatrix.GetZ()) && XMVector4Equal(lastVPMatrix.GetW(), currentMatrix.GetW()));
}

void ProgressiveRaytracingPipeline::update(float elapsedTime, UINT elapsedFrames, UINT prevFrameIndex, UINT frameIndex, UINT width, UINT height)
{
    if (mAnimationPaused) {
        elapsedTime = 142.0f;
    }

    if (hasCameraMoved(*mCamera, mLastCameraVPMatrix) || !mFrameAccumulationEnabled) {
        mAccumCount = 0;
        mLastCameraVPMatrix = mCamera->GetViewProjMatrix();
    }

    CameraParams &cameraParams = mConstantBuffer->cameraParams;
    XMStoreFloat4(&cameraParams.worldEyePos, mCamera->GetPosition());
    calculateCameraVariables(*mCamera, mCamera->GetAspectRatio(), &cameraParams.U, &cameraParams.V, &cameraParams.W);
    float xJitter = (mRngDist(mRng) - 0.5f) / float(width);
    float yJitter = (mRngDist(mRng) - 0.5f) / float(height);
    cameraParams.jitters = XMFLOAT2(xJitter, yJitter);
    cameraParams.frameCount = elapsedFrames;
    cameraParams.accumCount = mAccumCount++;

    mConstantBuffer->options = mShaderDebugOptions;
    mConstantBuffer.CopyStagingToGpu(frameIndex);

    XMVECTOR dirLightVector = XMVectorSet(0.3f, -0.2f, -1.0f, 0.0f);
    XMMATRIX rotation =  XMMatrixRotationY(sin(elapsedTime * 0.2f) * 3.14f * 0.5f);
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
}

void ProgressiveRaytracingPipeline::render(ID3D12GraphicsCommandList *commandList, UINT frameIndex, UINT width, UINT height)
{
    // Update shader table root arguments
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

    for (UINT rayType = 0; rayType < program->getMissProgramCount(); ++rayType) {
        auto &missVars = mRtBindings->getMissVars(rayType);
        missVars->appendHeapRanges(mTextureSrvGpuHandles[0].ptr);
        missVars->appendHeapRanges(mTextureSrvGpuHandles[1].ptr);
    }

    mRtBindings->apply(mRtContext, mRtState);

    // Set global root arguments
    mRtContext->bindDescriptorHeap();
    commandList->SetComputeRootSignature(program->getGlobalRootSignature());
    commandList->SetComputeRootConstantBufferView(GlobalRootSignatureParams::PerFrameConstantsSlot, mConstantBuffer.GpuVirtualAddress(frameIndex));
    commandList->SetComputeRootShaderResourceView(GlobalRootSignatureParams::DirectionalLightBufferSlot, mDirLights.GpuVirtualAddress(frameIndex));
    commandList->SetComputeRootShaderResourceView(GlobalRootSignatureParams::PointLightBufferSlot, mPointLights.GpuVirtualAddress(frameIndex));
    commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::MaterialTextureSrvSlot, mTextureSrvGpuHandles[2]);
    commandList->SetComputeRootShaderResourceView(GlobalRootSignatureParams::MaterialTextureParamsSlot, mTextureParams.GpuVirtualAddress());
    commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::OutputViewSlot, mOutputUavGpuHandle);
    mRtContext->getFallbackCommandList()->SetTopLevelAccelerationStructure(GlobalRootSignatureParams::AccelerationStructureSlot, mRtScene->getTlasWrappedPtr());

    mRtContext->raytrace(mRtBindings, mRtState, width, height, 3);

    mRtContext->insertUAVBarrier(mOutputResource.Get());
}

void ProgressiveRaytracingPipeline::userInterface()
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

    ui::Begin("Progressive Raytracing");
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
