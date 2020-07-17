#include "RtModel.h"
#include "Helpers/BottomLevelASGenerator.h"
#include "Helpers/DirectXRaytracingHelper.h"
#include "assimp/cimport.h"
#include "assimp/scene.h"
#include "assimp/postprocess.h"
#include <DirectXMath.h>
#include <numeric>

using namespace DirectX;

namespace DXRFramework
{
    void RtMesh::importMeshesFromFile(RtMesh::AddMeshCallback addMesh, RtContext::SharedPtr context, const std::string &filePath,
                                      const std::vector<UINT> materialMap, long overrideMaterialIndex)
    {
        auto flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs | aiProcess_JoinIdenticalVertices | aiProcess_PreTransformVertices;
        const aiScene *scene = aiImportFile(filePath.c_str(), flags);

        if (scene) {
            for (UINT meshId = 0; meshId < scene->mNumMeshes; ++meshId) {
                const auto &mesh = scene->mMeshes[meshId];
                std::vector<Vertex> interleavedVertexData;
                std::vector<uint32_t> indices;

                for (UINT i = 0; i < mesh->mNumVertices; ++i) {
                    aiVector3D &position = mesh->mVertices[i];
                    aiVector3D &normal = mesh->mNormals[i];
                    Vertex vertex;
                    vertex.position = XMFLOAT3(position.x, position.y, position.z);
                    vertex.normal = mesh->HasNormals() ? XMFLOAT3(normal.x, normal.y, normal.z) : XMFLOAT3(0.0f, 0.0f, 0.0f);
                    interleavedVertexData.emplace_back(vertex);
                }

                for (UINT i = 0; i < mesh->mNumFaces; ++i) {
                    const aiFace &face = mesh->mFaces[i];
                    assert(face.mNumIndices == 3);
                    indices.push_back(face.mIndices[0]);
                    indices.push_back(face.mIndices[1]);
                    indices.push_back(face.mIndices[2]);
                }

                const UINT materialIndex = overrideMaterialIndex < 0
                    ? materialMap[mesh->mMaterialIndex - 1] // aiMesh materials are 1-indexed
                    : static_cast<UINT>(overrideMaterialIndex);

                addMesh(RtMesh::create(context, interleavedVertexData, indices, materialIndex));
            }
        } else {
            std::vector<Vertex> interleavedVertexData;
            std::vector<uint32_t> indices;

            interleavedVertexData =
            {
                { { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
                { { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
                { { -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } }
            };
            indices = { 0, 2, 1 };

            if (overrideMaterialIndex < 0) {
                addMesh(RtMesh::create(context, interleavedVertexData, indices));
            } else {
                addMesh(RtMesh::create(context, interleavedVertexData, indices, static_cast<UINT>(overrideMaterialIndex)));
            }
        }

    }

    RtMesh::SharedPtr RtMesh::create(RtContext::SharedPtr context, std::vector<Vertex> interleavedVertexData, std::vector<uint32_t> indices, UINT materialIndex)
    {
        return SharedPtr(new RtMesh(context, interleavedVertexData, indices, materialIndex));
    }

    RtMesh::RtMesh(RtContext::SharedPtr context, std::vector<Vertex> interleavedVertexData, std::vector<uint32_t> indices, UINT materialIndex)
    {
        mMaterialIndex = materialIndex;
        mType = GeometryType::Triangles;
        mNumVertices = static_cast<UINT>(interleavedVertexData.size());
        mNumTriangles = static_cast<UINT>(indices.size() / 3);


        mHasIndexBuffer = indices.size() > 0;

        auto const inf = std::numeric_limits<float>::infinity();
        auto boxMinMax = std::accumulate(
            interleavedVertexData.begin(), interleavedVertexData.end(),
            std::pair<XMFLOAT3, XMFLOAT3>(XMFLOAT3(inf, inf, inf), XMFLOAT3(-inf, -inf, -inf)),
            [](std::pair<XMFLOAT3, XMFLOAT3> const& minmax, Vertex const& vertex) {
                return std::pair<XMFLOAT3, XMFLOAT3>(
                    XMFLOAT3(min(minmax.first.x, vertex.position.x),
                             min(minmax.first.y, vertex.position.y),
                             min(minmax.first.z, vertex.position.z)),
                    XMFLOAT3(max(minmax.second.x, vertex.position.x),
                             max(minmax.second.y, vertex.position.y),
                             max(minmax.second.z, vertex.position.z))
                    );
            });
        mBoundingBox = Math::BoundingBox(boxMinMax.first, boxMinMax.second);

        // calculate triangle areas and cdf
        mTriangleAreas.resize(mNumTriangles);
        for (UINT t = 0; t < mNumTriangles; t++) {
            XMVECTOR p0 = XMLoadFloat3(&interleavedVertexData[indices[t*3 + 0]].position);
            XMVECTOR p1 = XMLoadFloat3(&interleavedVertexData[indices[t*3 + 1]].position);
            XMVECTOR p2 = XMLoadFloat3(&interleavedVertexData[indices[t*3 + 2]].position);
            mTriangleAreas[t] = 0.5f * XMVectorGetX(XMVector3Length(XMVector3Cross(p1 - p0, p2 - p0)));
        }
        float sum = std::accumulate(mTriangleAreas.begin(), mTriangleAreas.end(), 0.0f);
        float scale = 1.0f / sum;
        std::accumulate(mTriangleAreas.begin(), mTriangleAreas.end(), 0.0f, [&](float prefixSum, float area) {
            float cdfEnd = min(prefixSum + area * scale, 1.0f);
            mTriangleCdf.push_back(cdfEnd);
            return cdfEnd;
        });

        auto device = context->getDevice();
        // Note: using upload heaps to transfer static data like vert buffers is not
        // recommended. Every time the GPU needs it, the upload heap will be marshalled
        // over. Please read up on Default Heap usage. An upload heap is used here for
        // code simplicity and because there are very few verts to actually transfer.
        AllocateUploadBuffer(device, interleavedVertexData.data(), mNumVertices * sizeof(Vertex), &mVertexBuffer);

        if (mHasIndexBuffer) {
            AllocateUploadBuffer(device, indices.data(), indices.size() * sizeof(uint32_t), &mIndexBuffer);
            AllocateUploadBuffer(device, mTriangleCdf.data(), mTriangleCdf.size() * sizeof(float), &mTriangleCdfResource);
        }
    }

    RtMesh::~RtMesh() = default;

    void RtMesh::build(RtContext::SharedPtr context)
    {
        auto device = context->getDevice();
        auto commandList = context->getCommandList();
        auto fallbackDevice = context->getFallbackDevice();
        auto fallbackCommandList = context->getFallbackCommandList();

        nv_helpers_dx12::BottomLevelASGenerator blasGenerator;

        // Just one vertex buffer per blas for now
        if (mHasIndexBuffer) {
            blasGenerator.AddVertexBuffer(mVertexBuffer.Get(), 0, mNumVertices, sizeof(Vertex),
                mIndexBuffer.Get(), 0, mNumTriangles * 3, DXGI_FORMAT_R32_UINT, nullptr, 0);
        } else {
            blasGenerator.AddVertexBuffer(mVertexBuffer.Get(), 0, mNumVertices, sizeof(Vertex), nullptr, 0);
        }

        UINT64 scratchSizeInBytes = 0;
        UINT64 resultSizeInBytes = 0;
        blasGenerator.ComputeASBufferSizes(fallbackDevice, false, &scratchSizeInBytes, &resultSizeInBytes);

        ComPtr<ID3D12Resource> scratch = CreateBuffer(device, scratchSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kDefaultHeapProps);

        D3D12_RESOURCE_STATES initialResourceState = fallbackDevice->GetAccelerationStructureResourceState();
        mBlasBuffer = CreateBuffer(device, resultSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, initialResourceState, kDefaultHeapProps);

        blasGenerator.Generate(commandList, fallbackCommandList, scratch.Get(), mBlasBuffer.Get());

        mVertexBufferSrvHandle = context->createBufferSRVHandle(mVertexBuffer.Get(), false, sizeof(Vertex));
        if (mIndexBuffer) {
            mIndexBufferSrvHandle = context->createBufferSRVHandle(mIndexBuffer.Get(), false, sizeof(uint32_t));
            mTriangleCdfSrvHandle = context->createBufferSRVHandle(mTriangleCdfResource.Get(), false, sizeof(float));
        }
    }
}

namespace DXRFramework
{
    RtProcedural::SharedPtr RtProcedural::create(RtContext::SharedPtr context, PrimitiveType::Enum primitiveType, XMFLOAT3 anchorPos, XMFLOAT3 size, UINT materialIndex)
    {
        return SharedPtr(new RtProcedural(context, primitiveType, anchorPos, size, materialIndex));
    }

    RtProcedural::RtProcedural(RtContext::SharedPtr context, PrimitiveType::Enum primitiveType, XMFLOAT3 anchorPos, XMFLOAT3 size, UINT materialIndex)
        : mPrimitiveType(primitiveType)
    {
        mMaterialIndex = materialIndex;

        // Set up AABB on a grid.
        switch ( primitiveType ) {
        case PrimitiveType::AnalyticPrimitive_AABB:
        case PrimitiveType::AnalyticPrimitive_Spheres:
            mType = GeometryType::AABB_Analytic;
            break;
        case PrimitiveType::VolumetricPrimitive_Metaballs:
            mType = GeometryType::AABB_Volumetric;
            break;
        case PrimitiveType::SignedDistancePrimitive_MiniSpheres:
        case PrimitiveType::SignedDistancePrimitive_IntersectedRoundCube:
        case PrimitiveType::SignedDistancePrimitive_SquareTorus:
        case PrimitiveType::SignedDistancePrimitive_TwistedTorus:
        case PrimitiveType::SignedDistancePrimitive_Cog:
        case PrimitiveType::SignedDistancePrimitive_Cylinder:
        case PrimitiveType::SignedDistancePrimitive_FractalPyramid:
            mType = GeometryType::AABB_SignedDistance;
            break;
        }

        mAabb = D3D12_RAYTRACING_AABB{
            anchorPos.x,
            anchorPos.y,
            anchorPos.z,
            anchorPos.x + size.x,
            anchorPos.y + size.y,
            anchorPos.z + size.z,
        };

        Math::Vector3 boxMin = anchorPos;
        Math::Vector3 boxMax = boxMin + size;
        mBoundingBox = Math::BoundingBox(boxMin, boxMax);

        auto device = context->getDevice();

        AllocateUploadBuffer(device, &mAabb, sizeof(mAabb), &mAabbBuffer);

        mPrimitiveConstantsBuffer.Create(device, 1, L"AABB primitive attributes");
        XMVECTOR vTranslation =
            0.5f * (XMLoadFloat3(reinterpret_cast<XMFLOAT3*>(&mAabb.MinX))
                   + XMLoadFloat3(reinterpret_cast<XMFLOAT3*>(&mAabb.MaxX)));
        XMMATRIX mTranslation = XMMatrixTranslationFromVector(vTranslation);
        XMMATRIX mTransform = XMMatrixTranspose(mTranslation);
        mPrimitiveConstantsBuffer->primitiveType = primitiveType;
        mPrimitiveConstantsBuffer->localSpaceToBottomLevelAS = mTransform;
        mPrimitiveConstantsBuffer->bottomLevelASToLocalSpace = XMMatrixInverse(nullptr, mTransform);
        mPrimitiveConstantsBuffer.CopyStagingToGpu(0);
    }

    RtProcedural::~RtProcedural() = default;

    void RtProcedural::build(RtContext::SharedPtr context)
    {
        auto device = context->getDevice();
        auto commandList = context->getCommandList();
        auto fallbackDevice = context->getFallbackDevice();
        auto fallbackCommandList = context->getFallbackCommandList();

        nv_helpers_dx12::BottomLevelASGenerator blasGenerator;

        blasGenerator.AddAabbBuffer(mAabbBuffer.Get());

        UINT64 scratchSizeInBytes = 0;
        UINT64 resultSizeInBytes = 0;
        blasGenerator.ComputeASBufferSizes(fallbackDevice, false, &scratchSizeInBytes, &resultSizeInBytes);

        ComPtr<ID3D12Resource> scratch = CreateBuffer(device, scratchSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kDefaultHeapProps);

        D3D12_RESOURCE_STATES initialResourceState = fallbackDevice->GetAccelerationStructureResourceState();
        mBlasBuffer = CreateBuffer(device, resultSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, initialResourceState, kDefaultHeapProps);

        blasGenerator.Generate(commandList, fallbackCommandList, scratch.Get(), mBlasBuffer.Get());

        mAabbBufferSrvHandle = context->createBufferSRVHandle(mAabbBuffer.Get(), false, sizeof(D3D12_RAYTRACING_AABB));
        mPrimitiveConstantsCbvHandle = context->createBufferCBVHandle(mPrimitiveConstantsBuffer.GetResource().Get(), sizeof(PrimitiveInstanceConstants));
    }
}
