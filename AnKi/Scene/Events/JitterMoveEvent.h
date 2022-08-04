// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Scene/Events/Event.h>
#include <AnKi/Math.h>

namespace anki {

/// @addtogroup scene
/// @{

/// An event for simple movable animations
class JitterMoveEvent : public Event
{
public:
	/// Constructor
	JitterMoveEvent(EventManager* manager)
		: Event(manager)
	{
	}

	Error init(Second startTime, Second duration, SceneNode* movableSceneNode);

	/// Implements Event::update
	Error update(Second prevUpdateTime, Second crntTime);

	void setPositionLimits(const Vec4& posMin, const Vec4& posMax);

private:
	Vec4 m_originalPos;
	Vec4 m_newPos;
};
/// @}

} // end namespace anki
