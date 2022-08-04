// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Gr/Sampler.h>
#include <AnKi/Gr/Vulkan/VulkanObject.h>
#include <AnKi/Gr/Vulkan/SamplerFactory.h>

namespace anki {

/// @addtogroup vulkan
/// @{

/// Vulkan implementation of Sampler.
class SamplerImpl final : public Sampler, public VulkanObject<Sampler, SamplerImpl>
{
public:
	MicroSamplerPtr m_sampler;

	SamplerImpl(GrManager* manager, CString name)
		: Sampler(manager, name)
	{
	}

	~SamplerImpl()
	{
	}

	Error init(const SamplerInitInfo& init);
};
/// @}

} // end namespace anki
