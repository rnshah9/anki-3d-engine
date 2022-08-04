// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#pragma once

#include <AnKi/Util/Assert.h>
#include <AnKi/Util/List.h>
#include <AnKi/Util/StdTypes.h>
#include <AnKi/Util/Allocator.h>
#include <algorithm>

namespace anki {

/// @addtogroup util_patterns
/// @{

/// A hierarchical object
template<typename T>
class Hierarchy
{
public:
	using Value = T;
	using Container = List<Value*>;

	Hierarchy()
		: m_parent(nullptr)
	{
	}

	Hierarchy(const Hierarchy&) = delete; // Non-copyable

	/// Delete children from the last entered to the first and update parent
	virtual ~Hierarchy()
	{
		ANKI_ASSERT(m_parent == nullptr && m_children.isEmpty() && "Requires manual desruction");
	}

	Hierarchy& operator=(const Hierarchy&) = delete; // Non-copyable

	template<typename TAllocator>
	void destroy(TAllocator alloc);

	const Value* getParent() const
	{
		return m_parent;
	}

	Value* getParent()
	{
		return m_parent;
	}

	Value& getChild(PtrSize i)
	{
		return *(*(m_children.getBegin() + i));
	}

	const Value& getChild(PtrSize i) const
	{
		return *(*(m_children.getBegin() + i));
	}

	/// Add a new child.
	template<typename TAllocator>
	void addChild(TAllocator alloc, Value* child);

	/// Remove a child.
	template<typename TAllocator>
	void removeChild(TAllocator alloc, Value* child);

	/// Visit the children and the children's children. Use it with lambda
	template<typename VisitorFunc>
	Error visitChildren(VisitorFunc vis);

	/// Visit this object and move to the children. Use it with lambda
	template<typename VisitorFunc>
	Error visitThisAndChildren(VisitorFunc vis);

	/// Visit the whole tree. Use it with lambda
	template<typename VisitorFunc>
	Error visitTree(VisitorFunc vis);

	/// Visit the children and limit the depth. Use it with lambda.
	template<typename VisitorFunc>
	Error visitChildrenMaxDepth(I maxDepth, VisitorFunc vis);

private:
	Value* m_parent; ///< May be nullptr
	Container m_children;

	/// Cast the Hierarchy to the given type
	Value* getSelf()
	{
		return static_cast<Value*>(this);
	}

	/// Find the child
	typename Container::Iterator findChild(Value* child)
	{
		typename Container::Iterator it = m_children.find(child);
		return it;
	}
};
/// @}

} // end namespace anki

#include <AnKi/Util/Hierarchy.inl.h>
