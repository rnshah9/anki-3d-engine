// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Resource/ImageResource.h>
#include <AnKi/Resource/ImageLoader.h>
#include <AnKi/Resource/ResourceManager.h>
#include <AnKi/Resource/AsyncLoader.h>
#include <AnKi/Core/ConfigSet.h>
#include <AnKi/Util/Filesystem.h>

namespace anki {

class ImageResource::LoadingContext
{
public:
	ImageLoader m_loader;
	U32 m_faces = 0;
	U32 m_layerCount = 0;
	GrManager* m_gr ANKI_DEBUG_CODE(= nullptr);
	TransferGpuAllocator* m_trfAlloc ANKI_DEBUG_CODE(= nullptr);
	TextureType m_texType;
	TexturePtr m_tex;

	LoadingContext(GenericMemoryPoolAllocator<U8> alloc)
		: m_loader(alloc)
	{
	}
};

/// Image upload async task.
class ImageResource::TexUploadTask : public AsyncLoaderTask
{
public:
	ImageResource::LoadingContext m_ctx;

	TexUploadTask(GenericMemoryPoolAllocator<U8> alloc)
		: m_ctx(alloc)
	{
	}

	Error operator()([[maybe_unused]] AsyncLoaderTaskContext& ctx) final
	{
		return ImageResource::load(m_ctx);
	}
};

ImageResource::~ImageResource()
{
}

Error ImageResource::load(const ResourceFilename& filename, Bool async)
{
	TexUploadTask* task;
	LoadingContext* ctx;
	LoadingContext localCtx(getTempAllocator());

	if(async)
	{
		task = getManager().getAsyncLoader().newTask<TexUploadTask>(getManager().getAsyncLoader().getAllocator());
		ctx = &task->m_ctx;
	}
	else
	{
		task = nullptr;
		ctx = &localCtx;
	}
	ImageLoader& loader = ctx->m_loader;

	StringAuto filenameExt(getTempAllocator());
	getFilepathFilename(filename, filenameExt);

	TextureInitInfo init(filenameExt);
	init.m_usage = TextureUsageBit::ALL_SAMPLED | TextureUsageBit::TRANSFER_DESTINATION;
	U32 faces = 0;

	ResourceFilePtr file;
	ANKI_CHECK(openFile(filename, file));

	ANKI_CHECK(loader.load(file, filename, getConfig().getRsrcMaxImageSize()));

	// Various sizes
	init.m_width = loader.getWidth();
	init.m_height = loader.getHeight();

	switch(loader.getImageType())
	{
	case ImageBinaryType::_2D:
		init.m_type = TextureType::_2D;
		init.m_depth = 1;
		faces = 1;
		init.m_layerCount = 1;
		break;
	case ImageBinaryType::CUBE:
		init.m_type = TextureType::CUBE;
		init.m_depth = 1;
		faces = 6;
		init.m_layerCount = 1;
		break;
	case ImageBinaryType::_2D_ARRAY:
		init.m_type = TextureType::_2D_ARRAY;
		init.m_layerCount = loader.getLayerCount();
		init.m_depth = 1;
		faces = 1;
		break;
	case ImageBinaryType::_3D:
		init.m_type = TextureType::_3D;
		init.m_depth = loader.getDepth();
		init.m_layerCount = 1;
		faces = 1;
		break;
	default:
		ANKI_ASSERT(0);
	}

	// Internal format
	if(loader.getColorFormat() == ImageBinaryColorFormat::RGB8)
	{
		switch(loader.getCompression())
		{
		case ImageBinaryDataCompression::RAW:
			init.m_format = Format::R8G8B8_UNORM;
			break;
		case ImageBinaryDataCompression::S3TC:
			init.m_format = Format::BC1_RGB_UNORM_BLOCK;
			break;
		case ImageBinaryDataCompression::ASTC:
			if(loader.getAstcBlockSize() == UVec2(4u))
			{
				init.m_format = Format::ASTC_4x4_UNORM_BLOCK;
			}
			else
			{
				ANKI_ASSERT(loader.getAstcBlockSize() == UVec2(8u));
				init.m_format = Format::ASTC_8x8_UNORM_BLOCK;
			}
			break;
		default:
			ANKI_ASSERT(0);
		}
	}
	else if(loader.getColorFormat() == ImageBinaryColorFormat::RGBA8)
	{
		switch(loader.getCompression())
		{
		case ImageBinaryDataCompression::RAW:
			init.m_format = Format::R8G8B8A8_UNORM;
			break;
		case ImageBinaryDataCompression::S3TC:
			init.m_format = Format::BC3_UNORM_BLOCK;
			break;
		case ImageBinaryDataCompression::ASTC:
			if(loader.getAstcBlockSize() == UVec2(4u))
			{
				init.m_format = Format::ASTC_4x4_UNORM_BLOCK;
			}
			else
			{
				ANKI_ASSERT(loader.getAstcBlockSize() == UVec2(8u));
				init.m_format = Format::ASTC_8x8_UNORM_BLOCK;
			}
			break;
		default:
			ANKI_ASSERT(0);
		}
	}
	else if(loader.getColorFormat() == ImageBinaryColorFormat::RGBF32)
	{
		switch(loader.getCompression())
		{
		case ImageBinaryDataCompression::S3TC:
			init.m_format = Format::BC6H_UFLOAT_BLOCK;
			break;
		case ImageBinaryDataCompression::ASTC:
			ANKI_ASSERT(loader.getAstcBlockSize() == UVec2(8u));
			init.m_format = Format::ASTC_8x8_SFLOAT_BLOCK_EXT;
			break;
		default:
			ANKI_ASSERT(0);
		}
	}
	else if(loader.getColorFormat() == ImageBinaryColorFormat::RGBAF32)
	{
		switch(loader.getCompression())
		{
		case ImageBinaryDataCompression::RAW:
			init.m_format = Format::R32G32B32A32_SFLOAT;
			break;
		case ImageBinaryDataCompression::ASTC:
			ANKI_ASSERT(loader.getAstcBlockSize() == UVec2(8u));
			init.m_format = Format::ASTC_8x8_SFLOAT_BLOCK_EXT;
			break;
		default:
			ANKI_ASSERT(0);
		}
	}
	else
	{
		ANKI_ASSERT(0);
	}

	// mipmapsCount
	init.m_mipmapCount = U8(loader.getMipmapCount());

	// Create the texture
	m_tex = getManager().getGrManager().newTexture(init);

	// Transition it. TODO remove that eventually
	{
		CommandBufferInitInfo cmdbinit;
		cmdbinit.m_flags = CommandBufferFlag::GENERAL_WORK | CommandBufferFlag::SMALL_BATCH;
		CommandBufferPtr cmdb = getManager().getGrManager().newCommandBuffer(cmdbinit);

		TextureSubresourceInfo subresource;
		subresource.m_faceCount = textureTypeIsCube(init.m_type) ? 6 : 1;
		subresource.m_layerCount = init.m_layerCount;
		subresource.m_mipmapCount = init.m_mipmapCount;

		cmdb->setTextureBarrier(m_tex, TextureUsageBit::NONE, TextureUsageBit::ALL_SAMPLED, subresource);

		FencePtr outFence;
		cmdb->flush({}, &outFence);
		outFence->clientWait(60.0_sec);
	}

	// Set the context
	ctx->m_faces = faces;
	ctx->m_layerCount = init.m_layerCount;
	ctx->m_gr = &getManager().getGrManager();
	ctx->m_trfAlloc = &getManager().getTransferGpuAllocator();
	ctx->m_texType = init.m_type;
	ctx->m_tex = m_tex;

	// Upload the data
	if(async)
	{
		getManager().getAsyncLoader().submitTask(task);
	}
	else
	{
		ANKI_CHECK(load(*ctx));
	}

	m_size = UVec3(init.m_width, init.m_height, init.m_depth);
	m_layerCount = init.m_layerCount;

	// Create the texture view
	TextureViewInitInfo viewInit(m_tex, "Rsrc");
	m_texView = getManager().getGrManager().newTextureView(viewInit);

	return Error::NONE;
}

Error ImageResource::load(LoadingContext& ctx)
{
	const U32 copyCount = ctx.m_layerCount * ctx.m_faces * ctx.m_loader.getMipmapCount();

	for(U32 b = 0; b < copyCount; b += MAX_COPIES_BEFORE_FLUSH)
	{
		const U32 begin = b;
		const U32 end = min(copyCount, b + MAX_COPIES_BEFORE_FLUSH);

		CommandBufferInitInfo ci;
		ci.m_flags = CommandBufferFlag::GENERAL_WORK | CommandBufferFlag::SMALL_BATCH;
		CommandBufferPtr cmdb = ctx.m_gr->newCommandBuffer(ci);

		// Set the barriers of the batch
		for(U32 i = begin; i < end; ++i)
		{
			U32 mip, layer, face;
			unflatten3dArrayIndex(ctx.m_layerCount, ctx.m_faces, ctx.m_loader.getMipmapCount(), i, layer, face, mip);

			if(ctx.m_texType == TextureType::_3D)
			{
				TextureVolumeInfo vol(mip);
				cmdb->setTextureVolumeBarrier(ctx.m_tex, TextureUsageBit::NONE, TextureUsageBit::TRANSFER_DESTINATION,
											  vol);
			}
			else
			{
				TextureSurfaceInfo surf(mip, 0, face, layer);
				cmdb->setTextureSurfaceBarrier(ctx.m_tex, TextureUsageBit::NONE, TextureUsageBit::TRANSFER_DESTINATION,
											   surf);
			}
		}

		// Do the copies
		Array<TransferGpuAllocatorHandle, MAX_COPIES_BEFORE_FLUSH> handles;
		U32 handleCount = 0;
		for(U32 i = begin; i < end; ++i)
		{
			U32 mip, layer, face;
			unflatten3dArrayIndex(ctx.m_layerCount, ctx.m_faces, ctx.m_loader.getMipmapCount(), i, layer, face, mip);

			PtrSize surfOrVolSize;
			const void* surfOrVolData;
			PtrSize allocationSize;

			if(ctx.m_texType == TextureType::_3D)
			{
				const auto& vol = ctx.m_loader.getVolume(mip);
				surfOrVolSize = vol.m_data.getSize();
				surfOrVolData = &vol.m_data[0];

				allocationSize = computeVolumeSize(ctx.m_tex->getWidth() >> mip, ctx.m_tex->getHeight() >> mip,
												   ctx.m_tex->getDepth() >> mip, ctx.m_tex->getFormat());
			}
			else
			{
				const auto& surf = ctx.m_loader.getSurface(mip, face, layer);
				surfOrVolSize = surf.m_data.getSize();
				surfOrVolData = &surf.m_data[0];

				allocationSize = computeSurfaceSize(ctx.m_tex->getWidth() >> mip, ctx.m_tex->getHeight() >> mip,
													ctx.m_tex->getFormat());
			}

			ANKI_ASSERT(allocationSize >= surfOrVolSize);
			TransferGpuAllocatorHandle& handle = handles[handleCount++];
			ANKI_CHECK(ctx.m_trfAlloc->allocate(allocationSize, handle));
			void* data = handle.getMappedMemory();
			ANKI_ASSERT(data);

			memcpy(data, surfOrVolData, surfOrVolSize);

			// Create temp tex view
			TextureSubresourceInfo subresource;
			if(ctx.m_texType == TextureType::_3D)
			{
				subresource = TextureSubresourceInfo(TextureVolumeInfo(mip));
			}
			else
			{
				subresource = TextureSubresourceInfo(TextureSurfaceInfo(mip, 0, face, layer));
			}

			TextureViewPtr tmpView = ctx.m_gr->newTextureView(TextureViewInitInfo(ctx.m_tex, subresource, "RsrcTmp"));

			cmdb->copyBufferToTextureView(handle.getBuffer(), handle.getOffset(), handle.getRange(), tmpView);
		}

		// Set the barriers of the batch
		for(U32 i = begin; i < end; ++i)
		{
			U32 mip, layer, face;
			unflatten3dArrayIndex(ctx.m_layerCount, ctx.m_faces, ctx.m_loader.getMipmapCount(), i, layer, face, mip);

			if(ctx.m_texType == TextureType::_3D)
			{
				TextureVolumeInfo vol(mip);
				cmdb->setTextureVolumeBarrier(ctx.m_tex, TextureUsageBit::TRANSFER_DESTINATION,
											  TextureUsageBit::SAMPLED_FRAGMENT | TextureUsageBit::SAMPLED_GEOMETRY,
											  vol);
			}
			else
			{
				TextureSurfaceInfo surf(mip, 0, face, layer);
				cmdb->setTextureSurfaceBarrier(ctx.m_tex, TextureUsageBit::TRANSFER_DESTINATION,
											   TextureUsageBit::SAMPLED_FRAGMENT | TextureUsageBit::SAMPLED_GEOMETRY,
											   surf);
			}
		}

		// Flush batch
		FencePtr fence;
		cmdb->flush({}, &fence);

		for(U i = 0; i < handleCount; ++i)
		{
			ctx.m_trfAlloc->release(handles[i], fence);
		}
		cmdb.reset(nullptr);
	}

	return Error::NONE;
}

} // end namespace anki
