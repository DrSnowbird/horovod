// Copyright 2019 Uber Technologies, Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include "gloo_operations.h"

#include "gloo/allgather_ring.h"
#include "gloo/allreduce_ring.h"
#include "gloo/broadcast_one_to_all.h"

namespace horovod {
namespace common {

IGlooAlgorithms* GetAlgorithmsForType(DataType dtype, GlooContext* gloo_context) {
  switch (dtype) {
    case HOROVOD_UINT8:
      return new GlooAlgorithms<u_int8_t>(gloo_context, 1);
    case HOROVOD_INT8:
      return new GlooAlgorithms<int8_t>(gloo_context, 1);
    case HOROVOD_UINT16:
      return new GlooAlgorithms<u_int16_t>(gloo_context, 2);
    case HOROVOD_INT16:
      return new GlooAlgorithms<int16_t>(gloo_context, 2);
    case HOROVOD_INT32:
      return new GlooAlgorithms<int32_t>(gloo_context, 4);
    case HOROVOD_INT64:
      return new GlooAlgorithms<int64_t>(gloo_context, 8);
    case HOROVOD_FLOAT32:
      return new GlooAlgorithms<float_t>(gloo_context, 4);
    case HOROVOD_FLOAT64:
      return new GlooAlgorithms<double_t>(gloo_context, 8);
    case HOROVOD_BOOL:
      return new GlooAlgorithms<bool>(gloo_context, 1);
    default:
      throw std::logic_error("Type " + DataType_Name(dtype) +
                             " is not supported in Gloo mode.");
  }
}

template<typename T>
GlooAlgorithms<T>::GlooAlgorithms(GlooContext* gloo_context, int element_size)
    : gloo_context_(gloo_context), element_size_(element_size) {}


template<typename T>
void GlooAlgorithms<T>::Allreduce(void* buffer_data, int num_elements) {
  std::vector<T*> ptrs;
  ptrs.push_back((T*) buffer_data);

  gloo::AllreduceRing<T> allreduce(gloo_context_->ctx, ptrs, num_elements);
  allreduce.run();
}

template<typename T>
void GlooAlgorithms<T>::Allgather(void* buffer_data, void* buffer_out, int num_elements) {
  std::vector<const T*> ptrs;
  ptrs.push_back((T*) buffer_data);

  gloo::AllgatherRing<T> allgather(gloo_context_->ctx, ptrs, (T*) buffer_out, num_elements);
  allgather.run();
}

template<typename T>
void GlooAlgorithms<T>::Broadcast(void* buffer_data, int num_elements, int root_rank) {
  std::vector<T*> ptrs;
  ptrs.push_back((T*) buffer_data);

  gloo::BroadcastOneToAll<T> bcast(gloo_context_->ctx, ptrs, (size_t) num_elements, root_rank, root_rank);
  bcast.run();
}

template<typename T>
int GlooAlgorithms<T>::ElementSize() const {
  return element_size_;
}

GlooAllreduce::GlooAllreduce(GlooContext* gloo_context, HorovodGlobalState* global_state)
    : AllreduceOp(global_state), gloo_context_(gloo_context) {}

Status GlooAllreduce::Execute(std::vector<TensorTableEntry>& entries, const Response& response) {
  auto& first_entry = entries[0];

  void* buffer_data;
  int num_elements = (int) NumElements(entries);

  // Copy memory into the fusion buffer.
  auto& timeline = global_state_->timeline;
  if (entries.size() > 1) {
    timeline.ActivityStartAll(entries, MEMCPY_IN_FUSION_BUFFER);
    const void* fused_input_data;
    size_t buffer_len;
    MemcpyInFusionBuffer(entries, fused_input_data, buffer_data, buffer_len);
    timeline.ActivityEndAll(entries);
  } else {
    buffer_data = (void*) first_entry.output->data();
    std::memcpy(buffer_data, first_entry.tensor->data(),
                (size_t) first_entry.tensor->size());
  }

  // Do allreduce.
  timeline.ActivityStartAll(entries, GLOO_ALLREDUCE);
  std::unique_ptr<IGlooAlgorithms> gloo_algos(GetAlgorithmsForType(first_entry.tensor->dtype(), gloo_context_));
  gloo_algos->Allreduce(buffer_data, num_elements);
  timeline.ActivityEndAll(entries);

  // Copy memory out of the fusion buffer.
  if (entries.size() > 1) {
    timeline.ActivityStartAll(entries, MEMCPY_OUT_FUSION_BUFFER);
    MemcpyOutFusionBuffer(buffer_data, entries);
    timeline.ActivityEndAll(entries);
  }

  return Status::OK();
}

bool GlooAllreduce::Enabled(const ParameterManager& param_manager,
                            const std::vector<TensorTableEntry>& entries,
                            const Response& response) const {
  return true;
}

GlooAllgather::GlooAllgather(GlooContext* gloo_context, HorovodGlobalState* global_state)
    : AllgatherOp(global_state), gloo_context_(gloo_context) {}

bool GlooAllgather::Enabled(const ParameterManager& param_manager,
                            const std::vector<TensorTableEntry>& entries,
                            const Response& response) const {
  return true;
}

Status GlooAllgather::Execute(std::vector<TensorTableEntry>& entries, const Response& response) {
  auto& timeline = global_state_->timeline;

  // Sizes of subcomponents of each entry from all ranks
  auto** entry_component_sizes = new int64_t* [entries.size()];

  // Offset of each subcomponent of every entry in the final buffer after
  // allgatherv
  auto** entry_component_offsets = new int64_t* [entries.size()];

  auto* recvcounts = new int[global_state_->size]();
  auto* displcmnts = new int[global_state_->size]();

  for (size_t ec = 0; ec < entries.size(); ++ec) {
    entry_component_sizes[ec] = new int64_t[global_state_->size]();
    entry_component_offsets[ec] = new int64_t[global_state_->size]();
  }

  auto& first_entry = entries[0];

  timeline.ActivityStartAll(entries, ALLOCATE_OUTPUT);
  Status status = AllocateOutput(entries, response, entry_component_sizes, recvcounts);
  if (!status.ok()) {
    return status;
  }
  timeline.ActivityEndAll(entries);

  SetDisplacements(recvcounts, displcmnts);
  SetEntryComponentOffsets(entries, entry_component_sizes, recvcounts, entry_component_offsets);

  std::unique_ptr<IGlooAlgorithms> gloo_algos(GetAlgorithmsForType(first_entry.tensor->dtype(), gloo_context_));
  int element_size = gloo_algos->ElementSize();

  const void* sendbuf = nullptr;
  void* buffer_data;
  int64_t total_num_elements = NumElements(entries);

  if (entries.size() > 1) {
    timeline.ActivityStartAll(entries, MEMCPY_IN_FUSION_BUFFER);
    MemcpyInFusionBuffer(entries, displcmnts, element_size, buffer_data);
    timeline.ActivityEndAll(entries);
  } else {
    sendbuf = first_entry.tensor->data();
    buffer_data = (void*) first_entry.output->data();
  }

  // TODO(travis): this will not work, need allgatherv implementation
  global_state_->timeline.ActivityStartAll(entries, GLOO_ALLGATHER);
  gloo_algos->Allgather(buffer_data, buffer_data, (int) total_num_elements);
  global_state_->timeline.ActivityEndAll(entries);

  if (entries.size() > 1) {
    timeline.ActivityStartAll(entries, MEMCPY_OUT_FUSION_BUFFER);
    MemcpyOutFusionBuffer(entry_component_offsets, entry_component_sizes,
                          buffer_data, element_size, entries);
    timeline.ActivityEndAll(entries);
  }

  delete[] recvcounts;
  delete[] displcmnts;

  for (size_t ec = 0; ec < entries.size(); ++ec) {
    delete[] entry_component_sizes[ec];
    delete[] entry_component_offsets[ec];
  }
  delete[] entry_component_sizes;
  delete[] entry_component_offsets;

  return Status::OK();
}

GlooBroadcast::GlooBroadcast(GlooContext* gloo_context, HorovodGlobalState* global_state)
    : BroadcastOp(global_state), gloo_context_(gloo_context) {}

Status GlooBroadcast::Execute(std::vector<TensorTableEntry>& entries, const Response& response) {
  assert(entries.size() == 1);
  auto e = entries[0];

  // On root rank, MPI_Bcast sends data, on other ranks it receives data.
  void* data_ptr;
  if (global_state_->rank == e.root_rank) {
    data_ptr = (void*) e.tensor->data();
  } else {
    data_ptr = (void*) e.output->data();
  }

  global_state_->timeline.ActivityStartAll(entries, GLOO_BCAST);
  std::unique_ptr<IGlooAlgorithms> gloo_algos(GetAlgorithmsForType(e.tensor->dtype(), gloo_context_));
  gloo_algos->Broadcast(data_ptr, (int) e.tensor->shape().num_elements(), e.root_rank);
  global_state_->timeline.ActivityEndAll(entries);

  return Status::OK();
}

bool GlooBroadcast::Enabled(const ParameterManager& param_manager,
                            const std::vector<TensorTableEntry>& entries,
                            const Response& response) const {
  return true;
}

} // namespace common
} // namespace horovod