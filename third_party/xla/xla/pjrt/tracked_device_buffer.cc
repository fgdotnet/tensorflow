/* Copyright 2019 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/pjrt/tracked_device_buffer.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/any_invocable.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "xla/pjrt/event_pool.h"
#include "xla/pjrt/pjrt_client.h"
#include "xla/pjrt/pjrt_common.h"
#include "xla/service/shaped_buffer.h"
#include "xla/shape.h"
#include "xla/shape_tree.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/device_memory_allocator.h"
#include "xla/stream_executor/event.h"
#include "xla/tsl/concurrency/async_value_ref.h"
#include "xla/tsl/concurrency/ref_count.h"
#include "xla/tsl/platform/logging.h"
#include "tsl/profiler/lib/connected_traceme.h"
#include "tsl/profiler/lib/context_types.h"

namespace xla {

void BufferSequencingEvent::SetSequencingEvent(EventPool::Handle event,
                                               se::Stream* stream) {
  {
    absl::MutexLock lock(&mu_);
    CHECK(!event_.event());
    event_ = std::move(event);
    CHECK(streams_defined_on_.empty());
    streams_defined_on_.push_back(stream);
    sequence_number_.store(event_.sequence_number(), std::memory_order_seq_cst);
  }
  defined_status_.emplace(absl::OkStatus());
}

bool BufferSequencingEvent::EventHasBeenRecorded() const {
  return event_.event() != nullptr;
}

uint64_t BufferSequencingEvent::sequence_number() const {
  uint64_t seq = sequence_number_.load(std::memory_order_seq_cst);
  return seq;
}

void BufferSequencingEvent::WaitForEventOnStream(se::Stream* stream) {
  absl::MutexLock lock(&mu_);

  // We cannot wait for an event until ThenRecordEvent has been called; on GPU
  // newly created events are deemed to have already happened past.
  mu_.Await(
      absl::Condition(this, &BufferSequencingEvent::EventHasBeenRecorded));

  // The set of defined streams is expected to be very small indeed (usually
  // 1-2), so a simple linear scan should be fast enough.
  if (std::find(streams_defined_on_.begin(), streams_defined_on_.end(),
                stream) != streams_defined_on_.end()) {
    // stream is in streams_defined_on_; it doesn't need to be waited on.
    return;
  }

  stream->WaitFor(event_.event()).IgnoreError();
  streams_defined_on_.push_back(stream);
}

absl::Status BufferSequencingEvent::WaitForEventOnExternalStream(
    std::intptr_t stream) {
  absl::MutexLock lock(&mu_);

  // We cannot wait for an event until ThenRecordEvent has been called; on GPU
  // newly created events are deemed to have already happened past.
  // TODO(skyewm): do we need this? WaitForEventOnExternalStream is only
  // implemented for GPU.
  mu_.Await(
      absl::Condition(this, &BufferSequencingEvent::EventHasBeenRecorded));

  return event_.event()->WaitForEventOnExternalStream(stream);
}

bool BufferSequencingEvent::IsPredeterminedErrorOrDefinedOn(
    se::Stream* stream) {
  tsl::BlockUntilReady(defined_status_);
  CHECK(defined_status_.IsConcrete());

  // IsPredeterminedError
  if (!defined_status_->ok()) {
    return true;
  }

  // The set of defined streams is expected to be very small indeed (usually
  // 1-2), so a simple linear scan should be fast enough.
  absl::MutexLock lock(&mu_);
  return absl::c_find(streams_defined_on_, stream) != streams_defined_on_.end();
}

bool BufferSequencingEvent::IsComplete() {
  absl::MutexLock lock(&mu_);

  // We cannot wait for an event until ThenRecordEvent has been called; on
  // GPU newly created events are deemed to have already happened past.
  mu_.Await(
      absl::Condition(this, &BufferSequencingEvent::EventHasBeenRecorded));

  return event_.event()->PollForStatus() == se::Event::Status::kComplete;
}

void BufferSequencingEvent::ExecuteOrAddToFutureTasks(
    const std::string& task_name, std::function<void()> task) {
  tsl::profiler::TraceMeProducer producer(
      "BufferSequencingEvent::ExecuteOrAddToFutureTasks",
      tsl::profiler::ContextType::kPjRt);

  auto traced_task = [task = std::move(task),
                      context_id = producer.GetContextId()]() {
    tsl::profiler::TraceMeConsumer consumer("BufferSequencingEvent::Execute",
                                            tsl::profiler::ContextType::kPjRt,
                                            context_id);
    task();
  };

  // Execute the `task` when definition event becomes available. If it's already
  // available, the task will be executed immediately.
  defined_status_.AndThen(
      [this, traced_task = std::move(traced_task)]() mutable {
        thread_pool_->Schedule(std::move(traced_task));
      });
}

ShapedBuffer RawSEDeviceMemory::AsShapedBuffer(
    PjRtDevice* device, const Shape& on_device_shape) const {
  ShapedBuffer shaped_buffer(on_device_shape, device->local_device_id().value(),
                             device->local_hardware_id().value());
  ShapeTree<se::DeviceMemoryBase>::iterator iterator =
      shaped_buffer.buffers().begin();
  CHECK(iterator != shaped_buffer.buffers().end());
  iterator->second = mem();
  ++iterator;
  CHECK(iterator == shaped_buffer.buffers().end());
  return shaped_buffer;
}

class AllocatedRawSEDeviceMemory : public RawSEDeviceMemory {
 public:
  AllocatedRawSEDeviceMemory(se::DeviceMemoryBase value, int device_ordinal,
                             se::DeviceMemoryAllocator* allocator)
      : RawSEDeviceMemory(value),
        allocator_(allocator),
        device_ordinal_(device_ordinal) {}

  ~AllocatedRawSEDeviceMemory() override {
    if (allocator_) {
      absl::Status status = allocator_->Deallocate(device_ordinal_, mem());
      if (!status.ok()) {
        LOG(ERROR) << "Buffer deallocation failed: " << status;
      }
    }
  }

  void UnsafeReleaseMemory() override { allocator_ = nullptr; }

 private:
  se::DeviceMemoryAllocator* allocator_;
  int device_ordinal_;
};

tsl::RCReference<RawSEDeviceMemory> RawSEDeviceMemory::Create(
    se::DeviceMemoryBase value, PjRtLocalDeviceId device_id,
    se::DeviceMemoryAllocator* allocator) {
  return tsl::MakeRef<AllocatedRawSEDeviceMemory>(value, device_id.value(),
                                                  allocator);
}

class ForeignRawSEDeviceMemory : public RawSEDeviceMemory {
 public:
  ForeignRawSEDeviceMemory(se::DeviceMemoryBase value,
                           absl::AnyInvocable<void() &&> on_delete_callback)
      : RawSEDeviceMemory(value),
        on_delete_callback_(std::move(on_delete_callback)) {}

  ~ForeignRawSEDeviceMemory() override { std::move(on_delete_callback_)(); }

  void UnsafeReleaseMemory() override {
    LOG(FATAL) << "ForeignRawSEDeviceMemory cannot be donated.";
  }

 private:
  absl::AnyInvocable<void() &&> on_delete_callback_;
};

tsl::RCReference<RawSEDeviceMemory> RawSEDeviceMemory::CreateForeign(
    se::DeviceMemoryBase value,
    absl::AnyInvocable<void() &&> on_delete_callback) {
  return tsl::MakeRef<ForeignRawSEDeviceMemory>(value,
                                                std::move(on_delete_callback));
}

ShapedBuffer TrackedDeviceBuffer::AsShapedBuffer(
    const Shape& on_device_shape) const {
  ShapedBuffer shaped_buffer(on_device_shape,
                             device_->local_device_id().value(),
                             device_->local_hardware_id().value());
  ShapeTree<se::DeviceMemoryBase>::iterator iterator =
      shaped_buffer.buffers().begin();
  if (device_memory_) {
    CHECK(iterator != shaped_buffer.buffers().end());
    iterator->second = device_memory_->mem();
    ++iterator;
  }
  CHECK(iterator == shaped_buffer.buffers().end());
  return shaped_buffer;
}

TrackedDeviceBuffer::TrackedDeviceBuffer(
    PjRtDevice* device, tsl::RCReference<RawSEDeviceMemory> device_memory,
    absl::Span<const std::shared_ptr<BufferSequencingEvent>> definition_events)
    : device_(device),
      device_memory_(std::move(device_memory)),
      definition_events_(std::make_move_iterator(definition_events.begin()),
                         std::make_move_iterator(definition_events.end())),
      in_use_(true) {}

TrackedDeviceBuffer::~TrackedDeviceBuffer() = default;

void TrackedDeviceBuffer::ReleaseDeviceMemory() {
  device_memory_ = tsl::RCReference<RawSEDeviceMemory>();
}

void TrackedDeviceBuffer::ConfirmDonation() {
  // As a sanity check ensure no more usage events can be added to the buffer.
  LockUseAndTransferUsageEvents();
  // Release the memory so that no new usage is possible.
  ReleaseDeviceMemory();
}

void TrackedDeviceBuffer::AddUsageEvent(
    se::Stream* usage_stream, std::shared_ptr<BufferSequencingEvent> event,
    bool reference_held) {
  CHECK(in_use_);

  // If the event is 0, it means that the event is not recorded yet and the task
  // related to this event is deferred, so just add it.
  if (*event == 0) {
    usage_events_.push_back({usage_stream, event, reference_held});
    return;
  }

  for (auto& existing : usage_events_) {
    // If the existing event is 0, it means that the event is not recorded yet
    // and the task related to this event is deferred, so don't replace it.
    if (*existing.event == 0) continue;
    if (existing.stream == usage_stream) {
      if (*existing.event < *event) {
        existing.event = event;
        existing.reference_held = reference_held;
      }
      return;
    }
  }
  usage_events_.push_back({usage_stream, event, reference_held});
}

TrackedDeviceBuffer::StreamAndEventContainer
TrackedDeviceBuffer::LockUseAndTransferUsageEvents() {
  CHECK(in_use_);
  in_use_ = false;
  return std::move(usage_events_);
}

void GetDeviceBufferEvents(
    const TrackedDeviceBuffer& buffer, bool get_usage_events,
    absl::flat_hash_set<BufferSequencingEvent*>* events) {
  if (get_usage_events) {
    for (const auto& e : buffer.usage_events()) {
      events->insert(e.event.get());
    }
  } else {
    for (const auto& e : buffer.definition_events()) {
      events->insert(e.get());
    }
  }
}

void WaitForBufferDefinitionEventsOnStream(
    absl::Span<const std::shared_ptr<BufferSequencingEvent>> definition_events,
    se::Stream* stream) {
  if (definition_events.size() <= 1) {
    for (const auto& event : definition_events) {
      event->WaitForEventOnStream(stream);
    }
  } else {
    absl::flat_hash_set<BufferSequencingEvent*> events;
    for (const auto& event : definition_events) {
      if (events.emplace(event.get()).second) {
        event->WaitForEventOnStream(stream);
      }
    }
  }
}

}  // namespace xla
