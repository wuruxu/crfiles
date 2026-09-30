// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_file_access.h"
#include <utility>
#include "base/base_paths.h"
#include "base/path_service.h"

#if !BUILDFLAG(IS_MAC)
namespace crfiles {
bool GetCrFilesHelperExecutable(base::FilePath* executable) {
  return base::PathService::Get(base::FILE_EXE, executable);
}
std::vector<uint8_t> CreateFileAccessBookmark(const base::FilePath&) { return {}; }
base::ScopedClosureRunner ResolveFileAccessBookmark(const std::vector<uint8_t>&) { return {}; }
void RememberFileAccess(const base::FilePath&) {}
void EnsureDirectoryAccess(views::Widget*, const base::FilePath&, bool,
                           base::OnceCallback<void(bool)> reply) {
  std::move(reply).Run(true);
}
}  // namespace crfiles
#endif
