#include "GeometricModel.h"
#include "Util/DirectXHelper.h"

namespace DXTKExtend
{
    GeometricModel::GeometricModel(ID3D12Device* pDevice, CreatePrimitive modelFunc)
    {
        std::vector<VertexType> vertices;
        std::vector<uint16_t> indices;

        modelFunc(vertices, indices);

        mNumVertices = static_cast<UINT>(vertices.size());
        mIndexCount = static_cast<UINT>(indices.size());

        // Vertex data
        uint64_t sizeInBytes = uint64_t(vertices.size()) * sizeof(vertices[0]);
        if (sizeInBytes > uint64_t(D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM * 1024u * 1024u))
            throw std::exception("VB too large for DirectX 12");

        auto vertSizeBytes = static_cast<size_t>(sizeInBytes);
        AllocateUploadBuffer(pDevice, vertices.data(), vertSizeBytes, &mVertexBuffer);

        // Index data
        sizeInBytes = uint64_t(indices.size()) * sizeof(indices[0]);
        if (sizeInBytes > uint64_t(D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM * 1024u * 1024u))
            throw std::exception("IB too large for DirectX 12");

        auto indSizeBytes = static_cast<size_t>(sizeInBytes);
        AllocateUploadBuffer(pDevice, indices.data(), indSizeBytes, &mIndexBuffer);

        // Create views
        mVertexBufferView.BufferLocation = mVertexBuffer->GetGPUVirtualAddress();
        mVertexBufferView.StrideInBytes = static_cast<UINT>(sizeof(VertexType));
        mVertexBufferView.SizeInBytes = static_cast<UINT>(mVertexBuffer->GetDesc().Width);

        mIndexBufferView.BufferLocation = mIndexBuffer->GetGPUVirtualAddress();
        mIndexBufferView.SizeInBytes = static_cast<UINT>(mIndexBuffer->GetDesc().Width);
        mIndexBufferView.Format = DXGI_FORMAT_R16_UINT;
    }

    GeometricModel::GeometricModel(ComPtr<ID3D12Resource> vertexBuffer, size_t vertexStride, ComPtr<ID3D12Resource> indexBuffer, DXGI_FORMAT indexFormat)
        : mVertexBuffer(vertexBuffer), mIndexBuffer(indexBuffer)
    {
        auto vertSizeBytes = mVertexBuffer->GetDesc().Width;
        auto indSizeBytes = mIndexBuffer->GetDesc().Width;
        mNumVertices = vertSizeBytes / vertexStride;
        mIndexCount = indSizeBytes / SizeOfInBytes(indexFormat);

        mVertexBufferView.BufferLocation = mVertexBuffer->GetGPUVirtualAddress();
        mVertexBufferView.StrideInBytes = vertexStride;
        mVertexBufferView.SizeInBytes = static_cast<UINT>(vertSizeBytes);

        mIndexBufferView.BufferLocation = mIndexBuffer->GetGPUVirtualAddress();
        mIndexBufferView.SizeInBytes = static_cast<UINT>(indSizeBytes);
        mIndexBufferView.Format = indexFormat;
    }

    void GeometricModel::Draw(ID3D12GraphicsCommandList* commandList, UINT instanceCount) const
    {
        commandList->IASetVertexBuffers(0, 1, &mVertexBufferView);
        commandList->IASetIndexBuffer(&mIndexBufferView);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        commandList->DrawIndexedInstanced(mIndexCount, instanceCount, 0, 0, 0);
    }
}
