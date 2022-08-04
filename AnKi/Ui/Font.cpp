// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Ui/Font.h>
#include <AnKi/Ui/UiManager.h>
#include <AnKi/Resource/ResourceManager.h>
#include <AnKi/Resource/ResourceFilesystem.h>
#include <AnKi/Gr/GrManager.h>
#include <AnKi/Gr/Buffer.h>
#include <AnKi/Gr/Texture.h>
#include <AnKi/Gr/CommandBuffer.h>

namespace anki {

Font::~Font()
{
	setImAllocator();
	m_imFontAtlas.destroy();
	unsetImAllocator();

	m_fonts.destroy(getAllocator());
	m_fontData.destroy(getAllocator());
}

Error Font::init(const CString& filename, ConstWeakArray<U32> fontHeights)
{
	setImAllocator();
	m_imFontAtlas.init();

	// Load font in memory
	ResourceFilePtr file;
	ANKI_CHECK(m_manager->getResourceManager().getFilesystem().openFile(filename, file));
	m_fontData.create(getAllocator(), U32(file->getSize()));
	ANKI_CHECK(file->read(&m_fontData[0], file->getSize()));

	m_fonts.create(getAllocator(), U32(fontHeights.getSize()));

	// Bake font
	ImFontConfig cfg;
	cfg.FontDataOwnedByAtlas = false;
	U32 count = 0;
	for(U32 height : fontHeights)
	{
		cfg.SizePixels = F32(height);

		m_fonts[count].m_imFont =
			m_imFontAtlas->AddFontFromMemoryTTF(&m_fontData[0], I32(m_fontData.getSize()), F32(height), &cfg);
		m_fonts[count].m_height = height;
		++count;
	}

	[[maybe_unused]] const Bool ok = m_imFontAtlas->Build();
	ANKI_ASSERT(ok);

	// Create the texture
	U8* img;
	int width, height;
	m_imFontAtlas->GetTexDataAsRGBA32(&img, &width, &height);
	createTexture(img, width, height);

	unsetImAllocator();
	return Error::NONE;
}

void Font::createTexture(const void* data, U32 width, U32 height)
{
	ANKI_ASSERT(data && width > 0 && height > 0);

	// Create and populate the buffer
	const U32 buffSize = width * height * 4;
	BufferPtr buff = m_manager->getGrManager().newBuffer(
		BufferInitInfo(buffSize, BufferUsageBit::TRANSFER_SOURCE, BufferMapAccessBit::WRITE, "UI"));
	void* mapped = buff->map(0, buffSize, BufferMapAccessBit::WRITE);
	memcpy(mapped, data, buffSize);
	buff->flush(0, buffSize);
	buff->unmap();

	// Create the texture
	TextureInitInfo texInit("Font");
	texInit.m_width = width;
	texInit.m_height = height;
	texInit.m_format = Format::R8G8B8A8_UNORM;
	texInit.m_usage =
		TextureUsageBit::TRANSFER_DESTINATION | TextureUsageBit::SAMPLED_FRAGMENT | TextureUsageBit::GENERATE_MIPMAPS;
	texInit.m_mipmapCount = 1; // No mips because it will appear blurry with trilinear filtering

	m_tex = m_manager->getGrManager().newTexture(texInit);

	// Create the whole texture view
	m_texView = m_manager->getGrManager().newTextureView(TextureViewInitInfo(m_tex, "Font"));
	m_imFontAtlas->SetTexID(UiImageId(m_texView));

	// Do the copy
	static const TextureSurfaceInfo surf(0, 0, 0, 0);
	CommandBufferInitInfo cmdbInit;
	cmdbInit.m_flags = CommandBufferFlag::GENERAL_WORK | CommandBufferFlag::SMALL_BATCH;
	CommandBufferPtr cmdb = m_manager->getGrManager().newCommandBuffer(cmdbInit);
	{
		TextureViewInitInfo viewInit(m_tex, surf, DepthStencilAspectBit::NONE, "TempFont");
		TextureViewPtr tmpView = m_manager->getGrManager().newTextureView(viewInit);

		cmdb->setTextureSurfaceBarrier(m_tex, TextureUsageBit::NONE, TextureUsageBit::TRANSFER_DESTINATION, surf);
		cmdb->copyBufferToTextureView(buff, 0, buffSize, tmpView);
		cmdb->setTextureSurfaceBarrier(m_tex, TextureUsageBit::TRANSFER_DESTINATION, TextureUsageBit::GENERATE_MIPMAPS,
									   surf);
	}

	// Gen mips
	cmdb->generateMipmaps2d(m_texView);
	cmdb->setTextureSurfaceBarrier(m_tex, TextureUsageBit::GENERATE_MIPMAPS, TextureUsageBit::SAMPLED_FRAGMENT, surf);

	cmdb->flush();
}

} // end namespace anki
