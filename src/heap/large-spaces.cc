// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/large-spaces.h"

#include "src/base/platform/mutex.h"
#include "src/base/sanitizer/msan.h"
#include "src/common/globals.h"
#include "src/execution/isolate.h"
#include "src/heap/combined-heap.h"
#include "src/heap/concurrent-marking.h"
#include "src/heap/heap-verifier.h"
#include "src/heap/incremental-marking.h"
#include "src/heap/large-page.h"
#include "src/heap/list.h"
#include "src/heap/marking-state-inl.h"
#include "src/heap/marking.h"
#include "src/heap/memory-allocator.h"
#include "src/heap/memory-chunk-inl.h"
#include "src/heap/memory-chunk-layout.h"
#include "src/heap/remembered-set.h"
#include "src/heap/slot-set.h"
#include "src/heap/spaces-inl.h"
#include "src/logging/log.h"
#include "src/objects/objects-inl.h"
#include "src/utils/ostreams.h"

namespace v8 {
namespace internal {

// -----------------------------------------------------------------------------
// LargeObjectSpaceObjectIterator

LargeObjectSpaceObjectIterator::LargeObjectSpaceObjectIterator(
    LargeObjectSpace* space) {
  current_ = space->first_page();
}

Tagged<HeapObject> LargeObjectSpaceObjectIterator::Next() {
  while (current_ != nullptr) {
    Tagged<HeapObject> object = current_->GetObject();
    current_ = current_->next_page();
    if (!IsFreeSpaceOrFiller(object)) return object;
  }
  return Tagged<HeapObject>();
}

// -----------------------------------------------------------------------------
// OldLargeObjectSpace

LargeObjectSpace::LargeObjectSpace(Heap* heap, AllocationSpace id)
    : Space(heap, id, nullptr),
      size_(0),
      page_count_(0),
      objects_size_(0),
      pending_object_(0) {}

size_t LargeObjectSpace::Available() const {
  // We return zero here since we cannot take advantage of already allocated
  // large object memory.
  return 0;
}

void LargeObjectSpace::TearDown() {
  while (!memory_chunk_list_.Empty()) {
    LargePage* page = first_page();
    LOG(heap()->isolate(),
        DeleteEvent("LargeObjectChunk",
                    reinterpret_cast<void*>(page->address())));
    memory_chunk_list_.Remove(page);
    heap()->memory_allocator()->Free(MemoryAllocator::FreeMode::kImmediately,
                                     page);
  }
}

void LargeObjectSpace::AdvanceAndInvokeAllocationObservers(Address soon_object,
                                                           size_t object_size) {
  if (!heap()->IsAllocationObserverActive()) return;

  if (object_size >= allocation_counter_.NextBytes()) {
    // Ensure that there is a valid object
    heap_->CreateFillerObjectAt(soon_object, static_cast<int>(object_size));

    allocation_counter_.InvokeAllocationObservers(soon_object, object_size,
                                                  object_size);
  }

  // Large objects can be accounted immediately since no LAB is involved.
  allocation_counter_.AdvanceAllocationObservers(object_size);
}

void LargeObjectSpace::AddAllocationObserver(AllocationObserver* observer) {
  allocation_counter_.AddAllocationObserver(observer);
}

void LargeObjectSpace::RemoveAllocationObserver(AllocationObserver* observer) {
  allocation_counter_.RemoveAllocationObserver(observer);
}

AllocationResult OldLargeObjectSpace::AllocateRaw(int object_size) {
  return AllocateRaw(object_size, NOT_EXECUTABLE);
}

AllocationResult OldLargeObjectSpace::AllocateRaw(int object_size,
                                                  Executability executable) {
  object_size = ALIGN_TO_ALLOCATION_ALIGNMENT(object_size);
  DCHECK(!v8_flags.enable_third_party_heap);
  // Check if we want to force a GC before growing the old space further.
  // If so, fail the allocation.
  if (!heap()->ShouldExpandOldGenerationOnSlowAllocation(
          heap()->main_thread_local_heap(), AllocationOrigin::kRuntime) ||
      !heap()->CanExpandOldGeneration(object_size)) {
    return AllocationResult::Failure();
  }

  heap()->StartIncrementalMarkingIfAllocationLimitIsReached(
      heap()->GCFlagsForIncrementalMarking(),
      kGCCallbackScheduleIdleGarbageCollection);

  LargePage* page = AllocateLargePage(object_size, executable);
  if (page == nullptr) return AllocationResult::Failure();
  page->SetOldGenerationPageFlags(
      heap()->incremental_marking()->marking_mode());
  Tagged<HeapObject> object = page->GetObject();
  UpdatePendingObject(object);
  if (heap()->incremental_marking()->black_allocation()) {
    heap()->marking_state()->TryMarkAndAccountLiveBytes(object, object_size);
  }
  DCHECK_IMPLIES(heap()->incremental_marking()->black_allocation(),
                 heap()->marking_state()->IsMarked(object));
  page->InitializationMemoryFence();
  heap()->NotifyOldGenerationExpansion(heap()->main_thread_local_heap(),
                                       identity(), page);
  AdvanceAndInvokeAllocationObservers(object.address(),
                                      static_cast<size_t>(object_size));
  return AllocationResult::FromObject(object);
}

AllocationResult OldLargeObjectSpace::AllocateRawBackground(
    LocalHeap* local_heap, int object_size) {
  return AllocateRawBackground(local_heap, object_size, NOT_EXECUTABLE);
}

AllocationResult OldLargeObjectSpace::AllocateRawBackground(
    LocalHeap* local_heap, int object_size, Executability executable) {
  object_size = ALIGN_TO_ALLOCATION_ALIGNMENT(object_size);
  DCHECK(!v8_flags.enable_third_party_heap);
  // Check if we want to force a GC before growing the old space further.
  // If so, fail the allocation.
  if (!heap()->CanExpandOldGeneration(object_size) ||
      !heap()->ShouldExpandOldGenerationOnSlowAllocation(
          local_heap, AllocationOrigin::kRuntime)) {
    return AllocationResult::Failure();
  }

  heap()->StartIncrementalMarkingIfAllocationLimitIsReachedBackground();

  LargePage* page = AllocateLargePage(object_size, executable);
  if (page == nullptr) return AllocationResult::Failure();
  page->SetOldGenerationPageFlags(
      heap()->incremental_marking()->marking_mode());
  Tagged<HeapObject> object = page->GetObject();
  if (heap()->incremental_marking()->black_allocation()) {
    heap()->marking_state()->TryMarkAndAccountLiveBytes(object, object_size);
  }
  DCHECK_IMPLIES(heap()->incremental_marking()->black_allocation(),
                 heap()->marking_state()->IsMarked(object));
  page->InitializationMemoryFence();
  heap()->NotifyOldGenerationExpansion(local_heap, identity(), page);
  return AllocationResult::FromObject(object);
}

LargePage* LargeObjectSpace::AllocateLargePage(int object_size,
                                               Executability executable) {
  base::MutexGuard expansion_guard(heap_->heap_expansion_mutex());

  if (identity() != NEW_LO_SPACE &&
      !heap()->IsOldGenerationExpansionAllowed(object_size, expansion_guard)) {
    return nullptr;
  }

  LargePage* page = heap()->memory_allocator()->AllocateLargePage(
      this, object_size, executable);
  if (page == nullptr) return nullptr;
  DCHECK_GE(page->area_size(), static_cast<size_t>(object_size));

  {
    base::RecursiveMutexGuard guard(&allocation_mutex_);
    AddPage(page, object_size);
  }

  return page;
}

size_t LargeObjectSpace::CommittedPhysicalMemory() const {
  // On a platform that provides lazy committing of memory, we over-account
  // the actually committed memory. There is no easy way right now to support
  // precise accounting of committed memory in large object space.
  return CommittedMemory();
}

void OldLargeObjectSpace::PromoteNewLargeObject(LargePage* page) {
  DCHECK_EQ(page->owner_identity(), NEW_LO_SPACE);
  DCHECK(page->IsLargePage());
  DCHECK(page->IsFlagSet(MemoryChunk::FROM_PAGE));
  DCHECK(!page->IsFlagSet(MemoryChunk::TO_PAGE));
  PtrComprCageBase cage_base(heap()->isolate());
  static_cast<LargeObjectSpace*>(page->owner())->RemovePage(page);
  page->ClearFlag(MemoryChunk::FROM_PAGE);
  AddPage(page, static_cast<size_t>(page->GetObject()->Size(cage_base)));
}

void LargeObjectSpace::AddPage(LargePage* page, size_t object_size) {
  size_ += static_cast<int>(page->size());
  AccountCommitted(page->size());
  objects_size_ += object_size;
  page_count_++;
  memory_chunk_list_.PushBack(page);
  page->set_owner(this);
  page->SetOldGenerationPageFlags(
      heap()->incremental_marking()->marking_mode());
  ForAll<ExternalBackingStoreType>(
      [this, page](ExternalBackingStoreType type, int index) {
        IncrementExternalBackingStoreBytes(
            type, page->ExternalBackingStoreBytes(type));
      });
}

void LargeObjectSpace::RemovePage(LargePage* page) {
  size_ -= static_cast<int>(page->size());
  AccountUncommitted(page->size());
  page_count_--;
  memory_chunk_list_.Remove(page);
  page->set_owner(nullptr);
  ForAll<ExternalBackingStoreType>(
      [this, page](ExternalBackingStoreType type, int index) {
        DecrementExternalBackingStoreBytes(
            type, page->ExternalBackingStoreBytes(type));
      });
}

void LargeObjectSpace::ShrinkPageToObjectSize(LargePage* page,
                                              Tagged<HeapObject> object,
                                              size_t object_size) {
#ifdef DEBUG
  PtrComprCageBase cage_base(heap()->isolate());
  DCHECK_EQ(object, page->GetObject());
  DCHECK_EQ(object_size, page->GetObject()->Size(cage_base));
  DCHECK_EQ(page->executable(), NOT_EXECUTABLE);
#endif  // DEBUG

  const size_t used_committed_size =
      ::RoundUp(object.address() - page->address() + object_size,
                MemoryAllocator::GetCommitPageSize());

  // Object shrunk since last GC.
  if (object_size < page->area_size()) {
    page->ClearOutOfLiveRangeSlots(object.address() + object_size);
    const Address new_area_end = page->area_start() + object_size;

    // Object shrunk enough that we can even free some OS pages.
    if (used_committed_size < page->size()) {
      const size_t bytes_to_free = page->size() - used_committed_size;
      heap()->memory_allocator()->PartialFreeMemory(
          page, page->address() + used_committed_size, bytes_to_free,
          new_area_end);
      size_ -= bytes_to_free;
      AccountUncommitted(bytes_to_free);
    } else {
      // Can't free OS page but keep object area up-to-date.
      page->set_area_end(new_area_end);
    }
  }

  DCHECK_EQ(used_committed_size, page->size());
  DCHECK_EQ(object_size, page->area_size());
}

bool LargeObjectSpace::Contains(Tagged<HeapObject> object) const {
  BasicMemoryChunk* chunk = BasicMemoryChunk::FromHeapObject(object);

  bool owned = (chunk->owner() == this);

  SLOW_DCHECK(!owned || ContainsSlow(object.address()));

  return owned;
}

bool LargeObjectSpace::ContainsSlow(Address addr) const {
  for (const LargePage* page : *this) {
    if (page->Contains(addr)) return true;
  }
  return false;
}

std::unique_ptr<ObjectIterator> LargeObjectSpace::GetObjectIterator(
    Heap* heap) {
  return std::unique_ptr<ObjectIterator>(
      new LargeObjectSpaceObjectIterator(this));
}

#ifdef VERIFY_HEAP
// We do not assume that the large object iterator works, because it depends
// on the invariants we are checking during verification.
void LargeObjectSpace::Verify(Isolate* isolate,
                              SpaceVerificationVisitor* visitor) const {
  size_t external_backing_store_bytes[static_cast<int>(
      ExternalBackingStoreType::kNumValues)] = {0};

  PtrComprCageBase cage_base(isolate);
  for (const LargePage* chunk = first_page(); chunk != nullptr;
       chunk = chunk->next_page()) {
    visitor->VerifyPage(chunk);

    // Each chunk contains an object that starts at the large object page's
    // object area start.
    Tagged<HeapObject> object = chunk->GetObject();
    Page* page = Page::FromHeapObject(object);
    CHECK(object.address() == page->area_start());

    // Only certain types may be in the large object space:
#define V(Name) Is##Name(object, cage_base) ||
    const bool is_valid_lo_space_object =
        DYNAMICALLY_SIZED_HEAP_OBJECT_LIST(V) false;
#undef V
    if (!is_valid_lo_space_object) {
      i::Print(object);
      FATAL("Found invalid Object (instance_type=%i) in large object space.",
            object->map(cage_base)->instance_type());
    }

    // Invoke visitor on each object.
    visitor->VerifyObject(object);

    ForAll<ExternalBackingStoreType>(
        [chunk, &external_backing_store_bytes](ExternalBackingStoreType type,
                                               int index) {
          external_backing_store_bytes[index] +=
              chunk->ExternalBackingStoreBytes(type);
        });

    visitor->VerifyPageDone(chunk);
  }
  ForAll<ExternalBackingStoreType>(
      [this, external_backing_store_bytes](ExternalBackingStoreType type,
                                           int index) {
        CHECK_EQ(external_backing_store_bytes[index],
                 ExternalBackingStoreBytes(type));
      });
}
#endif

#ifdef DEBUG
void LargeObjectSpace::Print() {
  StdoutStream os;
  LargeObjectSpaceObjectIterator it(this);
  for (Tagged<HeapObject> obj = it.Next(); !obj.is_null(); obj = it.Next()) {
    i::Print(obj, os);
  }
}
#endif  // DEBUG

void LargeObjectSpace::UpdatePendingObject(Tagged<HeapObject> object) {
  base::SharedMutexGuard<base::kExclusive> guard(&pending_allocation_mutex_);
  pending_object_.store(object.address(), std::memory_order_release);
}

OldLargeObjectSpace::OldLargeObjectSpace(Heap* heap)
    : LargeObjectSpace(heap, LO_SPACE) {}

OldLargeObjectSpace::OldLargeObjectSpace(Heap* heap, AllocationSpace id)
    : LargeObjectSpace(heap, id) {}

NewLargeObjectSpace::NewLargeObjectSpace(Heap* heap, size_t capacity)
    : LargeObjectSpace(heap, NEW_LO_SPACE), capacity_(capacity) {}

AllocationResult NewLargeObjectSpace::AllocateRaw(int object_size) {
  object_size = ALIGN_TO_ALLOCATION_ALIGNMENT(object_size);
  DCHECK(!v8_flags.enable_third_party_heap);
  // Do not allocate more objects if promoting the existing object would exceed
  // the old generation capacity.
  if (!heap()->CanExpandOldGeneration(SizeOfObjects())) {
    return AllocationResult::Failure();
  }

  // Allocation for the first object must succeed independent from the capacity.
  if (SizeOfObjects() > 0 && static_cast<size_t>(object_size) > Available()) {
    return AllocationResult::Failure();
  }

  LargePage* page = AllocateLargePage(object_size, NOT_EXECUTABLE);
  if (page == nullptr) return AllocationResult::Failure();

  // The size of the first object may exceed the capacity.
  capacity_ = std::max(capacity_, SizeOfObjects());

  Tagged<HeapObject> result = page->GetObject();
  page->SetYoungGenerationPageFlags(
      heap()->incremental_marking()->marking_mode());
  page->SetFlag(MemoryChunk::TO_PAGE);
  UpdatePendingObject(result);
  if (v8_flags.minor_ms) {
    page->ClearLiveness();
  }
  page->InitializationMemoryFence();
  DCHECK(page->IsLargePage());
  DCHECK_EQ(page->owner_identity(), NEW_LO_SPACE);
  AdvanceAndInvokeAllocationObservers(result.address(),
                                      static_cast<size_t>(object_size));
  return AllocationResult::FromObject(result);
}

size_t NewLargeObjectSpace::Available() const {
  return capacity_ - SizeOfObjects();
}

void NewLargeObjectSpace::Flip() {
  for (LargePage* chunk = first_page(); chunk != nullptr;
       chunk = chunk->next_page()) {
    chunk->SetFlag(MemoryChunk::FROM_PAGE);
    chunk->ClearFlag(MemoryChunk::TO_PAGE);
  }
}

void NewLargeObjectSpace::FreeDeadObjects(
    const std::function<bool(Tagged<HeapObject>)>& is_dead) {
  bool is_marking = heap()->incremental_marking()->IsMarking();
  DCHECK_IMPLIES(v8_flags.minor_ms, !is_marking);
  DCHECK_IMPLIES(is_marking, heap()->incremental_marking()->IsMajorMarking());
  size_t surviving_object_size = 0;
  PtrComprCageBase cage_base(heap()->isolate());
  for (auto it = begin(); it != end();) {
    LargePage* page = *it;
    it++;
    Tagged<HeapObject> object = page->GetObject();
    if (is_dead(object)) {
      RemovePage(page);
      heap()->memory_allocator()->Free(MemoryAllocator::FreeMode::kConcurrently,
                                       page);
      if (v8_flags.concurrent_marking && is_marking) {
        heap()->concurrent_marking()->ClearMemoryChunkData(page);
      }
    } else {
      surviving_object_size += static_cast<size_t>(object->Size(cage_base));
    }
  }
  // Right-trimming does not update the objects_size_ counter. We are lazily
  // updating it after every GC.
  objects_size_ = surviving_object_size;
}

void NewLargeObjectSpace::SetCapacity(size_t capacity) {
  capacity_ = std::max(capacity, SizeOfObjects());
}

CodeLargeObjectSpace::CodeLargeObjectSpace(Heap* heap)
    : OldLargeObjectSpace(heap, CODE_LO_SPACE) {}

AllocationResult CodeLargeObjectSpace::AllocateRaw(int object_size) {
  DCHECK(!v8_flags.enable_third_party_heap);
  CodePageHeaderModificationScope header_modification_scope(
      "Code allocation needs header access.");
  return OldLargeObjectSpace::AllocateRaw(object_size, EXECUTABLE);
}

AllocationResult CodeLargeObjectSpace::AllocateRawBackground(
    LocalHeap* local_heap, int object_size) {
  DCHECK(!v8_flags.enable_third_party_heap);
  return OldLargeObjectSpace::AllocateRawBackground(local_heap, object_size,
                                                    EXECUTABLE);
}

void CodeLargeObjectSpace::AddPage(LargePage* page, size_t object_size) {
  OldLargeObjectSpace::AddPage(page, object_size);
}

void CodeLargeObjectSpace::RemovePage(LargePage* page) {
  heap()->isolate()->RemoveCodeMemoryChunk(page);
  OldLargeObjectSpace::RemovePage(page);
}

SharedLargeObjectSpace::SharedLargeObjectSpace(Heap* heap)
    : OldLargeObjectSpace(heap, SHARED_LO_SPACE) {}

AllocationResult SharedLargeObjectSpace::AllocateRawBackground(
    LocalHeap* local_heap, int object_size) {
  DCHECK(!v8_flags.enable_third_party_heap);
  return OldLargeObjectSpace::AllocateRawBackground(local_heap, object_size,
                                                    NOT_EXECUTABLE);
}

TrustedLargeObjectSpace::TrustedLargeObjectSpace(Heap* heap)
    : OldLargeObjectSpace(heap, TRUSTED_LO_SPACE) {}

AllocationResult TrustedLargeObjectSpace::AllocateRawBackground(
    LocalHeap* local_heap, int object_size) {
  DCHECK(!v8_flags.enable_third_party_heap);
  return OldLargeObjectSpace::AllocateRawBackground(local_heap, object_size,
                                                    NOT_EXECUTABLE);
}

}  // namespace internal
}  // namespace v8
