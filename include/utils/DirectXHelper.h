#pragma once

#include "ResourceUploadBatch.h"
#include "Helpers/DirectXRaytracingHelper.h"

namespace DirectX {
    inline void AllocateRTVTexture(ID3D12Device* pDevice, DXGI_FORMAT textureFormat, UINT64 textureWidth, UINT64 textureHeight, ID3D12Resource **ppResource, D3D12_RESOURCE_STATES initialResourceState = D3D12_RESOURCE_STATE_COMMON, const wchar_t* resourceName = nullptr, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, std::array<FLOAT, 4> clearValue = {})
    {
        auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(textureFormat, textureWidth, static_cast<UINT>(textureHeight), 1, 1, 1, 0, flags | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        ThrowIfFailed(pDevice->CreateCommittedResource(
            &kDefaultHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            initialResourceState,
            &CD3DX12_CLEAR_VALUE(textureFormat, clearValue.data()),
            IID_PPV_ARGS(ppResource)));
        if (resourceName)
        {
            (*ppResource)->SetName(resourceName);
        }
    }

    inline void CreateTextureResource(
        ID3D12Device* pDevice, ResourceUploadBatch& upload,
        uint32_t width, uint32_t height, uint32_t depth,
        uint32_t stride, uint32_t rows,
        DXGI_FORMAT format,
        const void* data,
        ComPtr<ID3D12Resource>& textureResource)
    {
        D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex3D(format, width, height, depth);

        ThrowIfFailed(pDevice->CreateCommittedResource(
            &kDefaultHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(textureResource.ReleaseAndGetAddressOf())));

        D3D12_SUBRESOURCE_DATA subres = {};
        subres.pData = data;
        subres.RowPitch = stride;
        subres.SlicePitch = ptrdiff_t(stride) * ptrdiff_t(rows);

        upload.Upload(
            textureResource.Get(),
            0,
            &subres,
            1);

        upload.Transition(
            textureResource.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}

