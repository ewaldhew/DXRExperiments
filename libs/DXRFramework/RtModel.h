#pragma once

#include "RtPrefix.h"
#include "RtContext.h"
#include "RaytracingHlslCompat.h"

namespace DXRFramework
{
    class RtModel
    {
    public:
        enum GeometryType : UINT
        {
            Triangles = 0,
            AABB_Analytic,
            AABB_Volumetric,
            AABB_SignedDistance,
            Count
        };

        using SharedPtr = std::shared_ptr<RtModel>;

        GeometryType getGeometryType() const { return mType; }
        UINT mMaterialIndex;

        ComPtr<ID3D12Resource> mBlasBuffer;

    protected:
        friend class RtScene;

        virtual void build(RtContext::SharedPtr context) = 0;

        GeometryType mType;
    };

    class RtMesh : public RtModel
    {
    public:
        using SharedPtr = std::shared_ptr<RtMesh>;

        static SharedPtr create(RtContext::SharedPtr context, const std::string &filePath, UINT materialIndex = 0);
        ~RtMesh();

        ID3D12Resource *getVertexBuffer() const { return mVertexBuffer.Get(); }
        ID3D12Resource *getIndexBuffer() const { return mIndexBuffer.Get(); }

        D3D12_GPU_DESCRIPTOR_HANDLE getVertexBufferSrvHandle() const { return mVertexBufferSrvHandle; }
        D3D12_GPU_DESCRIPTOR_HANDLE getIndexBufferSrvHandle() const { return mIndexBufferSrvHandle; }

    private:
        friend class RtScene;
        RtMesh(RtContext::SharedPtr context, const std::string &filePath, UINT materialIndex = 0);

        void build(RtContext::SharedPtr context);

        bool mHasIndexBuffer;
        UINT mNumVertices;
        UINT mNumTriangles;

        ComPtr<ID3D12Resource> mVertexBuffer;
        ComPtr<ID3D12Resource> mIndexBuffer;

        D3D12_GPU_DESCRIPTOR_HANDLE mVertexBufferSrvHandle;
        D3D12_GPU_DESCRIPTOR_HANDLE mIndexBufferSrvHandle;
    };

    class RtProcedural : public RtModel
    {
    public:
        using SharedPtr = std::shared_ptr<RtProcedural>;

        static SharedPtr create(RtContext::SharedPtr context, PrimitiveType::Enum primitiveType, XMFLOAT3 anchorPos, XMFLOAT3 size, UINT materialIndex = 0);
        ~RtProcedural();

        PrimitiveInstanceConstantBuffer getPrimitiveConstants() const { return { static_cast<UINT>(mPrimitiveType) }; }

        D3D12_GPU_DESCRIPTOR_HANDLE getAabbBufferSrvHandle() const { return mAabbBufferSrvHandle; }

    private:
        friend class RtScene;
        RtProcedural(RtContext::SharedPtr context, PrimitiveType::Enum primitiveType, XMFLOAT3 anchorPos, XMFLOAT3 size, UINT materialIndex = 0);

        void build(RtContext::SharedPtr context);

        PrimitiveType::Enum mPrimitiveType;
        D3D12_RAYTRACING_AABB mAabb;

        ComPtr<ID3D12Resource> mAabbBuffer;

        D3D12_GPU_DESCRIPTOR_HANDLE mAabbBufferSrvHandle;
    };

    inline RtMesh::SharedPtr toRtMesh(RtModel::SharedPtr ptr) { return std::static_pointer_cast<RtMesh>(ptr); }
    inline RtProcedural::SharedPtr toRtProcedural(RtModel::SharedPtr ptr) { return std::static_pointer_cast<RtProcedural>(ptr); }
}
