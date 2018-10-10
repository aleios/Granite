/* Copyright (c) 2017-2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include "intrusive_list.hpp"
#include "hash.hpp"
#include "object_pool.hpp"
#include "read_write_lock.hpp"
#include <vector>
#include <assert.h>

namespace Util
{
template <typename T>
struct IntrusiveHashMapEnabled : IntrusiveListEnabled<T>
{
	Hash intrusive_hashmap_key;
};

template <typename T>
struct IntrusivePODWrapper : public IntrusiveHashMapEnabled<IntrusivePODWrapper<T>>
{
	template <typename U>
	IntrusivePODWrapper(U&& value_)
		: value(std::forward<U>(value_))
	{
	}

	T& get()
	{
		return value;
	}

	const T& get() const
	{
		return value;
	}

	T value;
};

// This HashMap is non-owning. It just arranges a list of pointers.
// It's kind of special purpose container used by the Vulkan backend.
// Dealing with memory ownership is done through composition by a different class.
// T must inherit from IntrusiveHashMapEnabled<T>.
// Each instance of T can only be part of one hashmap.

template <typename T>
class IntrusiveHashMapHolder
{
public:
	enum { InitialSize = 16, InitialLoadCount = 4 };

	T *find(Hash hash) const
	{
		if (values.empty())
			return nullptr;

		auto masked = hash & hash_mask;
		for (unsigned i = 0; i < load_count; i++)
		{
			if (values[masked] && get_hash(values[masked]) == hash)
				return values[masked];
			masked = (masked + 1) & hash_mask;
		}

		return nullptr;
	}

	// Inserts, if value already exists, insertion does not happen.
	// Return value is the data which is not part of the hashmap.
	// It should be deleted or similar.
	// Returns nullptr if nothing was in the hashmap for this key.
	T *insert_yield(T *&value)
	{
		if (values.empty())
			grow();

		auto hash = get_hash(value);
		auto masked = hash & hash_mask;

		for (unsigned i = 0; i < load_count; i++)
		{
			if (values[masked] && get_hash(values[masked]) == hash)
			{
				T *ret = value;
				value = values[masked];
				return ret;
			}
			else if (!values[masked])
			{
				values[masked] = value;
				list.insert_front(value);
				count++;
				return nullptr;
			}
			masked = (masked + 1) & hash_mask;
		}

		grow();
		return insert_yield(value);
	}

	T *insert_replace(T *value)
	{
		if (values.empty())
			grow();

		auto hash = get_hash(value);
		auto masked = hash & hash_mask;

		for (unsigned i = 0; i < load_count; i++)
		{
			if (values[masked] && get_hash(values[masked]) == hash)
			{
				std::swap(values[masked], value);
				list.erase(value);
				list.insert_front(values[masked]);
				return value;
			}
			else if (!values[masked])
			{
				assert(!values[masked]);
				values[masked] = value;
				list.insert_front(value);
				count++;
				return nullptr;
			}
			masked = (masked + 1) & hash_mask;
		}

		grow();
		return insert_replace(value);
	}

	void erase(T *value)
	{
		auto hash = get_hash(value);
		auto masked = hash & hash_mask;

		for (unsigned i = 0; i < load_count; i++)
		{
			if (values[masked] && get_hash(values[masked]) == hash)
			{
				assert(values[masked] == value);
				assert(count > 0);
				values[masked] = nullptr;
				list.erase(value);
				count--;
				return;
			}
			masked = (masked + 1) & hash_mask;
		}
	}

	void clear()
	{
		list.clear();
		values.clear();
		hash_mask = 0;
		count = 0;
		load_count = 0;
	}

	typename IntrusiveList<T>::Iterator begin()
	{
		return list.begin();
	}

	typename IntrusiveList<T>::Iterator end()
	{
		return list.end();
	}

private:

	inline bool compare_key(Hash masked, Hash hash) const
	{
		return get_key_for_index(masked) == hash;
	}

	inline Hash get_hash(const T *value) const
	{
		return static_cast<const IntrusiveHashMapEnabled<T> *>(value)->intrusive_hashmap_key;
	}

	inline Hash get_key_for_index(Hash masked) const
	{
		return get_hash(values[masked]);
	}

	void insert_inner(T *value)
	{
		auto hash = get_hash(value);
		auto masked = hash & hash_mask;
		while (values[masked])
			masked = (masked + 1) & hash_mask;
		values[masked] = value;
	}

	void grow()
	{
		if (values.empty())
		{
			values.resize(InitialSize);
			load_count = InitialLoadCount;
		}
		else
		{
			values.resize(values.size() * 2);
			load_count++;
		}

		for (auto &v : values)
			v = nullptr;

		hash_mask = Hash(values.size()) - 1;

		// Re-insert.
		for (auto &t : list)
			insert_inner(&t);
	}

	std::vector<T *> values;
	IntrusiveList<T> list;
	Hash hash_mask = 0;
	size_t count = 0;
	unsigned load_count = 0;
};

template <typename T>
class IntrusiveHashMap
{
public:
	~IntrusiveHashMap()
	{
		clear();
	}

	IntrusiveHashMap() = default;
	IntrusiveHashMap(const IntrusiveHashMap &) = delete;
	void operator=(const IntrusiveHashMap &) = delete;

	void clear()
	{
		for (auto &t : hashmap)
			pool.free(&t);
		hashmap.clear();
	}

	T *find(Hash hash) const
	{
		return hashmap.find(hash);
	}

	void erase(T *value)
	{
		hashmap.erase(value);
		pool.free(value);
	}

	template <typename... P>
	T *emplace_replace(Hash hash, P&&... p)
	{
		T *t = allocate(std::forward<P>(p)...);
		return insert_replace(hash, t);
	}

	template <typename... P>
	T *emplace_yield(Hash hash, P&&... p)
	{
		T *t = allocate(std::forward<P>(p)...);
		return insert_yield(hash, t);
	}

	template <typename... P>
	T *allocate(P&&... p)
	{
		return pool.allocate(std::forward<P>(p)...);
	}

	void free(T *value)
	{
		pool.free(value);
	}

	T *insert_replace(Hash hash, T *value)
	{
		static_cast<IntrusiveHashMapEnabled<T> *>(value)->intrusive_hashmap_key = hash;
		T *to_delete = hashmap.insert_replace(value);
		if (to_delete)
			pool.free(to_delete);
		return value;
	}

	T *insert_yield(Hash hash, T *value)
	{
		static_cast<IntrusiveHashMapEnabled<T> *>(value)->intrusive_hashmap_key = hash;
		T *to_delete = hashmap.insert_yield(value);
		if (to_delete)
			pool.free(to_delete);
		return value;
	}

	typename IntrusiveList<T>::Iterator begin()
	{
		return hashmap.begin();
	}

	typename IntrusiveList<T>::Iterator end()
	{
		return hashmap.end();
	}

	IntrusiveHashMap &get_thread_unsafe()
	{
		return *this;
	}

private:
	IntrusiveHashMapHolder<T> hashmap;
	ObjectPool<T> pool;
};

template <typename T>
class ThreadSafeIntrusiveHashMap
{
public:
	T *find(Hash hash) const
	{
		lock.lock_read();
		T *t = hashmap.find(hash);
		lock.unlock_read();

		// We can race with the intrusive list internal pointers,
		// but that's an internal detail which should never be touched outside the hashmap.
		return t;
	}

	void clear()
	{
		lock.lock_write();
		hashmap.clear();
		lock.unlock_write();
	}

	// Assumption is that readers will not be erased while in use by any other thread.
	void erase(T *value)
	{
		lock.lock_write();
		hashmap.erase(value);
		lock.unlock_write();
	}

	template <typename... P>
	T *allocate(P&&... p)
	{
		lock.lock_write();
		T *t = hashmap.allocate(std::forward<P>(p)...);
		lock.unlock_write();
		return t;
	}

	void free(T *value)
	{
		lock.lock_write();
		hashmap.free(value);
		lock.unlock_write();
	}

	T *insert_replace(Hash hash, T *value)
	{
		lock.lock_write();
		value = hashmap.insert_replace(hash, value);
		lock.unlock_write();
		return value;
	}

	T *insert_yield(Hash hash, T *value)
	{
		lock.lock_write();
		value = hashmap.insert_yield(hash, value);
		lock.unlock_write();
		return value;
	}

	// This one is very sketchy, since callers need to make sure there are no readers of this hash.
	template <typename... P>
	T *emplace_replace(Hash hash, P&&... p)
	{
		lock.lock_write();
		T *t = hashmap.emplace_replace(hash, std::forward<P>(p)...);
		lock.unlock_write();
		return t;
	}

	template <typename... P>
	T *emplace_yield(Hash hash, P&&... p)
	{
		lock.lock_write();
		T *t = hashmap.emplace_yield(hash, std::forward<P>(p)...);
		lock.unlock_write();
		return t;
	}

	// Not supposed to be called in racy conditions,
	// we could have a global read lock and unlock while iterating if necessary.
	typename IntrusiveList<T>::Iterator begin()
	{
		return hashmap.begin();
	}

	typename IntrusiveList<T>::Iterator end()
	{
		return hashmap.end();
	}

	IntrusiveHashMap<T> &get_thread_unsafe()
	{
		return hashmap;
	}

private:
	IntrusiveHashMap<T> hashmap;
	mutable RWSpinLock lock;
};
}