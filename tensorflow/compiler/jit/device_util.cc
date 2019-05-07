/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/jit/device_util.h"

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_set.h"
#include "tensorflow/compiler/xla/status_macros.h"

namespace tensorflow {
namespace jit {
using xla::StatusOr;

void DeviceSet::Insert(DeviceId device_id) {
  int word_index = device_id.id() / kWordSize;
  int bit_index = device_id.id() % kWordSize;

  if (word_index >= storage_.size()) {
    storage_.resize(word_index + 1, 0);
  }

  storage_[word_index] |= (1ull << bit_index);
}

void DeviceSet::UnionWith(const DeviceSet& other) {
  if (other.storage_.size() > storage_.size()) {
    storage_.resize(other.storage_.size(), 0);
  }

  for (int i = 0; i < other.storage_.size(); i++) {
    storage_[i] |= other.storage_[i];
  }
}

bool DeviceSet::IsEmpty() const {
  return absl::c_all_of(storage_, [&](uint64 val) { return val == 0; });
}

xla::StatusOr<DeviceId> DeviceInfoCache::GetIdFor(absl::string_view name) {
  TF_RET_CHECK(!name.empty());

  auto it = name_to_id_.find(name);
  if (it != name_to_id_.end()) {
    return it->second;
  }

  int new_id = names_.size();
  names_.push_back(string(name));
  id_to_device_type_.push_back(absl::make_unique<DeviceType>(""));
  DeviceType* device_type = id_to_device_type_.back().get();
  TF_RETURN_IF_ERROR(DeviceNameToDeviceType(names_.back(), device_type));

  is_cpu_.push_back(device_type->type_string() == DEVICE_CPU);
  is_gpu_.push_back(device_type->type_string() == DEVICE_GPU);

  name_to_id_.emplace(string(name), DeviceId(new_id));

  const XlaOpRegistry::DeviceRegistration* compilation_device;
  if (!XlaOpRegistry::GetCompilationDevice(device_type->type(),
                                           &compilation_device)) {
    compilation_device = nullptr;
  }
  id_to_compilation_device_.push_back(compilation_device);

  return DeviceId(new_id);
}

string DeviceInfoCache::DebugString(const DeviceSet& device_set) const {
  std::vector<string> names;
  device_set.ForEach([&](DeviceId device_id) {
    names.push_back(string(GetNameFor(device_id)));
    return false;
  });

  return absl::StrCat("[", absl::StrJoin(names, ","), "]");
}
}  // namespace jit

Status DeviceNameToDeviceType(const string& device, DeviceType* device_type) {
  DeviceNameUtils::ParsedName parsed;
  if (!DeviceNameUtils::ParseFullName(device, &parsed)) {
    return errors::Internal("Malformed assigned device '", device, "'");
  }
  *device_type = DeviceType(parsed.type);
  return Status::OK();
}

Status PickDeviceForXlaImpl(const jit::DeviceInfoCache& device_info_cache,
                            const jit::DeviceSet& devices,
                            bool allow_mixing_unknown_and_cpu,
                            bool* out_can_pick_device,
                            absl::optional<jit::DeviceId>* out_device_picked) {
  if (out_can_pick_device) {
    *out_can_pick_device = true;
  }

#define FAILED_TO_PICK_DEVICE(failing_status) \
  do {                                        \
    if (out_can_pick_device) {                \
      *out_can_pick_device = false;           \
      return Status::OK();                    \
    } else {                                  \
      return failing_status;                  \
    }                                         \
  } while (false)

  TF_RET_CHECK(!devices.IsEmpty()) << "No devices to choose from";
  DCHECK_NE(out_can_pick_device == nullptr, out_device_picked == nullptr);

  absl::optional<jit::DeviceId> maybe_gpu_device;
  absl::optional<jit::DeviceId> maybe_cpu_device;
  absl::optional<jit::DeviceId> maybe_unknown_device;

  bool multiple_cpu_devices = false;
  bool multiple_gpu_devices = false;
  bool multiple_unknown_devices = false;

  devices.ForEach([&](jit::DeviceId device) {
    if (device_info_cache.IsGpu(device)) {
      if (maybe_gpu_device) {
        multiple_gpu_devices = true;
        return false;
      }
      maybe_gpu_device = device;
    } else if (device_info_cache.IsCpu(device)) {
      if (maybe_cpu_device) {
        multiple_cpu_devices = true;
        return false;
      }
      maybe_cpu_device = device;
    } else {
      if (maybe_unknown_device) {
        multiple_unknown_devices = true;
        return false;
      }
      maybe_unknown_device = device;
    }

    return true;
  });

  if (multiple_cpu_devices) {
    FAILED_TO_PICK_DEVICE(errors::Internal(
        "Multiple CPU devices ", device_info_cache.DebugString(devices)));
  }

  if (multiple_gpu_devices) {
    FAILED_TO_PICK_DEVICE(errors::Internal(
        "Multiple GPU devices ", device_info_cache.DebugString(devices)));
  }

  if (multiple_unknown_devices) {
    FAILED_TO_PICK_DEVICE(errors::Internal(
        "Multiple unknown devices ", device_info_cache.DebugString(devices)));
  }

  if (maybe_unknown_device && maybe_gpu_device) {
    FAILED_TO_PICK_DEVICE(errors::Internal(
        "Found both unknown and GPU devices: ",
        device_info_cache.GetNameFor(*maybe_unknown_device), ", ",
        device_info_cache.GetNameFor(*maybe_gpu_device)));
  }

  if (!allow_mixing_unknown_and_cpu) {
    if (maybe_unknown_device && maybe_cpu_device) {
      FAILED_TO_PICK_DEVICE(errors::Internal(
          "Found both unknown and CPU devices: ",
          device_info_cache.GetNameFor(*maybe_unknown_device), ", ",
          device_info_cache.GetNameFor(*maybe_cpu_device)));
    }
  }

  if (out_device_picked) {
    if (maybe_gpu_device) {
      *out_device_picked = *maybe_gpu_device;
    } else if (maybe_unknown_device) {
      *out_device_picked = *maybe_unknown_device;
    } else {
      *out_device_picked = *maybe_cpu_device;
    }
  }

  return Status::OK();

#undef FAILED_TO_PICK_DEVICE
}

xla::StatusOr<jit::DeviceId> PickDeviceForXla(
    const jit::DeviceInfoCache& device_info_cache,
    const jit::DeviceSet& devices, bool allow_mixing_unknown_and_cpu) {
  absl::optional<jit::DeviceId> device;
  TF_RETURN_IF_ERROR(PickDeviceForXlaImpl(
      device_info_cache, devices, allow_mixing_unknown_and_cpu,
      /*out_can_pick_device=*/nullptr, &device));
  return *device;
}

xla::StatusOr<bool> CanPickDeviceForXla(
    const jit::DeviceInfoCache& device_info_cache,
    const jit::DeviceSet& devices, bool allow_mixing_unknown_and_cpu) {
  bool can_pick_device;
  TF_RETURN_IF_ERROR(PickDeviceForXlaImpl(device_info_cache, devices,
                                          allow_mixing_unknown_and_cpu,
                                          &can_pick_device,
                                          /*out_device_picked=*/nullptr));
  return can_pick_device;
}
}  // namespace tensorflow
