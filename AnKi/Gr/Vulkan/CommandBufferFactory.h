// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Gr/Vulkan/FenceFactory.h>
#include <AnKi/Gr/CommandBuffer.h>
#include <AnKi/Gr/Vulkan/MicroObjectRecycler.h>
#include <AnKi/Util/List.h>

namespace anki {

// Forward
class CommandBufferFactory;
class CommandBufferThreadAllocator;

/// @addtogroup vulkan
/// @{

class MicroCommandBuffer : public IntrusiveListEnabled<MicroCommandBuffer>
{
	friend class CommandBufferThreadAllocator;
	friend class MicroCommandBufferPtrDeleter;

public:
	MicroCommandBuffer(CommandBufferThreadAllocator* allocator)
		: m_threadAlloc(allocator)
	{
		ANKI_ASSERT(allocator);
	}

	~MicroCommandBuffer();

	void retain() const
	{
		m_refcount.fetchAdd(1);
	}

	I32 release() const
	{
		return m_refcount.fetchSub(1);
	}

	I32 getRefcount() const
	{
		return m_refcount.load();
	}

	void setFence(MicroFencePtr& fence)
	{
		ANKI_ASSERT(!(m_flags & CommandBufferFlag::SECOND_LEVEL));
		ANKI_ASSERT(!m_fence.isCreated());
		m_fence = fence;
	}

	MicroFencePtr& getFence()
	{
		return m_fence;
	}

	GrAllocator<U8>& getAllocator();

	/// Interface method.
	void onFenceDone()
	{
		reset();
	}

	StackAllocator<U8>& getFastAllocator()
	{
		return m_fastAlloc;
	}

	VkCommandBuffer getHandle() const
	{
		ANKI_ASSERT(m_handle);
		return m_handle;
	}

	template<typename T>
	void pushObjectRef(const GrObjectPtrT<T>& x)
	{
		pushToArray(m_objectRefs[T::CLASS_TYPE], x.get());
	}

	CommandBufferFlag getFlags() const
	{
		return m_flags;
	}

	VulkanQueueType getVulkanQueueType() const
	{
		ANKI_ASSERT(m_queue != VulkanQueueType::COUNT);
		return m_queue;
	}

private:
	static constexpr U32 MAX_REF_OBJECT_SEARCH = 16;

	StackAllocator<U8> m_fastAlloc;
	VkCommandBuffer m_handle = {};

	MicroFencePtr m_fence;
	Array<DynamicArray<GrObjectPtr>, U(GrObjectType::COUNT)> m_objectRefs;

	// Cacheline boundary

	CommandBufferThreadAllocator* m_threadAlloc;
	mutable Atomic<I32> m_refcount = {0};
	CommandBufferFlag m_flags = CommandBufferFlag::NONE;
	VulkanQueueType m_queue = VulkanQueueType::COUNT;

	void reset();

	void pushToArray(DynamicArray<GrObjectPtr>& arr, GrObject* grobj)
	{
		ANKI_ASSERT(grobj);

		// Search the temp cache to avoid setting the ref again
		if(arr.getSize() >= MAX_REF_OBJECT_SEARCH)
		{
			for(U32 i = arr.getSize() - MAX_REF_OBJECT_SEARCH; i < arr.getSize(); ++i)
			{
				if(arr[i].get() == grobj)
				{
					return;
				}
			}
		}

		// Not found in the temp cache, add it
		arr.emplaceBack(m_fastAlloc, grobj);
	}
};

template<>
inline void MicroCommandBuffer::pushObjectRef<GrObject>(const GrObjectPtr& x)
{
	pushToArray(m_objectRefs[x->getType()], x.get());
}

/// Deleter.
class MicroCommandBufferPtrDeleter
{
public:
	void operator()(MicroCommandBuffer* buff);
};

/// Micro command buffer pointer.
using MicroCommandBufferPtr = IntrusivePtr<MicroCommandBuffer, MicroCommandBufferPtrDeleter>;

/// Per-thread command buffer allocator.
class alignas(ANKI_CACHE_LINE_SIZE) CommandBufferThreadAllocator
{
	friend class CommandBufferFactory;
	friend class MicroCommandBuffer;

public:
	CommandBufferThreadAllocator(CommandBufferFactory* factory, ThreadId tid)
		: m_factory(factory)
		, m_tid(tid)
	{
		ANKI_ASSERT(factory);
	}

	~CommandBufferThreadAllocator()
	{
	}

	Error init();

	void destroy();

	GrAllocator<U8>& getAllocator();

	/// Request a new command buffer.
	Error newCommandBuffer(CommandBufferFlag cmdbFlags, MicroCommandBufferPtr& ptr);

	/// It will recycle it.
	void deleteCommandBuffer(MicroCommandBuffer* ptr);

private:
	CommandBufferFactory* m_factory;
	ThreadId m_tid;
	Array<VkCommandPool, U(VulkanQueueType::COUNT)> m_pools = {};

#if ANKI_EXTRA_CHECKS
	Atomic<U32> m_createdCmdbs = {0};
#endif

	Array3d<MicroObjectRecycler<MicroCommandBuffer>, 2, 2, U(VulkanQueueType::COUNT)> m_recyclers;
};

/// Command bufffer object recycler.
class CommandBufferFactory
{
	friend class CommandBufferThreadAllocator;
	friend class MicroCommandBuffer;

public:
	CommandBufferFactory() = default;

	CommandBufferFactory(const CommandBufferFactory&) = delete; // Non-copyable

	~CommandBufferFactory() = default;

	CommandBufferFactory& operator=(const CommandBufferFactory&) = delete; // Non-copyable

	Error init(GrAllocator<U8> alloc, VkDevice dev, const VulkanQueueFamilies& queueFamilies);

	void destroy();

	/// Request a new command buffer.
	Error newCommandBuffer(ThreadId tid, CommandBufferFlag cmdbFlags, MicroCommandBufferPtr& ptr);

	/// Stats.
	U32 getCreatedCommandBufferCount() const
	{
		return m_createdCmdBufferCount.load();
	}

private:
	GrAllocator<U8> m_alloc;
	VkDevice m_dev = VK_NULL_HANDLE;
	VulkanQueueFamilies m_queueFamilies;

	DynamicArray<CommandBufferThreadAllocator*> m_threadAllocs;
	RWMutex m_threadAllocMtx;

	Atomic<U32> m_createdCmdBufferCount = {0};
};
/// @}

} // end namespace anki

#include <AnKi/Gr/Vulkan/CommandBufferFactory.inl.h>
