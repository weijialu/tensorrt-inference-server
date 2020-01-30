// Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "src/core/scheduler_utils.h"

#include "src/core/provider.h"

namespace nvidia { namespace inferenceserver {

Status
InitPendingShape(
    const int64_t runner_id, const Scheduler::Payload& payload,
    const std::unordered_map<std::string, bool>& enforce_equal_shape_tensors,
    const Scheduler::StandardShapeTensorPeekFunc& OnPeek,
    PendingBatchShapes* pending_batch_shapes)
{
  pending_batch_shapes->clear();

  const InferRequestHeader& request =
      payload.request_provider_->RequestHeader();
  for (const auto& input : request.input()) {
    const auto itr = enforce_equal_shape_tensors.find(input.name());
    if (itr != enforce_equal_shape_tensors.end()) {
      std::pair<DimsList, std::vector<int64_t>> shapes;
      shapes.first = input.dims();

      // For shape tensors must compare the contents of the tensor in
      // addition to the tensor shape itself.
      if (itr->second) {
        RETURN_IF_ERROR(OnPeek(runner_id, input, payload, &shapes.second));
      }

      pending_batch_shapes->emplace(
          std::make_pair(input.name(), std::move(shapes)));
    }
  }

  return Status::Success;
}

bool
CompareWithPendingShape(
    const int64_t runner_id, const Scheduler::Payload& payload,
    const Scheduler::StandardShapeTensorPeekFunc& OnPeek,
    const PendingBatchShapes& pending_batch_shapes)
{
  const InferRequestHeader& request =
      payload.request_provider_->RequestHeader();

  for (const auto& input : request.input()) {
    const auto itr = pending_batch_shapes.find(input.name());
    if (itr != pending_batch_shapes.end()) {
      if (!CompareDims(itr->second.first, input.dims())) {
        return false;
      }

      // If there are shape-tensor contents then compare those as
      // well.
      if (!itr->second.second.empty()) {
        std::vector<int64_t> shape;

        // If fail getting the tensor shape then conservatively return
        // false to indicate that the shapes don't match.
        if (!OnPeek(runner_id, input, payload, &shape).IsOk()) {
          return false;
        }
        if (!CompareDims(itr->second.second, shape)) {
          return false;
        }
      }
    }
  }

  return true;
}

}}  // namespace nvidia::inferenceserver
