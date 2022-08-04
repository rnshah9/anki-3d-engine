// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Renderer/Scale.h>
#include <AnKi/Renderer/Renderer.h>
#include <AnKi/Renderer/TemporalAA.h>
#include <AnKi/Core/ConfigSet.h>

#include <AnKi/Renderer/LightShading.h>
#include <AnKi/Renderer/MotionVectors.h>
#include <AnKi/Renderer/GBuffer.h>
#include <AnKi/Renderer/Tonemapping.h>

#if ANKI_COMPILER_GCC_COMPATIBLE
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wunused-function"
#	pragma GCC diagnostic ignored "-Wignored-qualifiers"
#elif ANKI_COMPILER_MSVC
#	pragma warning(push)
#	pragma warning(disable : 4505)
#endif
#define A_CPU
#include <ThirdParty/FidelityFX/ffx_a.h>
#include <ThirdParty/FidelityFX/ffx_fsr1.h>
#if ANKI_COMPILER_GCC_COMPATIBLE
#	pragma GCC diagnostic pop
#elif ANKI_COMPILER_MSVC
#	pragma warning(pop)
#endif

namespace anki {

Scale::~Scale()
{
}

Error Scale::init()
{
	const Bool needsScaling = m_r->getPostProcessResolution() != m_r->getInternalResolution();
	const Bool needsSharpening = getConfig().getRSharpness() > 0.0f;
	if(!needsScaling && !needsSharpening)
	{
		return Error::NONE;
	}

	const Bool preferCompute = getConfig().getRPreferCompute();
	const U32 dlssQuality = getConfig().getRDlssQuality();
	const U32 fsrQuality = getConfig().getRFsrQuality();

	if(needsScaling)
	{
		if(dlssQuality > 0 && getGrManager().getDeviceCapabilities().m_dlss)
		{
			m_upscalingMethod = UpscalingMethod::GR;
		}
		else if(fsrQuality > 0)
		{
			m_upscalingMethod = UpscalingMethod::FSR;
		}
		else
		{
			m_upscalingMethod = UpscalingMethod::BILINEAR;
		}
	}
	else
	{
		m_upscalingMethod = UpscalingMethod::NONE;
	}

	m_sharpenMethod = (needsSharpening) ? SharpenMethod::RCAS : SharpenMethod::NONE;
	m_neeedsTonemapping = (m_upscalingMethod == UpscalingMethod::GR); // Because GR upscaling spits HDR

	static const Array<const Char*, U32(UpscalingMethod::COUNT)> upscalingMethodNames = {"none", "bilinear", "FSR 1.0",
																						 "DLSS 2"};
	static const Array<const Char*, U32(SharpenMethod::COUNT)> sharpenMethodNames = {"none", "RCAS"};

	ANKI_R_LOGV("Initializing upscaling. Upscaling method %s, sharpenning method %s",
				upscalingMethodNames[U32(m_upscalingMethod)], sharpenMethodNames[U32(m_sharpenMethod)]);

	// Scale programs
	if(m_upscalingMethod == UpscalingMethod::BILINEAR)
	{
		const CString shaderFname =
			(preferCompute) ? "ShaderBinaries/BlitCompute.ankiprogbin" : "ShaderBinaries/BlitRaster.ankiprogbin";

		ANKI_CHECK(getResourceManager().loadResource(shaderFname, m_scaleProg));

		const ShaderProgramResourceVariant* variant;
		m_scaleProg->getOrCreateVariant(variant);
		m_scaleGrProg = variant->getProgram();
	}
	else if(m_upscalingMethod == UpscalingMethod::FSR)
	{
		const CString shaderFname =
			(preferCompute) ? "ShaderBinaries/FsrCompute.ankiprogbin" : "ShaderBinaries/FsrRaster.ankiprogbin";

		ANKI_CHECK(getResourceManager().loadResource(shaderFname, m_scaleProg));

		ShaderProgramResourceVariantInitInfo variantInitInfo(m_scaleProg);
		variantInitInfo.addMutation("SHARPEN", 0);
		variantInitInfo.addMutation("FSR_QUALITY", fsrQuality - 1);
		const ShaderProgramResourceVariant* variant;
		m_scaleProg->getOrCreateVariant(variantInitInfo, variant);
		m_scaleGrProg = variant->getProgram();
	}
	else if(m_upscalingMethod == UpscalingMethod::GR)
	{
		GrUpscalerInitInfo inf;
		inf.m_sourceTextureResolution = m_r->getInternalResolution();
		inf.m_targetTextureResolution = m_r->getPostProcessResolution();
		inf.m_upscalerType = GrUpscalerType::DLSS_2;
		inf.m_qualityMode = GrUpscalerQualityMode(dlssQuality - 1);

		m_grUpscaler = getGrManager().newGrUpscaler(inf);
	}

	// Sharpen programs
	if(m_sharpenMethod == SharpenMethod::RCAS)
	{
		ANKI_CHECK(getResourceManager().loadResource((preferCompute) ? "ShaderBinaries/FsrCompute.ankiprogbin"
																	 : "ShaderBinaries/FsrRaster.ankiprogbin",
													 m_sharpenProg));
		ShaderProgramResourceVariantInitInfo variantInitInfo(m_sharpenProg);
		variantInitInfo.addMutation("SHARPEN", 1);
		variantInitInfo.addMutation("FSR_QUALITY", 0);
		const ShaderProgramResourceVariant* variant;
		m_sharpenProg->getOrCreateVariant(variantInitInfo, variant);
		m_sharpenGrProg = variant->getProgram();
	}

	// Tonemapping programs
	if(m_neeedsTonemapping)
	{
		ANKI_CHECK(getResourceManager().loadResource((preferCompute) ? "ShaderBinaries/TonemapCompute.ankiprogbin"
																	 : "ShaderBinaries/TonemapRaster.ankiprogbin",
													 m_tonemapProg));
		const ShaderProgramResourceVariant* variant;
		m_tonemapProg->getOrCreateVariant(variant);
		m_tonemapGrProg = variant->getProgram();
	}

	// Descriptors
	Format format;
	if(m_upscalingMethod == UpscalingMethod::GR)
	{
		format = m_r->getHdrFormat();
	}
	else if(getGrManager().getDeviceCapabilities().m_unalignedBbpTextureFormats)
	{
		format = Format::R8G8B8_UNORM;
	}
	else
	{
		format = Format::R8G8B8A8_UNORM;
	}

	m_upscaleAndSharpenRtDescr = m_r->create2DRenderTargetDescription(
		m_r->getPostProcessResolution().x(), m_r->getPostProcessResolution().y(), format, "Scaling");
	m_upscaleAndSharpenRtDescr.bake();

	if(m_neeedsTonemapping)
	{
		const Format fmt = (getGrManager().getDeviceCapabilities().m_unalignedBbpTextureFormats)
							   ? Format::R8G8B8_UNORM
							   : Format::R8G8B8A8_UNORM;
		m_tonemapedRtDescr = m_r->create2DRenderTargetDescription(
			m_r->getPostProcessResolution().x(), m_r->getPostProcessResolution().y(), fmt, "Tonemapped");
		m_tonemapedRtDescr.bake();
	}

	m_fbDescr.m_colorAttachmentCount = 1;
	m_fbDescr.bake();

	return Error::NONE;
}

void Scale::populateRenderGraph(RenderingContext& ctx)
{
	if(m_upscalingMethod == UpscalingMethod::NONE && m_sharpenMethod == SharpenMethod::NONE)
	{
		m_runCtx.m_upscaledTonemappedRt = m_r->getTemporalAA().getTonemappedRt();
		m_runCtx.m_upscaledHdrRt = m_r->getTemporalAA().getHdrRt();
		m_runCtx.m_sharpenedRt = m_r->getTemporalAA().getTonemappedRt();
		m_runCtx.m_tonemappedRt = m_r->getTemporalAA().getTonemappedRt();
		return;
	}

	RenderGraphDescription& rgraph = ctx.m_renderGraphDescr;
	const Bool preferCompute = getConfig().getRPreferCompute();

	// Step 1: Upscaling
	if(m_upscalingMethod == UpscalingMethod::GR)
	{
		m_runCtx.m_upscaledHdrRt = rgraph.newRenderTarget(m_upscaleAndSharpenRtDescr);
		m_runCtx.m_upscaledTonemappedRt = {};

		ComputeRenderPassDescription& pass = ctx.m_renderGraphDescr.newComputeRenderPass("DLSS");

		// DLSS says input textures in sampled state and out as storage image
		const TextureUsageBit readUsage = TextureUsageBit::ALL_SAMPLED & TextureUsageBit::ALL_COMPUTE;
		const TextureUsageBit writeUsage = TextureUsageBit::ALL_IMAGE & TextureUsageBit::ALL_COMPUTE;

		pass.newDependency(RenderPassDependency(m_r->getLightShading().getRt(), readUsage));
		pass.newDependency(RenderPassDependency(m_r->getMotionVectors().getMotionVectorsRt(), readUsage));
		pass.newDependency(RenderPassDependency(m_r->getGBuffer().getDepthRt(), readUsage,
												TextureSubresourceInfo(DepthStencilAspectBit::DEPTH)));
		pass.newDependency(RenderPassDependency(m_runCtx.m_upscaledHdrRt, writeUsage));

		pass.setWork([this, &ctx](RenderPassWorkContext& rgraphCtx) {
			runGrUpscaling(ctx, rgraphCtx);
		});
	}
	else if(m_upscalingMethod == UpscalingMethod::FSR || m_upscalingMethod == UpscalingMethod::BILINEAR)
	{
		m_runCtx.m_upscaledTonemappedRt = rgraph.newRenderTarget(m_upscaleAndSharpenRtDescr);
		m_runCtx.m_upscaledHdrRt = {};
		const RenderTargetHandle inRt = m_r->getTemporalAA().getTonemappedRt();
		const RenderTargetHandle outRt = m_runCtx.m_upscaledTonemappedRt;

		if(preferCompute)
		{
			ComputeRenderPassDescription& pass = ctx.m_renderGraphDescr.newComputeRenderPass("Scale");
			pass.newDependency(RenderPassDependency(inRt, TextureUsageBit::SAMPLED_COMPUTE));
			pass.newDependency(RenderPassDependency(outRt, TextureUsageBit::IMAGE_COMPUTE_WRITE));

			pass.setWork([this](RenderPassWorkContext& rgraphCtx) {
				runFsrOrBilinearScaling(rgraphCtx);
			});
		}
		else
		{
			GraphicsRenderPassDescription& pass = ctx.m_renderGraphDescr.newGraphicsRenderPass("Scale");
			pass.setFramebufferInfo(m_fbDescr, {outRt});
			pass.newDependency(RenderPassDependency(inRt, TextureUsageBit::SAMPLED_FRAGMENT));
			pass.newDependency(RenderPassDependency(outRt, TextureUsageBit::FRAMEBUFFER_ATTACHMENT_WRITE));

			pass.setWork([this](RenderPassWorkContext& rgraphCtx) {
				runFsrOrBilinearScaling(rgraphCtx);
			});
		}
	}
	else
	{
		ANKI_ASSERT(m_upscalingMethod == UpscalingMethod::NONE);
		// Pretend that it got scaled
		m_runCtx.m_upscaledTonemappedRt = m_r->getTemporalAA().getTonemappedRt();
		m_runCtx.m_upscaledHdrRt = m_r->getTemporalAA().getHdrRt();
	}

	// Step 2: Tonemapping
	if(m_neeedsTonemapping)
	{
		m_runCtx.m_tonemappedRt = rgraph.newRenderTarget(m_tonemapedRtDescr);
		const RenderTargetHandle inRt = m_runCtx.m_upscaledHdrRt;
		const RenderTargetHandle outRt = m_runCtx.m_tonemappedRt;

		if(preferCompute)
		{
			ComputeRenderPassDescription& pass = ctx.m_renderGraphDescr.newComputeRenderPass("Tonemap");
			pass.newDependency(RenderPassDependency(inRt, TextureUsageBit::SAMPLED_COMPUTE));
			pass.newDependency(RenderPassDependency(outRt, TextureUsageBit::IMAGE_COMPUTE_WRITE));

			pass.setWork([this](RenderPassWorkContext& rgraphCtx) {
				runTonemapping(rgraphCtx);
			});
		}
		else
		{
			GraphicsRenderPassDescription& pass = ctx.m_renderGraphDescr.newGraphicsRenderPass("Sharpen");
			pass.setFramebufferInfo(m_fbDescr, {outRt});
			pass.newDependency(RenderPassDependency(inRt, TextureUsageBit::SAMPLED_FRAGMENT));
			pass.newDependency(RenderPassDependency(outRt, TextureUsageBit::FRAMEBUFFER_ATTACHMENT_WRITE));

			pass.setWork([this](RenderPassWorkContext& rgraphCtx) {
				runTonemapping(rgraphCtx);
			});
		}
	}
	else
	{
		m_runCtx.m_tonemappedRt = m_runCtx.m_upscaledTonemappedRt;
	}

	// Step 3: Sharpenning
	if(m_sharpenMethod == SharpenMethod::RCAS)
	{
		m_runCtx.m_sharpenedRt = rgraph.newRenderTarget(m_upscaleAndSharpenRtDescr);
		const RenderTargetHandle inRt = m_runCtx.m_tonemappedRt;
		const RenderTargetHandle outRt = m_runCtx.m_sharpenedRt;

		if(preferCompute)
		{
			ComputeRenderPassDescription& pass = ctx.m_renderGraphDescr.newComputeRenderPass("Sharpen");
			pass.newDependency(RenderPassDependency(inRt, TextureUsageBit::SAMPLED_COMPUTE));
			pass.newDependency(RenderPassDependency(outRt, TextureUsageBit::IMAGE_COMPUTE_WRITE));

			pass.setWork([this](RenderPassWorkContext& rgraphCtx) {
				runRcasSharpening(rgraphCtx);
			});
		}
		else
		{
			GraphicsRenderPassDescription& pass = ctx.m_renderGraphDescr.newGraphicsRenderPass("Sharpen");
			pass.setFramebufferInfo(m_fbDescr, {outRt});
			pass.newDependency(RenderPassDependency(inRt, TextureUsageBit::SAMPLED_FRAGMENT));
			pass.newDependency(RenderPassDependency(outRt, TextureUsageBit::FRAMEBUFFER_ATTACHMENT_WRITE));

			pass.setWork([this](RenderPassWorkContext& rgraphCtx) {
				runRcasSharpening(rgraphCtx);
			});
		}
	}
	else
	{
		ANKI_ASSERT(m_sharpenMethod == SharpenMethod::NONE);
		// Pretend that it's sharpened
		m_runCtx.m_sharpenedRt = m_runCtx.m_tonemappedRt;
	}
}

void Scale::runFsrOrBilinearScaling(RenderPassWorkContext& rgraphCtx)
{
	CommandBufferPtr& cmdb = rgraphCtx.m_commandBuffer;
	const Bool preferCompute = getConfig().getRPreferCompute();
	const RenderTargetHandle inRt = m_r->getTemporalAA().getTonemappedRt();
	const RenderTargetHandle outRt = m_runCtx.m_upscaledTonemappedRt;

	cmdb->bindShaderProgram(m_scaleGrProg);

	cmdb->bindSampler(0, 0, m_r->getSamplers().m_trilinearClamp);
	rgraphCtx.bindColorTexture(0, 1, inRt);

	if(preferCompute)
	{
		rgraphCtx.bindImage(0, 2, outRt);
	}

	if(m_upscalingMethod == UpscalingMethod::FSR)
	{
		class
		{
		public:
			UVec4 m_fsrConsts0;
			UVec4 m_fsrConsts1;
			UVec4 m_fsrConsts2;
			UVec4 m_fsrConsts3;
			UVec2 m_viewportSize;
			UVec2 m_padding;
		} pc;

		const Vec2 inRez(m_r->getInternalResolution());
		const Vec2 outRez(m_r->getPostProcessResolution());
		FsrEasuCon(&pc.m_fsrConsts0[0], &pc.m_fsrConsts1[0], &pc.m_fsrConsts2[0], &pc.m_fsrConsts3[0], inRez.x(),
				   inRez.y(), inRez.x(), inRez.y(), outRez.x(), outRez.y());

		pc.m_viewportSize = m_r->getPostProcessResolution();

		cmdb->setPushConstants(&pc, sizeof(pc));
	}
	else if(preferCompute)
	{
		class
		{
		public:
			Vec2 m_viewportSize;
			UVec2 m_viewportSizeU;
		} pc;
		pc.m_viewportSize = Vec2(m_r->getPostProcessResolution());
		pc.m_viewportSizeU = m_r->getPostProcessResolution();

		cmdb->setPushConstants(&pc, sizeof(pc));
	}

	if(preferCompute)
	{
		dispatchPPCompute(cmdb, 8, 8, m_r->getPostProcessResolution().x(), m_r->getPostProcessResolution().y());
	}
	else
	{
		cmdb->setViewport(0, 0, m_r->getPostProcessResolution().x(), m_r->getPostProcessResolution().y());
		cmdb->drawArrays(PrimitiveTopology::TRIANGLES, 3);
	}
}

void Scale::runRcasSharpening(RenderPassWorkContext& rgraphCtx)
{
	CommandBufferPtr& cmdb = rgraphCtx.m_commandBuffer;
	const Bool preferCompute = getConfig().getRPreferCompute();
	const RenderTargetHandle inRt = m_runCtx.m_tonemappedRt;
	const RenderTargetHandle outRt = m_runCtx.m_sharpenedRt;

	cmdb->bindShaderProgram(m_sharpenGrProg);

	cmdb->bindSampler(0, 0, m_r->getSamplers().m_trilinearClamp);
	rgraphCtx.bindColorTexture(0, 1, inRt);

	if(preferCompute)
	{
		rgraphCtx.bindImage(0, 2, outRt);
	}

	class
	{
	public:
		UVec4 m_fsrConsts0;
		UVec4 m_fsrConsts1;
		UVec4 m_fsrConsts2;
		UVec4 m_fsrConsts3;
		UVec2 m_viewportSize;
		UVec2 m_padding;
	} pc;

	F32 sharpness = getConfig().getRSharpness(); // [0, 1]
	sharpness *= 3.0f; // [0, 3]
	sharpness = 3.0f - sharpness; // [3, 0], RCAS translates 0 to max sharpness
	FsrRcasCon(&pc.m_fsrConsts0[0], sharpness);

	pc.m_viewportSize = m_r->getPostProcessResolution();

	cmdb->setPushConstants(&pc, sizeof(pc));

	if(preferCompute)
	{
		dispatchPPCompute(cmdb, 8, 8, m_r->getPostProcessResolution().x(), m_r->getPostProcessResolution().y());
	}
	else
	{
		cmdb->setViewport(0, 0, m_r->getPostProcessResolution().x(), m_r->getPostProcessResolution().y());
		cmdb->drawArrays(PrimitiveTopology::TRIANGLES, 3);
	}
}

void Scale::runGrUpscaling(RenderingContext& ctx, RenderPassWorkContext& rgraphCtx)
{
	const Vec2 srcRes(m_r->getInternalResolution());
	const Bool reset = m_r->getFrameCount() == 0;
	const Vec2 mvScale = srcRes; // UV space to Pixel space factor
	// In [-texSize / 2, texSize / 2] -> sub-pixel space {-0.5, 0.5}
	const Vec2 jitterOffset = ctx.m_matrices.m_jitter.getTranslationPart().xy() * srcRes * 0.5f;

	CommandBufferPtr& cmdb = rgraphCtx.m_commandBuffer;

	TextureViewPtr srcView = rgraphCtx.createTextureView(m_r->getLightShading().getRt());
	TextureViewPtr motionVectorsView = rgraphCtx.createTextureView(m_r->getMotionVectors().getMotionVectorsRt());
	TextureViewPtr depthView = rgraphCtx.createTextureView(m_r->getGBuffer().getDepthRt());
	TextureViewPtr exposureView = rgraphCtx.createTextureView(m_r->getTonemapping().getRt());
	TextureViewPtr dstView = rgraphCtx.createTextureView(m_runCtx.m_upscaledHdrRt);

	cmdb->upscale(m_grUpscaler, srcView, dstView, motionVectorsView, depthView, exposureView, reset, jitterOffset,
				  mvScale);
}

void Scale::runTonemapping(RenderPassWorkContext& rgraphCtx)
{
	CommandBufferPtr& cmdb = rgraphCtx.m_commandBuffer;
	const Bool preferCompute = getConfig().getRPreferCompute();
	const RenderTargetHandle inRt = m_runCtx.m_upscaledHdrRt;
	const RenderTargetHandle outRt = m_runCtx.m_tonemappedRt;

	cmdb->bindShaderProgram(m_tonemapGrProg);

	cmdb->bindSampler(0, 0, m_r->getSamplers().m_nearestNearestClamp);
	rgraphCtx.bindColorTexture(0, 1, inRt);

	rgraphCtx.bindImage(0, 2, m_r->getTonemapping().getRt());


	if(preferCompute)
	{
		class
		{
		public:
			Vec2 m_viewportSizeOverOne;
			UVec2 m_viewportSize;
		} pc;
		pc.m_viewportSizeOverOne = 1.0f / Vec2(m_r->getPostProcessResolution());
		pc.m_viewportSize = m_r->getPostProcessResolution();
		cmdb->setPushConstants(&pc, sizeof(pc));
		rgraphCtx.bindImage(0, 3, outRt);

		dispatchPPCompute(cmdb, 8, 8, m_r->getPostProcessResolution().x(), m_r->getPostProcessResolution().y());
	}
	else
	{
		cmdb->setViewport(0, 0, m_r->getPostProcessResolution().x(), m_r->getPostProcessResolution().y());
		cmdb->drawArrays(PrimitiveTopology::TRIANGLES, 3);
	}
}

} // end namespace anki
