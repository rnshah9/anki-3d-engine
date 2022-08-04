// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Gr.h>
#include <AnKi/ShaderCompiler.h>
#include <AnKi/ShaderCompiler/ShaderProgramParser.h>
#include <AnKi/ShaderCompiler/Glslang.h>
#include <Tests/Framework/Framework.h>

namespace anki {

inline ShaderPtr createShader(CString src, ShaderType type, GrManager& gr,
							  ConstWeakArray<ShaderSpecializationConstValue> specVals = {})
{
	HeapAllocator<U8> alloc(allocAligned, nullptr);
	StringAuto header(alloc);
	ShaderCompilerOptions compilerOptions;
	ShaderProgramParser::generateAnkiShaderHeader(type, compilerOptions, header);
	header.append(src);
	DynamicArrayAuto<U8> spirv(alloc);
	StringAuto errorLog(alloc);
	ANKI_TEST_EXPECT_NO_ERR(compilerGlslToSpirv(header, type, alloc, spirv, errorLog));

	ShaderInitInfo initInf(type, spirv);
	initInf.m_constValues = specVals;

	return gr.newShader(initInf);
}

} // end namespace anki
