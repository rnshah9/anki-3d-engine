// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Renderer/Drawer.h>
#include <AnKi/Renderer/RenderQueue.h>
#include <AnKi/Resource/ImageResource.h>
#include <AnKi/Renderer/Renderer.h>
#include <AnKi/Util/Tracer.h>
#include <AnKi/Util/Logger.h>
#include <AnKi/Shaders/Include/MaterialTypes.h>

namespace anki {

/// Drawer's context
class RenderableDrawer::Context
{
public:
	RenderQueueDrawContext m_queueCtx;

	const RenderableQueueElement* m_renderableElement = nullptr;

	Array<RenderableQueueElement, MAX_INSTANCE_COUNT> m_cachedRenderElements;
	Array<U8, MAX_INSTANCE_COUNT> m_cachedRenderElementLods;
	Array<const void*, MAX_INSTANCE_COUNT> m_userData;
	U32 m_cachedRenderElementCount = 0;
	U8 m_minLod = 0;
	U8 m_maxLod = 0;
};

/// Check if the drawcalls can be merged.
static Bool canMergeRenderableQueueElements(const RenderableQueueElement& a, const RenderableQueueElement& b)
{
	return a.m_callback == b.m_callback && a.m_mergeKey != 0 && a.m_mergeKey == b.m_mergeKey;
}

RenderableDrawer::~RenderableDrawer()
{
}

void RenderableDrawer::drawRange(RenderingTechnique technique, const RenderableDrawerArguments& args,
								 const RenderableQueueElement* begin, const RenderableQueueElement* end,
								 CommandBufferPtr& cmdb)
{
	ANKI_ASSERT(begin && end && begin < end);

	// Allocate, set and bind global uniforms
	{
		StagingGpuMemoryToken globalUniformsToken;
		MaterialGlobalUniforms* globalUniforms =
			static_cast<MaterialGlobalUniforms*>(m_r->getStagingGpuMemory().allocateFrame(
				sizeof(MaterialGlobalUniforms), StagingGpuMemoryType::UNIFORM, globalUniformsToken));

		globalUniforms->m_viewProjectionMatrix = args.m_viewProjectionMatrix;
		globalUniforms->m_previousViewProjectionMatrix = args.m_previousViewProjectionMatrix;
		globalUniforms->m_viewMatrix = args.m_viewMatrix;
		globalUniforms->m_cameraTransform = args.m_cameraTransform;

		cmdb->bindUniformBuffer(MATERIAL_SET_GLOBAL, MATERIAL_BINDING_GLOBAL_UNIFORMS, globalUniformsToken.m_buffer,
								globalUniformsToken.m_offset, globalUniformsToken.m_range);
	}

	// More globals
	cmdb->bindAllBindless(MATERIAL_SET_BINDLESS);
	cmdb->bindSampler(MATERIAL_SET_GLOBAL, MATERIAL_BINDING_TRILINEAR_REPEAT_SAMPLER, args.m_sampler);

	// Set a few things
	Context ctx;
	ctx.m_queueCtx.m_viewMatrix = args.m_viewMatrix;
	ctx.m_queueCtx.m_viewProjectionMatrix = args.m_viewProjectionMatrix;
	ctx.m_queueCtx.m_projectionMatrix = Mat4::getIdentity(); // TODO
	ctx.m_queueCtx.m_previousViewProjectionMatrix = args.m_previousViewProjectionMatrix;
	ctx.m_queueCtx.m_cameraTransform = args.m_cameraTransform;
	ctx.m_queueCtx.m_stagingGpuAllocator = &m_r->getStagingGpuMemory();
	ctx.m_queueCtx.m_commandBuffer = cmdb;
	ctx.m_queueCtx.m_key = RenderingKey(technique, 0, 1, false, false);
	ctx.m_queueCtx.m_debugDraw = false;
	ctx.m_queueCtx.m_sampler = args.m_sampler;

	ANKI_ASSERT(args.m_minLod < MAX_LOD_COUNT && args.m_maxLod < MAX_LOD_COUNT && args.m_minLod <= args.m_maxLod);
	ctx.m_minLod = U8(args.m_minLod);
	ctx.m_maxLod = U8(args.m_maxLod);

	for(; begin != end; ++begin)
	{
		ctx.m_renderableElement = begin;

		drawSingle(ctx);
	}

	// Flush the last drawcall
	flushDrawcall(ctx);
}

void RenderableDrawer::flushDrawcall(Context& ctx)
{
	ctx.m_queueCtx.m_key.setLod(ctx.m_cachedRenderElementLods[0]);
	ctx.m_queueCtx.m_key.setInstanceCount(ctx.m_cachedRenderElementCount);

	ctx.m_cachedRenderElements[0].m_callback(
		ctx.m_queueCtx, ConstWeakArray<void*>(const_cast<void**>(&ctx.m_userData[0]), ctx.m_cachedRenderElementCount));

	// Rendered something, reset the cached transforms
	if(ctx.m_cachedRenderElementCount > 1)
	{
		ANKI_TRACE_INC_COUNTER(R_MERGED_DRAWCALLS, ctx.m_cachedRenderElementCount - 1);
	}
	ctx.m_cachedRenderElementCount = 0;
}

void RenderableDrawer::drawSingle(Context& ctx)
{
	if(ctx.m_cachedRenderElementCount == MAX_INSTANCE_COUNT)
	{
		flushDrawcall(ctx);
	}

	const RenderableQueueElement& rqel = *ctx.m_renderableElement;

	const U8 overridenLod = clamp(rqel.m_lod, ctx.m_minLod, ctx.m_maxLod);

	const Bool shouldFlush =
		ctx.m_cachedRenderElementCount > 0
		&& (!canMergeRenderableQueueElements(ctx.m_cachedRenderElements[ctx.m_cachedRenderElementCount - 1], rqel)
			|| ctx.m_cachedRenderElementLods[ctx.m_cachedRenderElementCount - 1] != overridenLod);

	if(shouldFlush)
	{
		flushDrawcall(ctx);
	}

	// Cache the new one
	ctx.m_cachedRenderElements[ctx.m_cachedRenderElementCount] = rqel;
	ctx.m_cachedRenderElementLods[ctx.m_cachedRenderElementCount] = overridenLod;
	ctx.m_userData[ctx.m_cachedRenderElementCount] = rqel.m_userData;
	++ctx.m_cachedRenderElementCount;
}

} // end namespace anki
