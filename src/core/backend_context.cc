// Copyright (c) 2019-2020, NVIDIA CORPORATION. All rights reserved.
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

#include "src/core/backend_context.h"

#include "src/core/cuda_utils.h"
#include "src/core/logging.h"
#include "src/core/nvtx.h"
#include "src/core/provider.h"

namespace nvidia { namespace inferenceserver {

BackendContext::BackendContext(
    const std::string& name, const int gpu_device, const int max_batch_size)
    : name_(name), gpu_device_(gpu_device), max_batch_size_(max_batch_size)
{
#ifdef TRTIS_ENABLE_GPU
  stream_ = nullptr;
#endif  // TRTIS_ENABLE_GPU
}

BackendContext::~BackendContext()
{
#ifdef TRTIS_ENABLE_GPU
  if (stream_ != nullptr) {
    cudaError_t err = cudaStreamDestroy(stream_);
    if (err != cudaSuccess) {
      LOG_ERROR << "Failed to destroy cuda stream: " << cudaGetErrorString(err);
    }
    stream_ = nullptr;
  }
#endif  // TRTIS_ENABLE_GPU
}

Status
BackendContext::CreateCudaStream(
    const int cuda_stream_priority, cudaStream_t* stream)
{
#ifdef TRTIS_ENABLE_GPU
  if (gpu_device_ != NO_GPU_DEVICE) {
    // Make sure that correct device is set before creating stream and
    // then restore the device to what was set by the caller.
    int current_device;
    auto cuerr = cudaGetDevice(&current_device);
    bool overridden = false;
    if (cuerr == cudaSuccess) {
      overridden = (current_device != gpu_device_);
      if (overridden) {
        cuerr = cudaSetDevice(gpu_device_);
      }
    }

    if (cuerr == cudaSuccess) {
      cudaStream_t* s = (stream == nullptr) ? &stream_ : stream;
      cuerr = cudaStreamCreateWithPriority(
          s, cudaStreamDefault, cuda_stream_priority);
    }

    if (overridden) {
      cudaSetDevice(current_device);
    }

    if (cuerr != cudaSuccess) {
      return Status(
          RequestStatusCode::INTERNAL, "unable to create stream for " + name_ +
                                           ": " + cudaGetErrorString(cuerr));
    }
  }
#endif  // TRTIS_ENABLE_GPU

  return Status::Success;
}

bool
BackendContext::SetInputBuffer(
    const std::string& name, const std::vector<size_t>& expected_byte_sizes,
    std::vector<Scheduler::Payload>* payloads, InputInfo* input)
{
  return SetInputBuffer(name, expected_byte_sizes, payloads, stream_, input);
}

bool
BackendContext::SetInputBuffer(
    const std::string& name, const std::vector<size_t>& expected_byte_sizes,
    std::vector<Scheduler::Payload>* payloads, cudaStream_t stream,
    InputInfo* input)
{
  bool cuda_copy = false;

  bool need_buffer;
  TRTSERVER_Memory_Type candidate_type;
  GetIndirectBufferRequirement(
      input->memory_type_, &candidate_type, &need_buffer);
  BufferInfo pinned_buffer_info{0, 0, {}};

  // Visit the payloads in order and copy the input tensors to
  // 'buffer'.
  size_t buffer_copy_offset = 0;
  for (size_t idx = 0; idx < expected_byte_sizes.size(); idx++) {
    auto& payload = (*payloads)[idx];
    const size_t expected_byte_size = expected_byte_sizes[idx];

    const Memory* data;
    payload.status_ =
        payload.request_provider_->GetMemoryWithOverride(name, &data);
    size_t copied_byte_size = 0;
    size_t data_idx = 0;
    while (payload.status_.IsOk()) {
      auto src_memory_type = input->memory_type_;
      auto src_memory_type_id = input->memory_type_id_;
      size_t content_byte_size = expected_byte_size - copied_byte_size;
      const void* content = data->BufferAt(
          data_idx, &content_byte_size, &src_memory_type, &src_memory_type_id);

      // No more input content available then done with copying...
      if (content == nullptr) {
        break;
      }

      if ((copied_byte_size + content_byte_size) > expected_byte_size) {
        payload.status_ = Status(
            RequestStatusCode::INVALID_ARG,
            "unexpected size " +
                std::to_string(copied_byte_size + content_byte_size) +
                " for inference input '" + name + "', expecting " +
                std::to_string(expected_byte_size));
        break;
      }

      if (content_byte_size > 0) {
        // Defer memory copy for the buffer if it's better put into an
        // intermediate pinned buffer first.
        if (need_buffer && (src_memory_type == candidate_type)) {
          std::get<1>(pinned_buffer_info) += content_byte_size;
          std::get<2>(pinned_buffer_info).emplace_back(idx, data, data_idx);
        } else {
          // If copy should be perform directly, two steps to be done:
          // 1. Issue copy for the current buffer
          // 2. Finalize the existing intermediate buffer
          bool cuda_used = false;
          payload.status_ = CopyBuffer(
              name, src_memory_type, src_memory_type_id, input->memory_type_,
              input->memory_type_id_, content_byte_size, content,
              input->input_buffer_ + buffer_copy_offset + copied_byte_size,
              stream, &cuda_used);
          cuda_copy |= cuda_used;

          if (std::get<1>(pinned_buffer_info) > 0) {
            cuda_copy |= IssueIndirectInputBufferCopy(
                name, pinned_buffer_info, payloads, stream, input);
            // reset 'pinned_buffer_info'
            pinned_buffer_info = {
                buffer_copy_offset + copied_byte_size + content_byte_size,
                0,
                {}};
          }
        }
      }

      copied_byte_size += content_byte_size;
      data_idx++;
    }

    if (payload.status_.IsOk() && (copied_byte_size != expected_byte_size)) {
      payload.status_ = Status(
          RequestStatusCode::INTERNAL,
          "expected " + std::to_string(expected_byte_size) +
              " bytes of data for inference input '" + name + "', got " +
              std::to_string(copied_byte_size));
    }

    // When the payload has unexpected status, maintain a new indirect buffer
    // as the contiguousity ends here. And there are pending indirect buffer
    // copies, issue them.
    if (!payload.status_.IsOk()) {
      if (std::get<1>(pinned_buffer_info) > 0) {
        cuda_copy |= IssueIndirectInputBufferCopy(
            name, pinned_buffer_info, payloads, stream, input);
      }
      // reset 'pinned_buffer_info'
      pinned_buffer_info = {buffer_copy_offset + expected_byte_size, 0, {}};
    }

    buffer_copy_offset += expected_byte_size;
  }

  // Issue pending indirect copy if any
  if (std::get<1>(pinned_buffer_info) > 0) {
    cuda_copy |= IssueIndirectInputBufferCopy(
        name, pinned_buffer_info, payloads, stream, input);
  }

  return cuda_copy;
}

void
BackendContext::GetIndirectBufferRequirement(
    TRTSERVER_Memory_Type ref_buffer_type,
    TRTSERVER_Memory_Type* candidate_type, bool* need_indirect_buffer)
{
  // The following matrix is used for both input and output, but we may want
  // to handle the two cases separately, because it is not symmetric when the
  // 'ref_buffer' is used as dst v.s. when it is used as src.
  // src   \ dest | non-pinned    | pinned     | device
  // non-pinned   | memcpy        | memcpy     | buffer needed
  // pinned       | memcpy        | memcpy     | cudaMemcpy
  // device       | buffer needed | cudaMemcpy | cudaMemcpy
  *need_indirect_buffer = (ref_buffer_type != TRTSERVER_MEMORY_CPU_PINNED);
  *candidate_type = ref_buffer_type == TRTSERVER_MEMORY_CPU
                        ? TRTSERVER_MEMORY_GPU
                        : TRTSERVER_MEMORY_CPU;
  return;
}

bool
BackendContext::IssueIndirectInputBufferCopy(
    const std::string& name,
    const BackendContext::BufferInfo& pinned_buffer_info,
    std::vector<Scheduler::Payload>* payloads, cudaStream_t stream,
    InputInfo* input)
{
  NVTX_RANGE(nvtx_, "IndirectInputBufferCopy");

  bool cuda_copy = false;
  bool cuda_used = false;
  auto mem_type = TRTSERVER_MEMORY_CPU_PINNED;
  int64_t mem_id = 0;
  const auto input_offset = std::get<0>(pinned_buffer_info);
  const auto pinned_buffer_size = std::get<1>(pinned_buffer_info);
  std::unique_ptr<AllocatedSystemMemory> local_indirect_buffer(
      new AllocatedSystemMemory(pinned_buffer_size, mem_type, mem_id));
  char* buffer = local_indirect_buffer->MutableBuffer(&mem_type, &mem_id);
  std::vector<size_t> payload_idxs;

  // If can't reserve the intermediate buffer, the copy should be
  // perform directly to input buffer
  bool direct_copy = (mem_type != TRTSERVER_MEMORY_CPU_PINNED);
  if (direct_copy) {
    buffer = input->input_buffer_ + input_offset;
    mem_type = input->memory_type_;
    mem_id = input->memory_type_id_;
  }

  auto src_mem_type = input->memory_type_;
  auto src_mem_type_id = input->memory_type_id_;
  size_t src_byte_size;
  size_t buffer_offset = 0;
  for (const auto& data_info : std::get<2>(pinned_buffer_info)) {
    payload_idxs.emplace_back(std::get<0>(data_info));
    const void* src_data = std::get<1>(data_info)->BufferAt(
        std::get<2>(data_info), &src_byte_size, &src_mem_type,
        &src_mem_type_id);
    (*payloads)[payload_idxs.back()].status_ = CopyBuffer(
        name, src_mem_type, src_mem_type_id, mem_type, mem_id, src_byte_size,
        src_data, buffer + buffer_offset, stream, &cuda_used);
    buffer_offset += src_byte_size;
    cuda_copy |= cuda_used;
  }

  if (!direct_copy) {
    input->indirect_buffers_.emplace_back(
        std::move(local_indirect_buffer), input_offset,
        std::move(payload_idxs));
  }

  return cuda_copy;
}

bool
BackendContext::SetFixedSizeOutputBuffer(
    const std::string& name, const size_t batch1_byte_size, const char* content,
    const std::vector<int64_t>& content_shape,
    TRTSERVER_Memory_Type src_memory_type, int64_t src_memory_type_id,
    std::vector<Scheduler::Payload>* payloads)
{
  bool cuda_copy = false;
  size_t content_offset = 0;
  for (auto& payload : *payloads) {
    const InferRequestHeader& request_header =
        payload.request_provider_->RequestHeader();
    const size_t expected_byte_size =
        request_header.batch_size() * batch1_byte_size;

    // If 'payload' should have valid output (status ok) and
    // if 'payload' requested this output then copy it from
    // 'content'. If it did not request this output then just
    // skip it in the 'content'.
    if (payload.status_.IsOk() && (payload.response_provider_ != nullptr) &&
        payload.response_provider_->RequiresOutput(name)) {
      auto dst_memory_type = src_memory_type;
      int64_t dst_memory_type_id;
      void* buffer = nullptr;

      // try to get buffer with the same memory type as the output tensor
      Status status = payload.response_provider_->AllocateOutputBuffer(
          name, &buffer, expected_byte_size, content_shape, src_memory_type,
          src_memory_type_id, &dst_memory_type, &dst_memory_type_id);
      if (status.IsOk() && (expected_byte_size != 0)) {
        if (buffer == nullptr) {
          status = Status(
              RequestStatusCode::INTERNAL,
              "failed to allocate buffer for output '" + name + "'");
        } else {
          bool cuda_used = false;
          status = CopyBuffer(
              name, src_memory_type, src_memory_type_id, dst_memory_type,
              dst_memory_type_id, expected_byte_size, content + content_offset,
              buffer, stream_, &cuda_used);
          cuda_copy |= cuda_used;
        }
      }

      payload.status_ = status;
    }

    content_offset += expected_byte_size;
  }

  return cuda_copy;
}

Status
BackendContext::GetContiguousInputContent(
    const std::string& name, TRTSERVER_Memory_Type memory_type,
    int64_t memory_type_id, const Scheduler::Payload& payload,
    const char** content, size_t* content_byte_size,
    std::unique_ptr<AllocatedSystemMemory>* contiguous_buffer, bool* cuda_copy)
{
  contiguous_buffer->reset();

  // Peek input buffers to check if data copy is necessary
  MemoryReference input_buffers;
  size_t chunk_count = 0;
  bool type_mismatch = false;
  while (true) {
    TRTSERVER_Memory_Type src_memory_type = memory_type;
    int64_t src_memory_type_id = memory_type_id;
    size_t src_byte_size = *content_byte_size;
    const void* src_ptr;

    RETURN_IF_ERROR(payload.request_provider_->GetNextInputContent(
        name, &src_ptr, &src_byte_size, &src_memory_type, &src_memory_type_id));

    if (src_ptr != nullptr) {
      input_buffers.AddBuffer(
          (const char*)src_ptr, src_byte_size, src_memory_type,
          src_memory_type_id);
      chunk_count++;
      type_mismatch |=
          ((src_memory_type != memory_type) ||
           (src_memory_type_id != memory_type_id));
    } else {
      break;
    }
  }

  if (chunk_count == 0) {
    *content = nullptr;
    *content_byte_size = 0;
  } else if ((chunk_count == 1) && !type_mismatch) {
    *content = input_buffers.BufferAt(
        0, content_byte_size, &memory_type, &memory_type_id);
  } else {
    contiguous_buffer->reset(new AllocatedSystemMemory(
        input_buffers.TotalByteSize(), memory_type, memory_type_id));
    auto dst_ptr =
        (*contiguous_buffer)->MutableBuffer(&memory_type, &memory_type_id);
    if (dst_ptr == nullptr) {
      return Status(
          RequestStatusCode::INTERNAL, "failed to allocate contiguous buffer");
    }

    size_t offset = 0;
    for (size_t i = 0; i < chunk_count; i++) {
      bool cuda_used;
      TRTSERVER_Memory_Type src_memory_type;
      int64_t src_memory_type_id;
      auto src_ptr = input_buffers.BufferAt(
          i, content_byte_size, &src_memory_type, &src_memory_type_id);
      RETURN_IF_ERROR(CopyBuffer(
          name, src_memory_type, src_memory_type_id, memory_type,
          memory_type_id, *content_byte_size, src_ptr, dst_ptr + offset,
          stream_, &cuda_used));
      *cuda_copy |= cuda_used;
      offset += *content_byte_size;
    }

    *content = dst_ptr;
    *content_byte_size = (*contiguous_buffer)->TotalByteSize();
  }

  return Status::Success;
}

Status
BackendContext::CompareOutputDims(
    const std::string& tensor_name, const std::vector<int64_t>& model_shape,
    const DimsList& dims, const bool supports_batching)
{
  if (supports_batching) {
    DimsList full_dims;
    full_dims.Add(WILDCARD_DIM);
    for (int i = 0; i < dims.size(); ++i) {
      full_dims.Add(dims[i]);
    }

    bool succ = (model_shape.size() == (size_t)full_dims.size());
    if (succ) {
      for (int i = 0; i < full_dims.size(); ++i) {
        const int64_t model_dim = model_shape[i];
        if (full_dims[i] != WILDCARD_DIM) {
          succ &= (model_dim == full_dims[i]);
        }
      }
    }

    if (!succ) {
      return Status(
          RequestStatusCode::INVALID_ARG,
          "tensor '" + tensor_name + "': the model expects " +
              std::to_string(model_shape.size()) + " dimensions (shape " +
              DimsListToString(model_shape) +
              ") but the model configuration specifies " +
              std::to_string(full_dims.size()) +
              " dimensions (an initial batch dimension because max_batch_size "
              "> 0 followed by the explicit tensor shape, making complete "
              "shape " +
              DimsListToString(full_dims) + ")");
    }
  } else {
    // ! supports_batching
    bool succ = (model_shape.size() == (size_t)dims.size());
    if (succ) {
      for (int i = 0; i < dims.size(); ++i) {
        const int64_t model_dim = model_shape[i];
        if (dims[i] != WILDCARD_DIM) {
          succ &= (model_dim == dims[i]);
        }
      }
    }

    if (!succ) {
      return Status(
          RequestStatusCode::INVALID_ARG,
          "tensor '" + tensor_name + "': the model expects " +
              std::to_string(model_shape.size()) + " dimensions (shape " +
              DimsListToString(model_shape) +
              ") but the model configuration specifies " +
              std::to_string(dims.size()) + " dimensions (shape " +
              DimsListToString(dims) + ")");
    }
  }

  return Status::Success;
}

}}  // namespace nvidia::inferenceserver
