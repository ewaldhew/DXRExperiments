#pragma once

#include <vector>
#include "RtPrefix.h"
#include "RtContext.h"
#include "RtModel.h"

namespace DXRFramework
{
    class RtScene
    {
    public:
        using SharedPtr = std::shared_ptr<RtScene const>;
        using SharedPtrMut = std::shared_ptr<RtScene>;

        static SharedPtrMut create();
        ~RtScene();
        SharedPtrMut copy() const { return SharedPtrMut(new RtScene(*this)); }

        class Node
        {
        public:
            using SharedPtr = std::shared_ptr<Node>;
            static SharedPtr create(RtModel::SharedPtr model, DirectX::XMMATRIX transform, UCHAR mask) { return SharedPtr(new Node(model, transform, mask)); }
        private:
            friend class RtScene;
            Node(RtModel::SharedPtr model, DirectX::XMMATRIX transform, UCHAR mask) : mModel(model), mTransform(transform), mMask(mask) {}

            RtModel::SharedPtr mModel;
            DirectX::XMMATRIX mTransform;
            UCHAR mMask;
        };

        void addModel(RtModel::SharedPtr model, DirectX::XMMATRIX transform, UCHAR instanceMask) { mInstances.emplace_back(Node::create(model, transform, instanceMask)); }
        RtModel::SharedPtr getModel(UINT index) const { return mInstances[index]->mModel; }
        DirectX::XMMATRIX getTransform(UINT index) const { return mInstances[index]->mTransform; }
        UINT getNumInstances() const { return static_cast<UINT>(mInstances.size()); }

        ID3D12Resource *getTlasResource() const { return mTlasBuffer.Get(); }
        WRAPPED_GPU_POINTER getTlasWrappedPtr() const { return mTlasWrappedPointer; }

        void build(RtContext::SharedPtr context, UINT hitGroupCount);
    private:
        RtScene();

        std::vector<Node::SharedPtr> mInstances;

        ComPtr<ID3D12Resource> mTlasBuffer;
        WRAPPED_GPU_POINTER mTlasWrappedPointer;
    };
}
