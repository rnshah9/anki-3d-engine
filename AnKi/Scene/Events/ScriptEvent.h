// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Scene/Events/Event.h>
#include <AnKi/Resource/Forward.h>
#include <AnKi/Script/ScriptEnvironment.h>

namespace anki {

/// @addtogroup scene
/// @{

/// A generic event that uses scripts. The script should contain something like this:
/// @code
/// function update(event, prevTime, crntTime)
/// 	-- Do something
/// 	return 1
/// end
///
/// function onKilled(event, prevTime, crntTime)
/// 	-- Do something
/// 	return 1
/// end
/// @endcode
class ScriptEvent : public Event
{
public:
	ScriptEvent(EventManager* manager);

	~ScriptEvent();

	/// @param script It's a script or a filename to a script.
	Error init(Second startTime, Second duration, CString script);

	Error update(Second prevUpdateTime, Second crntTime) override;

	Error onKilled(Second prevUpdateTime, Second crntTime) override;

private:
	ScriptResourcePtr m_scriptRsrc;
	String m_script;
	ScriptEnvironment m_env;
};
/// @}

} // end namespace anki
