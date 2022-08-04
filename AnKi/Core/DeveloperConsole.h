// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Ui.h>
#include <AnKi/Core/Common.h>
#include <AnKi/Util/List.h>
#include <AnKi/Script/ScriptEnvironment.h>

namespace anki {

/// @addtogroup core
/// @{

/// Developer console UI.
class DeveloperConsole : public UiImmediateModeBuilder
{
public:
	DeveloperConsole(UiManager* ui)
		: UiImmediateModeBuilder(ui)
	{
	}

	~DeveloperConsole();

	Error init(AllocAlignedCallback allocCb, void* allocCbUserData, ScriptManager* scriptManager);

	void build(CanvasPtr ctx) override;

private:
	static constexpr U MAX_LOG_ITEMS = 64;

	class LogItem : public IntrusiveListEnabled<LogItem>
	{
	public:
		const char* m_file;
		const char* m_func;
		const char* m_subsystem;
		String m_msg;
		I32 m_line;
		LoggerMessageType m_type;
	};

	HeapAllocator<U8> m_alloc;
	FontPtr m_font;
	IntrusiveList<LogItem> m_logItems;
	U32 m_logItemCount = 0;
	Array<char, 256> m_inputText;

	Atomic<U32> m_logItemsTimestamp = {1};
	U32 m_logItemsTimestampConsumed = 0;

	ScriptEnvironment m_scriptEnv;

	void newLogItem(const LoggerMessageInfo& inf);

	static void loggerCallback(void* userData, const LoggerMessageInfo& info)
	{
		static_cast<DeveloperConsole*>(userData)->newLogItem(info);
	}
};
/// @}

} // end namespace anki
