#include "RtBindings.h"
#include "Helpers/DirectXRaytracingHelper.h"
#include <codecvt>

namespace DXRFramework
{
    RtBindings::SharedPtr RtBindings::create(RtContext::SharedPtr context, RtProgram::SharedPtr program, RtScene::SharedPtr scene)
    {
        return SharedPtr(new RtBindings(context, program, scene));
    }

    RtBindings::RtBindings(RtContext::SharedPtr context, RtProgram::SharedPtr program, RtScene::SharedPtr scene)
        : mProgram(program), mScene(scene)
    {
        init(context);
    }

    RtBindings::~RtBindings() = default;

    bool RtBindings::init(RtContext::SharedPtr context)
    {
        mHitProgCount = mProgram->getHitProgramCount();
        mMissProgCount = mProgram->getMissProgramCount();
        mFirstHitVarEntry = kFirstMissRecordIndex + mMissProgCount;

        mProgramIdentifierSize = context->getFallbackDevice()->GetShaderIdentifierSize();
        assert(mProgramIdentifierSize == D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

        mGlobalParams = RtParams::create();

        // Find the max root-signature size, create params with root signatures and reserve space
        uint32_t maxRootSigSize = 96; // TEMP

        mRayGenParams = RtParams::create(mProgramIdentifierSize);
        mRayGenParams->allocateStorage(maxRootSigSize);
        // update maxRootSigSize

        UINT recordCountPerHit = mScene->getNumInstances();
        mHitParams.resize(mHitProgCount);
        for (UINT i = 0 ; i < mHitProgCount; ++i) {
            mHitParams[i].resize(recordCountPerHit);
            for (UINT j = 0; j < recordCountPerHit; ++j) {
                mHitParams[i][j] = RtParams::create(mProgramIdentifierSize);
                mHitParams[i][j]->allocateStorage(maxRootSigSize);
                // update maxRootSigSize
            }
        }

        mMissParams.resize(mMissProgCount);
        for (UINT i = 0 ; i < mMissProgCount; ++i) {
            mMissParams[i] = RtParams::create(mProgramIdentifierSize);
            mMissParams[i]->allocateStorage(maxRootSigSize);
            // update maxRootSigSize
        }

        // Allocate shader table
        mNumHitRecords = recordCountPerHit * mHitProgCount;
        UINT numEntries = mMissProgCount + mNumHitRecords + 1 /* ray-gen */;

        mRecordSize = mProgramIdentifierSize + maxRootSigSize;
        mRecordSize = ROUND_UP(mRecordSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
        assert(mRecordSize != 0);

        UINT shaderTableSize = numEntries * mRecordSize;

        mShaderTableData.resize(shaderTableSize);
        mShaderTable = CreateBuffer(context->getDevice(), shaderTableSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, kUploadHeapProps);

        return true;
    }

    void RtBindings::applyShaderId(uint8_t* & record, std::string shaderId, ID3D12RaytracingFallbackStateObject *rtso)
    {
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        std::wstring entryPoint = converter.from_bytes(shaderId);
        void *id = rtso->GetShaderIdentifier(entryPoint.c_str());
        if ( !id ) {
            throw std::logic_error("Unknown shader identifier used in the SBT: " + shaderId);
        }
        size_t index = std::distance(mShaderTableData.data(), record) / mRecordSize;
        d_shaderRecords[index] = entryPoint.c_str();
        memcpy(record, id, mProgramIdentifierSize);
        record += mProgramIdentifierSize;
    }

    void RtBindings::applyRtProgramVars(uint8_t *record, RtShader::SharedPtr shader, ID3D12RaytracingFallbackStateObject *rtso, RtParams::SharedPtr params)
    {
        applyShaderId(record, shader->getEntryPoint(), rtso);
        params->applyRootParams(shader, record);
    }

    void RtBindings::applyRtProgramVars(uint8_t *record, const RtProgram::HitGroup &hitGroup, ID3D12RaytracingFallbackStateObject *rtso, RtParams::SharedPtr params)
    {
        applyShaderId(record, hitGroup.mExportName, rtso);
        params->applyRootParams(hitGroup, record);
    }

    void RtBindings::apply(RtContext::SharedPtr context, RtState::SharedPtr state)
    {
        auto rtso = state->getFallbackRtso();

        d_shaderRecords.resize(1 + mMissProgCount + mNumHitRecords);

        uint8_t *rayGenRecord = getRayGenRecordPtr();
        applyRtProgramVars(rayGenRecord, mProgram->getRayGenProgram(), rtso, mRayGenParams);

        UINT hitCount = mProgram->getHitProgramCount(); // = MultiplierForGeometryContributionToHitGroupIndex
        for (UINT h = 0; h < hitCount; h++) {
            UINT geometryCount = mScene->getNumInstances();
            for (UINT i = 0; i < geometryCount; i++) {
                uint8_t *pHitRecord = getHitRecordPtr(h, i);
                uint32_t type = mScene->getModel(i)->getGeometryType();
                if (type >= mProgram->getGeometryTypeCount(h)) {
                    throw std::invalid_argument("Model " + std::to_string(i) + " has geometry with no corresponding hit group");
                }
                applyRtProgramVars(pHitRecord, mProgram->getHitProgram(h, type), rtso, mHitParams[h][i]);
            }
        }

        for (UINT m = 0; m < mProgram->getMissProgramCount(); m++) {
            uint8_t *pMissRecord = getMissRecordPtr(m);
            applyRtProgramVars(pMissRecord, mProgram->getMissProgram(m), rtso, mMissParams[m]);
        }

#if _DEBUG
        DebugPrint();
#endif

        // Update shader table
        uint8_t *mappedBuffer;
        HRESULT hr = mShaderTable->Map(0, nullptr, reinterpret_cast<void**>(&mappedBuffer));
        if (FAILED(hr)) {
            throw std::logic_error("Could not map the shader binding table");
        }
        memcpy(mappedBuffer, mShaderTableData.data(), mShaderTableData.size());
        mShaderTable->Unmap(0, nullptr);
    }

    // We are using the following layout for the shader-table:
    //
    // +------------+---------+---------+-----+--------+---------+--------+-----+--------+--------+-----+--------+-----+--------+--------+-----+--------+
    // |            |         |         | ... |        |         |        | ... |        |        | ... |        | ... |        |        | ... |        |
    // |   RayGen   |   Ray0  |   Ray1  | ... |  RayN  |   Ray0  |  Ray1  | ... |  RayN  |  Ray0  | ... |  RayN  | ... |  Ray0  |  Ray0  | ... |  RayN  |
    // |   Entry    |   Miss  |   Miss  | ... |  Miss  |   Hit   |   Hit  | ... |  Hit   |  Hit   | ... |  Hit   | ... |  Hit   |  Hit   | ... |  Hit   |
    // |            |         |         | ... |        |  Mesh0  |  Mesh0 | ... |  Mesh0 |  Mesh1 | ... |  Mesh1 | ... | MeshN  |  MeshN | ... |  MeshN |
    // +------------+---------+---------+-----+--------+---------+--------+-----+--------+--------+-----+--------+-----+--------+--------+-----+--------+
    //
    // The first record is the ray gen, followed by the miss records, followed by the meshes records.
    // For each mesh we have N hit records, N == number of mesh instances in the model
    // The size of each record is mRecordSize
    //
    // If this layout changes, we also need to change the constants kRayGenRecordIndex and kFirstMissRecordIndex

    uint8_t *RtBindings::getRayGenRecordPtr()
    {
        return mShaderTableData.data() + (kRayGenRecordIndex * mRecordSize);
    }

    uint8_t *RtBindings::getMissRecordPtr(uint32_t missId)
    {
        assert(missId < mMissProgCount);
        uint32_t offset = mRecordSize * (kFirstMissRecordIndex + missId);
        return mShaderTableData.data() + offset;
    }

    uint8_t *RtBindings::getHitRecordPtr(uint32_t hitId, uint32_t meshId)
    {
        assert(hitId < mHitProgCount);
        uint32_t meshIndex = mFirstHitVarEntry + mHitProgCount * meshId;    // base record of the requested mesh
        uint32_t recordIndex = meshIndex + hitId;
        return mShaderTableData.data() + (recordIndex * mRecordSize);
    }

    // Pretty-print the shader records.
    void RtBindings::DebugPrint()
    {
        if (mDebugLastShaderTableSize == mShaderTableData.size())
            return;

        std::wstringstream wstr;
        wstr << L"|--------------------------------------------------------------------\n";
        wstr << L"|Shader table - " << L": "
            << mRecordSize << L" | " << mShaderTableData.size() << L" bytes\n";

        for ( UINT i = 0; i < d_shaderRecords.size(); i++ )
        {
            wstr << L"| [" << i << L"]: ";
            wstr << d_shaderRecords[i] << L", ";
            wstr << mProgramIdentifierSize << L" + " << mRecordSize - mProgramIdentifierSize << L" bytes \n";
        }
        wstr << L"|--------------------------------------------------------------------\n";
        wstr << L"\n";
        OutputDebugStringW(wstr.str().c_str());

        mDebugLastShaderTableSize = mShaderTableData.size();
    }
}
