// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Gr/TextureView.h>
#include <AnKi/Gr/Vulkan/VulkanObject.h>
#include <AnKi/Gr/Vulkan/TextureImpl.h>

namespace anki {

/// @addtogroup vulkan
/// @{

/// Texture view implementation.
class TextureViewImpl final : public TextureView, public VulkanObject<TextureView, TextureViewImpl>
{
public:
	TextureViewImpl(GrManager* manager, CString name)
		: TextureView(manager, name)
	{
	}

	~TextureViewImpl();

	Error init(const TextureViewInitInfo& inf);

	VkImageSubresourceRange getVkImageSubresourceRange() const
	{
		VkImageSubresourceRange out;
		static_cast<const TextureImpl&>(*m_tex).computeVkImageSubresourceRange(getSubresource(), out);
		return out;
	}

	VkImageView getHandle() const
	{
		ANKI_ASSERT(m_handle);
		return m_handle;
	}

	U64 getHash() const
	{
		ANKI_ASSERT(m_hash);
		return m_hash;
	}

	const TextureImpl& getTextureImpl() const
	{
		return static_cast<const TextureImpl&>(*m_tex);
	}

	U32 getOrCreateBindlessIndex();

private:
	VkImageView m_handle = {}; ///< Cache the handle.
	U32 m_bindlessIndex = MAX_U32; ///< Cache it.

	/// This is a hash that depends on the Texture and the VkImageView. It's used as a replacement of
	/// TextureView::m_uuid since it creates less unique IDs.
	U64 m_hash = 0;

	const MicroImageView* m_microImageView = nullptr;

	TexturePtr m_tex; ///< Hold a reference.
};
/// @}

} // end namespace anki
