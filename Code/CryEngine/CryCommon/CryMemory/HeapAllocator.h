// Copyright 2001-2016 Crytek GmbH / Crytek Group. All rights reserved.

// Created by: Scott Peter
//---------------------------------------------------------------------------

#ifndef _HEAP_ALLOCATOR_H
#define _HEAP_ALLOCATOR_H

#include <CryThreading/Synchronization.h>
#include <CryParticleSystem/Options.h>

//---------------------------------------------------------------------------
#define bMEM_ACCESS_CHECK 0
#define bMEM_HEAP_CHECK   0

namespace stl
{
class HeapSysAllocator
{
public:
#if bMEM_ACCESS_CHECK
	static void* SysAlloc(size_t nSize)
	{ return VirtualAlloc(0, nSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
	static void  SysDealloc(void* ptr)
	{ VirtualFree(ptr, 0, MEM_DECOMMIT); }
#else
	static void* SysAlloc(size_t nSize)
	{ return malloc(nSize); }
	static void  SysDealloc(void* ptr)
	{ free(ptr); }
#endif
};

class GlobalHeapSysAllocator
{
public:
	static void* SysAlloc(size_t nSize)
	{
		return malloc(nSize);
	}
	static void SysDealloc(void* ptr)
	{
		free(ptr);
	}
};

//! Round up to next multiple of nAlign. Handles any positive integer.
inline size_t RoundUpTo(size_t nSize, size_t nAlign)
{
	assert(nAlign > 0);
	nSize += nAlign - 1;
	return nSize - nSize % nAlign;
}

/*---------------------------------------------------------------------------
   HeapAllocator
   A memory pool that can allocate arbitrary amounts of memory of arbitrary size
   and alignment. The heap may be freed all at once. Individual block deallocation
   is not provided.

   Usable as a base class to implement more general-purpose allocators that
   track, free, and reuse individual memory blocks.

   The class can optionally support multi-threading, using the second
   template parameter. By default it is multithread-safe.
   See Synchronization.h.

   Allocation details: Maintains a linked list of pages.
   All pages after first are in order of most free memory first.
   Allocations are from the smallest free page available.

   ---------------------------------------------------------------------------*/

struct SMemoryUsage
{
	size_t nAlloc, nUsed;

	SMemoryUsage(size_t _nAlloc = 0, size_t _nUsed = 0)
		: nAlloc(_nAlloc), nUsed(_nUsed)
	{
		Validate();
	}

	size_t nFree() const
	{
		return nAlloc - nUsed;
	}
	void Validate() const
	{
		assert(nUsed <= nAlloc);
	}
	void Clear()
	{
		nAlloc = nUsed = 0;
	}

	void operator+=(SMemoryUsage const& op)
	{
		nAlloc += op.nAlloc;
		nUsed += op.nUsed;
	}
};

//////////////////////////////////////////////////////////////////////////
struct FHeap
{
	OPT_STRUCT(FHeap)
	OPT_VAR_INIT(size_t, PageSize, 0x1000); //!< Pages allocated at this size, or multiple thereof if needed
	OPT_VAR(bool, SinglePage)               //!< Only 1 page allowed (fixed alloc)
	OPT_VAR(bool, FreeWhenEmpty)            //!< Release all memory when no longer used
	OPT_VAR(bool, StackDealloc)             //!< Support reverse-order deallocation and memory re-use
	OPT_VAR(size_t, StackAlignment)         //!< Alignment to use for stack-based allocation
};

template<typename L = PSyncMultiThread, typename A = HeapSysAllocator>
class HeapAllocator : public FHeap, public L, private A
{
public:

	typedef AutoLock<L> Lock;

private:

	struct PageNode
	{
		PageNode* pNext;
		char*     pEndAlloc;
		char*     pEndUsed;
		size_t    nUsed;

		char*     StartUsed() const
		{
			return (char*)(this + 1);
		}

		PageNode(size_t nAlloc)
		{
			pNext = 0;
			pEndAlloc = (char*)this + nAlloc;
			pEndUsed = StartUsed();
			nUsed = 0;
		}

		ILINE void* Allocate(size_t nSize, size_t nAlign)
		{
			// Align current mem.
			char* pNew = Align(pEndUsed, nAlign);
			if (pNew + nSize > pEndAlloc)
				return 0;
			pEndUsed = pNew + nSize;
			nUsed += nSize;
			return pNew;
		}

		ILINE bool Reallocate(void* ptr, size_t nOldSize, size_t nNewSize, size_t nAlign)
		{
			char* cptr = (char*)ptr;
			if (cptr + nOldSize <= pEndUsed && Align(cptr + nOldSize, nAlign) >= pEndUsed && cptr + nNewSize <= pEndAlloc)
			{
				// Last allocation chunk, can re-use
				assert(cptr >= StartUsed());
				pEndUsed = cptr + nNewSize;
				nUsed += nNewSize - nOldSize;
				return true;
			}
			return false;
		}

		void Reset()
		{
			pEndUsed = StartUsed();
		}

		size_t GetMemoryAlloc() const
		{
			return pEndAlloc - (char*)this;
		}
		size_t GetMemoryUsed() const
		{
			assert(StartUsed() + nUsed <= pEndUsed);
			return nUsed;
		}
		size_t GetMemoryFree(size_t nAlign = 1) const
		{
			return pEndAlloc - Align(pEndUsed, nAlign);
		}
		SMemoryUsage GetMemoryUsage() const
		{
			return SMemoryUsage(GetMemoryAlloc(), GetMemoryUsed());
		}

		void Validate() const
		{
			assert(pEndAlloc >= (char*)this);
			assert(pEndUsed >= StartUsed() && pEndUsed <= pEndAlloc);
			assert(StartUsed() + nUsed <= pEndUsed);
		}

		bool CheckPtr(void* ptr, size_t nSize) const
		{
			return (char*)ptr >= StartUsed() && (char*)ptr + nSize <= pEndUsed;
		}
	};

public:

	HeapAllocator(FHeap opts = {})
		: FHeap(opts)
		, _pPageList(0)
		, _pFreeList(0)
	{
	}

	~HeapAllocator()
	{
		Clear();
	}

	void SupportStackDealloc(size_t nAlign)
	{
		StackDealloc(true);
		StackAlignment(std::max(StackAlignment(), nAlign));
	}

	// Raw memory allocation.

	void* Allocate(const Lock& lock, size_t nSize, size_t nAlign)
	{
		for (;; )
		{
			// Try allocating from head page first.
			if (_pPageList)
			{
				if (void* ptr = _pPageList->Allocate(nSize, nAlign))
				{
					_TotalMem.nUsed += nSize;
					Validate(lock);
					return ptr;
				}

				if (!StackDealloc() && _pPageList->pNext && _pPageList->pNext->GetMemoryFree() > _pPageList->GetMemoryFree())
				{
					SortPage(lock, _pPageList);
					Validate(lock);

					// Try allocating from new head, which has the most free memory.
					// If this fails, we know no further pages will succeed.
					if (void* ptr = _pPageList->Allocate(nSize, nAlign))
					{
						_TotalMem.nUsed += nSize;
						return ptr;
					}
				}
				if (SinglePage())
					return 0;
			}

			PageNode* pPageNode = 0;

			// Find first free page that can allocate mem
			for (PageNode** ppFree = &_pFreeList; *ppFree; ppFree = &(*ppFree)->pNext)
			{
				if ((*ppFree)->GetMemoryFree() >= nSize)
				{
					pPageNode = *ppFree;
					*ppFree = (*ppFree)->pNext;
					break;
				}
			}

			if (!pPageNode)
			{
				// Allocate the new page of the required size.
				size_t nAllocSize = Align(sizeof(PageNode), nAlign) + nSize;
				nAllocSize = RoundUpTo(nAllocSize, PageSize());

				void* pAlloc = A::SysAlloc(nAllocSize);
				pPageNode = new(pAlloc) PageNode(nAllocSize);

				_TotalMem.nAlloc += nAllocSize;
			}

			if (_pPageList && _pPageList->GetMemoryUsed() == 0)
			{
				// Move unused empty page to free list
				RecyclePage();
			}

			// Insert at head of list.
			pPageNode->pNext = _pPageList;
			_pPageList = pPageNode;
		}
	}

	bool Reallocate(const Lock& lock, void* ptr, size_t nOldSize, size_t nNewSize, size_t nAlign)
	{
		// Reallocate only if last allocated block in last page
		assert(ptr && nOldSize);
		assert(_pPageList);
		if (_pPageList->Reallocate(ptr, nOldSize, nNewSize, nAlign))
		{
			assert(_TotalMem.nUsed + nNewSize >= nOldSize);
			_TotalMem.nUsed += nNewSize - nOldSize;
			Validate(lock);
			return true;
		}
		return false;
	}

	bool Deallocate(const Lock& lock, void* ptr, size_t nSize, size_t nAlign)
	{
		assert(CheckPtr(lock, ptr, nSize));
		if (StackDealloc())
		{
			if (ptr && _pPageList)
			{
				if (!_pPageList->Reallocate(ptr, nSize, 0, nAlign))
					return false;
				if (_pPageList->GetMemoryUsed() == 0)
				{
					_pPageList->pEndUsed = _pPageList->StartUsed();
					RecyclePage();
				}
			}
		}

		assert(_TotalMem.nUsed >= nSize);
		_TotalMem.nUsed -= nSize;
		Validate(lock);
		return true;
	}

	// Templated type allocation.

	template<typename T>
	T* New(size_t nAlign = 0)
	{
		void* pMemory = Allocate(Lock(*this), sizeof(T), nAlign ? nAlign : alignof(T));
		return pMemory ? new(pMemory) T : 0;
	}

	template<typename T>
	T* NewArray(size_t nCount, size_t nAlign = 0)
	{
		void* pMemory = Allocate(Lock(*this), sizeof(T) * nCount, nAlign ? nAlign : alignof(T));
		return pMemory ? new(pMemory) T[nCount] : 0;
	}

	//! Storage base for DynArray, providing stack-based allocation and reallocation.
	template<int nALIGN>
	struct HeapAlloc
	{
		HeapAllocator* _pHeap;

		HeapAlloc()
			: _pHeap(0) {}

		void SetHeap(HeapAllocator& heap)
		{
			_pHeap = &heap;
			heap.SupportStackDealloc(nALIGN);
		}

		NAlloc::AllocArray alloc(NAlloc::AllocArray a, size_t new_size, size_t align = 1, bool allow_slack = false)
		{
			align = std::max(align, _pHeap->StackAlignment());
			if (new_size)
			{
				new_size = Align(new_size, _pHeap->StackAlignment());
				if (a.size)
				{
					if (new_size != a.size)
					{
						// Attempt to realloc existing array
						if (_pHeap->Reallocate(Lock(*_pHeap), a.data, a.size, new_size, align))
							a.size = new_size;
						else
							assert(!"HeapAllocator array reallocation failed");
					}
				}
				else
				{
					// Alloc
					a.data = _pHeap->Allocate(Lock(*_pHeap), new_size, align);
					a.size = new_size;
				}
			}
			else
			{
				// Dealloc
				if (!_pHeap->Deallocate(Lock(*_pHeap), a.data, a.size, align))
					assert(!"HeapAllocator array deallocation failed");
				a = NAlloc::AllocArray();
			}
			return a;
		}
	};

	template<typename T, typename I = int, int nALIGN = alignof(T)>
	struct Array : FastDynArray<T, I, HeapAlloc<nALIGN>>
	{
		Array(HeapAllocator& heap, I size = 0)
		{
			this->SetHeap(heap);
			if (size)
				this->resize(size);
		}
	};

	// Maintenance.

	SMemoryUsage GetTotalMemory(const Lock&)
	{
		return _TotalMem;
	}
	SMemoryUsage GetTotalMemory()
	{
		Lock lock(*this);
		return _TotalMem;
	}

	//! Facility to defer freeing of dead pages during memory release calls.
	struct FreeMemLock : Lock
	{
		struct PageNode* _pPageList;

		FreeMemLock(L& lock)
			: Lock(lock), _pPageList(0) {}

		~FreeMemLock()
		{
			while (_pPageList != 0)
			{
				// Read the "next" pointer before deleting.
				PageNode* pNext = _pPageList->pNext;

				// Delete the current page.
				A::SysDealloc(_pPageList);

				// Move to the next page in the list.
				_pPageList = pNext;
			}
		}
	};

	void Clear(FreeMemLock& lock)
	{
		// Remove the pages from the object.
		Validate(lock);
		lock._pPageList = _pPageList;
		_pPageList = 0;
		_TotalMem.Clear();
	}

	void Clear()
	{
		FreeMemLock lock(*this);
		Clear(lock);
	}

	void Reset(const Lock& lock)
	{
		// Reset all pages, allowing memory re-use.
		Validate(lock);
		size_t nPrevSize = ~0;
		for (PageNode** ppPage = &_pPageList; *ppPage; )
		{
			(*ppPage)->Reset();
			if ((*ppPage)->GetMemoryAlloc() > nPrevSize)
			{
				// Move page to sorted location near beginning.
				SortPage(lock, *ppPage);

				// ppPage is now next page, so continue loop.
				continue;
			}
			nPrevSize = (*ppPage)->GetMemoryAlloc();
			ppPage = &(*ppPage)->pNext;
		}
		_TotalMem.nUsed = 0;
		Validate(lock);
	}

	void Reset()
	{
		Reset(Lock(*this));
	}

	// Validation.

	bool CheckPtr(const Lock&, void* ptr, size_t size) const
	{
		assert(ptr || !size);
		if (!ptr)
			return true;
		for (PageNode* pNode = _pPageList; pNode; pNode = pNode->pNext)
		{
			if (pNode->CheckPtr(ptr, size))
				return true;
		}
		return false;
	}

	void Validate(const Lock&) const
	{
#if defined(_DEBUG)
		// Check page validity, and memory counts.
		SMemoryUsage MemCheck;

		for (PageNode* pPage = _pPageList; pPage; pPage = pPage->pNext)
		{
			pPage->Validate();
			MemCheck += pPage->GetMemoryUsage();
			if (StackDealloc())
			{
				if (pPage != _pPageList)
					assert(pPage->GetMemoryUsed() > 0);
			}
			else
			{
				if (pPage != _pPageList && pPage->pNext)
					assert(pPage->GetMemoryFree() >= pPage->pNext->GetMemoryFree());
			}
		}
		for (PageNode* pPage = _pFreeList; pPage; pPage = pPage->pNext)
		{
			pPage->Validate();
			MemCheck += pPage->GetMemoryUsage();
			assert(pPage->GetMemoryUsed() == 0);
		}
		assert(MemCheck.nAlloc == _TotalMem.nAlloc);
		assert(MemCheck.nUsed >= _TotalMem.nUsed);
#endif

#if bMEM_HEAP_CHECK
		static int nCount = 0, nInterval = 0;
		if (nCount++ >= nInterval)
		{
			nInterval++;
			nCount = 0;
			assert(IsHeapValid());
		}
#endif
	}

	void GetMemoryUsage(ICrySizer* pSizer) const
	{
		Lock lock(non_const(*this));
		for (PageNode* pNode = _pPageList; pNode; pNode = pNode->pNext)
		{
			pSizer->AddObject(pNode, pNode->GetMemoryAlloc());
		}
	}

private:

	void RecyclePage()
	{
		// Recycle empty page
		PageNode* pFree = _pPageList;
		_pPageList = _pPageList->pNext;
		pFree->pNext = _pFreeList;
		_pFreeList = pFree;
	}

	void SortPage(const Lock&, PageNode*& rpPage)
	{
		// Unlink rpPage.
		PageNode* pPage = rpPage;
		rpPage = pPage->pNext;

		// Insert into list based on free memory.
		PageNode** ppBefore = &_pPageList;
		while (*ppBefore && (*ppBefore)->GetMemoryFree() > pPage->GetMemoryFree())
			ppBefore = &(*ppBefore)->pNext;

		//! Link before rpList.
		pPage->pNext = *ppBefore;
		*ppBefore = pPage;
	}

	PageNode*    _pPageList; //<! Pages with current allocations
	PageNode*    _pFreeList; //<! Freed pages, for reuse
	SMemoryUsage _TotalMem;  //<! Track memory allocated and used
};
}

#endif //_HEAP_ALLOCATOR_H
