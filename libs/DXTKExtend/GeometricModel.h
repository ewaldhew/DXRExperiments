#pragma once

#include <functional>
#include "CommonInclude.h"
#include "GeometricPrimitive.h"

using namespace DirectX;

namespace DXTKExtend
{
    class GeometricModel
    {
    public:
        using SharedPtr = std::shared_ptr<GeometricModel>;

        using CreatePrimitive = std::function<void(std::vector<GeometricPrimitive::VertexType>&V, std::vector<uint16_t>&I)>;

        GeometricModel(ID3D12Device* pDevice, CreatePrimitive modelFunc);
        ~GeometricModel() = default;

        ID3D12Resource *getVertexBuffer() const { return mVertexBuffer.Get(); }
        ID3D12Resource *getIndexBuffer() const { return mIndexBuffer.Get(); }

        D3D12_VERTEX_BUFFER_VIEW getVertexBufferView() const { return mVertexBufferView; }
        D3D12_INDEX_BUFFER_VIEW getIndexBufferView() const { return mIndexBufferView; }

        void Draw(ID3D12GraphicsCommandList* commandList, UINT instanceCount = 1) const;

    private:
        UINT mNumVertices;
        UINT mIndexCount;

        Microsoft::WRL::ComPtr<ID3D12Resource> mVertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> mIndexBuffer;

        D3D12_VERTEX_BUFFER_VIEW mVertexBufferView;
        D3D12_INDEX_BUFFER_VIEW mIndexBufferView;
    };
}