// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Resource/TransferGpuAllocator.h>
#include <AnKi/Util/List.h>
#include <AnKi/Util/Functions.h>
#include <AnKi/Util/String.h>

namespace anki {

// Forward
class ConfigSet;
class GrManager;
class PhysicsWorld;
class ResourceManager;
class AsyncLoader;
class ResourceManagerModel;
class ShaderCompilerCache;
class ShaderProgramResourceSystem;
class VertexGpuMemoryPool;

/// @addtogroup resource
/// @{

/// Manage resources of a certain type
template<typename Type>
class TypeResourceManager
{
protected:
	TypeResourceManager()
	{
	}

	~TypeResourceManager()
	{
		ANKI_ASSERT(m_ptrs.isEmpty() && "Forgot to delete some resources");
		m_ptrs.destroy(m_alloc);
	}

	Type* findLoadedResource(const CString& filename)
	{
		auto it = find(filename);
		return (it != m_ptrs.end()) ? *it : nullptr;
	}

	void registerResource(Type* ptr)
	{
		ANKI_ASSERT(find(ptr->getFilename()) == m_ptrs.getEnd());
		m_ptrs.pushBack(m_alloc, ptr);
	}

	void unregisterResource(Type* ptr)
	{
		auto it = find(ptr->getFilename());
		ANKI_ASSERT(it != m_ptrs.end());
		m_ptrs.erase(m_alloc, it);
	}

	void init(ResourceAllocator<U8> alloc)
	{
		m_alloc = alloc;
	}

private:
	using Container = List<Type*>;

	ResourceAllocator<U8> m_alloc;
	Container m_ptrs;

	typename Container::Iterator find(const CString& filename)
	{
		typename Container::Iterator it;

		for(it = m_ptrs.getBegin(); it != m_ptrs.getEnd(); ++it)
		{
			if((*it)->getFilename() == filename)
			{
				break;
			}
		}

		return it;
	}
};

class ResourceManagerInitInfo
{
public:
	GrManager* m_gr = nullptr;
	PhysicsWorld* m_physics = nullptr;
	ResourceFilesystem* m_resourceFs = nullptr;
	ConfigSet* m_config = nullptr;
	VertexGpuMemoryPool* m_vertexMemory = nullptr;
	AllocAlignedCallback m_allocCallback = 0;
	void* m_allocCallbackData = nullptr;
};

/// Resource manager. It holds a few global variables
class ResourceManager:

#define ANKI_INSTANTIATE_RESOURCE(rsrc_, ptr_) \
public \
	TypeResourceManager<rsrc_>

#define ANKI_INSTANSIATE_RESOURCE_DELIMITER() ,

#include <AnKi/Resource/InstantiationMacros.h>
#undef ANKI_INSTANTIATE_RESOURCE
#undef ANKI_INSTANSIATE_RESOURCE_DELIMITER

{
	template<typename T>
	friend class ResourcePtrDeleter;

public:
	ResourceManager();

	~ResourceManager();

	Error init(ResourceManagerInitInfo& init);

	/// Load a resource.
	template<typename T>
	Error loadResource(const CString& filename, ResourcePtr<T>& out, Bool async = true);

	// Internals:

	ANKI_INTERNAL ResourceAllocator<U8>& getAllocator()
	{
		return m_alloc;
	}

	ANKI_INTERNAL TempResourceAllocator<U8>& getTempAllocator()
	{
		return m_tmpAlloc;
	}

	ANKI_INTERNAL GrManager& getGrManager()
	{
		ANKI_ASSERT(m_gr);
		return *m_gr;
	}

	ANKI_INTERNAL TransferGpuAllocator& getTransferGpuAllocator()
	{
		return *m_transferGpuAlloc;
	}

	ANKI_INTERNAL PhysicsWorld& getPhysicsWorld()
	{
		ANKI_ASSERT(m_physics);
		return *m_physics;
	}

	ANKI_INTERNAL ResourceFilesystem& getFilesystem()
	{
		ANKI_ASSERT(m_fs);
		return *m_fs;
	}

	template<typename T>
	ANKI_INTERNAL T* findLoadedResource(const CString& filename)
	{
		return TypeResourceManager<T>::findLoadedResource(filename);
	}

	template<typename T>
	ANKI_INTERNAL void registerResource(T* ptr)
	{
		TypeResourceManager<T>::registerResource(ptr);
	}

	template<typename T>
	ANKI_INTERNAL void unregisterResource(T* ptr)
	{
		TypeResourceManager<T>::unregisterResource(ptr);
	}

	ANKI_INTERNAL AsyncLoader& getAsyncLoader()
	{
		return *m_asyncLoader;
	}

	/// Get the number of times loadResource() was called.
	ANKI_INTERNAL U64 getLoadingRequestCount() const
	{
		return m_loadRequestCount;
	}

	/// Get the total number of completed async tasks.
	ANKI_INTERNAL U64 getAsyncTaskCompletedCount() const;

	/// Return the container of program libraries.
	const ShaderProgramResourceSystem& getShaderProgramResourceSystem() const
	{
		return *m_shaderProgramSystem;
	}

	VertexGpuMemoryPool& getVertexGpuMemory()
	{
		ANKI_ASSERT(m_vertexMem);
		return *m_vertexMem;
	}

	const ConfigSet& getConfig() const
	{
		ANKI_ASSERT(m_config);
		return *m_config;
	}

private:
	GrManager* m_gr = nullptr;
	PhysicsWorld* m_physics = nullptr;
	ResourceFilesystem* m_fs = nullptr;
	ConfigSet* m_config = nullptr;
	ResourceAllocator<U8> m_alloc;
	TempResourceAllocator<U8> m_tmpAlloc;
	AsyncLoader* m_asyncLoader = nullptr; ///< Async loading thread
	ShaderProgramResourceSystem* m_shaderProgramSystem = nullptr;
	VertexGpuMemoryPool* m_vertexMem = nullptr;
	U64 m_uuid = 0;
	U64 m_loadRequestCount = 0;
	TransferGpuAllocator* m_transferGpuAlloc = nullptr;
};
/// @}

} // end namespace anki
