// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mapped_resource.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <fuchsia/io/cpp/fidl.h>
#include <fuchsia/mem/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/io.h>
#include <lib/syslog/global.h>
#include <lib/trace/event.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <zircon/dlfcn.h>
#include <zircon/status.h>

#include "third_party/dart/runtime/include/dart_api.h"

#include "inlines.h"
#include "logging.h"
#include "vmo.h"

namespace dart_utils {

static bool OpenVmo(fuchsia::mem::Buffer* resource_vmo,
                    fdio_ns_t* namespc,
                    const std::string& path,
                    bool executable) {
  TRACE_DURATION("dart", "LoadFromNamespace", "path", path);

  if (namespc == nullptr) {
    // Opening a file in the root namespace expects an absolute path.
    dart_utils::Check(path[0] == '/', LOG_TAG);
    if (!VmoFromFilename(path, executable, resource_vmo)) {
      return false;
    }
  } else {
    // openat of a path with a leading '/' ignores the namespace fd.
    // require a relative path.
    dart_utils::Check(path[0] != '/', LOG_TAG);

    auto root_dir = fdio_ns_opendir(namespc);
    if (root_dir < 0) {
      FX_LOG(ERROR, LOG_TAG, "Failed to open namespace directory");
      return false;
    }

    bool result =
        dart_utils::VmoFromFilenameAt(root_dir, path, executable, resource_vmo);
    close(root_dir);
    if (!result) {
      return result;
    }
  }

  return true;
}

bool MappedResource::LoadFromNamespace(fdio_ns_t* namespc,
                                       const std::string& path,
                                       MappedResource& resource,
                                       bool executable) {
  fuchsia::mem::Buffer resource_vmo;
  return OpenVmo(&resource_vmo, namespc, path, executable) &&
         LoadFromVmo(path, std::move(resource_vmo), resource, executable);
}

bool MappedResource::LoadFromVmo(const std::string& path,
                                 fuchsia::mem::Buffer resource_vmo,
                                 MappedResource& resource,
                                 bool executable) {
  if (resource_vmo.size == 0) {
    return true;
  }

  uint32_t flags = ZX_VM_PERM_READ;
  if (executable) {
    flags |= ZX_VM_PERM_EXECUTE;
  }
  uintptr_t addr;
  zx_status_t status = zx::vmar::root_self()->map(flags, 0, resource_vmo.vmo, 0,
                                                  resource_vmo.size, &addr);
  if (status != ZX_OK) {
    FX_LOGF(ERROR, LOG_TAG, "Failed to map: %s", zx_status_get_string(status));
    return false;
  }

  resource.address_ = reinterpret_cast<void*>(addr);
  resource.size_ = resource_vmo.size;
  return true;
}

MappedResource::~MappedResource() {
  if (address_ != nullptr) {
    zx::vmar::root_self()->unmap(reinterpret_cast<uintptr_t>(address_), size_);
    address_ = nullptr;
    size_ = 0;
  }
}

static int OpenFdExec(const std::string& path, int dirfd) {
  int fd = -1;
  zx_status_t result;
  if (dirfd == AT_FDCWD) {
    // fdio_open_fd_at does not support AT_FDCWD, by design.  Use fdio_open_fd
    // and expect an absolute path for that usage pattern.
    dart_utils::Check(path[0] == '/', LOG_TAG);
    result =
        fdio_open_fd(path.c_str(),
                     static_cast<uint32_t>(fuchsia::io::OPEN_RIGHT_READABLE |
                                           fuchsia::io::OPEN_RIGHT_EXECUTABLE),
                     &fd);
  } else {
    dart_utils::Check(path[0] != '/', LOG_TAG);
    result = fdio_open_fd_at(
        dirfd, path.c_str(),
        static_cast<uint32_t>(fuchsia::io::OPEN_RIGHT_READABLE |
                              fuchsia::io::OPEN_RIGHT_EXECUTABLE),
        &fd);
  }
  if (result != ZX_OK) {
    FX_LOGF(ERROR, LOG_TAG, "fdio_open_fd_at(%s) failed: %s", path.c_str(),
            zx_status_get_string(result));
    return -1;
  }
  return fd;
}

bool ElfSnapshot::Load(fdio_ns_t* namespc, const std::string& path) {
  int root_dir = -1;
  if (namespc == nullptr) {
    root_dir = AT_FDCWD;
  } else {
    root_dir = fdio_ns_opendir(namespc);
    if (root_dir < 0) {
      FX_LOG(ERROR, LOG_TAG, "Failed to open namespace directory");
      return false;
    }
  }
  return Load(root_dir, path);
}

bool ElfSnapshot::Load(int dirfd, const std::string& path) {
  const int fd = OpenFdExec(path, dirfd);
  if (fd < 0) {
    FX_LOGF(ERROR, LOG_TAG, "Failed to open VMO for %s from dir.",
            path.c_str());
    return false;
  }
  return Load(fd);
}

bool ElfSnapshot::Load(int fd) {
  const char* error;
  handle_ = Dart_LoadELF_Fd(fd, 0, &error, &vm_data_, &vm_instrs_,
                            &isolate_data_, &isolate_instrs_);
  if (handle_ == nullptr) {
    FX_LOGF(ERROR, LOG_TAG, "Failed load ELF: %s", error);
    return false;
  }
  return true;
}

ElfSnapshot::~ElfSnapshot() {
  Dart_UnloadELF(handle_);
}

}  // namespace dart_utils
