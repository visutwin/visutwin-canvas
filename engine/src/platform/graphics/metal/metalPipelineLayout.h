// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.11.2025.
//
#pragma once

#include <Metal/Metal.hpp>
#include <vector>

namespace visutwin::canvas::metal
{
    enum class BindingType {
        UniformBuffer,
        StorageBuffer,
        Sampler,
        SampledTexture,
        StorageTexture,
    };

    enum ShaderStageFlags : uint32_t {
        ShaderStage_None     = 0,
        ShaderStage_Vertex   = 1 << 0,
        ShaderStage_Fragment = 1 << 1,
        ShaderStage_Compute  = 1 << 2,
        ShaderStage_All      = 0x7
    };

    struct BindGroupLayoutEntryDesc {
        uint32_t binding;
        BindingType type;
        ShaderStageFlags visibility;
    };

    struct BindGroupLayoutDesc {
        uint32_t groupIndex;
        std::vector<BindGroupLayoutEntryDesc> entries;
    };

    class BindGroupLayout {
    public:
        explicit BindGroupLayout(const BindGroupLayoutDesc& desc): _groupIndex(desc.groupIndex), _entries(desc.entries) {}

        [[nodiscard]] uint32_t groupIndex() const { return _groupIndex; }

        [[nodiscard]] const std::vector<BindGroupLayoutEntryDesc>& entries() const { return _entries; }

        [[nodiscard]] const BindGroupLayoutEntryDesc* findEntry(const uint32_t binding) const {
            for (auto& e : _entries) {
                if (e.binding == binding)
                {
                    return &e;
                }
            }
            return nullptr;
        }
    private:
        uint32_t _groupIndex;
        std::vector<BindGroupLayoutEntryDesc> _entries;
    };

    class BindGroup {
    public:
        BindGroup(MTL::Device* device, BindGroupLayout* layout, MTL::ArgumentEncoder* encoder):
            _device(device), _layout(layout), _encoder(encoder)
        {
            assert(_device && "Device must not be null");
            assert(_layout && "Layout must not be null");
            assert(_encoder && "Argument encoder must not be null");

            NS::UInteger length = _encoder->encodedLength();

            _argBuffer= _device->newBuffer(length, MTL::ResourceStorageModeManaged);
            assert(_argBuffer && "Failed to allocate argument buffer");

            _encoder->setArgumentBuffer(_argBuffer, 0);
        }

        ~BindGroup()
        {
            if (_argBuffer)
            {
                _argBuffer->release();
            }
            if (_encoder)
            {
                _encoder->release();
            }
        }

        [[nodiscard]] BindGroupLayout* layout() const { return _layout; }

        [[nodiscard]] MTL::Buffer* argBuffer() const { return _argBuffer; }

        void setBuffer(const uint32_t binding, const MTL::Buffer* buffer, const NS::UInteger offset = 0) const
        {
            if (auto* entry = _layout->findEntry(binding); !entry ||
                (entry->type != BindingType::UniformBuffer && entry->type != BindingType::StorageBuffer))
            {
                return;
            }
            _encoder->setBuffer(buffer, offset, binding);
        }

        void setSampler(const uint32_t binding, const MTL::SamplerState* sampler) const
        {
            if (auto* entry = _layout->findEntry(binding); !entry || entry->type != BindingType::Sampler)
            {
                return;
            }
            _encoder->setSamplerState(sampler, binding);
        }

        void setTexture(const uint32_t binding, const MTL::Texture* texture) const
        {
            if (auto* entry = _layout->findEntry(binding); !entry ||
                (entry->type != BindingType::SampledTexture && entry->type != BindingType::StorageTexture))
            {
                return;
            }
            _encoder->setTexture(texture, binding);
        }

        // Call this before encoding draws if you use Managed storage
        void didModifyRange() const
        {
            _argBuffer->didModifyRange(NS::Range::Make(0, _argBuffer->length()));
        }

    private:
        MTL::Device* _device = nil;
        BindGroupLayout* _layout = nullptr;
        MTL::ArgumentEncoder* _encoder = nil;
        MTL::Buffer* _argBuffer = nil;
    };

    struct PipelineLayoutDesc {
        std::vector<BindGroupLayout*> bindGroupLayouts;
    };

    class PipelineLayout
    {
    public:
        explicit PipelineLayout(const PipelineLayoutDesc& desc) : _bindGroupLayouts(desc.bindGroupLayouts) {}

        [[nodiscard]] BindGroupLayout* getBindGroupLayout(const uint32_t index) const {
            if (index >= _bindGroupLayouts.size())
            {
                return nullptr;
            }
            return _bindGroupLayouts[index];
        }

        void setDebugLabel(int layoutIt, const std::string& label) {}
    private:
        std::vector<BindGroupLayout*> _bindGroupLayouts;
    };
}
