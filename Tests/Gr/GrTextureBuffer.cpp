// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <Tests/Framework/Framework.h>
#include <Tests/Gr/GrCommon.h>
#include <AnKi/Gr.h>

ANKI_TEST(Gr, TextureBuffer)
{
	ConfigSet cfg(allocAligned, nullptr);
	cfg.setGrValidation(true);

	NativeWindow* win = createWindow(cfg);
	GrManager* gr = createGrManager(&cfg, win);

	{
		const CString shaderSrc = R"(
layout(binding = 0) uniform textureBuffer u_tbuff;
layout(binding = 1) buffer b_buff
{
	Vec4 u_buff[];
};

void main()
{
	u_buff[0] = texelFetch(u_tbuff, I32(gl_GlobalInvocationID.x));
}
	)";

		ShaderPtr shader = createShader(shaderSrc, ShaderType::COMPUTE, *gr);

		ShaderProgramInitInfo progInit;
		progInit.m_computeShader = shader;
		ShaderProgramPtr prog = gr->newShaderProgram(progInit);

		BufferInitInfo buffInit;
		buffInit.m_mapAccess = BufferMapAccessBit::WRITE;
		buffInit.m_size = sizeof(U8) * 4;
		buffInit.m_usage = BufferUsageBit::ALL_TEXTURE;
		BufferPtr texBuff = gr->newBuffer(buffInit);

		I8* data = static_cast<I8*>(texBuff->map(0, MAX_PTR_SIZE, BufferMapAccessBit::WRITE));
		const Vec4 values(-1.0f, -0.25f, 0.1345f, 0.8952f);
		for(U i = 0; i < 4; ++i)
		{
			data[i] = I8(values[i] * 127.0f);
		}

		texBuff->unmap();

		buffInit.m_mapAccess = BufferMapAccessBit::READ;
		buffInit.m_size = sizeof(F32) * 4;
		buffInit.m_usage = BufferUsageBit::ALL_STORAGE;
		BufferPtr storageBuff = gr->newBuffer(buffInit);

		CommandBufferInitInfo cmdbInit;
		cmdbInit.m_flags = CommandBufferFlag::SMALL_BATCH | CommandBufferFlag::GENERAL_WORK;
		CommandBufferPtr cmdb = gr->newCommandBuffer(cmdbInit);

		cmdb->bindReadOnlyTextureBuffer(0, 0, texBuff, 0, MAX_PTR_SIZE, Format::R8G8B8A8_SNORM);
		cmdb->bindStorageBuffer(0, 1, storageBuff, 0, MAX_PTR_SIZE);
		cmdb->bindShaderProgram(prog);
		cmdb->dispatchCompute(1, 1, 1);
		cmdb->flush();
		gr->finish();

		const Vec4* inData = static_cast<const Vec4*>(storageBuff->map(0, MAX_PTR_SIZE, BufferMapAccessBit::READ));
		for(U i = 0; i < 4; ++i)
		{
			ANKI_TEST_EXPECT_NEAR(values[i], (*inData)[i], 0.01f);
		}

		storageBuff->unmap();
	}

	GrManager::deleteInstance(gr);
	NativeWindow::deleteInstance(win);
}
