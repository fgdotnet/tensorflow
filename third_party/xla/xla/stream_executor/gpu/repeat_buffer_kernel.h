/* Copyright 2025 The OpenXLA Authors.

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

#ifndef XLA_STREAM_EXECUTOR_GPU_REPEAT_BUFFER_KERNEL_H_
#define XLA_STREAM_EXECUTOR_GPU_REPEAT_BUFFER_KERNEL_H_

#include <cstdint>

#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/kernel.h"

namespace stream_executor::gpu {

// Defines a trait for the RepeatBufferKernel that can be used to register
// and look up the kernel in the GPU kernel registry.
struct RepeatBufferKernel {
  using KernelType =
      stream_executor::TypedKernel<stream_executor::DeviceMemoryBase, int64_t,
                                   int64_t>;
};

}  // namespace stream_executor::gpu

#endif  // XLA_STREAM_EXECUTOR_GPU_REPEAT_BUFFER_KERNEL_H_
