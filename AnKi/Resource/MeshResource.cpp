// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Resource/MeshResource.h>
#include <AnKi/Resource/ResourceManager.h>
#include <AnKi/Resource/MeshBinaryLoader.h>
#include <AnKi/Resource/AsyncLoader.h>
#include <AnKi/Core/GpuMemoryPools.h>
#include <AnKi/Util/Functions.h>
#include <AnKi/Util/Filesystem.h>

namespace anki {

class MeshResource::LoadContext
{
public:
	MeshResourcePtr m_mesh;
	MeshBinaryLoader m_loader;

	LoadContext(const MeshResourcePtr& mesh, GenericMemoryPoolAllocator<U8> alloc)
		: m_mesh(mesh)
		, m_loader(&mesh->getManager(), alloc)
	{
	}
};

/// Mesh upload async task.
class MeshResource::LoadTask : public AsyncLoaderTask
{
public:
	MeshResource::LoadContext m_ctx;

	LoadTask(const MeshResourcePtr& mesh)
		: m_ctx(mesh, mesh->getManager().getAsyncLoader().getAllocator())
	{
	}

	Error operator()([[maybe_unused]] AsyncLoaderTaskContext& ctx) final
	{
		return m_ctx.m_mesh->loadAsync(m_ctx.m_loader);
	}

	GenericMemoryPoolAllocator<U8> getAllocator() const
	{
		return m_ctx.m_mesh->getManager().getAsyncLoader().getAllocator();
	}
};

MeshResource::MeshResource(ResourceManager* manager)
	: ResourceObject(manager)
{
	memset(&m_meshGpuDescriptor, 0, sizeof(m_meshGpuDescriptor));
}

MeshResource::~MeshResource()
{
	m_subMeshes.destroy(getAllocator());
	m_vertexBufferInfos.destroy(getAllocator());

	if(m_vertexBuffersOffset != MAX_PTR_SIZE)
	{
		getManager().getVertexGpuMemory().free(m_vertexBuffersSize, m_vertexBuffersOffset);
	}

	if(m_indexBufferOffset != MAX_PTR_SIZE)
	{
		const PtrSize indexBufferSize = PtrSize(m_indexCount) * ((m_indexType == IndexType::U32) ? 4 : 2);
		getManager().getVertexGpuMemory().free(indexBufferSize, m_indexBufferOffset);
	}
}

Bool MeshResource::isCompatible(const MeshResource& other) const
{
	return hasBoneWeights() == other.hasBoneWeights() && getSubMeshCount() == other.getSubMeshCount();
}

Error MeshResource::load(const ResourceFilename& filename, Bool async)
{
	UniquePtr<LoadTask> task;
	LoadContext* ctx;
	LoadContext localCtx(MeshResourcePtr(this), getTempAllocator());

	StringAuto basename(getTempAllocator());
	getFilepathFilename(filename, basename);

	const Bool rayTracingEnabled = getManager().getGrManager().getDeviceCapabilities().m_rayTracingEnabled;

	if(async)
	{
		task.reset(getManager().getAsyncLoader().newTask<LoadTask>(MeshResourcePtr(this)));
		ctx = &task->m_ctx;
	}
	else
	{
		task.reset(nullptr);
		ctx = &localCtx;
	}

	// Open file
	MeshBinaryLoader& loader = ctx->m_loader;
	ANKI_CHECK(loader.load(filename));
	const MeshBinaryHeader& header = loader.getHeader();

	//
	// Submeshes
	//
	m_subMeshes.create(getAllocator(), header.m_subMeshCount);
	for(U32 i = 0; i < m_subMeshes.getSize(); ++i)
	{
		m_subMeshes[i].m_firstIndex = loader.getSubMeshes()[i].m_firstIndex;
		m_subMeshes[i].m_indexCount = loader.getSubMeshes()[i].m_indexCount;
		m_subMeshes[i].m_aabb.setMin(loader.getSubMeshes()[i].m_aabbMin);
		m_subMeshes[i].m_aabb.setMax(loader.getSubMeshes()[i].m_aabbMax);
	}

	//
	// Index stuff
	//
	m_indexCount = header.m_totalIndexCount;
	ANKI_ASSERT((m_indexCount % 3) == 0 && "Expecting triangles");
	m_indexType = header.m_indexType;

	const PtrSize indexBufferSize = PtrSize(m_indexCount) * ((m_indexType == IndexType::U32) ? 4 : 2);
	ANKI_CHECK(getManager().getVertexGpuMemory().allocate(indexBufferSize, m_indexBufferOffset));

	//
	// Vertex stuff
	//
	m_vertexCount = header.m_totalVertexCount;
	m_vertexBufferInfos.create(getAllocator(), header.m_vertexBufferCount);

	m_vertexBuffersSize = 0;
	for(U32 i = 0; i < header.m_vertexBufferCount; ++i)
	{
		alignRoundUp(MESH_BINARY_BUFFER_ALIGNMENT, m_vertexBuffersSize);

		m_vertexBufferInfos[i].m_offset = m_vertexBuffersSize;
		m_vertexBufferInfos[i].m_stride = header.m_vertexBuffers[i].m_vertexStride;

		m_vertexBuffersSize += m_vertexCount * m_vertexBufferInfos[i].m_stride;
	}

	ANKI_CHECK(getManager().getVertexGpuMemory().allocate(m_vertexBuffersSize, m_vertexBuffersOffset));

	// Readjust the individual offset now that we have a global offset
	for(U32 i = 0; i < header.m_vertexBufferCount; ++i)
	{
		m_vertexBufferInfos[i].m_offset += m_vertexBuffersOffset;
	}

	for(VertexAttributeId attrib = VertexAttributeId::FIRST; attrib < VertexAttributeId::COUNT; ++attrib)
	{
		AttribInfo& out = m_attributes[attrib];
		const MeshBinaryVertexAttribute& in = header.m_vertexAttributes[attrib];

		if(!!in.m_format)
		{
			out.m_format = in.m_format;
			out.m_relativeOffset = in.m_relativeOffset;
			out.m_buffIdx = U8(in.m_bufferBinding);
			ANKI_ASSERT(in.m_scale == 1.0f && "Not supported ATM");
		}
	}

	// Other
	m_aabb.setMin(header.m_aabbMin);
	m_aabb.setMax(header.m_aabbMax);
	m_vertexBuffer = getManager().getVertexGpuMemory().getVertexBuffer();

	//
	// Clear the buffers
	//
	if(async)
	{
		CommandBufferInitInfo cmdbinit;
		cmdbinit.m_flags = CommandBufferFlag::SMALL_BATCH | CommandBufferFlag::GENERAL_WORK;
		CommandBufferPtr cmdb = getManager().getGrManager().newCommandBuffer(cmdbinit);

		cmdb->fillBuffer(m_vertexBuffer, m_vertexBuffersOffset, m_vertexBuffersSize, 0);
		cmdb->fillBuffer(m_vertexBuffer, m_indexBufferOffset, indexBufferSize, 0);

		cmdb->setBufferBarrier(m_vertexBuffer, BufferUsageBit::TRANSFER_DESTINATION, BufferUsageBit::VERTEX, 0,
							   MAX_PTR_SIZE);

		cmdb->flush();
	}

	//
	// Create the BLAS
	//
	if(rayTracingEnabled)
	{
		AccelerationStructureInitInfo inf(StringAuto(getTempAllocator()).sprintf("%s_%s", "Blas", basename.cstr()));
		inf.m_type = AccelerationStructureType::BOTTOM_LEVEL;

		inf.m_bottomLevel.m_indexBuffer = m_vertexBuffer;
		inf.m_bottomLevel.m_indexBufferOffset = m_indexBufferOffset;
		inf.m_bottomLevel.m_indexCount = m_indexCount;
		inf.m_bottomLevel.m_indexType = m_indexType;

		U32 bufferIdx;
		Format format;
		U32 relativeOffset;
		getVertexAttributeInfo(VertexAttributeId::POSITION, bufferIdx, format, relativeOffset);

		BufferPtr buffer;
		PtrSize offset;
		PtrSize stride;
		getVertexBufferInfo(bufferIdx, buffer, offset, stride);

		inf.m_bottomLevel.m_positionBuffer = buffer;
		inf.m_bottomLevel.m_positionBufferOffset = offset;
		inf.m_bottomLevel.m_positionStride = U32(stride);
		inf.m_bottomLevel.m_positionsFormat = format;
		inf.m_bottomLevel.m_positionCount = m_vertexCount;

		m_blas = getManager().getGrManager().newAccelerationStructure(inf);
	}

	// Fill the GPU descriptor
	if(rayTracingEnabled)
	{
		m_meshGpuDescriptor.m_indexBufferPtr = m_vertexBuffer->getGpuAddress() + m_indexBufferOffset;

		U32 bufferIdx;
		Format format;
		U32 relativeOffset;
		getVertexAttributeInfo(VertexAttributeId::POSITION, bufferIdx, format, relativeOffset);
		BufferPtr buffer;
		PtrSize offset;
		PtrSize stride;
		getVertexBufferInfo(bufferIdx, buffer, offset, stride);
		m_meshGpuDescriptor.m_vertexBufferPtrs[VertexAttributeBufferId::POSITION] = buffer->getGpuAddress() + offset;

		getVertexAttributeInfo(VertexAttributeId::NORMAL, bufferIdx, format, relativeOffset);
		getVertexBufferInfo(bufferIdx, buffer, offset, stride);
		m_meshGpuDescriptor.m_vertexBufferPtrs[VertexAttributeBufferId::NORMAL_TANGENT_UV0] =
			buffer->getGpuAddress() + offset;

		if(hasBoneWeights())
		{
			getVertexAttributeInfo(VertexAttributeId::BONE_WEIGHTS, bufferIdx, format, relativeOffset);
			getVertexBufferInfo(bufferIdx, buffer, offset, stride);
			m_meshGpuDescriptor.m_vertexBufferPtrs[VertexAttributeBufferId::BONE] = buffer->getGpuAddress() + offset;
		}

		m_meshGpuDescriptor.m_indexCount = m_indexCount;
		m_meshGpuDescriptor.m_vertexCount = m_vertexCount;
		m_meshGpuDescriptor.m_aabbMin = header.m_aabbMin;
		m_meshGpuDescriptor.m_aabbMax = header.m_aabbMax;
	}

	// Submit the loading task
	if(async)
	{
		getManager().getAsyncLoader().submitTask(task.get());
		LoadTask* pTask;
		task.moveAndReset(pTask);
	}
	else
	{
		ANKI_CHECK(loadAsync(loader));
	}

	return Error::NONE;
}

Error MeshResource::loadAsync(MeshBinaryLoader& loader) const
{
	GrManager& gr = getManager().getGrManager();
	TransferGpuAllocator& transferAlloc = getManager().getTransferGpuAllocator();
	Array<TransferGpuAllocatorHandle, 2> handles;

	CommandBufferInitInfo cmdbinit;
	cmdbinit.m_flags = CommandBufferFlag::SMALL_BATCH | CommandBufferFlag::GENERAL_WORK;
	CommandBufferPtr cmdb = gr.newCommandBuffer(cmdbinit);

	// Set barriers
	cmdb->setBufferBarrier(m_vertexBuffer, BufferUsageBit::VERTEX, BufferUsageBit::TRANSFER_DESTINATION, 0,
						   MAX_PTR_SIZE);

	// Write index buffer
	{
		const PtrSize indexBufferSize = PtrSize(m_indexCount) * ((m_indexType == IndexType::U32) ? 4 : 2);

		ANKI_CHECK(transferAlloc.allocate(indexBufferSize, handles[1]));
		void* data = handles[1].getMappedMemory();
		ANKI_ASSERT(data);

		ANKI_CHECK(loader.storeIndexBuffer(data, indexBufferSize));

		cmdb->copyBufferToBuffer(handles[1].getBuffer(), handles[1].getOffset(), m_vertexBuffer, m_indexBufferOffset,
								 handles[1].getRange());
	}

	// Write vert buff
	{
		ANKI_CHECK(transferAlloc.allocate(m_vertexBuffersSize, handles[0]));
		U8* data = static_cast<U8*>(handles[0].getMappedMemory());
		ANKI_ASSERT(data);

		// Load to staging
		PtrSize offset = 0;
		for(U32 i = 0; i < m_vertexBufferInfos.getSize(); ++i)
		{
			alignRoundUp(MESH_BINARY_BUFFER_ALIGNMENT, offset);
			ANKI_CHECK(
				loader.storeVertexBuffer(i, data + offset, PtrSize(m_vertexBufferInfos[i].m_stride) * m_vertexCount));

			offset += PtrSize(m_vertexBufferInfos[i].m_stride) * m_vertexCount;
		}

		ANKI_ASSERT(offset == m_vertexBuffersSize);

		// Copy
		cmdb->copyBufferToBuffer(handles[0].getBuffer(), handles[0].getOffset(), m_vertexBuffer, m_vertexBuffersOffset,
								 handles[0].getRange());
	}

	// Build the BLAS
	if(gr.getDeviceCapabilities().m_rayTracingEnabled)
	{
		cmdb->setBufferBarrier(m_vertexBuffer, BufferUsageBit::TRANSFER_DESTINATION,
							   BufferUsageBit::ACCELERATION_STRUCTURE_BUILD | BufferUsageBit::VERTEX
								   | BufferUsageBit::INDEX,
							   0, MAX_PTR_SIZE);

		cmdb->setAccelerationStructureBarrier(m_blas, AccelerationStructureUsageBit::NONE,
											  AccelerationStructureUsageBit::BUILD);

		cmdb->buildAccelerationStructure(m_blas);

		cmdb->setAccelerationStructureBarrier(m_blas, AccelerationStructureUsageBit::BUILD,
											  AccelerationStructureUsageBit::ALL_READ);
	}
	else
	{
		cmdb->setBufferBarrier(m_vertexBuffer, BufferUsageBit::TRANSFER_DESTINATION,
							   BufferUsageBit::VERTEX | BufferUsageBit::INDEX, 0, MAX_PTR_SIZE);
	}

	// Finalize
	FencePtr fence;
	cmdb->flush({}, &fence);

	transferAlloc.release(handles[0], fence);
	transferAlloc.release(handles[1], fence);

	return Error::NONE;
}

} // end namespace anki
