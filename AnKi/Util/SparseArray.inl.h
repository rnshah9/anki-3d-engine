// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Util/SparseArray.h>

namespace anki {

template<typename T, typename TIndex>
template<typename TAlloc>
void SparseArray<T, TIndex>::destroy(TAlloc& alloc)
{
	if(m_elements)
	{
		for(Index i = 0; i < m_capacity; ++i)
		{
			if(m_metadata[i].m_alive)
			{
				destroyElement(m_elements[i]);
			}
		}

		alloc.deallocate(m_elements, m_capacity);

		ANKI_ASSERT(m_metadata);
		alloc.deallocate(m_metadata, m_capacity);
	}

	resetMembers();
}

template<typename T, typename TIndex>
template<typename TAlloc, typename... TArgs>
void SparseArray<T, TIndex>::emplaceInternal(TAlloc& alloc, Index idx, TArgs&&... args)
{
	if(m_capacity == 0 || calcLoadFactor() > m_maxLoadFactor)
	{
		grow(alloc);
	}

	Value tmp(std::forward<TArgs>(args)...);
	m_elementCount += insert(alloc, idx, tmp);

	invalidateIterators();
}

template<typename T, typename TIndex>
template<typename TAlloc, typename... TArgs>
typename SparseArray<T, TIndex>::Iterator SparseArray<T, TIndex>::emplace(TAlloc& alloc, Index idx, TArgs&&... args)
{
	emplaceInternal(alloc, idx, std::forward<TArgs>(args)...);

	return Iterator(this, findInternal(idx)
#if ANKI_EXTRA_CHECKS
							  ,
					m_iteratorVer
#endif
	);
}

template<typename T, typename TIndex>
template<typename TAlloc>
TIndex SparseArray<T, TIndex>::insert(TAlloc& alloc, Index idx, Value& val)
{
	while(true)
	{
		const Index desiredPos = mod(idx);
		const Index endPos = mod(desiredPos + m_probeCount);
		Index pos = desiredPos;

		while(pos != endPos)
		{
			Metadata& meta = m_metadata[pos];
			Value& crntVal = m_elements[pos];

			if(!meta.m_alive)
			{
				// Empty slot was found, construct in-place

				meta.m_alive = true;
				meta.m_idx = idx;
				alloc.construct(&crntVal, std::move(val));

				return 1;
			}
			else if(meta.m_idx == idx)
			{
				// Same index was found, replace

				meta.m_idx = idx;
				destroyElement(crntVal);
				alloc.construct(&crntVal, std::move(val));

				return 0;
			}

			// Do the robin-hood
			const Index otherDesiredPos = mod(meta.m_idx);
			if(distanceFromDesired(pos, otherDesiredPos) < distanceFromDesired(pos, desiredPos))
			{
				// Swap
				std::swap(val, crntVal);
				std::swap(idx, meta.m_idx);
				break;
			}

			pos = mod(pos + 1u);
		}

		if(pos == endPos)
		{
			// Didn't found an empty place, need to grow and try again
			grow(alloc);
		}
	}

	return 0;
}

template<typename T, typename TIndex>
template<typename TAlloc>
void SparseArray<T, TIndex>::grow(TAlloc& alloc)
{
	if(m_capacity == 0)
	{
		ANKI_ASSERT(m_elementCount == 0);
		m_capacity = m_initialStorageSize;
		m_elements = static_cast<Value*>(alloc.getMemoryPool().allocate(m_capacity * sizeof(Value), alignof(Value)));

		m_metadata =
			static_cast<Metadata*>(alloc.getMemoryPool().allocate(m_capacity * sizeof(Metadata), alignof(Metadata)));

		memset(m_metadata, 0, m_capacity * sizeof(Metadata));

		return;
	}

	// Allocate new storage
	Value* const oldElements = m_elements;
	Metadata* const oldMetadata = m_metadata;
	const Index oldCapacity = m_capacity;
	[[maybe_unused]] const Index oldElementCount = m_elementCount;

	m_capacity *= 2;
	m_elements = static_cast<Value*>(alloc.getMemoryPool().allocate(m_capacity * sizeof(Value), alignof(Value)));
	m_metadata =
		static_cast<Metadata*>(alloc.getMemoryPool().allocate(m_capacity * sizeof(Metadata), alignof(Metadata)));
	memset(m_metadata, 0, m_capacity * sizeof(Metadata));
	m_elementCount = 0;

	// Find from where we start
	Index startPos = ~Index(0);
	for(Index i = 0; i < oldCapacity; ++i)
	{
		if(oldMetadata[i].m_alive)
		{
			const Index desiredPos = mod(oldMetadata[i].m_idx, oldCapacity);
			if(desiredPos <= i)
			{
				startPos = i;
				break;
			}
		}
	}
	ANKI_ASSERT(startPos != ~Index(0));

	// Start re-inserting
	Index count = oldCapacity;
	Index pos = startPos;
	while(count--)
	{
		if(oldMetadata[pos].m_alive)
		{
			Index c = insert(alloc, oldMetadata[pos].m_idx, oldElements[pos]);
			ANKI_ASSERT(c > 0);
			m_elementCount += c;

			// The element was moved to a new storage so we need to destroy the original
			destroyElement(oldElements[pos]);
		}

		pos = mod(pos + 1, oldCapacity);
	}

	ANKI_ASSERT(oldElementCount == m_elementCount);

	// Finalize
	alloc.getMemoryPool().free(oldElements);
	alloc.getMemoryPool().free(oldMetadata);
}

template<typename T, typename TIndex>
template<typename TAlloc>
void SparseArray<T, TIndex>::erase(TAlloc& alloc, Iterator it)
{
	ANKI_ASSERT(it.m_array == this);
	ANKI_ASSERT(it.m_elementIdx != getMaxNumericLimit<Index>());
	ANKI_ASSERT(it.m_iteratorVer == m_iteratorVer);
	ANKI_ASSERT(m_elementCount > 0);

	const Index pos = it.m_elementIdx;
	ANKI_ASSERT(pos < m_capacity);
	ANKI_ASSERT(m_metadata[pos].m_alive);

	// Shift elements
	Index crntPos; // Also the one that will get deleted
	Index nextPos = pos;
	while(true)
	{
		crntPos = nextPos;
		nextPos = mod(nextPos + 1);

		Metadata& crntMeta = m_metadata[crntPos];
		Metadata& nextMeta = m_metadata[nextPos];
		Value& crntEl = m_elements[crntPos];
		Value& nextEl = m_elements[nextPos];

		if(!nextMeta.m_alive)
		{
			// On gaps, stop
			break;
		}

		const Index nextDesiredPos = mod(nextMeta.m_idx);
		if(nextDesiredPos == nextPos)
		{
			// The element is where it want's to be, stop
			break;
		}

		// Shift left
		std::swap(crntEl, nextEl);
		crntMeta.m_idx = nextMeta.m_idx;
	}

	// Delete the element in the given pos
	destroyElement(m_elements[crntPos]);
	m_metadata[crntPos].m_alive = false;
	--m_elementCount;

	// If you erased everything destroy the storage
	if(m_elementCount == 0)
	{
		destroy(alloc);
	}

	invalidateIterators();
}

template<typename T, typename TIndex>
void SparseArray<T, TIndex>::validate() const
{
	if(m_capacity == 0)
	{
		ANKI_ASSERT(m_elementCount == 0 && m_elements == nullptr && m_metadata == nullptr);
		return;
	}

	ANKI_ASSERT(m_elementCount < m_capacity);

	// Find from where we start
	Index startPos = ~Index(0);
	for(Index i = 0; i < m_capacity; ++i)
	{
		if(m_metadata[i].m_alive)
		{
			const Index desiredPos = mod(m_metadata[i].m_idx);
			if(desiredPos <= i)
			{
				startPos = i;
				break;
			}
		}
	}

	// Start iterating
	U elementCount = 0;
	Index count = m_capacity;
	Index pos = startPos;
	Index prevPos = ~Index(0);
	while(count--)
	{
		if(m_metadata[pos].m_alive)
		{
			[[maybe_unused]] const Index myDesiredPos = mod(m_metadata[pos].m_idx);
			ANKI_ASSERT(distanceFromDesired(pos, myDesiredPos) < m_probeCount);

			if(prevPos != ~Index(0))
			{
				[[maybe_unused]] Index prevDesiredPos = mod(m_metadata[prevPos].m_idx);
				ANKI_ASSERT(myDesiredPos >= prevDesiredPos);
			}

			++elementCount;
			prevPos = pos;
		}
		else
		{
			prevPos = ~Index(0);
		}

		pos = mod(pos + 1);
	}

	ANKI_ASSERT(m_elementCount == elementCount);
}

template<typename T, typename TIndex>
TIndex SparseArray<T, TIndex>::findInternal(Index idx) const
{
	if(ANKI_UNLIKELY(m_elementCount == 0))
	{
		return getMaxNumericLimit<Index>();
	}

	const Index desiredPos = mod(idx);
	const Index endPos = mod(desiredPos + m_probeCount);
	Index pos = desiredPos;
	while(pos != endPos)
	{
		if(m_metadata[pos].m_alive && m_metadata[pos].m_idx == idx)
		{
			return pos;
		}

		pos = mod(pos + 1);
	}

	return getMaxNumericLimit<Index>();
}

template<typename T, typename TIndex>
template<typename TAlloc>
void SparseArray<T, TIndex>::clone(TAlloc& alloc, SparseArray& b) const
{
	ANKI_ASSERT(b.m_elements == nullptr && b.m_metadata == nullptr);
	if(m_capacity == 0)
	{
		return;
	}

	// Allocate memory
	b.m_elements = static_cast<Value*>(alloc.getMemoryPool().allocate(m_capacity * sizeof(Value), alignof(Value)));
	b.m_metadata =
		static_cast<Metadata*>(alloc.getMemoryPool().allocate(m_capacity * sizeof(Metadata), alignof(Metadata)));
	memcpy(b.m_metadata, m_metadata, m_capacity * sizeof(Metadata));

	for(U i = 0; i < m_capacity; ++i)
	{
		if(m_metadata[i].m_alive)
		{
			::new(&b.m_elements[i]) Value(m_elements[i]);
		}
	}

	// Set the rest
	b.m_elementCount = m_elementCount;
	b.m_capacity = m_capacity;
	b.m_initialStorageSize = m_initialStorageSize;
	b.m_probeCount = m_probeCount;
	b.m_maxLoadFactor = m_maxLoadFactor;
	b.invalidateIterators();
}

} // end namespace anki
