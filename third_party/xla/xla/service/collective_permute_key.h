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

#ifndef XLA_SERVICE_COLLECTIVE_PERMUTE_KEY_H_
#define XLA_SERVICE_COLLECTIVE_PERMUTE_KEY_H_

#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/service/hlo_domain_map.h"
#include "xla/xla_data.pb.h"

namespace xla {

// Encapsulates all of the properties which must match for two
// collective-permute instructions to be compatible with each other (and hence
// be possible to combine the instructions).
using CollectivePermuteKey = std::tuple<
    /*source_target_pairs*/ std::vector<std::pair<int64_t, int64_t>>>;

std::optional<CollectivePermuteKey> GetCollectivePermuteKey(
    const HloInstruction* instruction);

}  // namespace xla

#endif  // XLA_SERVICE_COLLECTIVE_PERMUTE_KEY_H_
