// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Gr/CommandBuffer.h>
#include <AnKi/Gr/Vulkan/VulkanObject.h>
#include <AnKi/Gr/Vulkan/CommandBufferFactory.h>
#include <AnKi/Gr/CommandBuffer.h>
#include <AnKi/Gr/Texture.h>
#include <AnKi/Gr/Buffer.h>
#include <AnKi/Gr/Shader.h>
#include <AnKi/Gr/Vulkan/BufferImpl.h>
#include <AnKi/Gr/Vulkan/TextureImpl.h>
#include <AnKi/Gr/Vulkan/Pipeline.h>
#include <AnKi/Gr/Vulkan/GrManagerImpl.h>
#include <AnKi/Util/List.h>

namespace anki {

#define ANKI_BATCH_COMMANDS 1

// Forward
class CommandBufferInitInfo;

/// @addtogroup vulkan
/// @{

#define ANKI_CMD(x_, t_) \
	flushBatches(CommandBufferCommandType::t_); \
	x_;

/// List the commands that can be batched.
enum class CommandBufferCommandType : U8
{
	SET_BARRIER,
	RESET_QUERY,
	WRITE_QUERY_RESULT,
	PUSH_SECOND_LEVEL,
	ANY_OTHER_COMMAND
};

/// Command buffer implementation.
class CommandBufferImpl final : public CommandBuffer, public VulkanObject<CommandBuffer, CommandBufferImpl>
{
public:
	/// Default constructor
	CommandBufferImpl(GrManager* manager, CString name)
		: CommandBuffer(manager, name)
		, m_renderedToDefaultFb(false)
		, m_finalized(false)
		, m_empty(false)
		, m_beganRecording(false)
	{
	}

	~CommandBufferImpl();

	Error init(const CommandBufferInitInfo& init);

	void setFence(MicroFencePtr& fence)
	{
		m_microCmdb->setFence(fence);
	}

	const MicroCommandBufferPtr& getMicroCommandBuffer()
	{
		return m_microCmdb;
	}

	VkCommandBuffer getHandle() const
	{
		ANKI_ASSERT(m_handle);
		return m_handle;
	}

	Bool renderedToDefaultFramebuffer() const
	{
		return m_renderedToDefaultFb;
	}

	Bool isEmpty() const
	{
		return m_empty;
	}

	Bool isSecondLevel() const
	{
		return !!(m_flags & CommandBufferFlag::SECOND_LEVEL);
	}

	void bindVertexBufferInternal(U32 binding, const BufferPtr& buff, PtrSize offset, PtrSize stride,
								  VertexStepRate stepRate)
	{
		commandCommon();
		m_state.bindVertexBuffer(binding, stride, stepRate);
		const VkBuffer vkbuff = static_cast<const BufferImpl&>(*buff).getHandle();
		ANKI_CMD(vkCmdBindVertexBuffers(m_handle, binding, 1, &vkbuff, &offset), ANY_OTHER_COMMAND);
		m_microCmdb->pushObjectRef(buff);
	}

	void setVertexAttributeInternal(U32 location, U32 buffBinding, const Format fmt, PtrSize relativeOffset)
	{
		commandCommon();
		m_state.setVertexAttribute(location, buffBinding, fmt, relativeOffset);
	}

	void bindIndexBufferInternal(const BufferPtr& buff, PtrSize offset, IndexType type)
	{
		commandCommon();
		ANKI_CMD(vkCmdBindIndexBuffer(m_handle, static_cast<const BufferImpl&>(*buff).getHandle(), offset,
									  convertIndexType(type)),
				 ANY_OTHER_COMMAND);
		m_microCmdb->pushObjectRef(buff);
	}

	void setPrimitiveRestartInternal(Bool enable)
	{
		commandCommon();
		m_state.setPrimitiveRestart(enable);
	}

	void setFillModeInternal(FillMode mode)
	{
		commandCommon();
		m_state.setFillMode(mode);
	}

	void setCullModeInternal(FaceSelectionBit mode)
	{
		commandCommon();
		m_state.setCullMode(mode);
	}

	void setViewportInternal(U32 minx, U32 miny, U32 width, U32 height)
	{
		ANKI_ASSERT(width > 0 && height > 0);
		commandCommon();

		if(m_viewport[0] != minx || m_viewport[1] != miny || m_viewport[2] != width || m_viewport[3] != height)
		{
			m_viewportDirty = true;

			m_viewport[0] = minx;
			m_viewport[1] = miny;
			m_viewport[2] = width;
			m_viewport[3] = height;
		}
	}

	void setScissorInternal(U32 minx, U32 miny, U32 width, U32 height)
	{
		ANKI_ASSERT(width > 0 && height > 0);
		commandCommon();

		if(m_scissor[0] != minx || m_scissor[1] != miny || m_scissor[2] != width || m_scissor[3] != height)
		{
			m_scissorDirty = true;

			m_scissor[0] = minx;
			m_scissor[1] = miny;
			m_scissor[2] = width;
			m_scissor[3] = height;
		}
	}

	void setPolygonOffsetInternal(F32 factor, F32 units)
	{
		commandCommon();
		m_state.setPolygonOffset(factor, units);
	}

	void setStencilOperationsInternal(FaceSelectionBit face, StencilOperation stencilFail,
									  StencilOperation stencilPassDepthFail, StencilOperation stencilPassDepthPass)
	{
		commandCommon();
		m_state.setStencilOperations(face, stencilFail, stencilPassDepthFail, stencilPassDepthPass);
	}

	void setStencilCompareOperationInternal(FaceSelectionBit face, CompareOperation comp)
	{
		commandCommon();
		m_state.setStencilCompareOperation(face, comp);
	}

	void setStencilCompareMaskInternal(FaceSelectionBit face, U32 mask);

	void setStencilWriteMaskInternal(FaceSelectionBit face, U32 mask);

	void setStencilReferenceInternal(FaceSelectionBit face, U32 ref);

	void setDepthWriteInternal(Bool enable)
	{
		commandCommon();
		m_state.setDepthWrite(enable);
	}

	void setDepthCompareOperationInternal(CompareOperation op)
	{
		commandCommon();
		m_state.setDepthCompareOperation(op);
	}

	void setAlphaToCoverageInternal(Bool enable)
	{
		commandCommon();
		m_state.setAlphaToCoverage(enable);
	}

	void setColorChannelWriteMaskInternal(U32 attachment, ColorBit mask)
	{
		commandCommon();
		m_state.setColorChannelWriteMask(attachment, mask);
	}

	void setBlendFactorsInternal(U32 attachment, BlendFactor srcRgb, BlendFactor dstRgb, BlendFactor srcA,
								 BlendFactor dstA)
	{
		commandCommon();
		m_state.setBlendFactors(attachment, srcRgb, dstRgb, srcA, dstA);
	}

	void setBlendOperationInternal(U32 attachment, BlendOperation funcRgb, BlendOperation funcA)
	{
		commandCommon();
		m_state.setBlendOperation(attachment, funcRgb, funcA);
	}

	void bindTextureAndSamplerInternal(U32 set, U32 binding, const TextureViewPtr& texView, const SamplerPtr& sampler,
									   U32 arrayIdx)
	{
		commandCommon();
		const TextureViewImpl& view = static_cast<const TextureViewImpl&>(*texView);
		const TextureImpl& tex = view.getTextureImpl();
		ANKI_ASSERT(tex.isSubresourceGoodForSampling(view.getSubresource()));
		const VkImageLayout lay = tex.computeLayout(TextureUsageBit::ALL_SAMPLED & tex.getTextureUsage(), 0);

		m_dsetState[set].bindTextureAndSampler(binding, arrayIdx, &view, sampler.get(), lay);

		m_microCmdb->pushObjectRef(texView);
		m_microCmdb->pushObjectRef(sampler);
	}

	void bindTextureInternal(U32 set, U32 binding, const TextureViewPtr& texView, U32 arrayIdx)
	{
		commandCommon();
		const TextureViewImpl& view = static_cast<const TextureViewImpl&>(*texView);
		const TextureImpl& tex = view.getTextureImpl();
		ANKI_ASSERT(tex.isSubresourceGoodForSampling(view.getSubresource()));
		const VkImageLayout lay = tex.computeLayout(TextureUsageBit::ALL_SAMPLED & tex.getTextureUsage(), 0);

		m_dsetState[set].bindTexture(binding, arrayIdx, &view, lay);

		m_microCmdb->pushObjectRef(texView);
	}

	void bindSamplerInternal(U32 set, U32 binding, const SamplerPtr& sampler, U32 arrayIdx)
	{
		commandCommon();
		m_dsetState[set].bindSampler(binding, arrayIdx, sampler.get());
		m_microCmdb->pushObjectRef(sampler);
	}

	void bindImageInternal(U32 set, U32 binding, const TextureViewPtr& img, U32 arrayIdx)
	{
		commandCommon();
		m_dsetState[set].bindImage(binding, arrayIdx, img.get());

		const Bool isPresentable =
			!!(static_cast<const TextureViewImpl&>(*img).getTextureImpl().getTextureUsage() & TextureUsageBit::PRESENT);
		if(isPresentable)
		{
			m_renderedToDefaultFb = true;
		}

		m_microCmdb->pushObjectRef(img);
	}

	void bindAccelerationStructureInternal(U32 set, U32 binding, const AccelerationStructurePtr& as, U32 arrayIdx)
	{
		commandCommon();
		m_dsetState[set].bindAccelerationStructure(binding, arrayIdx, as.get());
		m_microCmdb->pushObjectRef(as);
	}

	void bindAllBindlessInternal(U32 set)
	{
		commandCommon();
		m_dsetState[set].bindBindlessDescriptorSet();
	}

	void beginRenderPassInternal(const FramebufferPtr& fb,
								 const Array<TextureUsageBit, MAX_COLOR_ATTACHMENTS>& colorAttachmentUsages,
								 TextureUsageBit depthStencilAttachmentUsage, U32 minx, U32 miny, U32 width,
								 U32 height);

	void endRenderPassInternal();

	void setVrsRateInternal(VrsRate rate);

	void drawArraysInternal(PrimitiveTopology topology, U32 count, U32 instanceCount, U32 first, U32 baseInstance);

	void drawElementsInternal(PrimitiveTopology topology, U32 count, U32 instanceCount, U32 firstIndex, U32 baseVertex,
							  U32 baseInstance);

	void drawArraysIndirectInternal(PrimitiveTopology topology, U32 drawCount, PtrSize offset, const BufferPtr& buff);

	void drawElementsIndirectInternal(PrimitiveTopology topology, U32 drawCount, PtrSize offset, const BufferPtr& buff);

	void dispatchComputeInternal(U32 groupCountX, U32 groupCountY, U32 groupCountZ);

	void traceRaysInternal(const BufferPtr& sbtBuffer, PtrSize sbtBufferOffset, U32 sbtRecordSize,
						   U32 hitGroupSbtRecordCount, U32 rayTypeCount, U32 width, U32 height, U32 depth);

	void resetOcclusionQueryInternal(const OcclusionQueryPtr& query);

	void beginOcclusionQueryInternal(const OcclusionQueryPtr& query);

	void endOcclusionQueryInternal(const OcclusionQueryPtr& query);

	void resetTimestampQueryInternal(const TimestampQueryPtr& query);

	void writeTimestampInternal(const TimestampQueryPtr& query);

	void generateMipmaps2dInternal(const TextureViewPtr& texView);

	void clearTextureViewInternal(const TextureViewPtr& texView, const ClearValue& clearValue);

	void pushSecondLevelCommandBufferInternal(const CommandBufferPtr& cmdb);

	// To enable using Anki's commandbuffers for external workloads
	void beginRecordingExt()
	{
		commandCommon();
	}

	void endRecording();

	void setTextureBarrierInternal(const TexturePtr& tex, TextureUsageBit prevUsage, TextureUsageBit nextUsage,
								   const TextureSubresourceInfo& subresource);

	void setTextureSurfaceBarrierInternal(const TexturePtr& tex, TextureUsageBit prevUsage, TextureUsageBit nextUsage,
										  const TextureSurfaceInfo& surf);

	void setTextureVolumeBarrierInternal(const TexturePtr& tex, TextureUsageBit prevUsage, TextureUsageBit nextUsage,
										 const TextureVolumeInfo& vol);

	void setTextureBarrierRangeInternal(const TexturePtr& tex, TextureUsageBit prevUsage, TextureUsageBit nextUsage,
										const VkImageSubresourceRange& range);

	void setBufferBarrierInternal(VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
								  VkAccessFlags dstAccess, PtrSize offset, PtrSize size, VkBuffer buff);

	void setBufferBarrierInternal(const BufferPtr& buff, BufferUsageBit before, BufferUsageBit after, PtrSize offset,
								  PtrSize size);

	void setAccelerationStructureBarrierInternal(const AccelerationStructurePtr& as,
												 AccelerationStructureUsageBit prevUsage,
												 AccelerationStructureUsageBit nextUsage);

	void fillBufferInternal(const BufferPtr& buff, PtrSize offset, PtrSize size, U32 value);

	void writeOcclusionQueryResultToBufferInternal(const OcclusionQueryPtr& query, PtrSize offset,
												   const BufferPtr& buff);

	void bindShaderProgramInternal(const ShaderProgramPtr& prog);

	void bindUniformBufferInternal(U32 set, U32 binding, const BufferPtr& buff, PtrSize offset, PtrSize range,
								   U32 arrayIdx)
	{
		commandCommon();
		m_dsetState[set].bindUniformBuffer(binding, arrayIdx, buff.get(), offset, range);
		m_microCmdb->pushObjectRef(buff);
	}

	void bindStorageBufferInternal(U32 set, U32 binding, const BufferPtr& buff, PtrSize offset, PtrSize range,
								   U32 arrayIdx)
	{
		commandCommon();
		m_dsetState[set].bindStorageBuffer(binding, arrayIdx, buff.get(), offset, range);
		m_microCmdb->pushObjectRef(buff);
	}

	void bindReadOnlyTextureBufferInternal(U32 set, U32 binding, const BufferPtr& buff, PtrSize offset, PtrSize range,
										   Format fmt, U32 arrayIdx)
	{
		commandCommon();
		m_dsetState[set].bindReadOnlyTextureBuffer(binding, arrayIdx, buff.get(), offset, range, fmt);
		m_microCmdb->pushObjectRef(buff);
	}

	void copyBufferToTextureViewInternal(const BufferPtr& buff, PtrSize offset, PtrSize range,
										 const TextureViewPtr& texView);

	void copyBufferToBufferInternal(const BufferPtr& src, PtrSize srcOffset, const BufferPtr& dst, PtrSize dstOffset,
									PtrSize range);

	void buildAccelerationStructureInternal(const AccelerationStructurePtr& as);

	void upscaleInternal(const GrUpscalerPtr& upscaler, const TextureViewPtr& inColor,
						 const TextureViewPtr& outUpscaledColor, const TextureViewPtr& motionVectors,
						 const TextureViewPtr& depth, const TextureViewPtr& exposure, const Bool resetAccumulation,
						 const Vec2& jitterOffset, const Vec2& motionVectorsScale);

	void setPushConstantsInternal(const void* data, U32 dataSize);

	void setRasterizationOrderInternal(RasterizationOrder order);

	void setLineWidthInternal(F32 width);

private:
	StackAllocator<U8> m_alloc;

	MicroCommandBufferPtr m_microCmdb;
	VkCommandBuffer m_handle = VK_NULL_HANDLE;
	ThreadId m_tid = ~ThreadId(0);
	CommandBufferFlag m_flags = CommandBufferFlag::NONE;
	Bool m_renderedToDefaultFb : 1;
	Bool m_finalized : 1;
	Bool m_empty : 1;
	Bool m_beganRecording : 1;
#if ANKI_EXTRA_CHECKS
	U32 m_commandCount = 0;
	U32 m_setPushConstantsSize = 0;
#endif

	FramebufferPtr m_activeFb;
	Array<U32, 4> m_renderArea = {0, 0, MAX_U32, MAX_U32};
	Array<U32, 2> m_fbSize = {0, 0};
	U32 m_rpCommandCount = 0; ///< Number of drawcalls or pushed cmdbs in rp.
	Array<TextureUsageBit, MAX_COLOR_ATTACHMENTS> m_colorAttachmentUsages = {};
	TextureUsageBit m_depthStencilAttachmentUsage = TextureUsageBit::NONE;

	PipelineStateTracker m_state;

	Array<DescriptorSetState, MAX_DESCRIPTOR_SETS> m_dsetState;

	ShaderProgramImpl* m_graphicsProg ANKI_DEBUG_CODE(= nullptr); ///< Last bound graphics program
	ShaderProgramImpl* m_computeProg ANKI_DEBUG_CODE(= nullptr);
	ShaderProgramImpl* m_rtProg ANKI_DEBUG_CODE(= nullptr);

	VkSubpassContents m_subpassContents = VK_SUBPASS_CONTENTS_MAX_ENUM;

	CommandBufferCommandType m_lastCmdType = CommandBufferCommandType::ANY_OTHER_COMMAND;

	/// @name state_opts
	/// @{
	Array<U32, 4> m_viewport = {0, 0, 0, 0};
	Array<U32, 4> m_scissor = {0, 0, MAX_U32, MAX_U32};
	VkViewport m_lastViewport = {};
	Bool m_viewportDirty = true;
	Bool m_scissorDirty = true;
	VkRect2D m_lastScissor = {{-1, -1}, {MAX_U32, MAX_U32}};
	Array<U32, 2> m_stencilCompareMasks = {0x5A5A5A5A, 0x5A5A5A5A}; ///< Use a stupid number to initialize.
	Array<U32, 2> m_stencilWriteMasks = {0x5A5A5A5A, 0x5A5A5A5A};
	Array<U32, 2> m_stencilReferenceMasks = {0x5A5A5A5A, 0x5A5A5A5A};
#if ANKI_ENABLE_ASSERTIONS
	Bool m_lineWidthSet = false;
#endif
	Bool m_vrsRateDirty = true;
	VrsRate m_vrsRate = VrsRate::_1x1;

	/// Rebind the above dynamic state. Needed after pushing secondary command buffers (they dirty the state).
	void rebindDynamicState();
	/// @}

	/// @name barrier_batch
	/// @{
	DynamicArray<VkImageMemoryBarrier> m_imgBarriers;
	DynamicArray<VkBufferMemoryBarrier> m_buffBarriers;
	DynamicArray<VkMemoryBarrier> m_memBarriers;
	U16 m_imgBarrierCount = 0;
	U16 m_buffBarrierCount = 0;
	U16 m_memBarrierCount = 0;
	VkPipelineStageFlags m_srcStageMask = 0;
	VkPipelineStageFlags m_dstStageMask = 0;
	/// @}

	/// @name reset_query_batch
	/// @{
	class QueryResetAtom
	{
	public:
		VkQueryPool m_pool;
		U32 m_queryIdx;
	};

	DynamicArray<QueryResetAtom> m_queryResetAtoms;
	/// @}

	/// @name write_query_result_batch
	/// @{
	class WriteQueryAtom
	{
	public:
		VkQueryPool m_pool;
		U32 m_queryIdx;
		VkBuffer m_buffer;
		PtrSize m_offset;
	};

	DynamicArray<WriteQueryAtom> m_writeQueryAtoms;
	/// @}

	/// @name push_second_level_batch
	/// @{
	DynamicArray<VkCommandBuffer> m_secondLevelAtoms;
	U16 m_secondLevelAtomCount = 0;
	/// @}

	/// Some common operations per command.
	void commandCommon();

	/// Flush batches. Use ANKI_CMD on every vkCmdXXX to do that automatically and call it manually before adding to a
	/// batch.
	void flushBatches(CommandBufferCommandType type);

	void drawcallCommon();

	Bool insideRenderPass() const
	{
		return m_activeFb.isCreated();
	}

	void beginRenderPassInternal();

	Bool secondLevel() const
	{
		return !!(m_flags & CommandBufferFlag::SECOND_LEVEL);
	}

	/// Flush batched image and buffer barriers.
	void flushBarriers();

	void flushQueryResets();

	void flushWriteQueryResults();

	void setImageBarrier(VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkImageLayout prevLayout,
						 VkPipelineStageFlags dstStage, VkAccessFlags dstAccess, VkImageLayout newLayout, VkImage img,
						 const VkImageSubresourceRange& range);

	void beginRecording();

	Bool flipViewport() const;

	static VkViewport computeViewport(U32* viewport, U32 fbWidth, U32 fbHeight, Bool flipvp)
	{
		const U32 minx = viewport[0];
		const U32 miny = viewport[1];
		const U32 width = min<U32>(fbWidth, viewport[2]);
		const U32 height = min<U32>(fbHeight, viewport[3]);
		ANKI_ASSERT(width > 0 && height > 0);
		ANKI_ASSERT(minx + width <= fbWidth);
		ANKI_ASSERT(miny + height <= fbHeight);

		VkViewport s = {};
		s.x = F32(minx);
		s.y = (flipvp) ? F32(fbHeight - miny) : F32(miny); // Move to the bottom;
		s.width = F32(width);
		s.height = (flipvp) ? -F32(height) : F32(height);
		s.minDepth = 0.0f;
		s.maxDepth = 1.0f;
		return s;
	}

	static VkRect2D computeScissor(U32* scissor, U32 fbWidth, U32 fbHeight, Bool flipvp)
	{
		const U32 minx = scissor[0];
		const U32 miny = scissor[1];
		const U32 width = min<U32>(fbWidth, scissor[2]);
		const U32 height = min<U32>(fbHeight, scissor[3]);
		ANKI_ASSERT(minx + width <= fbWidth);
		ANKI_ASSERT(miny + height <= fbHeight);

		VkRect2D out = {};
		out.extent.width = width;
		out.extent.height = height;
		out.offset.x = minx;
		out.offset.y = (flipvp) ? (fbHeight - (miny + height)) : miny;

		return out;
	}

	const ShaderProgramImpl& getBoundProgram()
	{
		if(m_graphicsProg)
		{
			ANKI_ASSERT(m_computeProg == nullptr && m_rtProg == nullptr);
			return *m_graphicsProg;
		}
		else if(m_computeProg)
		{
			ANKI_ASSERT(m_graphicsProg == nullptr && m_rtProg == nullptr);
			return *m_computeProg;
		}
		else
		{
			ANKI_ASSERT(m_graphicsProg == nullptr && m_computeProg == nullptr && m_rtProg != nullptr);
			return *m_rtProg;
		}
	}
};
/// @}

} // end namespace anki

#include <AnKi/Gr/Vulkan/CommandBufferImpl.inl.h>
