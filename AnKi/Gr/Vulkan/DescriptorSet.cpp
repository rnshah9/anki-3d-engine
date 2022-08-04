// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Gr/Vulkan/DescriptorSet.h>
#include <AnKi/Gr/Buffer.h>
#include <AnKi/Gr/Vulkan/BufferImpl.h>
#include <AnKi/Util/List.h>
#include <AnKi/Util/HashMap.h>
#include <AnKi/Util/Tracer.h>
#include <algorithm>

namespace anki {

thread_local DescriptorSetFactory::ThreadLocal* DescriptorSetFactory::m_threadLocal = nullptr;

/// Wraps a global descriptor set that is used to store bindless textures.
class DescriptorSetFactory::BindlessDescriptorSet
{
public:
	~BindlessDescriptorSet();

	Error init(const GrAllocator<U8>& alloc, VkDevice dev, const U32 bindlessTextureCount, U32 bindlessImageCount);

	/// Bind a sampled image.
	/// @note It's thread-safe.
	U32 bindTexture(const VkImageView view, const VkImageLayout layout);

	/// Bind a uniform texel buffer.
	/// @note It's thread-safe.
	U32 bindUniformTexelBuffer(VkBufferView view);

	/// @note It's thread-safe.
	void unbindTexture(U32 idx)
	{
		unbindCommon(idx, m_freeTexIndices, m_freeTexIndexCount);
	}

	/// @note It's thread-safe.
	void unbindUniformTexelBuffer(U32 idx)
	{
		unbindCommon(idx, m_freeTexelBufferIndices, m_freeTexelBufferIndexCount);
	}

	DescriptorSet getDescriptorSet() const
	{
		ANKI_ASSERT(m_dset);
		DescriptorSet out;
		out.m_handle = m_dset;
		return out;
	}

	VkDescriptorSetLayout getDescriptorSetLayout() const
	{
		ANKI_ASSERT(m_layout);
		return m_layout;
	}

private:
	GrAllocator<U8> m_alloc;
	VkDevice m_dev = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
	VkDescriptorPool m_pool = VK_NULL_HANDLE;
	VkDescriptorSet m_dset = VK_NULL_HANDLE;
	Mutex m_mtx;

	DynamicArray<U16> m_freeTexIndices;
	DynamicArray<U16> m_freeTexelBufferIndices;

	U16 m_freeTexIndexCount = MAX_U16;
	U16 m_freeTexelBufferIndexCount = MAX_U16;

	void unbindCommon(U32 idx, DynamicArray<U16>& freeIndices, U16& freeIndexCount);
};

/// Descriptor set internal class.
class DS : public IntrusiveListEnabled<DS>
{
public:
	VkDescriptorSet m_handle = {};
	U64 m_lastFrameUsed = MAX_U64;
	U64 m_hash;
};

/// Per thread allocator.
class DescriptorSetFactory::DSAllocator
{
public:
	DSAllocator(const DSAllocator&) = delete; // Non-copyable

	DSAllocator& operator=(const DSAllocator&) = delete; // Non-copyable

	DSAllocator(const DSLayoutCacheEntry* layout)
		: m_layoutEntry(layout)
	{
		ANKI_ASSERT(m_layoutEntry);
	}

	~DSAllocator();

	Error init();
	Error createNewPool();

	Error getOrCreateSet(U64 hash, const Array<AnyBindingExtended, MAX_BINDINGS_PER_DESCRIPTOR_SET>& bindings,
						 StackAllocator<U8>& tmpAlloc, const DS*& out)
	{
		out = tryFindSet(hash);
		if(out == nullptr)
		{
			ANKI_CHECK(newSet(hash, bindings, tmpAlloc, out));
		}

		return Error::NONE;
	}

private:
	const DSLayoutCacheEntry* m_layoutEntry; ///< Know your father.

	DynamicArray<VkDescriptorPool> m_pools;
	U32 m_lastPoolDSCount = 0;
	U32 m_lastPoolFreeDSCount = 0;

	IntrusiveList<DS> m_list; ///< At the left of the list are the least used sets.
	HashMap<U64, DS*> m_hashmap;

	[[nodiscard]] const DS* tryFindSet(U64 hash);
	Error newSet(U64 hash, const Array<AnyBindingExtended, MAX_BINDINGS_PER_DESCRIPTOR_SET>& bindings,
				 StackAllocator<U8>& tmpAlloc, const DS*& out);
	void writeSet(const Array<AnyBindingExtended, MAX_BINDINGS_PER_DESCRIPTOR_SET>& bindings, const DS& set,
				  StackAllocator<U8>& tmpAlloc);
};

class alignas(ANKI_CACHE_LINE_SIZE) DescriptorSetFactory::ThreadLocal
{
public:
	DynamicArray<DSAllocator*> m_allocators;
};

/// Cache entry. It's built around a specific descriptor set layout.
class DSLayoutCacheEntry
{
public:
	DescriptorSetFactory* m_factory;

	U64 m_hash = 0; ///< Layout hash.
	VkDescriptorSetLayout m_layoutHandle = {};
	BitSet<MAX_BINDINGS_PER_DESCRIPTOR_SET, U32> m_activeBindings = {false};
	Array<U32, MAX_BINDINGS_PER_DESCRIPTOR_SET> m_bindingArraySize = {};
	Array<DescriptorType, MAX_BINDINGS_PER_DESCRIPTOR_SET> m_bindingType = {};
	U32 m_minBinding = MAX_U32;
	U32 m_maxBinding = 0;
	U32 m_index = 0; ///< Index in DescriptorSetFactory::m_caches

	// Cache the create info
	Array<VkDescriptorPoolSize, U(DescriptorType::COUNT)> m_poolSizesCreateInf = {};
	VkDescriptorPoolCreateInfo m_poolCreateInf = {};

	DSLayoutCacheEntry(DescriptorSetFactory* factory, U32 index)
		: m_factory(factory)
		, m_index(index)
	{
	}

	~DSLayoutCacheEntry();

	Error init(const DescriptorBinding* bindings, U32 bindingCount, U64 hash);

	/// @note Thread-safe.
	Error getOrCreateDSAllocator(DescriptorSetFactory::DSAllocator*& alloc);
};

DescriptorSetFactory::BindlessDescriptorSet::~BindlessDescriptorSet()
{
	ANKI_ASSERT(m_freeTexIndexCount == m_freeTexIndices.getSize() && "Forgot to unbind some textures");
	ANKI_ASSERT(m_freeTexelBufferIndexCount == m_freeTexelBufferIndices.getSize()
				&& "Forgot to unbind some texel buffers");

	if(m_pool)
	{
		vkDestroyDescriptorPool(m_dev, m_pool, nullptr);
		m_pool = VK_NULL_HANDLE;
		m_dset = VK_NULL_HANDLE;
	}

	if(m_layout)
	{
		vkDestroyDescriptorSetLayout(m_dev, m_layout, nullptr);
		m_layout = VK_NULL_HANDLE;
	}

	m_freeTexIndices.destroy(m_alloc);
	m_freeTexelBufferIndices.destroy(m_alloc);
}

Error DescriptorSetFactory::BindlessDescriptorSet::init(const GrAllocator<U8>& alloc, VkDevice dev,
														U32 bindlessTextureCount, U32 bindlessTextureBuffers)
{
	ANKI_ASSERT(dev);
	m_alloc = alloc;
	m_dev = dev;

	// Create the layout
	{
		Array<VkDescriptorSetLayoutBinding, 2> bindings = {};
		bindings[0].binding = 0;
		bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
		bindings[0].descriptorCount = bindlessTextureCount;
		bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		bindings[1].binding = 1;
		bindings[1].stageFlags = VK_SHADER_STAGE_ALL;
		bindings[1].descriptorCount = bindlessTextureBuffers;
		bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;

		Array<VkDescriptorBindingFlagsEXT, 2> bindingFlags = {};
		bindingFlags[0] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT
						  | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT_EXT
						  | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT;
		bindingFlags[1] = bindingFlags[0];

		VkDescriptorSetLayoutBindingFlagsCreateInfoEXT extraInfos = {};
		extraInfos.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT;
		extraInfos.bindingCount = bindingFlags.getSize();
		extraInfos.pBindingFlags = &bindingFlags[0];

		VkDescriptorSetLayoutCreateInfo ci = {};
		ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT_EXT;
		ci.bindingCount = bindings.getSize();
		ci.pBindings = &bindings[0];
		ci.pNext = &extraInfos;

		ANKI_VK_CHECK(vkCreateDescriptorSetLayout(m_dev, &ci, nullptr, &m_layout));
	}

	// Create the pool
	{
		Array<VkDescriptorPoolSize, 2> sizes = {};
		sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		sizes[0].descriptorCount = bindlessTextureCount;
		sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
		sizes[1].descriptorCount = bindlessTextureBuffers;

		VkDescriptorPoolCreateInfo ci = {};
		ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		ci.maxSets = 1;
		ci.poolSizeCount = sizes.getSize();
		ci.pPoolSizes = &sizes[0];
		ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT;

		ANKI_VK_CHECK(vkCreateDescriptorPool(m_dev, &ci, nullptr, &m_pool));
	}

	// Create the descriptor set
	{
		VkDescriptorSetAllocateInfo ci = {};
		ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		ci.descriptorPool = m_pool;
		ci.descriptorSetCount = 1;
		ci.pSetLayouts = &m_layout;

		ANKI_VK_CHECK(vkAllocateDescriptorSets(m_dev, &ci, &m_dset));
	}

	// Init the free arrays
	{
		m_freeTexIndices.create(m_alloc, bindlessTextureCount);
		m_freeTexIndexCount = U16(m_freeTexIndices.getSize());

		for(U32 i = 0; i < m_freeTexIndices.getSize(); ++i)
		{
			m_freeTexIndices[i] = U16(m_freeTexIndices.getSize() - i - 1);
		}

		m_freeTexelBufferIndices.create(m_alloc, bindlessTextureBuffers);
		m_freeTexelBufferIndexCount = U16(m_freeTexelBufferIndices.getSize());

		for(U32 i = 0; i < m_freeTexelBufferIndices.getSize(); ++i)
		{
			m_freeTexelBufferIndices[i] = U16(m_freeTexelBufferIndices.getSize() - i - 1);
		}
	}

	return Error::NONE;
}

U32 DescriptorSetFactory::BindlessDescriptorSet::bindTexture(const VkImageView view, const VkImageLayout layout)
{
	ANKI_ASSERT(layout == VK_IMAGE_LAYOUT_GENERAL || layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	ANKI_ASSERT(view);
	LockGuard<Mutex> lock(m_mtx);
	ANKI_ASSERT(m_freeTexIndexCount > 0 && "Out of indices");

	// Pop the index
	--m_freeTexIndexCount;
	const U16 idx = m_freeTexIndices[m_freeTexIndexCount];
	ANKI_ASSERT(idx < m_freeTexIndices.getSize());

	// Update the set
	VkDescriptorImageInfo imageInf = {};
	imageInf.imageView = view;
	imageInf.imageLayout = layout;

	VkWriteDescriptorSet write = {};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.pNext = nullptr;
	write.dstSet = m_dset;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	write.dstArrayElement = idx;
	write.pImageInfo = &imageInf;

	vkUpdateDescriptorSets(m_dev, 1, &write, 0, nullptr);

	return idx;
}

U32 DescriptorSetFactory::BindlessDescriptorSet::bindUniformTexelBuffer(VkBufferView view)
{
	ANKI_ASSERT(view);
	LockGuard<Mutex> lock(m_mtx);
	ANKI_ASSERT(m_freeTexelBufferIndexCount > 0 && "Out of indices");

	// Pop the index
	--m_freeTexelBufferIndexCount;
	const U16 idx = m_freeTexelBufferIndices[m_freeTexelBufferIndexCount];
	ANKI_ASSERT(idx < m_freeTexelBufferIndices.getSize());

	// Update the set
	VkWriteDescriptorSet write = {};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.pNext = nullptr;
	write.dstSet = m_dset;
	write.dstBinding = 1;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
	write.dstArrayElement = idx;
	write.pTexelBufferView = &view;

	vkUpdateDescriptorSets(m_dev, 1, &write, 0, nullptr);

	return idx;
}

void DescriptorSetFactory::BindlessDescriptorSet::unbindCommon(U32 idx, DynamicArray<U16>& freeIndices,
															   U16& freeIndexCount)
{
	LockGuard<Mutex> lock(m_mtx);

	ANKI_ASSERT(idx < freeIndices.getSize());
	ANKI_ASSERT(freeIndexCount < freeIndices.getSize());

	freeIndices[freeIndexCount] = U16(idx);
	++freeIndexCount;

	// Sort the free indices to minimize fragmentation
	std::sort(&freeIndices[0], &freeIndices[0] + freeIndexCount, std::greater<U16>());

	// Make sure there are no duplicates
	for(U32 i = 1; i < freeIndexCount; ++i)
	{
		ANKI_ASSERT(freeIndices[i] != freeIndices[i - 1]);
	}
}

DescriptorSetFactory::DSAllocator::~DSAllocator()
{
	auto alloc = m_layoutEntry->m_factory->m_alloc;

	while(!m_list.isEmpty())
	{
		DS* ds = &m_list.getFront();
		m_list.popFront();

		alloc.deleteInstance(ds);
	}

	for(VkDescriptorPool pool : m_pools)
	{
		vkDestroyDescriptorPool(m_layoutEntry->m_factory->m_dev, pool, nullptr);
	}
	m_pools.destroy(alloc);

	m_hashmap.destroy(alloc);
}

Error DescriptorSetFactory::DSAllocator::init()
{
	ANKI_CHECK(createNewPool());
	return Error::NONE;
}

Error DescriptorSetFactory::DSAllocator::createNewPool()
{
	m_lastPoolDSCount = (m_lastPoolDSCount != 0) ? U32(F32(m_lastPoolDSCount) * DESCRIPTOR_POOL_SIZE_SCALE)
												 : DESCRIPTOR_POOL_INITIAL_SIZE;
	m_lastPoolFreeDSCount = m_lastPoolDSCount;

	// Set the create info
	Array<VkDescriptorPoolSize, U(DescriptorType::COUNT)> poolSizes;
	memcpy(&poolSizes[0], &m_layoutEntry->m_poolSizesCreateInf[0],
		   sizeof(poolSizes[0]) * m_layoutEntry->m_poolCreateInf.poolSizeCount);

	for(U i = 0; i < m_layoutEntry->m_poolCreateInf.poolSizeCount; ++i)
	{
		poolSizes[i].descriptorCount *= m_lastPoolDSCount;
		ANKI_ASSERT(poolSizes[i].descriptorCount > 0);
	}

	VkDescriptorPoolCreateInfo ci = m_layoutEntry->m_poolCreateInf;
	ci.pPoolSizes = &poolSizes[0];
	ci.maxSets = m_lastPoolDSCount;

	// Create
	VkDescriptorPool pool;
	ANKI_VK_CHECK(vkCreateDescriptorPool(m_layoutEntry->m_factory->m_dev, &ci, nullptr, &pool));
	ANKI_TRACE_INC_COUNTER(VK_DESCRIPTOR_POOL_CREATE, 1);

	// Push back
	m_pools.resize(m_layoutEntry->m_factory->m_alloc, m_pools.getSize() + 1);
	m_pools[m_pools.getSize() - 1] = pool;

	return Error::NONE;
}

const DS* DescriptorSetFactory::DSAllocator::tryFindSet(U64 hash)
{
	ANKI_ASSERT(hash > 0);

	auto it = m_hashmap.find(hash);
	if(it == m_hashmap.getEnd())
	{
		return nullptr;
	}
	else
	{
		DS* ds = *it;

		// Remove from the list and place at the end of the list
		m_list.erase(ds);
		m_list.pushBack(ds);
		ds->m_lastFrameUsed = m_layoutEntry->m_factory->m_frameCount;

		return ds;
	}
}

Error DescriptorSetFactory::DSAllocator::newSet(
	U64 hash, const Array<AnyBindingExtended, MAX_BINDINGS_PER_DESCRIPTOR_SET>& bindings, StackAllocator<U8>& tmpAlloc,
	const DS*& out_)
{
	DS* out = nullptr;

	// First try to see if there are unused to recycle
	const U64 crntFrame = m_layoutEntry->m_factory->m_frameCount;
	auto it = m_list.getBegin();
	const auto end = m_list.getEnd();
	while(it != end)
	{
		DS* set = &(*it);
		U64 frameDiff = crntFrame - set->m_lastFrameUsed;
		if(frameDiff > DESCRIPTOR_FRAME_BUFFERING)
		{
			// Found something, recycle
			auto it2 = m_hashmap.find(set->m_hash);
			ANKI_ASSERT(it2 != m_hashmap.getEnd());
			m_hashmap.erase(m_layoutEntry->m_factory->m_alloc, it2);
			m_list.erase(set);

			m_list.pushBack(set);
			m_hashmap.emplace(m_layoutEntry->m_factory->m_alloc, hash, set);

			out = set;
			break;
		}
		++it;
	}

	if(out == nullptr)
	{
		// Need to allocate one

		if(m_lastPoolFreeDSCount == 0)
		{
			// Can't allocate one from the current pool, create new
			ANKI_CHECK(createNewPool());
		}

		--m_lastPoolFreeDSCount;

		VkDescriptorSetAllocateInfo ci = {};
		ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		ci.descriptorPool = m_pools.getBack();
		ci.pSetLayouts = &m_layoutEntry->m_layoutHandle;
		ci.descriptorSetCount = 1;

		VkDescriptorSet handle;
		[[maybe_unused]] VkResult rez = vkAllocateDescriptorSets(m_layoutEntry->m_factory->m_dev, &ci, &handle);
		ANKI_ASSERT(rez == VK_SUCCESS && "That allocation can't fail");
		ANKI_TRACE_INC_COUNTER(VK_DESCRIPTOR_SET_CREATE, 1);

		out = m_layoutEntry->m_factory->m_alloc.newInstance<DS>();
		out->m_handle = handle;

		m_hashmap.emplace(m_layoutEntry->m_factory->m_alloc, hash, out);
		m_list.pushBack(out);
	}

	ANKI_ASSERT(out);
	out->m_lastFrameUsed = crntFrame;
	out->m_hash = hash;

	// Finally, write it
	writeSet(bindings, *out, tmpAlloc);

	out_ = out;
	return Error::NONE;
}

void DescriptorSetFactory::DSAllocator::writeSet(
	const Array<AnyBindingExtended, MAX_BINDINGS_PER_DESCRIPTOR_SET>& bindings, const DS& set,
	StackAllocator<U8>& tmpAlloc)
{
	DynamicArrayAuto<VkWriteDescriptorSet> writeInfos(tmpAlloc);
	DynamicArrayAuto<VkDescriptorImageInfo> texInfos(tmpAlloc);
	DynamicArrayAuto<VkDescriptorBufferInfo> buffInfos(tmpAlloc);
	DynamicArrayAuto<VkWriteDescriptorSetAccelerationStructureKHR> asInfos(tmpAlloc);
	DynamicArrayAuto<VkBufferView> bufferViews(tmpAlloc);

	// First pass: Populate the VkDescriptorImageInfo and VkDescriptorBufferInfo
	for(U bindingIdx = m_layoutEntry->m_minBinding; bindingIdx <= m_layoutEntry->m_maxBinding; ++bindingIdx)
	{
		if(m_layoutEntry->m_activeBindings.get(bindingIdx))
		{
			for(U arrIdx = 0; arrIdx < m_layoutEntry->m_bindingArraySize[bindingIdx]; ++arrIdx)
			{
				ANKI_ASSERT(bindings[bindingIdx].m_arraySize >= m_layoutEntry->m_bindingArraySize[bindingIdx]);
				const AnyBinding& b = (bindings[bindingIdx].m_arraySize == 1) ? bindings[bindingIdx].m_single
																			  : bindings[bindingIdx].m_array[arrIdx];

				switch(b.m_type)
				{
				case DescriptorType::COMBINED_TEXTURE_SAMPLER:
				{
					VkDescriptorImageInfo& info = *texInfos.emplaceBack();
					info.sampler = b.m_texAndSampler.m_samplerHandle;
					info.imageView = b.m_texAndSampler.m_imgViewHandle;
					info.imageLayout = b.m_texAndSampler.m_layout;
					break;
				}
				case DescriptorType::TEXTURE:
				{
					VkDescriptorImageInfo& info = *texInfos.emplaceBack();
					info.sampler = VK_NULL_HANDLE;
					info.imageView = b.m_tex.m_imgViewHandle;
					info.imageLayout = b.m_tex.m_layout;
					break;
				}
				case DescriptorType::SAMPLER:
				{
					VkDescriptorImageInfo& info = *texInfos.emplaceBack();
					info.sampler = b.m_sampler.m_samplerHandle;
					info.imageView = VK_NULL_HANDLE;
					info.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
					break;
				}
				case DescriptorType::UNIFORM_BUFFER:
				case DescriptorType::STORAGE_BUFFER:
				{
					VkDescriptorBufferInfo& info = *buffInfos.emplaceBack();
					info.buffer = b.m_buff.m_buffHandle;
					info.offset = 0;
					info.range = (b.m_buff.m_range == MAX_PTR_SIZE) ? VK_WHOLE_SIZE : b.m_buff.m_range;
					break;
				}
				case DescriptorType::READ_TEXTURE_BUFFER:
				case DescriptorType::READ_WRITE_TEXTURE_BUFFER:
				{
					VkBufferView& view = *bufferViews.emplaceBack();
					view = b.m_textureBuffer.m_buffView;
					break;
				}
				case DescriptorType::IMAGE:
				{
					VkDescriptorImageInfo& info = *texInfos.emplaceBack();
					info.sampler = VK_NULL_HANDLE;
					info.imageView = b.m_image.m_imgViewHandle;
					info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
					break;
				}
				case DescriptorType::ACCELERATION_STRUCTURE:
				{
					VkWriteDescriptorSetAccelerationStructureKHR& info = *asInfos.emplaceBack();
					info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
					info.pNext = nullptr;
					info.accelerationStructureCount = 1;
					info.pAccelerationStructures = &b.m_accelerationStructure.m_accelerationStructureHandle;
					break;
				}
				default:
					ANKI_ASSERT(0);
				}
			}
		}
	}

	// Second pass: Populate the VkWriteDescriptorSet with VkDescriptorImageInfo and VkDescriptorBufferInfo
	U32 texCounter = 0;
	U32 buffCounter = 0;
	U32 asCounter = 0;
	U32 buffViewsCounter = 0;

	VkWriteDescriptorSet writeTemplate = {};
	writeTemplate.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writeTemplate.pNext = nullptr;
	writeTemplate.dstSet = set.m_handle;
	writeTemplate.descriptorCount = 1;

	for(U32 bindingIdx = m_layoutEntry->m_minBinding; bindingIdx <= m_layoutEntry->m_maxBinding; ++bindingIdx)
	{
		if(m_layoutEntry->m_activeBindings.get(bindingIdx))
		{
			for(U32 arrIdx = 0; arrIdx < m_layoutEntry->m_bindingArraySize[bindingIdx]; ++arrIdx)
			{
				const AnyBinding& b = (bindings[bindingIdx].m_arraySize == 1) ? bindings[bindingIdx].m_single
																			  : bindings[bindingIdx].m_array[arrIdx];

				VkWriteDescriptorSet& writeInfo = *writeInfos.emplaceBack(writeTemplate);
				writeInfo.descriptorType = convertDescriptorType(b.m_type);
				writeInfo.dstArrayElement = arrIdx;
				writeInfo.dstBinding = bindingIdx;

				switch(b.m_type)
				{
				case DescriptorType::COMBINED_TEXTURE_SAMPLER:
				case DescriptorType::TEXTURE:
				case DescriptorType::SAMPLER:
				case DescriptorType::IMAGE:
					writeInfo.pImageInfo = &texInfos[texCounter++];
					break;
				case DescriptorType::UNIFORM_BUFFER:
				case DescriptorType::STORAGE_BUFFER:
					writeInfo.pBufferInfo = &buffInfos[buffCounter++];
					break;
				case DescriptorType::READ_TEXTURE_BUFFER:
				case DescriptorType::READ_WRITE_TEXTURE_BUFFER:
					writeInfo.pTexelBufferView = &bufferViews[buffViewsCounter++];
					break;
				case DescriptorType::ACCELERATION_STRUCTURE:
					writeInfo.pNext = &asInfos[asCounter++];
					break;
				default:
					ANKI_ASSERT(0);
				}
			}
		}
	}

	// Write
	vkUpdateDescriptorSets(m_layoutEntry->m_factory->m_dev, writeInfos.getSize(),
						   (writeInfos.getSize() > 0) ? &writeInfos[0] : nullptr, 0, nullptr);
}

DSLayoutCacheEntry::~DSLayoutCacheEntry()
{
	auto alloc = m_factory->m_alloc;

	if(m_layoutHandle)
	{
		vkDestroyDescriptorSetLayout(m_factory->m_dev, m_layoutHandle, nullptr);
	}
}

Error DSLayoutCacheEntry::init(const DescriptorBinding* bindings, U32 bindingCount, U64 hash)
{
	ANKI_ASSERT(bindings);
	ANKI_ASSERT(hash > 0);

	m_hash = hash;

	// Create the VK layout
	Array<VkDescriptorSetLayoutBinding, MAX_BINDINGS_PER_DESCRIPTOR_SET> vkBindings;
	VkDescriptorSetLayoutCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;

	for(U i = 0; i < bindingCount; ++i)
	{
		VkDescriptorSetLayoutBinding& vk = vkBindings[i];
		const DescriptorBinding& ak = bindings[i];

		vk.binding = ak.m_binding;
		vk.descriptorCount = ak.m_arraySize;
		vk.descriptorType = convertDescriptorType(ak.m_type);
		vk.pImmutableSamplers = nullptr;
		vk.stageFlags = convertShaderTypeBit(ak.m_stageMask);

		ANKI_ASSERT(m_activeBindings.get(ak.m_binding) == false);
		m_activeBindings.set(ak.m_binding);
		m_bindingType[ak.m_binding] = ak.m_type;
		m_bindingArraySize[ak.m_binding] = ak.m_arraySize;
		m_minBinding = min<U32>(m_minBinding, ak.m_binding);
		m_maxBinding = max<U32>(m_maxBinding, ak.m_binding);
	}

	ci.bindingCount = bindingCount;
	ci.pBindings = &vkBindings[0];

	ANKI_VK_CHECK(vkCreateDescriptorSetLayout(m_factory->m_dev, &ci, nullptr, &m_layoutHandle));

	// Create the pool info
	U32 poolSizeCount = 0;
	for(U i = 0; i < bindingCount; ++i)
	{
		U j;
		for(j = 0; j < poolSizeCount; ++j)
		{
			if(m_poolSizesCreateInf[j].type == convertDescriptorType(bindings[i].m_type))
			{
				m_poolSizesCreateInf[j].descriptorCount += bindings[i].m_arraySize;
				break;
			}
		}

		if(j == poolSizeCount)
		{
			m_poolSizesCreateInf[poolSizeCount].type = convertDescriptorType(bindings[i].m_type);
			m_poolSizesCreateInf[poolSizeCount].descriptorCount = bindings[i].m_arraySize;
			++poolSizeCount;
		}
	}

	if(poolSizeCount == 0)
	{
		// If the poolSizeCount it means that the DS layout has 0 descriptors. Since the pool sizes can't be zero put
		// something in them
		m_poolSizesCreateInf[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		m_poolSizesCreateInf[0].descriptorCount = 1;
		++poolSizeCount;
	}

	ANKI_ASSERT(poolSizeCount > 0);

	m_poolCreateInf.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	m_poolCreateInf.poolSizeCount = poolSizeCount;

	return Error::NONE;
}

Error DSLayoutCacheEntry::getOrCreateDSAllocator(DescriptorSetFactory::DSAllocator*& alloc)
{
	alloc = nullptr;

	// Get or create thread-local
	DescriptorSetFactory::ThreadLocal* threadLocal = DescriptorSetFactory::m_threadLocal;
	if(ANKI_UNLIKELY(threadLocal == nullptr))
	{
		threadLocal = m_factory->m_alloc.newInstance<DescriptorSetFactory::ThreadLocal>();
		DescriptorSetFactory::m_threadLocal = threadLocal;

		LockGuard<Mutex> lock(m_factory->m_allThreadLocalsMtx);
		m_factory->m_allThreadLocals.emplaceBack(m_factory->m_alloc, threadLocal);
	}

	// Get or create the allocator
	if(ANKI_UNLIKELY(m_index >= threadLocal->m_allocators.getSize()))
	{
		threadLocal->m_allocators.resize(m_factory->m_alloc, m_index + 1, nullptr);
		alloc = m_factory->m_alloc.newInstance<DescriptorSetFactory::DSAllocator>(this);
		ANKI_CHECK(alloc->init());
		threadLocal->m_allocators[m_index] = alloc;
	}
	else if(ANKI_UNLIKELY(threadLocal->m_allocators[m_index] == nullptr))
	{
		alloc = m_factory->m_alloc.newInstance<DescriptorSetFactory::DSAllocator>(this);
		ANKI_CHECK(alloc->init());
		threadLocal->m_allocators[m_index] = alloc;
	}
	else
	{
		alloc = threadLocal->m_allocators[m_index];
	}

	ANKI_ASSERT(alloc);
	return Error::NONE;
}

AnyBinding& DescriptorSetState::getBindingToPopulate(U32 bindingIdx, U32 arrayIdx)
{
	ANKI_ASSERT(bindingIdx < MAX_BINDINGS_PER_DESCRIPTOR_SET);

	AnyBindingExtended& extended = m_bindings[bindingIdx];
	AnyBinding* out;
	const Bool bindingIsSet = m_bindingSet.get(bindingIdx);
	m_bindingSet.set(bindingIdx);
	extended.m_arraySize = (!bindingIsSet) ? 0 : extended.m_arraySize;

	if(ANKI_LIKELY(arrayIdx == 0 && extended.m_arraySize <= 1))
	{
		// Array idx is zero, most common case
		out = &extended.m_single;
		extended.m_arraySize = 1;
	}
	else if(arrayIdx < extended.m_arraySize)
	{
		// It's (or was) an array and there enough space in thar array
		out = &extended.m_array[arrayIdx];
	}
	else
	{
		// Need to grow
		const U32 newSize = max(extended.m_arraySize * 2, arrayIdx + 1);
		AnyBinding* newArr = m_alloc.newArray<AnyBinding>(newSize);

		if(extended.m_arraySize == 1)
		{
			newArr[0] = extended.m_single;
		}
		else if(extended.m_arraySize > 1)
		{
			// Copy old to new.
			memcpy(newArr, extended.m_array, sizeof(AnyBinding) * extended.m_arraySize);
		}

		// Zero the rest
		memset(newArr + extended.m_arraySize, 0, sizeof(AnyBinding) * (newSize - extended.m_arraySize));
		extended.m_arraySize = newSize;
		extended.m_array = newArr;

		// Return
		out = &extended.m_array[arrayIdx];
	}

	ANKI_ASSERT(out);
	return *out;
}

void DescriptorSetState::flush(U64& hash, Array<PtrSize, MAX_BINDINGS_PER_DESCRIPTOR_SET>& dynamicOffsets,
							   U32& dynamicOffsetCount, Bool& bindlessDSet)
{
	// Set some values
	hash = 0;
	dynamicOffsetCount = 0;
	bindlessDSet = false;

	// There is a chance where the bindless set is bound but the actual shaders have an empty DS layout (maybe because
	// the dead code elimination eliminated the bindless set). In that case we can't bind the bindless DS. We have to
	// treat it as regular set
	ANKI_ASSERT(!(m_layout.m_entry == nullptr && !m_bindlessDSetBound)
				&& "DS layout points to bindless but no bindless is bound");
	const Bool reallyBindless = m_bindlessDSetBound && m_layout.m_entry == nullptr;

	if(!reallyBindless)
	{
		// Get cache entry
		ANKI_ASSERT(m_layout.m_entry);
		const DSLayoutCacheEntry& entry = *m_layout.m_entry;

		// Early out if nothing happened
		const Bool anyActiveBindingDirty = !!(entry.m_activeBindings & m_dirtyBindings);
		if(!anyActiveBindingDirty && !m_layoutDirty)
		{
			return;
		}

		Bool dynamicOffsetsDirty = false;

		// Compute the hash
		Array<U64, MAX_BINDINGS_PER_DESCRIPTOR_SET * 2 * 2> toHash;
		U toHashCount = 0;

		const U minBinding = entry.m_minBinding;
		const U maxBinding = entry.m_maxBinding;
		for(U i = minBinding; i <= maxBinding; ++i)
		{
			if(entry.m_activeBindings.get(i))
			{
				ANKI_ASSERT(m_bindingSet.get(i) && "Forgot to bind");
				ANKI_ASSERT(m_bindings[i].m_arraySize >= entry.m_bindingArraySize[i] && "Bound less");

				const Bool crntBindingDirty = m_dirtyBindings.get(i);
				m_dirtyBindings.unset(i);

				for(U arrIdx = 0; arrIdx < entry.m_bindingArraySize[i]; ++arrIdx)
				{
					ANKI_ASSERT(arrIdx < m_bindings[i].m_arraySize);
					if(arrIdx > 1)
					{
						ANKI_ASSERT(m_bindings[i].m_array[arrIdx].m_type == m_bindings[i].m_array[arrIdx - 1].m_type);
					}

					const AnyBinding& anyBinding =
						(m_bindings[i].m_arraySize == 1) ? m_bindings[i].m_single : m_bindings[i].m_array[arrIdx];

					ANKI_ASSERT(anyBinding.m_uuids[0] != 0 && "Forgot to bind");

					toHash[toHashCount++] = anyBinding.m_uuids[0];

					switch(entry.m_bindingType[i])
					{
					case DescriptorType::COMBINED_TEXTURE_SAMPLER:
						ANKI_ASSERT(anyBinding.m_type == DescriptorType::COMBINED_TEXTURE_SAMPLER
									&& "Have bound the wrong type");
						toHash[toHashCount++] = anyBinding.m_uuids[1];
						toHash[toHashCount++] = U64(anyBinding.m_texAndSampler.m_layout);
						break;
					case DescriptorType::TEXTURE:
						ANKI_ASSERT(anyBinding.m_type == DescriptorType::TEXTURE && "Have bound the wrong type");
						toHash[toHashCount++] = U64(anyBinding.m_tex.m_layout);
						break;
					case DescriptorType::SAMPLER:
						ANKI_ASSERT(anyBinding.m_type == DescriptorType::SAMPLER && "Have bound the wrong type");
						break;
					case DescriptorType::UNIFORM_BUFFER:
						ANKI_ASSERT(anyBinding.m_type == DescriptorType::UNIFORM_BUFFER && "Have bound the wrong type");
						toHash[toHashCount++] = anyBinding.m_buff.m_range;
						dynamicOffsets[dynamicOffsetCount++] = anyBinding.m_buff.m_offset;
						dynamicOffsetsDirty = dynamicOffsetsDirty || crntBindingDirty;
						break;
					case DescriptorType::STORAGE_BUFFER:
						ANKI_ASSERT(anyBinding.m_type == DescriptorType::STORAGE_BUFFER && "Have bound the wrong type");
						toHash[toHashCount++] = anyBinding.m_buff.m_range;
						dynamicOffsets[dynamicOffsetCount++] = anyBinding.m_buff.m_offset;
						dynamicOffsetsDirty = dynamicOffsetsDirty || crntBindingDirty;
						break;
					case DescriptorType::READ_TEXTURE_BUFFER:
						ANKI_ASSERT(anyBinding.m_type == DescriptorType::READ_TEXTURE_BUFFER
									&& "Have bound the wrong type");
						toHash[toHashCount++] = anyBinding.m_uuids[1];
						break;
					case DescriptorType::READ_WRITE_TEXTURE_BUFFER:
						ANKI_ASSERT(anyBinding.m_type == DescriptorType::READ_WRITE_TEXTURE_BUFFER
									&& "Have bound the wrong type");
						toHash[toHashCount++] = anyBinding.m_uuids[1];
						break;
					case DescriptorType::IMAGE:
						ANKI_ASSERT(anyBinding.m_type == DescriptorType::IMAGE && "Have bound the wrong type");
						break;
					case DescriptorType::ACCELERATION_STRUCTURE:
						ANKI_ASSERT(anyBinding.m_type == DescriptorType::ACCELERATION_STRUCTURE
									&& "Have bound the wrong type");
						break;
					default:
						ANKI_ASSERT(0);
					}
				}
			}
		}

		const U64 newHash = computeHash(&toHash[0], toHashCount * sizeof(U64));

		if(newHash != m_lastHash || dynamicOffsetsDirty || m_layoutDirty)
		{
			// DS needs rebind
			m_lastHash = newHash;
			hash = newHash;
		}
		else
		{
			// All clean, keep hash equal to 0
		}

		m_layoutDirty = false;
	}
	else
	{
		// Custom set

		if(!m_bindlessDSetDirty && !m_layoutDirty)
		{
			return;
		}

		bindlessDSet = true;
		hash = 1;
		m_bindlessDSetDirty = false;
		m_layoutDirty = false;
	}
}

DescriptorSetFactory::~DescriptorSetFactory()
{
}

Error DescriptorSetFactory::init(const GrAllocator<U8>& alloc, VkDevice dev, U32 bindlessTextureCount,
								 U32 bindlessTextureBuffers)
{
	m_alloc = alloc;
	m_dev = dev;

	m_bindless = m_alloc.newInstance<BindlessDescriptorSet>();
	ANKI_CHECK(m_bindless->init(alloc, dev, bindlessTextureCount, bindlessTextureBuffers));
	m_bindlessTextureCount = bindlessTextureCount;
	m_bindlessUniformTexelBufferCount = bindlessTextureBuffers;

	return Error::NONE;
}

void DescriptorSetFactory::destroy()
{
	for(ThreadLocal* threadLocal : m_allThreadLocals)
	{
		for(DSAllocator* alloc : threadLocal->m_allocators)
		{
			m_alloc.deleteInstance(alloc);
		}

		threadLocal->m_allocators.destroy(m_alloc);
		m_alloc.deleteInstance(threadLocal);
	}

	m_allThreadLocals.destroy(m_alloc);

	for(DSLayoutCacheEntry* l : m_caches)
	{
		m_alloc.deleteInstance(l);
	}

	m_caches.destroy(m_alloc);

	if(m_bindless)
	{
		m_alloc.deleteInstance(m_bindless);
	}
}

Error DescriptorSetFactory::newDescriptorSetLayout(const DescriptorSetLayoutInitInfo& init, DescriptorSetLayout& layout)
{
	// Compute the hash for the layout
	Array<DescriptorBinding, MAX_BINDINGS_PER_DESCRIPTOR_SET> bindings;
	const U32 bindingCount = init.m_bindings.getSize();
	U64 hash;

	if(init.m_bindings.getSize() > 0)
	{
		memcpy(bindings.getBegin(), init.m_bindings.getBegin(), init.m_bindings.getSizeInBytes());
		std::sort(bindings.getBegin(), bindings.getBegin() + bindingCount,
				  [](const DescriptorBinding& a, const DescriptorBinding& b) {
					  return a.m_binding < b.m_binding;
				  });

		hash = computeHash(&bindings[0], init.m_bindings.getSizeInBytes());
		ANKI_ASSERT(hash != 1);
	}
	else
	{
		hash = 1;
	}

	// Identify if the DS is the bindless one. It is if there is at least one binding that matches the criteria
	Bool isBindless = false;
	if(bindingCount > 0)
	{
		isBindless = true;
		for(U32 i = 0; i < bindingCount; ++i)
		{
			const DescriptorBinding& binding = bindings[i];
			if(binding.m_binding == 0 && binding.m_type == DescriptorType::TEXTURE
			   && binding.m_arraySize == m_bindlessTextureCount)
			{
				// All good
			}
			else if(binding.m_binding == 1 && binding.m_type == DescriptorType::READ_TEXTURE_BUFFER
					&& binding.m_arraySize == m_bindlessUniformTexelBufferCount)
			{
				// All good
			}
			else
			{
				isBindless = false;
			}
		}
	}

	// Find or create the cache entry
	if(isBindless)
	{
		layout.m_handle = m_bindless->getDescriptorSetLayout();
		layout.m_entry = nullptr;
	}
	else
	{
		LockGuard<SpinLock> lock(m_cachesMtx);

		DSLayoutCacheEntry* cache = nullptr;
		U count = 0;
		for(DSLayoutCacheEntry* it : m_caches)
		{
			if(it->m_hash == hash)
			{
				cache = it;
				break;
			}
			++count;
		}

		if(cache == nullptr)
		{
			cache = m_alloc.newInstance<DSLayoutCacheEntry>(this, m_caches.getSize());
			ANKI_CHECK(cache->init(bindings.getBegin(), bindingCount, hash));

			m_caches.emplaceBack(m_alloc, cache);
		}

		// Set the layout
		layout.m_handle = cache->m_layoutHandle;
		layout.m_entry = cache;
	}

	return Error::NONE;
}

Error DescriptorSetFactory::newDescriptorSet(StackAllocator<U8>& tmpAlloc, DescriptorSetState& state,
											 DescriptorSet& set, Bool& dirty,
											 Array<PtrSize, MAX_BINDINGS_PER_DESCRIPTOR_SET>& dynamicOffsets,
											 U32& dynamicOffsetCount)
{
	ANKI_TRACE_SCOPED_EVENT(VK_DESCRIPTOR_SET_GET_OR_CREATE);

	U64 hash;
	Bool bindlessDSet;
	state.flush(hash, dynamicOffsets, dynamicOffsetCount, bindlessDSet);

	if(hash == 0)
	{
		dirty = false;
		return Error::NONE;
	}
	else
	{
		dirty = true;

		if(!bindlessDSet)
		{
			DescriptorSetLayout layout = state.m_layout;
			DSLayoutCacheEntry& entry = *layout.m_entry;

			// Get thread allocator
			DSAllocator* alloc;
			ANKI_CHECK(entry.getOrCreateDSAllocator(alloc));

			// Finally, allocate
			const DS* s;
			ANKI_CHECK(alloc->getOrCreateSet(hash, state.m_bindings, tmpAlloc, s));
			set.m_handle = s->m_handle;
			ANKI_ASSERT(set.m_handle != VK_NULL_HANDLE);
		}
		else
		{
			set = m_bindless->getDescriptorSet();
		}
	}

	return Error::NONE;
}

U32 DescriptorSetFactory::bindBindlessTexture(const VkImageView view, const VkImageLayout layout)
{
	ANKI_ASSERT(m_bindless);
	return m_bindless->bindTexture(view, layout);
}

U32 DescriptorSetFactory::bindBindlessUniformTexelBuffer(const VkBufferView view)
{
	ANKI_ASSERT(m_bindless);
	return m_bindless->bindUniformTexelBuffer(view);
}

void DescriptorSetFactory::unbindBindlessTexture(U32 idx)
{
	ANKI_ASSERT(m_bindless);
	m_bindless->unbindTexture(idx);
}

void DescriptorSetFactory::unbindBindlessUniformTexelBuffer(U32 idx)
{
	ANKI_ASSERT(m_bindless);
	m_bindless->unbindUniformTexelBuffer(idx);
}

} // end namespace anki
