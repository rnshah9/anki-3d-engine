// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Renderer/Renderer.h>
#include <AnKi/Renderer/RenderQueue.h>
#include <AnKi/Util/Tracer.h>
#include <AnKi/Util/ThreadHive.h>
#include <AnKi/Core/ConfigSet.h>
#include <AnKi/Util/HighRezTimer.h>
#include <AnKi/Collision/Aabb.h>
#include <AnKi/Collision/Plane.h>
#include <AnKi/Collision/Functions.h>
#include <AnKi/Shaders/Include/ClusteredShadingTypes.h>

#include <AnKi/Renderer/ProbeReflections.h>
#include <AnKi/Renderer/GBuffer.h>
#include <AnKi/Renderer/GBufferPost.h>
#include <AnKi/Renderer/LightShading.h>
#include <AnKi/Renderer/ShadowMapping.h>
#include <AnKi/Renderer/FinalComposite.h>
#include <AnKi/Renderer/Bloom.h>
#include <AnKi/Renderer/Tonemapping.h>
#include <AnKi/Renderer/ForwardShading.h>
#include <AnKi/Renderer/LensFlare.h>
#include <AnKi/Renderer/Dbg.h>
#include <AnKi/Renderer/DownscaleBlur.h>
#include <AnKi/Renderer/VolumetricFog.h>
#include <AnKi/Renderer/DepthDownscale.h>
#include <AnKi/Renderer/TemporalAA.h>
#include <AnKi/Renderer/UiStage.h>
#include <AnKi/Renderer/IndirectSpecular.h>
#include <AnKi/Renderer/VolumetricLightingAccumulation.h>
#include <AnKi/Renderer/IndirectDiffuseProbes.h>
#include <AnKi/Renderer/GenericCompute.h>
#include <AnKi/Renderer/ShadowmapsResolve.h>
#include <AnKi/Renderer/RtShadows.h>
#include <AnKi/Renderer/AccelerationStructureBuilder.h>
#include <AnKi/Renderer/MotionVectors.h>
#include <AnKi/Renderer/ClusterBinning.h>
#include <AnKi/Renderer/Scale.h>
#include <AnKi/Renderer/IndirectDiffuse.h>
#include <AnKi/Renderer/VrsSriGeneration.h>

namespace anki {

/// Generate a Halton jitter in [-0.5, 0.5]
static Vec2 generateJitter(U32 frame)
{
	// Halton jitter
	Vec2 result(0.0f);

	constexpr U32 baseX = 2;
	U32 index = frame + 1;
	F32 invBase = 1.0f / baseX;
	F32 fraction = invBase;
	while(index > 0)
	{
		result.x() += (index % baseX) * fraction;
		index /= baseX;
		fraction *= invBase;
	}

	constexpr U32 baseY = 3;
	index = frame + 1;
	invBase = 1.0f / baseY;
	fraction = invBase;
	while(index > 0)
	{
		result.y() += (index % baseY) * fraction;
		index /= baseY;
		fraction *= invBase;
	}

	result.x() -= 0.5f;
	result.y() -= 0.5f;
	return result;
}

Renderer::Renderer()
	: m_sceneDrawer(this)
{
}

Renderer::~Renderer()
{
	for(DebugRtInfo& info : m_debugRts)
	{
		info.m_rtName.destroy(getAllocator());
	}
	m_debugRts.destroy(getAllocator());
	m_currentDebugRtName.destroy(getAllocator());
}

Error Renderer::init(ThreadHive* hive, ResourceManager* resources, GrManager* gl, StagingGpuMemoryPool* stagingMem,
					 UiManager* ui, HeapAllocator<U8> alloc, ConfigSet* config, Timestamp* globTimestamp,
					 UVec2 swapchainSize)
{
	ANKI_TRACE_SCOPED_EVENT(R_INIT);

	m_globTimestamp = globTimestamp;
	m_threadHive = hive;
	m_resources = resources;
	m_gr = gl;
	m_stagingMem = stagingMem;
	m_ui = ui;
	m_alloc = alloc;
	m_config = config;

	const Error err = initInternal(swapchainSize);
	if(err)
	{
		ANKI_R_LOGE("Failed to initialize the renderer");
	}

	return err;
}

Error Renderer::initInternal(UVec2 swapchainResolution)
{
	m_frameCount = 0;

	// Set from the config
	m_postProcessResolution = UVec2(Vec2(swapchainResolution) * m_config->getRRenderScaling());
	alignRoundDown(2, m_postProcessResolution.x());
	alignRoundDown(2, m_postProcessResolution.y());

	m_internalResolution = UVec2(Vec2(m_postProcessResolution) * m_config->getRInternalRenderScaling());
	alignRoundDown(2, m_internalResolution.x());
	alignRoundDown(2, m_internalResolution.y());

	ANKI_R_LOGI("Initializing offscreen renderer. Resolution %ux%u. Internal resolution %ux%u",
				m_postProcessResolution.x(), m_postProcessResolution.y(), m_internalResolution.x(),
				m_internalResolution.y());

	m_tileSize = m_config->getRTileSize();
	m_tileCounts.x() = (m_internalResolution.x() + m_tileSize - 1) / m_tileSize;
	m_tileCounts.y() = (m_internalResolution.y() + m_tileSize - 1) / m_tileSize;
	m_zSplitCount = m_config->getRZSplitCount();

	// A few sanity checks
	if(m_internalResolution.x() < 64 || m_internalResolution.y() < 64)
	{
		ANKI_R_LOGE("Incorrect sizes");
		return Error::USER_DATA;
	}

	ANKI_CHECK(m_resources->loadResource("ShaderBinaries/ClearTextureCompute.ankiprogbin", m_clearTexComputeProg));

	// Dummy resources
	{
		TextureInitInfo texinit("RendererDummy");
		texinit.m_width = texinit.m_height = 4;
		texinit.m_usage = TextureUsageBit::ALL_SAMPLED | TextureUsageBit::IMAGE_COMPUTE_WRITE;
		texinit.m_format = Format::R8G8B8A8_UNORM;
		TexturePtr tex = createAndClearRenderTarget(texinit, TextureUsageBit::ALL_SAMPLED);

		TextureViewInitInfo viewinit(tex);
		m_dummyTexView2d = getGrManager().newTextureView(viewinit);

		texinit.m_depth = 4;
		texinit.m_type = TextureType::_3D;
		tex = createAndClearRenderTarget(texinit, TextureUsageBit::ALL_SAMPLED);
		viewinit = TextureViewInitInfo(tex);
		m_dummyTexView3d = getGrManager().newTextureView(viewinit);

		m_dummyBuff = getGrManager().newBuffer(BufferInitInfo(
			1024, BufferUsageBit::ALL_UNIFORM | BufferUsageBit::ALL_STORAGE, BufferMapAccessBit::NONE, "Dummy"));
	}

	// Init the stages. Careful with the order!!!!!!!!!!
	m_genericCompute.reset(m_alloc.newInstance<GenericCompute>(this));
	ANKI_CHECK(m_genericCompute->init());

	m_volumetricLightingAccumulation.reset(m_alloc.newInstance<VolumetricLightingAccumulation>(this));
	ANKI_CHECK(m_volumetricLightingAccumulation->init());

	m_indirectDiffuseProbes.reset(m_alloc.newInstance<IndirectDiffuseProbes>(this));
	ANKI_CHECK(m_indirectDiffuseProbes->init());

	m_probeReflections.reset(m_alloc.newInstance<ProbeReflections>(this));
	ANKI_CHECK(m_probeReflections->init());

	m_vrsSriGeneration.reset(m_alloc.newInstance<VrsSriGeneration>(this));
	ANKI_CHECK(m_vrsSriGeneration->init());

	m_scale.reset(m_alloc.newInstance<Scale>(this));
	ANKI_CHECK(m_scale->init());

	m_gbuffer.reset(m_alloc.newInstance<GBuffer>(this));
	ANKI_CHECK(m_gbuffer->init());

	m_gbufferPost.reset(m_alloc.newInstance<GBufferPost>(this));
	ANKI_CHECK(m_gbufferPost->init());

	m_shadowMapping.reset(m_alloc.newInstance<ShadowMapping>(this));
	ANKI_CHECK(m_shadowMapping->init());

	m_volumetricFog.reset(m_alloc.newInstance<VolumetricFog>(this));
	ANKI_CHECK(m_volumetricFog->init());

	m_lightShading.reset(m_alloc.newInstance<LightShading>(this));
	ANKI_CHECK(m_lightShading->init());

	m_depthDownscale.reset(m_alloc.newInstance<DepthDownscale>(this));
	ANKI_CHECK(m_depthDownscale->init());

	m_forwardShading.reset(m_alloc.newInstance<ForwardShading>(this));
	ANKI_CHECK(m_forwardShading->init());

	m_lensFlare.reset(m_alloc.newInstance<LensFlare>(this));
	ANKI_CHECK(m_lensFlare->init());

	m_downscaleBlur.reset(getAllocator().newInstance<DownscaleBlur>(this));
	ANKI_CHECK(m_downscaleBlur->init());

	m_indirectSpecular.reset(m_alloc.newInstance<IndirectSpecular>(this));
	ANKI_CHECK(m_indirectSpecular->init());

	m_tonemapping.reset(getAllocator().newInstance<Tonemapping>(this));
	ANKI_CHECK(m_tonemapping->init());

	m_temporalAA.reset(getAllocator().newInstance<TemporalAA>(this));
	ANKI_CHECK(m_temporalAA->init());

	m_bloom.reset(m_alloc.newInstance<Bloom>(this));
	ANKI_CHECK(m_bloom->init());

	m_finalComposite.reset(m_alloc.newInstance<FinalComposite>(this));
	ANKI_CHECK(m_finalComposite->init());

	m_dbg.reset(m_alloc.newInstance<Dbg>(this));
	ANKI_CHECK(m_dbg->init());

	m_uiStage.reset(m_alloc.newInstance<UiStage>(this));
	ANKI_CHECK(m_uiStage->init());

	m_indirectDiffuse.reset(m_alloc.newInstance<IndirectDiffuse>(this));
	ANKI_CHECK(m_indirectDiffuse->init());

	if(getGrManager().getDeviceCapabilities().m_rayTracingEnabled && getConfig().getSceneRayTracedShadows())
	{
		m_accelerationStructureBuilder.reset(m_alloc.newInstance<AccelerationStructureBuilder>(this));
		ANKI_CHECK(m_accelerationStructureBuilder->init());

		m_rtShadows.reset(m_alloc.newInstance<RtShadows>(this));
		ANKI_CHECK(m_rtShadows->init());
	}
	else
	{
		m_shadowmapsResolve.reset(m_alloc.newInstance<ShadowmapsResolve>(this));
		ANKI_CHECK(m_shadowmapsResolve->init());
	}

	m_motionVectors.reset(m_alloc.newInstance<MotionVectors>(this));
	ANKI_CHECK(m_motionVectors->init());

	m_clusterBinning.reset(m_alloc.newInstance<ClusterBinning>(this));
	ANKI_CHECK(m_clusterBinning->init());

	// Init samplers
	{
		SamplerInitInfo sinit("Renderer");
		sinit.m_addressing = SamplingAddressing::CLAMP;
		sinit.m_mipmapFilter = SamplingFilter::NEAREST;
		sinit.m_minMagFilter = SamplingFilter::NEAREST;
		m_samplers.m_nearestNearestClamp = m_gr->newSampler(sinit);

		sinit.m_minMagFilter = SamplingFilter::LINEAR;
		sinit.m_mipmapFilter = SamplingFilter::LINEAR;
		m_samplers.m_trilinearClamp = m_gr->newSampler(sinit);

		sinit.m_addressing = SamplingAddressing::REPEAT;
		m_samplers.m_trilinearRepeat = m_gr->newSampler(sinit);

		sinit.m_anisotropyLevel = m_config->getRTextureAnisotropy();
		m_samplers.m_trilinearRepeatAniso = m_gr->newSampler(sinit);

		F32 scalingMipBias = log2(F32(m_internalResolution.x()) / F32(m_postProcessResolution.x()));
		if(getScale().getUsingGrUpscaler())
		{
			// DLSS wants more bias
			scalingMipBias -= 1.0f;
		}

		sinit.m_lodBias = scalingMipBias;
		m_samplers.m_trilinearRepeatAnisoResolutionScalingBias = m_gr->newSampler(sinit);
	}

	for(U32 i = 0; i < m_jitterOffsets.getSize(); ++i)
	{
		m_jitterOffsets[i] = generateJitter(i);
	}

	return Error::NONE;
}

Error Renderer::populateRenderGraph(RenderingContext& ctx)
{
	ctx.m_prevMatrices = m_prevMatrices;

	ctx.m_matrices.m_cameraTransform = ctx.m_renderQueue->m_cameraTransform;
	ctx.m_matrices.m_view = ctx.m_renderQueue->m_viewMatrix;
	ctx.m_matrices.m_projection = ctx.m_renderQueue->m_projectionMatrix;
	ctx.m_matrices.m_viewProjection = ctx.m_renderQueue->m_viewProjectionMatrix;

	Vec2 jitter = m_jitterOffsets[m_frameCount & (m_jitterOffsets.getSize() - 1)]; // In [-0.5, 0.5]
	const Vec2 ndcPixelSize = 2.0f / Vec2(m_internalResolution);
	jitter *= ndcPixelSize;
	ctx.m_matrices.m_jitter = Mat4::getIdentity();
	ctx.m_matrices.m_jitter.setTranslationPart(Vec4(jitter, 0.0f, 1.0f));

	ctx.m_matrices.m_projectionJitter = ctx.m_matrices.m_jitter * ctx.m_matrices.m_projection;
	ctx.m_matrices.m_viewProjectionJitter =
		ctx.m_matrices.m_projectionJitter * Mat4(ctx.m_matrices.m_view, Vec4(0.0f, 0.0f, 0.0f, 1.0f));
	ctx.m_matrices.m_invertedViewProjectionJitter = ctx.m_matrices.m_viewProjectionJitter.getInverse();
	ctx.m_matrices.m_invertedViewProjection = ctx.m_matrices.m_viewProjection.getInverse();
	ctx.m_matrices.m_invertedProjectionJitter = ctx.m_matrices.m_projectionJitter.getInverse();

	ctx.m_matrices.m_reprojection =
		ctx.m_matrices.m_jitter * ctx.m_prevMatrices.m_viewProjection * ctx.m_matrices.m_invertedViewProjectionJitter;

	ctx.m_matrices.m_unprojectionParameters = ctx.m_matrices.m_projection.extractPerspectiveUnprojectionParams();

	// Check if resources got loaded
	if(m_prevLoadRequestCount != m_resources->getLoadingRequestCount()
	   || m_prevAsyncTasksCompleted != m_resources->getAsyncTaskCompletedCount())
	{
		m_prevLoadRequestCount = m_resources->getLoadingRequestCount();
		m_prevAsyncTasksCompleted = m_resources->getAsyncTaskCompletedCount();
		m_resourcesDirty = true;
	}
	else
	{
		m_resourcesDirty = false;
	}

	// Import RTs first
	m_downscaleBlur->importRenderTargets(ctx);
	m_tonemapping->importRenderTargets(ctx);
	m_depthDownscale->importRenderTargets(ctx);
	m_vrsSriGeneration->importRenderTargets(ctx);

	// Populate render graph. WARNING Watch the order
	m_genericCompute->populateRenderGraph(ctx);
	m_clusterBinning->populateRenderGraph(ctx);
	if(m_accelerationStructureBuilder)
	{
		m_accelerationStructureBuilder->populateRenderGraph(ctx);
	}
	m_shadowMapping->populateRenderGraph(ctx);
	m_indirectDiffuseProbes->populateRenderGraph(ctx);
	m_probeReflections->populateRenderGraph(ctx);
	m_volumetricLightingAccumulation->populateRenderGraph(ctx);
	m_gbuffer->populateRenderGraph(ctx);
	m_motionVectors->populateRenderGraph(ctx);
	m_gbufferPost->populateRenderGraph(ctx);
	m_depthDownscale->populateRenderGraph(ctx);
	if(m_rtShadows)
	{
		m_rtShadows->populateRenderGraph(ctx);
	}
	else
	{
		m_shadowmapsResolve->populateRenderGraph(ctx);
	}
	m_volumetricFog->populateRenderGraph(ctx);
	m_lensFlare->populateRenderGraph(ctx);
	m_indirectSpecular->populateRenderGraph(ctx);
	m_indirectDiffuse->populateRenderGraph(ctx);
	m_lightShading->populateRenderGraph(ctx);
	if(!getScale().getUsingGrUpscaler())
	{
		m_temporalAA->populateRenderGraph(ctx);
	}
	m_vrsSriGeneration->populateRenderGraph(ctx);
	m_scale->populateRenderGraph(ctx);
	m_downscaleBlur->populateRenderGraph(ctx);
	m_tonemapping->populateRenderGraph(ctx);
	m_bloom->populateRenderGraph(ctx);
	m_dbg->populateRenderGraph(ctx);

	m_finalComposite->populateRenderGraph(ctx);

	// Populate the uniforms
	m_clusterBinning->writeClusterBuffersAsync();

	return Error::NONE;
}

void Renderer::finalize(const RenderingContext& ctx)
{
	++m_frameCount;

	m_prevMatrices = ctx.m_matrices;

	// Inform about the HiZ map. Do it as late as possible
	if(ctx.m_renderQueue->m_fillCoverageBufferCallback)
	{
		F32* depthValues;
		U32 width;
		U32 height;
		m_depthDownscale->getClientDepthMapInfo(depthValues, width, height);
		ctx.m_renderQueue->m_fillCoverageBufferCallback(ctx.m_renderQueue->m_fillCoverageBufferCallbackUserData,
														depthValues, width, height);
	}
}

TextureInitInfo Renderer::create2DRenderTargetInitInfo(U32 w, U32 h, Format format, TextureUsageBit usage, CString name)
{
	ANKI_ASSERT(!!(usage & TextureUsageBit::FRAMEBUFFER_ATTACHMENT_WRITE)
				|| !!(usage & TextureUsageBit::IMAGE_COMPUTE_WRITE));
	TextureInitInfo init(name);

	init.m_width = w;
	init.m_height = h;
	init.m_depth = 1;
	init.m_layerCount = 1;
	init.m_type = TextureType::_2D;
	init.m_format = format;
	init.m_mipmapCount = 1;
	init.m_samples = 1;
	init.m_usage = usage;

	return init;
}

RenderTargetDescription Renderer::create2DRenderTargetDescription(U32 w, U32 h, Format format, CString name)
{
	RenderTargetDescription init(name);

	init.m_width = w;
	init.m_height = h;
	init.m_depth = 1;
	init.m_layerCount = 1;
	init.m_type = TextureType::_2D;
	init.m_format = format;
	init.m_mipmapCount = 1;
	init.m_samples = 1;
	init.m_usage = TextureUsageBit::NONE;

	return init;
}

TexturePtr Renderer::createAndClearRenderTarget(const TextureInitInfo& inf, TextureUsageBit initialUsage,
												const ClearValue& clearVal)
{
	ANKI_ASSERT(!!(inf.m_usage & TextureUsageBit::FRAMEBUFFER_ATTACHMENT_WRITE)
				|| !!(inf.m_usage & TextureUsageBit::IMAGE_COMPUTE_WRITE));

	const U faceCount = (inf.m_type == TextureType::CUBE || inf.m_type == TextureType::CUBE_ARRAY) ? 6 : 1;

	Bool useCompute = false;
	if(!!(inf.m_usage & TextureUsageBit::FRAMEBUFFER_ATTACHMENT_WRITE))
	{
		useCompute = false;
	}
	else if(!!(inf.m_usage & TextureUsageBit::IMAGE_COMPUTE_WRITE))
	{
		useCompute = true;
	}
	else
	{
		ANKI_ASSERT(!"Can't handle that");
	}

	// Create tex
	TexturePtr tex = m_gr->newTexture(inf);

	// Clear all surfaces
	CommandBufferInitInfo cmdbinit;
	cmdbinit.m_flags = CommandBufferFlag::GENERAL_WORK;
	if((inf.m_mipmapCount * faceCount * inf.m_layerCount * 4) < COMMAND_BUFFER_SMALL_BATCH_MAX_COMMANDS)
	{
		cmdbinit.m_flags |= CommandBufferFlag::SMALL_BATCH;
	}
	CommandBufferPtr cmdb = m_gr->newCommandBuffer(cmdbinit);

	for(U32 mip = 0; mip < inf.m_mipmapCount; ++mip)
	{
		for(U32 face = 0; face < faceCount; ++face)
		{
			for(U32 layer = 0; layer < inf.m_layerCount; ++layer)
			{
				TextureSurfaceInfo surf(mip, 0, face, layer);

				if(!useCompute)
				{
					FramebufferInitInfo fbInit("RendererClearRT");
					Array<TextureUsageBit, MAX_COLOR_ATTACHMENTS> colUsage = {};
					TextureUsageBit dsUsage = TextureUsageBit::NONE;

					if(getFormatInfo(inf.m_format).isDepthStencil())
					{
						DepthStencilAspectBit aspect = DepthStencilAspectBit::NONE;
						if(getFormatInfo(inf.m_format).isDepth())
						{
							aspect |= DepthStencilAspectBit::DEPTH;
						}

						if(getFormatInfo(inf.m_format).isStencil())
						{
							aspect |= DepthStencilAspectBit::STENCIL;
						}

						TextureViewPtr view = getGrManager().newTextureView(TextureViewInitInfo(tex, surf, aspect));

						fbInit.m_depthStencilAttachment.m_textureView = view;
						fbInit.m_depthStencilAttachment.m_loadOperation = AttachmentLoadOperation::CLEAR;
						fbInit.m_depthStencilAttachment.m_stencilLoadOperation = AttachmentLoadOperation::CLEAR;
						fbInit.m_depthStencilAttachment.m_clearValue = clearVal;

						dsUsage = TextureUsageBit::FRAMEBUFFER_ATTACHMENT_WRITE;
					}
					else
					{
						TextureViewPtr view = getGrManager().newTextureView(TextureViewInitInfo(tex, surf));

						fbInit.m_colorAttachmentCount = 1;
						fbInit.m_colorAttachments[0].m_textureView = view;
						fbInit.m_colorAttachments[0].m_loadOperation = AttachmentLoadOperation::CLEAR;
						fbInit.m_colorAttachments[0].m_clearValue = clearVal;

						colUsage[0] = TextureUsageBit::FRAMEBUFFER_ATTACHMENT_WRITE;
					}
					FramebufferPtr fb = m_gr->newFramebuffer(fbInit);

					cmdb->setTextureSurfaceBarrier(tex, TextureUsageBit::NONE,
												   TextureUsageBit::FRAMEBUFFER_ATTACHMENT_WRITE, surf);

					cmdb->beginRenderPass(fb, colUsage, dsUsage);
					cmdb->endRenderPass();

					if(!!initialUsage)
					{
						cmdb->setTextureSurfaceBarrier(tex, TextureUsageBit::FRAMEBUFFER_ATTACHMENT_WRITE, initialUsage,
													   surf);
					}
				}
				else
				{
					// Compute
					ShaderProgramResourceVariantInitInfo variantInitInfo(m_clearTexComputeProg);
					variantInitInfo.addMutation("TEXTURE_DIMENSIONS", I32((inf.m_type == TextureType::_3D) ? 3 : 2));

					const FormatInfo formatInfo = getFormatInfo(inf.m_format);
					I32 componentType = 0;
					if(formatInfo.m_shaderType == 0)
					{
						componentType = 0;
					}
					else if(formatInfo.m_shaderType == 1)
					{
						componentType = 1;
					}
					else
					{
						ANKI_ASSERT(!"Not supported");
					}
					variantInitInfo.addMutation("COMPONENT_TYPE", componentType);

					const ShaderProgramResourceVariant* variant;
					m_clearTexComputeProg->getOrCreateVariant(variantInitInfo, variant);

					cmdb->bindShaderProgram(variant->getProgram());

					cmdb->setPushConstants(&clearVal.m_colorf[0], sizeof(clearVal.m_colorf));

					TextureViewPtr view = getGrManager().newTextureView(TextureViewInitInfo(tex, surf));
					cmdb->bindImage(0, 0, view);

					cmdb->setTextureSurfaceBarrier(tex, TextureUsageBit::NONE, TextureUsageBit::IMAGE_COMPUTE_WRITE,
												   surf);

					UVec3 wgSize;
					wgSize.x() = (8 - 1 + (tex->getWidth() >> mip)) / 8;
					wgSize.y() = (8 - 1 + (tex->getHeight() >> mip)) / 8;
					wgSize.z() = (inf.m_type == TextureType::_3D) ? ((8 - 1 + (tex->getDepth() >> mip)) / 8) : 1;

					cmdb->dispatchCompute(wgSize.x(), wgSize.y(), wgSize.z());

					if(!!initialUsage)
					{
						cmdb->setTextureSurfaceBarrier(tex, TextureUsageBit::IMAGE_COMPUTE_WRITE, initialUsage, surf);
					}
				}
			}
		}
	}

	cmdb->flush();

	return tex;
}

void Renderer::registerDebugRenderTarget(RendererObject* obj, CString rtName)
{
#if ANKI_ENABLE_ASSERTIONS
	for(const DebugRtInfo& inf : m_debugRts)
	{
		ANKI_ASSERT(inf.m_rtName != rtName && "Choose different name");
	}
#endif

	ANKI_ASSERT(obj);
	DebugRtInfo inf;
	inf.m_obj = obj;
	inf.m_rtName.create(getAllocator(), rtName);

	m_debugRts.emplaceBack(getAllocator(), std::move(inf));
}

void Renderer::getCurrentDebugRenderTarget(RenderTargetHandle& handle, Bool& handleValid,
										   ShaderProgramPtr& optionalShaderProgram)
{
	if(ANKI_LIKELY(m_currentDebugRtName.isEmpty()))
	{
		handleValid = false;
		return;
	}

	RendererObject* obj = nullptr;
	for(const DebugRtInfo& inf : m_debugRts)
	{
		if(inf.m_rtName == m_currentDebugRtName)
		{
			obj = inf.m_obj;
		}
	}
	ANKI_ASSERT(obj);

	obj->getDebugRenderTarget(m_currentDebugRtName, handle, optionalShaderProgram);
	handleValid = true;
}

void Renderer::setCurrentDebugRenderTarget(CString rtName)
{
	m_currentDebugRtName.destroy(getAllocator());

	if(!rtName.isEmpty() && rtName.getLength() > 0)
	{
		m_currentDebugRtName.create(getAllocator(), rtName);
	}
}

Format Renderer::getHdrFormat() const
{
	Format out;
	if(!m_config->getRHighQualityHdr())
	{
		out = Format::B10G11R11_UFLOAT_PACK32;
	}
	else if(m_gr->getDeviceCapabilities().m_unalignedBbpTextureFormats)
	{
		out = Format::R16G16B16_SFLOAT;
	}
	else
	{
		out = Format::R16G16B16A16_SFLOAT;
	}
	return out;
}

Format Renderer::getDepthNoStencilFormat() const
{
	if(ANKI_PLATFORM_MOBILE)
	{
		return Format::X8_D24_UNORM_PACK32;
	}
	else
	{
		return Format::D32_SFLOAT;
	}
}

} // end namespace anki
