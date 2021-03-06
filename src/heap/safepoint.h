// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_SAFEPOINT_H_
#define V8_HEAP_SAFEPOINT_H_

#include "src/base/platform/condition-variable.h"
#include "src/base/platform/mutex.h"
#include "src/common/globals.h"
#include "src/handles/persistent-handles.h"
#include "src/heap/local-heap.h"
#include "src/objects/visitors.h"

namespace v8 {
namespace internal {

class Heap;
class LocalHeap;
class RootVisitor;

// Used to bring all threads with heap access in an isolate to a safepoint such
// that e.g. a garbage collection can be performed.
class IsolateSafepoint final {
 public:
  explicit IsolateSafepoint(Heap* heap);

  V8_EXPORT_PRIVATE bool ContainsLocalHeap(LocalHeap* local_heap);
  V8_EXPORT_PRIVATE bool ContainsAnyLocalHeap();

  // Iterate handles in local heaps
  void Iterate(RootVisitor* visitor);

  // Iterate local heaps
  template <typename Callback>
  void IterateLocalHeaps(Callback callback) {
    AssertActive();
    for (LocalHeap* current = local_heaps_head_; current;
         current = current->next_) {
      callback(current);
    }
  }

  void AssertActive() { local_heaps_mutex_.AssertHeld(); }

  void AssertMainThreadIsOnlyThread();

 private:
  class Barrier {
    base::Mutex mutex_;
    base::ConditionVariable cv_resume_;
    base::ConditionVariable cv_stopped_;
    bool armed_;

    size_t stopped_ = 0;

    bool IsArmed() { return armed_; }

   public:
    Barrier() : armed_(false), stopped_(0) {}

    void Arm();
    void Disarm();
    void WaitUntilRunningThreadsInSafepoint(size_t running);

    void WaitInSafepoint();
    void WaitInUnpark();
    void NotifyPark();
  };

  enum class IncludeMainThread { kYes, kNo };

  // Wait until unpark operation is safe again.
  void WaitInUnpark();

  // Enter the safepoint from a running thread.
  void WaitInSafepoint();

  // Running thread reached a safepoint by parking itself.
  void NotifyPark();

  void EnterLocalSafepointScope();
  void EnterGlobalSafepointScope(Isolate* initiator);

  void LeaveLocalSafepointScope();
  void LeaveGlobalSafepointScope(Isolate* initiator);

  IncludeMainThread IncludeMainThreadUnlessInitiator(Isolate* initiator);

  void LockMutex(LocalHeap* local_heap);

  size_t SetSafepointRequestedFlags(IncludeMainThread include_main_thread);
  void ClearSafepointRequestedFlags(IncludeMainThread include_main_thread);

  template <typename Callback>
  void AddLocalHeap(LocalHeap* local_heap, Callback callback) {
    // Safepoint holds this lock in order to stop threads from starting or
    // stopping.
    base::RecursiveMutexGuard guard(&local_heaps_mutex_);

    // Additional code protected from safepoint
    callback();

    // Add list to doubly-linked list
    if (local_heaps_head_) local_heaps_head_->prev_ = local_heap;
    local_heap->prev_ = nullptr;
    local_heap->next_ = local_heaps_head_;
    local_heaps_head_ = local_heap;
  }

  template <typename Callback>
  void RemoveLocalHeap(LocalHeap* local_heap, Callback callback) {
    base::RecursiveMutexGuard guard(&local_heaps_mutex_);

    // Additional code protected from safepoint
    callback();

    // Remove list from doubly-linked list
    if (local_heap->next_) local_heap->next_->prev_ = local_heap->prev_;
    if (local_heap->prev_)
      local_heap->prev_->next_ = local_heap->next_;
    else
      local_heaps_head_ = local_heap->next_;
  }

  Barrier barrier_;
  Heap* heap_;

  // Mutex is used both for safepointing and adding/removing threads. A
  // RecursiveMutex is needed since we need to support nested SafepointScopes.
  base::RecursiveMutex local_heaps_mutex_;
  LocalHeap* local_heaps_head_;

  std::atomic<int> active_safepoint_scopes_;

  friend class Heap;
  friend class GlobalSafepoint;
  friend class GlobalSafepointScope;
  friend class LocalHeap;
  friend class PersistentHandles;
  friend class SafepointScope;
};

class V8_NODISCARD SafepointScope {
 public:
  V8_EXPORT_PRIVATE explicit SafepointScope(Heap* heap);
  V8_EXPORT_PRIVATE ~SafepointScope();

 private:
  IsolateSafepoint* safepoint_;
};

// Used for reaching a global safepoint, a safepoint across all client isolates
// of the shared isolate.
class GlobalSafepoint final {
 public:
  explicit GlobalSafepoint(Isolate* isolate);

  void AppendClient(Isolate* client);
  void RemoveClient(Isolate* client);

  template <typename Callback>
  void IterateClientIsolates(Callback callback) {
    for (Isolate* current = clients_head_; current;
         current = current->global_safepoint_next_client_isolate_) {
      callback(current);
    }
  }

  void AssertNoClients();

 private:
  void EnterGlobalSafepointScope(Isolate* initiator);
  void LeaveGlobalSafepointScope(Isolate* initiator);

  Isolate* const shared_isolate_;
  Heap* const shared_heap_;
  base::Mutex clients_mutex_;
  Isolate* clients_head_ = nullptr;

  friend class GlobalSafepointScope;
  friend class Isolate;
};

class V8_NODISCARD GlobalSafepointScope {
 public:
  V8_EXPORT_PRIVATE explicit GlobalSafepointScope(Isolate* initiator);
  V8_EXPORT_PRIVATE ~GlobalSafepointScope();

 private:
  Isolate* const initiator_;
  Isolate* const shared_isolate_;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_HEAP_SAFEPOINT_H_
