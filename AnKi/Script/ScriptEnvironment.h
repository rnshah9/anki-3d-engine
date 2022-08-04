// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Script/LuaBinder.h>

namespace anki {

/// @addtogroup script
/// @{

/// A sandboxed LUA environment.
class ScriptEnvironment
{
public:
	ScriptEnvironment()
	{
	}

	~ScriptEnvironment()
	{
	}

	Error init(ScriptManager* manager);

	Bool isInitialized() const
	{
		return m_manager != nullptr;
	}

	/// Expose a variable to the scripting engine.
	template<typename T>
	void exposeVariable(const char* name, T* y)
	{
		ANKI_ASSERT(isInitialized());
		LuaBinder::exposeVariable<T>(m_thread.getLuaState(), name, y);
	}

	/// Evaluate a string
	Error evalString(const CString& str)
	{
		ANKI_ASSERT(isInitialized());
		return LuaBinder::evalString(m_thread.getLuaState(), str);
	}

	void serializeGlobals(LuaBinderSerializeGlobalsCallback& callback)
	{
		ANKI_ASSERT(isInitialized());
		LuaBinder::serializeGlobals(m_thread.getLuaState(), callback);
	}

	void deserializeGlobals(const void* data, PtrSize dataSize)
	{
		ANKI_ASSERT(isInitialized());
		LuaBinder::deserializeGlobals(m_thread.getLuaState(), data, dataSize);
	}

	lua_State& getLuaState()
	{
		ANKI_ASSERT(isInitialized());
		return *m_thread.getLuaState();
	}

private:
	ScriptManager* m_manager = nullptr;
	LuaBinder m_thread;
};
/// @}

} // end namespace anki
