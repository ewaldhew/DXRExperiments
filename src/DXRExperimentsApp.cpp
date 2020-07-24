#include "pch.h"
#include "DXRExperimentsApp.h"
#include "ProgressiveRaytracingPipeline.h"
#include "RealtimeRaytracingPipeline.h"
#include "HybridPipeline.h"
#include "Helpers/DirectXRaytracingHelper.h"
#include "ImGuiRendererDX.h"
#include "GameInput.h"
#include "OpenSimplexNoise.h"

using namespace std;
using namespace DXRFramework;

namespace GameCore
{
    extern HWND g_hWnd;
}

DXRExperimentsApp::DXRExperimentsApp(UINT width, UINT height, std::wstring name) :
    DXSample(width, height, name),
    mBypassRaytracing(false),
    mForceComputeFallback(false) // Set this to true if you're running on RTX cards but wants to force compute path
{
    UpdateForSizeChange(width, height);
}

void DXRExperimentsApp::OnInit()
{
    m_deviceResources = std::make_unique<DX::DeviceResources>(
        DXGI_FORMAT_R16G16B16A16_FLOAT,//DXGI_FORMAT_R8G8B8A8_UNORM
        DXGI_FORMAT_UNKNOWN,
        FrameCount,
        D3D_FEATURE_LEVEL_11_0,
        // Sample shows handling of use cases with tearing support, which is OS dependent and has been supported since TH2.
        // Since the Fallback Layer requires Fall Creator's update (RS3), we don't need to handle non-tearing cases.
        DX::DeviceResources::c_RequireTearingSupport,
        m_adapterIDoverride
    );
    m_deviceResources->RegisterDeviceNotify(this);
    m_deviceResources->SetWindow(Win32Application::GetHwnd(), GetWidth(), GetHeight());
    m_deviceResources->InitializeDXGIAdapter();
    mNativeDxrSupported = IsDirectXRaytracingSupported(m_deviceResources->GetAdapter());
    if (!mNativeDxrSupported || mForceComputeFallback) {
        // This requires Windows developer mode
        ThrowIfFalse(EnableExperimentalFeatureForComputeFallback(m_deviceResources->GetAdapter()));
    }

    m_deviceResources->CreateDeviceResources();
    m_deviceResources->CreateWindowSizeDependentResources();

    GameInput::Initialize();

    // Initialize texture loader
    #if (_WIN32_WINNT >= 0x0A00 /*_WIN32_WINNT_WIN10*/)
        Microsoft::WRL::Wrappers::RoInitializeWrapper initialize(RO_INIT_MULTITHREADED);
        ThrowIfFailed(initialize, L"Cannot initialize WIC");
    #else
        #error Unsupported Windows version
    #endif

    // Setup camera states
    mCamera.reset(new Math::Camera());
    mCamera->SetAspectRatio(m_aspectRatio);
    mCamera->SetEyeAtUp(Math::Vector3(0.0, 0.0, 35.5), Math::Vector3(0.0, 0.0, 1.0), Math::Vector3(Math::kYUnitVector));
    mCamera->SetZRange(1.0f, 10000.0f);
    mCamController.reset(new GameCore::CameraController(*mCamera, mCamera->GetUpVec()));
    mCamController->EnableFirstPersonMouse(false);

    InitRaytracing();

    // Initialize UI renderer
    ui::RendererDX::Initialize(GameCore::g_hWnd, m_deviceResources->GetD3DDevice(), m_deviceResources->GetBackBufferFormat(), FrameCount, [&] () {
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
        UINT heapOffset = mRtContext->allocateDescriptor(&cpuHandle);
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = mRtContext->getDescriptorGPUHandle(heapOffset);
        return std::make_pair(cpuHandle, gpuHandle);
    });
}

void DXRExperimentsApp::InitRaytracing()
{
    auto device = m_deviceResources->GetD3DDevice();
    auto commandList = m_deviceResources->GetCommandList();

    mRtContext = RtContext::create(device, commandList, mForceComputeFallback);

    // Create scene
    mRtScene = RtScene::create();
    {
        auto identity = DirectX::XMMatrixIdentity();

        // working directory is "vc2015"
        RtMesh::importMeshesFromFile([&](RtMesh::SharedPtr mesh) {
            mRtScene->addModel(mesh, DirectX::XMMatrixScaling(10, 10, 10), MaterialSceneFlags::None);
        }, mRtContext, "..\\assets\\models\\cornell.obj", { 2, 0, 0, 1, 0, 0, 0 });

        auto lightMeshTransform =
            DirectX::XMMatrixRotationX(XM_PIDIV2) *
            DirectX::XMMatrixScaling(2, 2, 2) *
            DirectX::XMMatrixTranslation(0, 9.999f, 0);
        std::vector<Vertex> squareVerts =
        {
            { { -1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
            { { -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
            { { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
            { { 1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
        };
        mRtScene->addModel(RtMesh::create(mRtContext, squareVerts, { 0, 1, 3, 1, 2, 3 }, 3), lightMeshTransform, MaterialSceneFlags::Emissive);

        auto volumeTransform = XMMatrixScaling(20, 20, 20) * XMMatrixTranslation(-10., -10., -10.);
        mRtScene->addModel(RtProcedural::create(mRtContext, PrimitiveType::AnalyticPrimitive_AABB, XMFLOAT3(), XMFLOAT3(1, 1, 1), 5), volumeTransform, MaterialSceneFlags::Volume);
    }

    // Create materials
    std::vector<RaytracingPipeline::Material> materials;
    auto material = std::back_inserter(materials);
    {
        material = {};
        RaytracingPipeline::Material &white = materials.back();
        white.params.albedo = XMFLOAT4(0.72f, 0.75f, 0.65f, 1.0f);
        white.params.roughness = 1.0f;
        white.params.type = MaterialType::Diffuse;
    }
    {
        material = {};
        RaytracingPipeline::Material &green = materials.back();
        green.params.albedo = XMFLOAT4(0.15f, 0.45f, 0.10f, 1.0f);
        green.params.roughness = 1.0f;
        green.params.type = MaterialType::Diffuse;
    }
    {
        material = {};
        RaytracingPipeline::Material &red = materials.back();
        red.params.albedo = XMFLOAT4(0.60f, 0.07f, 0.05f, 1.0f);
        red.params.roughness = 1.0f;
        red.params.type = MaterialType::Diffuse;
    }
    {
        material = {};
        RaytracingPipeline::Material &light = materials.back();
        light.params.albedo = XMFLOAT4(0.78f, 0.78f, 0.78f, 1.0f);
        light.params.emissive = XMFLOAT4(16.8f, 14.5f, 8.0f, 1.0f);
        light.params.reflectivity = 0.7f;
        light.params.type = MaterialType::Diffuse;
    }
    {
        material = {};
        RaytracingPipeline::Material &glass = materials.back();
        glass.params.albedo = XMFLOAT4(0.2f, 0.2f, 0.2f, 1.0f);
        glass.params.reflectivity = 0.7f;
        glass.params.type = MaterialType::Glass;
        glass.params.IoR = 1.2;
    }
    {
        material = {};
        RaytracingPipeline::Material &texTest = materials.back();
        texTest.params.type = MaterialType::ParticipatingMedia;
        texTest.params.reflectivity = 0.6f;
        RaytracingPipeline::MaterialTexture tex{ &MaterialParams::albedo };
        tex.data = { // starts bottom-left
        };
        tex.depth = 50;
        tex.height = 50;
        tex.width = 50;
        tex.data.resize(tex.depth * tex.height * tex.width);
        OpenSimplexNoise::Noise noise;
        for (UINT i = 0; i < tex.depth; i++) {
            for (UINT j = 0; j < tex.height; j++) {
                for (UINT k = 0; k < tex.width; k++) {
                    float extinction = (noise.eval(i / (double)tex.depth, j/ (double)tex.height, k/ (double)tex.width) + 0.23) ;
                    extinction = 0.02;//(j + k) % 2 ? 0.8 : 0.4;
                    //extinction = min(max(extinction, 0.1), 0.8);
                    tex.data[i*tex.height*tex.width + j*tex.width + k] = XMFLOAT4(extinction, extinction * 0.8, 0, 1);
                }
            }
        }
        tex.params.objectSpaceToTex = XMMatrixIdentity();
        texTest.textures.push_back(tex);
    }

    // Create raytracing pipelines
    mRaytracingPipelines.emplace_back(ProgressiveRaytracingPipeline::create(mRtContext));
    mRaytracingPipelines.emplace_back(HybridPipeline::create(mRtContext));

    // Populate raytracing pipelines
    for (auto pipeline : mRaytracingPipelines) {
        mPipelineNames.emplace_back(pipeline->getName());

        pipeline->setScene(mRtScene);
        for (auto &m : materials) {
            pipeline->addMaterial(m);
        }

        pipeline->setCamera(mCamera);
        pipeline->loadResources(m_deviceResources->GetCommandQueue(), FrameCount);
        pipeline->createOutputResource(m_deviceResources->GetBackBufferFormat(), GetWidth(), GetHeight());

        // Build acceleration structures
        if (!mBypassRaytracing) {
            commandList->Reset(m_deviceResources->GetCommandAllocator(), nullptr);
            pipeline->buildAccelerationStructures();
            m_deviceResources->ExecuteCommandList();
            m_deviceResources->WaitForGpu();
        }
    }

    mActiveRaytracingPipeline = mRaytracingPipelines.front().get();
    mActivePipelineIndex = 0;

    mDenoiser = DenoiseCompositor::create(mRtContext);
    mDenoiser->loadResources(m_deviceResources->GetCommandQueue(), FrameCount, mBypassRaytracing);
    mDenoiser->createOutputResource(m_deviceResources->GetBackBufferFormat(), GetWidth(), GetHeight());
}

void DXRExperimentsApp::OnUpdate()
{
    DXSample::OnUpdate();

    // Begin recording UI draw list
    ui::RendererDX::NewFrame();

    float elapsedTime = static_cast<float>(mTimer.GetTotalSeconds());
    float deltaTime = static_cast<float>(mTimer.GetElapsedSeconds());

    GameInput::Update(deltaTime);
    mCamController->Update(deltaTime);

    {
        if (ui::Combo("Pipeline Select", &mActivePipelineIndex, mPipelineNames.data(), static_cast<int>(mRaytracingPipelines.size()))) {
            mActiveRaytracingPipeline = mRaytracingPipelines[mActivePipelineIndex].get();
        }

        ui::Checkbox(mActiveRaytracingPipeline->getName(), mActiveRaytracingPipeline->isActive());
        ui::Checkbox("Denoise Compositor", &mDenoiser->mActive);
    }

    if (*mActiveRaytracingPipeline->isActive()) {
        mActiveRaytracingPipeline->userInterface();
        mActiveRaytracingPipeline->update(elapsedTime, GetFrameCount(), m_deviceResources->GetPreviousFrameIndex(), m_deviceResources->GetCurrentFrameIndex(), GetWidth(), GetHeight());
    }

    if (dynamic_cast<RealtimeRaytracingPipeline*>(mActiveRaytracingPipeline) && mDenoiser->mActive) {
        mDenoiser->userInterface();
    }
}

void DXRExperimentsApp::OnRender()
{
    if (!m_deviceResources->IsWindowVisible()) return;

    // Reset command list
    m_deviceResources->Prepare();
    auto commandList = m_deviceResources->GetCommandList();
    auto currentFrame = m_deviceResources->GetCurrentFrameIndex();

    if (mBypassRaytracing || !*mActiveRaytracingPipeline->isActive()) {
        auto rtvHandle = m_deviceResources->GetRenderTargetView();
        const float clearColor[] = { 0.3f, 0.2f, 0.1f, 1.0f };
        commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        // Insert rasterizeration code here

        // Run denoiser with mock input textures
        if (mBypassRaytracing && mDenoiser->mActive) {
            mDenoiser->dispatch(commandList, DenoiseCompositor::InputComponents{0}, currentFrame, GetWidth(), GetHeight());
            BlitToBackbuffer(mDenoiser->getOutputResource());
        }
    } else {
        mActiveRaytracingPipeline->render(commandList, currentFrame, GetWidth(), GetHeight());

        if (dynamic_cast<RealtimeRaytracingPipeline*>(mActiveRaytracingPipeline) && mDenoiser->mActive) {
            for (int i = 0; i < mActiveRaytracingPipeline->getNumOutputs(); ++i) {
                mRtContext->transitionResource(mActiveRaytracingPipeline->getOutputResource(i), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }

            DenoiseCompositor::InputComponents inputs = {};
            inputs.directLightingSrv = mActiveRaytracingPipeline->getOutputSrvHandle(0);
            inputs.indirectSpecularSrv = mActiveRaytracingPipeline->getOutputSrvHandle(1);

            mDenoiser->dispatch(commandList, inputs, currentFrame, GetWidth(), GetHeight());

            for (int i = 0; i < mActiveRaytracingPipeline->getNumOutputs(); ++i) {
                mRtContext->transitionResource(mActiveRaytracingPipeline->getOutputResource(i), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }

            BlitToBackbuffer(mDenoiser->getOutputResource());
        } else {
            BlitToBackbuffer(mActiveRaytracingPipeline->getOutputResource(0));
        }
    }

    // Render UI
    {
        mRtContext->bindDescriptorHeap();

        auto rtvHandle = m_deviceResources->GetRenderTargetView();
        commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        ui::RendererDX::Render(commandList);
    }

    // Execute command list and insert fence
    m_deviceResources->Present(D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void DXRExperimentsApp::OnKeyDown(UINT8 key)
{
    switch (key) {
    case 'F':
        mCamController->EnableFirstPersonMouse(!mCamController->IsFirstPersonMouseEnabled());
        break;
    case VK_RIGHT:
        mActivePipelineIndex++;
        break;
    case VK_LEFT:
        mActivePipelineIndex--;
        break;
    }

    mActivePipelineIndex %= mRaytracingPipelines.size();
    mActiveRaytracingPipeline = mRaytracingPipelines[mActivePipelineIndex].get();
}

void DXRExperimentsApp::OnDestroy()
{
    m_deviceResources->WaitForGpu();

    ui::RendererDX::Shutdown();
    GameInput::Shutdown();
}

void DXRExperimentsApp::OnSizeChanged(UINT width, UINT height, bool minimized)
{
    if (!m_deviceResources->WindowSizeChanged(width, height, minimized)) {
        return;
    }

    UpdateForSizeChange(width, height);

    mCamera->SetAspectRatio(m_aspectRatio);

    for (auto pipeline : mRaytracingPipelines) {
        pipeline->createOutputResource(m_deviceResources->GetBackBufferFormat(), GetWidth(), GetHeight());
    }
    mDenoiser->createOutputResource(m_deviceResources->GetBackBufferFormat(), GetWidth(), GetHeight());
}

void DXRExperimentsApp::BlitToBackbuffer(ID3D12Resource *textureResource, D3D12_RESOURCE_STATES fromState, D3D12_RESOURCE_STATES toState)
{
    auto commandList= m_deviceResources->GetCommandList();
    auto renderTarget = m_deviceResources->GetRenderTarget();

    mRtContext->transitionResource(renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    mRtContext->transitionResource(textureResource, fromState, D3D12_RESOURCE_STATE_COPY_SOURCE);

    commandList->CopyResource(renderTarget, textureResource);

    mRtContext->transitionResource(renderTarget, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    mRtContext->transitionResource(textureResource, D3D12_RESOURCE_STATE_COPY_SOURCE, toState);
}

LRESULT DXRExperimentsApp::WindowProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return ui::RendererDX::WindowProcHandler(hwnd, msg, wParam, lParam);
}
