#pragma once

#include "RtPrefix.h"
#include "RtContext.h"
#include "RtShader.h"
#include "Helpers/RootSignatureGenerator.h"
#include "dxcapi.h"
#include <functional>
#include <vector>

// The max scalars supported by Nvidia driver
#define DXR_MAX_PAYLOAD_SIZE_IN_BYTES (14 * sizeof(float))

namespace DXRFramework
{
    using nv_helpers_dx12::RootSignatureGenerator;

    class RtProgram
    {
    public:
        using SharedPtr = std::shared_ptr<RtProgram>;

        class ShaderLibrary
        {
        public:
            ShaderLibrary(IDxcBlob* dxilLibrary, const std::vector<std::wstring>& symbolExports);
            ShaderLibrary(const uint8_t *bytecode, UINT bytecodeSize, const std::vector<std::wstring>& symbolExports);
        private:
            friend class RtState;

            IDxcBlob* mDXIL;
            const std::vector<std::wstring> mExportedSymbols;

            std::vector<D3D12_EXPORT_DESC> mExports;
            D3D12_DXIL_LIBRARY_DESC mLibDesc;
        };

        class Desc
        {
        public:
            Desc() = default;
            Desc(IDxcBlob* dxilLibrary, const std::vector<std::wstring>& symbolExports) { addShaderLibrary(dxilLibrary, symbolExports); }
            Desc(const uint8_t *bytecode, UINT bytecodeSize, const std::vector<std::wstring>& symbolExports) { addShaderLibrary(bytecode, bytecodeSize, symbolExports); }

            Desc& addShaderLibrary(const std::shared_ptr<ShaderLibrary>& library);
            Desc& addShaderLibrary(IDxcBlob* dxilLibrary, const std::vector<std::wstring>& symbolExports);
            Desc& addShaderLibrary(const uint8_t *bytecode, UINT bytecodeSize, const std::vector<std::wstring>& symbolExports);

            Desc& setRayGen(const std::string& raygen);
            Desc& addMiss(uint32_t missIndex, const std::string& miss);
            Desc& addHitGroup(uint32_t hitIndex, const std::string& closestHit, const std::string& anyHit, const std::string& intersection = "");

            using RootSignatureConfigurator = std::function<void(RootSignatureGenerator &config)>;
            Desc& configureGlobalRootSignature(RootSignatureConfigurator configure);
            Desc& configureRayGenRootSignature(RootSignatureConfigurator configure);
            Desc& configureHitGroupRootSignature(RootSignatureConfigurator configure);
            Desc& configureMissRootSignature(RootSignatureConfigurator configure);
        private:
            friend class RtProgram;

            struct ShaderEntry
            {
                uint32_t libraryIndex = -1;
                std::string entryPoint;
                RootSignatureGenerator localRootSignatureConfig;
            };

            struct HitProgramEntry
            {
                std::string intersection;
                std::string anyHit;
                std::string closestHit;
                uint32_t libraryIndex = -1;
                RootSignatureGenerator localRootSignatureConfig;
            };

            ShaderEntry mRayGen;
            std::vector<ShaderEntry> mMiss;
            std::vector<HitProgramEntry> mHit;

            std::vector<std::shared_ptr<ShaderLibrary>> mShaderLibraries;
            uint32_t mActiveLibraryIndex = -1;

            RootSignatureGenerator mGlobalRootSignatureConfig;
            RootSignatureGenerator mRayGenRootSignatureConfig;
            RootSignatureGenerator mHitGroupRootSignatureConfig;
            RootSignatureGenerator mMissRootSignatureConfig;
        };

        struct HitGroup
        {
            RtShader::SharedPtr mClosestHit;
            RtShader::SharedPtr mAnyHit;
            RtShader::SharedPtr mIntersection;
            std::string mExportName;
        };

        static SharedPtr create(
            RtContext::SharedPtr context, const Desc& desc,
            uint32_t maxPayloadSize = DXR_MAX_PAYLOAD_SIZE_IN_BYTES,
            uint32_t maxAttributesSize = D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES);

        const std::vector<std::shared_ptr<ShaderLibrary>> &getShaderLibraries() const { return mDesc.mShaderLibraries; }

        RtShader::SharedPtr getRayGenProgram() const { return mRayGenProgram; }

        uint32_t getHitProgramCount() const { return (uint32_t)mHitPrograms.size(); }
        HitGroup getHitProgram(uint32_t rayIndex) const { return mHitPrograms[rayIndex]; }

        uint32_t getMissProgramCount() const { return (uint32_t)mMissPrograms.size(); }
        RtShader::SharedPtr getMissProgram(uint32_t rayIndex) const { return mMissPrograms[rayIndex]; }

        ID3D12RootSignature *getGlobalRootSignature() const { return mGlobalRootSignature.Get(); }

        ~RtProgram();
    private:
        RtProgram(RtContext::SharedPtr context, const Desc& desc, uint32_t maxPayloadSize, uint32_t maxAttributesSize);

        Desc mDesc;
        ComPtr<ID3D12RootSignature> mGlobalRootSignature;

        RtShader::SharedPtr mRayGenProgram;
        std::vector<HitGroup> mHitPrograms;
        std::vector<RtShader::SharedPtr> mMissPrograms;

        ID3D12RaytracingFallbackDevice *mFallbackDevice;
    };
}
