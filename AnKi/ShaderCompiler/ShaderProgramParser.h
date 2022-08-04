// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/ShaderCompiler/Common.h>
#include <AnKi/Util/StringList.h>
#include <AnKi/Util/WeakArray.h>
#include <AnKi/Util/DynamicArray.h>

namespace anki {

// Forward
class ShaderProgramParser;
class ShaderProgramParserVariant;

/// @addtogroup shader_compiler
/// @{

/// @memberof ShaderProgramParser
class ShaderProgramParserMutator
{
	friend ShaderProgramParser;

public:
	ShaderProgramParserMutator(GenericMemoryPoolAllocator<U8> alloc)
		: m_name(alloc)
		, m_values(alloc)
	{
	}

	CString getName() const
	{
		return m_name;
	}

	ConstWeakArray<MutatorValue> getValues() const
	{
		return m_values;
	}

private:
	StringAuto m_name;
	DynamicArrayAuto<MutatorValue> m_values;
};

/// @memberof ShaderProgramParser
class ShaderProgramParserMember
{
public:
	StringAuto m_name;
	ShaderVariableDataType m_type;
	U32 m_dependentMutator = MAX_U32;
	MutatorValue m_mutatorValue = 0;

	ShaderProgramParserMember(GenericMemoryPoolAllocator<U8> alloc)
		: m_name(alloc)
	{
	}
};

/// @memberof ShaderProgramParser
class ShaderProgramParserGhostStruct
{
public:
	DynamicArrayAuto<ShaderProgramParserMember> m_members;
	StringAuto m_name;

	ShaderProgramParserGhostStruct(GenericMemoryPoolAllocator<U8> alloc)
		: m_members(alloc)
		, m_name(alloc)
	{
	}
};

/// @memberof ShaderProgramParser
class ShaderProgramParserVariant
{
	friend class ShaderProgramParser;

public:
	~ShaderProgramParserVariant()
	{
		for(String& s : m_sources)
		{
			s.destroy(m_alloc);
		}
	}

	CString getSource(ShaderType type) const
	{
		return m_sources[type];
	}

private:
	GenericMemoryPoolAllocator<U8> m_alloc;
	Array<String, U32(ShaderType::COUNT)> m_sources;
};

/// This is a special preprocessor that run before the usual preprocessor. Its purpose is to add some meta information
/// in the shader programs.
///
/// It supports the following expressions:
/// #include {<> | ""}
/// #pragma once
/// #pragma anki mutator NAME VALUE0 [VALUE1 [VALUE2] ...]
/// #pragma anki start {vert | tessc | tesse | geom | frag | comp | rgen | ahit | chit | miss | int | call}
/// #pragma anki end
/// #pragma anki library "name"
/// #pragma anki ray_type NUMBER
/// #pragma anki reflect NAME
/// #pragma anki skip_mutation MUTATOR0 VALUE0 MUTATOR1 VALUE1 [MUTATOR2 VALUE2 ...]
///
/// #pragma anki struct NAME
/// #	pragma anki member [ANKI_RP] TYPE NAME [if MUTATOR_NAME is MUTATOR_VALUE]
/// 	...
/// #pragma anki struct end
///
/// None of the pragmas should be in an ifdef-like guard. It's ignored.
class ShaderProgramParser
{
public:
	ShaderProgramParser(CString fname, ShaderProgramFilesystemInterface* fsystem, GenericMemoryPoolAllocator<U8> alloc,
						const ShaderCompilerOptions& compilerOptions);

	ShaderProgramParser(const ShaderProgramParser&) = delete; // Non-copyable

	~ShaderProgramParser();

	ShaderProgramParser& operator=(const ShaderProgramParser&) = delete; // Non-copyable

	/// Parse the file and its includes.
	Error parse();

	/// Returns true if the mutation should be skipped.
	Bool skipMutation(ConstWeakArray<MutatorValue> mutation) const;

	/// Get the source (and a few more things) given a list of mutators.
	Error generateVariant(ConstWeakArray<MutatorValue> mutation, ShaderProgramParserVariant& variant) const;

	ConstWeakArray<ShaderProgramParserMutator> getMutators() const
	{
		return m_mutators;
	}

	ShaderTypeBit getShaderTypes() const
	{
		return m_shaderTypes;
	}

	U64 getHash() const
	{
		ANKI_ASSERT(m_codeSourceHash != 0);
		return m_codeSourceHash;
	}

	CString getLibraryName() const
	{
		return m_libName;
	}

	U32 getRayType() const
	{
		return m_rayType;
	}

	const StringListAuto& getSymbolsToReflect() const
	{
		return m_symbolsToReflect;
	}

	ConstWeakArray<ShaderProgramParserGhostStruct> getGhostStructs() const
	{
		return m_ghostStructs;
	}

	/// Generates the common header that will be used by all AnKi shaders.
	static void generateAnkiShaderHeader(ShaderType shaderType, const ShaderCompilerOptions& compilerOptions,
										 StringAuto& header);

private:
	using Mutator = ShaderProgramParserMutator;
	using Member = ShaderProgramParserMember;
	using GhostStruct = ShaderProgramParserGhostStruct;

	class PartialMutationSkip
	{
	public:
		DynamicArrayAuto<MutatorValue> m_partialMutation;

		PartialMutationSkip(const GenericMemoryPoolAllocator<U8>& alloc)
			: m_partialMutation(alloc)
		{
		}
	};

	static constexpr U32 MAX_INCLUDE_DEPTH = 8;

	GenericMemoryPoolAllocator<U8> m_alloc;
	StringAuto m_fname;
	ShaderProgramFilesystemInterface* m_fsystem = nullptr;

	StringListAuto m_codeLines = {m_alloc}; ///< The code.
	StringAuto m_codeSource = {m_alloc};
	U64 m_codeSourceHash = 0;

	DynamicArrayAuto<Mutator> m_mutators = {m_alloc};
	DynamicArrayAuto<PartialMutationSkip> m_skipMutations = {m_alloc};

	ShaderTypeBit m_shaderTypes = ShaderTypeBit::NONE;
	Bool m_insideShader = false;
	ShaderCompilerOptions m_compilerOptions;

	StringAuto m_libName = {m_alloc};
	U32 m_rayType = MAX_U32;

	StringListAuto m_symbolsToReflect = {m_alloc};

	DynamicArrayAuto<GhostStruct> m_ghostStructs = {m_alloc};
	Bool m_insideStruct = false;

	Error parseFile(CString fname, U32 depth);
	Error parseLine(CString line, CString fname, Bool& foundPragmaOnce, U32 depth);
	Error parseInclude(const StringAuto* begin, const StringAuto* end, CString line, CString fname, U32 depth);
	Error parsePragmaMutator(const StringAuto* begin, const StringAuto* end, CString line, CString fname);
	Error parsePragmaStart(const StringAuto* begin, const StringAuto* end, CString line, CString fname);
	Error parsePragmaEnd(const StringAuto* begin, const StringAuto* end, CString line, CString fname);
	Error parsePragmaSkipMutation(const StringAuto* begin, const StringAuto* end, CString line, CString fname);
	Error parsePragmaLibraryName(const StringAuto* begin, const StringAuto* end, CString line, CString fname);
	Error parsePragmaRayType(const StringAuto* begin, const StringAuto* end, CString line, CString fname);
	Error parsePragmaReflect(const StringAuto* begin, const StringAuto* end, CString line, CString fname);
	Error parsePragmaStructBegin(const StringAuto* begin, const StringAuto* end, CString line, CString fname);
	Error parsePragmaStructEnd(const StringAuto* begin, const StringAuto* end, CString line, CString fname);
	Error parsePragmaMember(const StringAuto* begin, const StringAuto* end, CString line, CString fname);

	void tokenizeLine(CString line, DynamicArrayAuto<StringAuto>& tokens) const;

	static Bool tokenIsComment(CString token)
	{
		return token.getLength() >= 2 && token[0] == '/' && (token[1] == '/' || token[1] == '*');
	}

	static Bool mutatorHasValue(const ShaderProgramParserMutator& mutator, MutatorValue value);

	Error checkNoActiveStruct() const
	{
		if(m_insideStruct)
		{
			ANKI_SHADER_COMPILER_LOGE("Unsupported \"pragma anki\" inside \"pragma anki struct\"");
			return Error::USER_DATA;
		}
		return Error::NONE;
	}

	Error checkActiveStruct() const
	{
		if(!m_insideStruct)
		{
			ANKI_SHADER_COMPILER_LOGE("Expected a \"pragma anki struct\" to open");
			return Error::USER_DATA;
		}
		return Error::NONE;
	}
};
/// @}

} // end namespace anki
