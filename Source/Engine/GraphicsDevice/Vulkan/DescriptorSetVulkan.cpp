// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

#if GRAPHICS_API_VULKAN

#include "DescriptorSetVulkan.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Utilities/Crc.h"
#include "GPUDeviceVulkan.h"
#include "RenderToolsVulkan.h"
#include "GPUContextVulkan.h"
#include "CmdBufferVulkan.h"
#include "Engine/Threading/Threading.h"
#include "Engine/Engine/Engine.h"

void DescriptorSetLayoutInfoVulkan::CacheTypesUsageID()
{
    static CriticalSection locker;
    static uint32 uniqueID = 1;
    static Dictionary<uint32, uint32> typesUsageHashMap;

    const uint32 typesUsageHash = Crc::MemCrc32(_layoutTypes, sizeof(_layoutTypes));
    ScopeLock lock(locker);
    uint32 id;
    if (!typesUsageHashMap.TryGet(typesUsageHash, id))
    {
        id = uniqueID++;
        typesUsageHashMap.Add(typesUsageHash, id);
    }
    _typesUsageID = id;
}

void DescriptorSetLayoutInfoVulkan::AddBindingsForStage(VkShaderStageFlagBits stageFlags, DescriptorSet::Stage descSet, const SpirvShaderDescriptorInfo* descriptorInfo)
{
    const int32 descriptorSetIndex = descSet;
    if (descriptorSetIndex >= _setLayouts.Count())
        _setLayouts.Resize(descriptorSetIndex + 1);
    SetLayout& descSetLayout = _setLayouts[descriptorSetIndex];

    VkDescriptorSetLayoutBinding binding;
    binding.stageFlags = stageFlags;
    binding.pImmutableSamplers = nullptr;
    for (uint32 descriptorIndex = 0; descriptorIndex < descriptorInfo->DescriptorTypesCount; descriptorIndex++)
    {
        auto& descriptor = descriptorInfo->DescriptorTypes[descriptorIndex];
        binding.binding = descriptorIndex;
        binding.descriptorType = descriptor.DescriptorType;
        binding.descriptorCount = descriptor.Count;

        _layoutTypes[binding.descriptorType]++;
        descSetLayout.LayoutBindings.Add(binding);
        _hash = Crc::MemCrc32(&binding, sizeof(binding), _hash);
    }
}

bool DescriptorSetLayoutInfoVulkan::operator==(const DescriptorSetLayoutInfoVulkan& other) const
{
    if (other._setLayouts.Count() != _setLayouts.Count())
        return false;
    if (other._typesUsageID != _typesUsageID)
        return false;

    for (int32 index = 0; index < other._setLayouts.Count(); index++)
    {
        const int32 bindingsCount = _setLayouts[index].LayoutBindings.Count();
        if (other._setLayouts[index].LayoutBindings.Count() != bindingsCount)
            return false;
        if (bindingsCount != 0 && Platform::MemoryCompare(other._setLayouts[index].LayoutBindings.Get(), _setLayouts[index].LayoutBindings.Get(), bindingsCount * sizeof(VkDescriptorSetLayoutBinding)))
            return false;
    }

    return true;
}

DescriptorSetLayoutVulkan::DescriptorSetLayoutVulkan(GPUDeviceVulkan* device)
    : _device(device)
{
}

DescriptorSetLayoutVulkan::~DescriptorSetLayoutVulkan()
{
    for (VkDescriptorSetLayout& handle : _handles)
    {
        _device->DeferredDeletionQueue.EnqueueResource(DeferredDeletionQueueVulkan::Type::DescriptorSetLayout, handle);
    }
}

void DescriptorSetLayoutVulkan::Compile()
{
    ASSERT(_handles.IsEmpty());

    // Validate device limits for the engine
    const VkPhysicalDeviceLimits& limits = _device->PhysicalDeviceLimits;
    ASSERT(_layoutTypes[VK_DESCRIPTOR_TYPE_SAMPLER] + _layoutTypes[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER] < limits.maxDescriptorSetSamplers);
    ASSERT(_layoutTypes[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER]+ _layoutTypes[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC] < limits.maxDescriptorSetUniformBuffers);
    ASSERT(_layoutTypes[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC] < limits.maxDescriptorSetUniformBuffersDynamic);
    ASSERT(_layoutTypes[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER] + _layoutTypes[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC] < limits.maxDescriptorSetStorageBuffers);
    ASSERT(_layoutTypes[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC] < limits.maxDescriptorSetStorageBuffersDynamic);
    ASSERT(_layoutTypes[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER] + _layoutTypes[VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE] + _layoutTypes[VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER] < limits.maxDescriptorSetSampledImages);
    ASSERT(_layoutTypes[VK_DESCRIPTOR_TYPE_STORAGE_IMAGE] + _layoutTypes[VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER]< limits.maxDescriptorSetStorageImages);

    _handles.Resize(_setLayouts.Count());
    for (int32 i = 0; i < _setLayouts.Count(); i++)
    {
        auto& layout = _setLayouts[i];
        VkDescriptorSetLayoutCreateInfo layoutInfo;
        RenderToolsVulkan::ZeroStruct(layoutInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        layoutInfo.bindingCount = layout.LayoutBindings.Count();
        layoutInfo.pBindings = layout.LayoutBindings.Get();
        VALIDATE_VULKAN_RESULT(vkCreateDescriptorSetLayout(_device->Device, &layoutInfo, nullptr, &_handles[i]));
    }

    if (_typesUsageID == ~0)
    {
        CacheTypesUsageID();
    }

    RenderToolsVulkan::ZeroStruct(_allocateInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    _allocateInfo.descriptorSetCount = _handles.Count();
    _allocateInfo.pSetLayouts = _handles.Get();
}

DescriptorPoolVulkan::DescriptorPoolVulkan(GPUDeviceVulkan* device, const DescriptorSetLayoutVulkan& layout)
    : _device(device)
    , _handle(VK_NULL_HANDLE)
    , _descriptorSetsMax(0)
    , _allocatedDescriptorSetsCount(0)
    , _allocatedDescriptorSetsCountMax(0)
    , _layout(layout)
{
    Array<VkDescriptorPoolSize, FixedAllocation<VULKAN_DESCRIPTOR_TYPE_END + 1>> types;

    // The maximum amount of descriptor sets layout allocations to hold
    const uint32 MaxSetsAllocations = 256;
    _descriptorSetsMax = MaxSetsAllocations * (VULKAN_HASH_POOLS_WITH_TYPES_USAGE_ID ? 1 : _layout.GetLayouts().Count());
    for (uint32 typeIndex = VULKAN_DESCRIPTOR_TYPE_BEGIN; typeIndex <= VULKAN_DESCRIPTOR_TYPE_END; typeIndex++)
    {
        const VkDescriptorType descriptorType = (VkDescriptorType)typeIndex;
        const uint32 typesUsed = _layout.GetTypesUsed(descriptorType);
        if (typesUsed > 0)
        {
            VkDescriptorPoolSize& type = types.AddOne();
            Platform::MemoryClear(&type, sizeof(type));
            type.type = descriptorType;
            type.descriptorCount = typesUsed * MaxSetsAllocations;
        }
    }

    VkDescriptorPoolCreateInfo createInfo;
    RenderToolsVulkan::ZeroStruct(createInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    createInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    createInfo.poolSizeCount = types.Count();
    createInfo.pPoolSizes = types.Get();
    createInfo.maxSets = _descriptorSetsMax;
    VALIDATE_VULKAN_RESULT(vkCreateDescriptorPool(_device->Device, &createInfo, nullptr, &_handle));
}

DescriptorPoolVulkan::~DescriptorPoolVulkan()
{
    if (_handle != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(_device->Device, _handle, nullptr);
    }
}

void DescriptorPoolVulkan::Track(const DescriptorSetLayoutVulkan& layout)
{
#if !BUILD_RELEASE
    for (uint32 typeIndex = VULKAN_DESCRIPTOR_TYPE_BEGIN; typeIndex <= VULKAN_DESCRIPTOR_TYPE_END; typeIndex++)
    {
        ASSERT(_layout.GetTypesUsed((VkDescriptorType)typeIndex) == layout.GetTypesUsed((VkDescriptorType)typeIndex));
    }
#endif
    _allocatedDescriptorSetsCount += layout.GetLayouts().Count();
    _allocatedDescriptorSetsCountMax = Math::Max(_allocatedDescriptorSetsCount, _allocatedDescriptorSetsCountMax);
}

void DescriptorPoolVulkan::TrackRemoveUsage(const DescriptorSetLayoutVulkan& layout)
{
    // Check and increment our current type usage
    for (uint32 typeIndex = VULKAN_DESCRIPTOR_TYPE_BEGIN; typeIndex <= VULKAN_DESCRIPTOR_TYPE_END; typeIndex++)
    {
        ASSERT(_layout.GetTypesUsed((VkDescriptorType)typeIndex) == layout.GetTypesUsed((VkDescriptorType)typeIndex));
    }

    _allocatedDescriptorSetsCount -= layout.GetLayouts().Count();
}

void DescriptorPoolVulkan::Reset()
{
    if (_handle != VK_NULL_HANDLE)
    {
        VALIDATE_VULKAN_RESULT(vkResetDescriptorPool(_device->Device, _handle, 0));
    }
    _allocatedDescriptorSetsCount = 0;
}

bool DescriptorPoolVulkan::AllocateDescriptorSets(const VkDescriptorSetAllocateInfo& descriptorSetAllocateInfo, VkDescriptorSet* result)
{
    VkDescriptorSetAllocateInfo allocateInfo = descriptorSetAllocateInfo;
    allocateInfo.descriptorPool = _handle;
    return vkAllocateDescriptorSets(_device->Device, &allocateInfo, result) == VK_SUCCESS;
}

TypedDescriptorPoolSetVulkan::~TypedDescriptorPoolSetVulkan()
{
    for (auto pool = _poolListHead; pool;)
    {
        const auto next = pool->Next;
        Delete(pool->Element);
        Delete(pool);
        pool = next;
    }
}

bool TypedDescriptorPoolSetVulkan::AllocateDescriptorSets(const DescriptorSetLayoutVulkan& layout, VkDescriptorSet* outSets)
{
    const auto& layoutHandles = layout.GetHandles();
    if (layoutHandles.HasItems())
    {
        auto* pool = _poolListCurrent->Element;
        while (!pool->AllocateDescriptorSets(layout.GetAllocateInfo(), outSets))
        {
            pool = GetFreePool(true);
        }
        return true;
    }
    return true;
}

DescriptorPoolVulkan* TypedDescriptorPoolSetVulkan::GetFreePool(bool forceNewPool)
{
    if (!forceNewPool)
    {
        return _poolListCurrent->Element;
    }
    if (_poolListCurrent->Next)
    {
        _poolListCurrent = _poolListCurrent->Next;
        return _poolListCurrent->Element;
    }
    return PushNewPool();
}

DescriptorPoolVulkan* TypedDescriptorPoolSetVulkan::PushNewPool()
{
    auto* newPool = New<DescriptorPoolVulkan>(_device, _layout);
    if (_poolListCurrent)
    {
        _poolListCurrent->Next = New<PoolList>(newPool);
        _poolListCurrent = _poolListCurrent->Next;
    }
    else
    {
        _poolListCurrent = _poolListHead = New<PoolList>(newPool);
    }
    return newPool;
}

void TypedDescriptorPoolSetVulkan::Reset()
{
    for (PoolList* pool = _poolListHead; pool; pool = pool->Next)
    {
        pool->Element->Reset();
    }
    _poolListCurrent = _poolListHead;
}

DescriptorPoolSetContainerVulkan::DescriptorPoolSetContainerVulkan(GPUDeviceVulkan* device)
    : _device(device)
    , _lastFrameUsed(Engine::FrameCount)
    , _used(true)
{
}

DescriptorPoolSetContainerVulkan::~DescriptorPoolSetContainerVulkan()
{
    _typedDescriptorPools.ClearDelete();
}

TypedDescriptorPoolSetVulkan* DescriptorPoolSetContainerVulkan::AcquireTypedPoolSet(const DescriptorSetLayoutVulkan& layout)
{
    const uint32 hash = VULKAN_HASH_POOLS_WITH_TYPES_USAGE_ID ? layout.GetTypesUsageID() : GetHash(layout);
    TypedDescriptorPoolSetVulkan* typedPool;
    if (!_typedDescriptorPools.TryGet(hash, typedPool))
    {
        typedPool = New<TypedDescriptorPoolSetVulkan>(_device, this, layout);
        _typedDescriptorPools.Add(hash, typedPool);
    }
    return typedPool;
}

void DescriptorPoolSetContainerVulkan::Reset()
{
    for (auto i = _typedDescriptorPools.Begin(); i.IsNotEnd(); ++i)
    {
        TypedDescriptorPoolSetVulkan* typedPool = i->Value;
        typedPool->Reset();
    }
}

void DescriptorPoolSetContainerVulkan::SetUsed(bool used)
{
    _used = used;
    _lastFrameUsed = used ? Engine::FrameCount : _lastFrameUsed;
}

DescriptorPoolsManagerVulkan::DescriptorPoolsManagerVulkan(GPUDeviceVulkan* device)
    : _device(device)
{
}

DescriptorPoolsManagerVulkan::~DescriptorPoolsManagerVulkan()
{
    _poolSets.ClearDelete();
}

DescriptorPoolSetContainerVulkan& DescriptorPoolsManagerVulkan::AcquirePoolSetContainer()
{
    ScopeLock lock(_locker);

    for (auto* poolSet : _poolSets)
    {
        if (poolSet->IsUnused())
        {
            poolSet->SetUsed(true);
            poolSet->Reset();
            return *poolSet;
        }
    }

    const auto poolSet = New<DescriptorPoolSetContainerVulkan>(_device);
    _poolSets.Add(poolSet);
    return *poolSet;
}

void DescriptorPoolsManagerVulkan::ReleasePoolSet(DescriptorPoolSetContainerVulkan& poolSet)
{
    poolSet.SetUsed(false);
}

void DescriptorPoolsManagerVulkan::GC()
{
    ScopeLock lock(_locker);
    for (int32 i = _poolSets.Count() - 1; i >= 0; i--)
    {
        const auto poolSet = _poolSets[i];
        if (poolSet->IsUnused() && Engine::FrameCount - poolSet->GetLastFrameUsed() > VULKAN_RESOURCE_DELETE_SAFE_FRAMES_COUNT)
        {
            _poolSets.RemoveAt(i);
            Delete(poolSet);
            break;
        }
    }
}

PipelineLayoutVulkan::PipelineLayoutVulkan(GPUDeviceVulkan* device, const DescriptorSetLayoutInfoVulkan& layout)
    : _device(device)
    , _handle(VK_NULL_HANDLE)
    , _descriptorSetLayout(device)
{
    _descriptorSetLayout.CopyFrom(layout);
    _descriptorSetLayout.Compile();

    const auto& layoutHandles = _descriptorSetLayout.GetHandles();

    VkPipelineLayoutCreateInfo createInfo;
    RenderToolsVulkan::ZeroStruct(createInfo, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    createInfo.setLayoutCount = layoutHandles.Count();
    createInfo.pSetLayouts = layoutHandles.Get();
    VALIDATE_VULKAN_RESULT(vkCreatePipelineLayout(_device->Device, &createInfo, nullptr, &_handle));
}

PipelineLayoutVulkan::~PipelineLayoutVulkan()
{
    if (_handle != VK_NULL_HANDLE)
    {
        _device->DeferredDeletionQueue.EnqueueResource(DeferredDeletionQueueVulkan::Type::PipelineLayout, _handle);
    }
}

uint32 DescriptorSetWriterVulkan::SetupDescriptorWrites(const SpirvShaderDescriptorInfo& info, VkWriteDescriptorSet* writeDescriptors, VkDescriptorImageInfo* imageInfo, VkDescriptorBufferInfo* bufferInfo, VkBufferView* texelBufferView, uint8* bindingToDynamicOffset)
{
    ASSERT(info.DescriptorTypesCount <= SpirvShaderDescriptorInfo::MaxDescriptors);
    WriteDescriptors = writeDescriptors;
    WritesCount = info.DescriptorTypesCount;
    BindingToDynamicOffset = bindingToDynamicOffset;
    uint32 dynamicOffsetIndex = 0;
    for (uint32 i = 0; i < info.DescriptorTypesCount; i++)
    {
        const auto& descriptor = info.DescriptorTypes[i];
        writeDescriptors->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescriptors->dstBinding = i;
        writeDescriptors->descriptorCount = descriptor.Count;
        writeDescriptors->descriptorType = descriptor.DescriptorType;

        switch (writeDescriptors->descriptorType)
        {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            BindingToDynamicOffset[i] = dynamicOffsetIndex;
            dynamicOffsetIndex++;
            writeDescriptors->pBufferInfo = bufferInfo++;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            writeDescriptors->pBufferInfo = bufferInfo;
            bufferInfo += descriptor.Count;
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            writeDescriptors->pImageInfo = imageInfo;
            imageInfo += descriptor.Count;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            writeDescriptors->pTexelBufferView = texelBufferView;
            texelBufferView += descriptor.Count;
            break;
        default:
            CRASH;
            break;
        }

        writeDescriptors++;
    }
    return dynamicOffsetIndex;
}

#endif
