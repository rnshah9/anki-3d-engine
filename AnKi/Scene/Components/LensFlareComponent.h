// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Scene/SceneNode.h>
#include <AnKi/Gr.h>
#include <AnKi/Resource/ImageResource.h>
#include <AnKi/Renderer/RenderQueue.h>

namespace anki {

/// @addtogroup scene
/// @{

/// Lens flare scene component.
class LensFlareComponent final : public SceneComponent
{
	ANKI_SCENE_COMPONENT(LensFlareComponent)

public:
	LensFlareComponent(SceneNode* node);

	~LensFlareComponent();

	Error loadImageResource(CString filename);

	Bool isLoaded() const
	{
		return m_image.isCreated();
	}

	CString getImageResourceFilename() const
	{
		return (m_image) ? m_image->getFilename() : CString();
	}

	void setWorldPosition(const Vec3& worldPosition)
	{
		m_worldPosition = worldPosition;
	}

	const Vec3& getWorldPosition() const
	{
		return m_worldPosition;
	}

	void setFirstFlareSize(const Vec2& size)
	{
		m_firstFlareSize = size;
	}

	const Vec2& getFirstFlareSize() const
	{
		return m_firstFlareSize;
	}

	void setOtherFlareSize(const Vec2& size)
	{
		m_otherFlareSize = size;
	}

	const Vec2& getOtherFlareSize() const
	{
		return m_otherFlareSize;
	}

	void setColorMultiplier(const Vec4& color)
	{
		m_colorMul = color;
	}

	const Vec4& getColorMultiplier() const
	{
		return m_colorMul;
	}

	TexturePtr getTexture() const
	{
		return m_image->getTexture();
	}

	void setupLensFlareQueueElement(LensFlareQueueElement& el) const
	{
		el.m_worldPosition = m_worldPosition;
		el.m_firstFlareSize = m_firstFlareSize;
		el.m_colorMultiplier = m_colorMul;
		el.m_textureView = m_image->getTextureView().get();
		el.m_userData = this;
		el.m_drawCallback = debugDrawCallback;
	}

private:
	Vec4 m_colorMul = Vec4(1.0f); ///< Color multiplier.

	SceneNode* m_node;
	ImageResourcePtr m_image; ///< Array of textures.

	Vec2 m_firstFlareSize = Vec2(1.0f);
	Vec2 m_otherFlareSize = Vec2(1.0f);

	Vec3 m_worldPosition = Vec3(0.0f);

	static void debugDrawCallback([[maybe_unused]] RenderQueueDrawContext& ctx,
								  [[maybe_unused]] ConstWeakArray<void*> userData)
	{
		// Do nothing
	}
};
/// @}

} // end namespace anki
