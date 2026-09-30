// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_CRFILES_FILE_ACCESS_H_
#define CRFILES_CRFILES_FILE_ACCESS_H_

#include <cstdint>
#include <vector>
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "build/build_config.h"

namespace views { class Widget; }
namespace crfiles {
// Packaged macOS builds launch the separately signed inherited-sandbox helper.
// Loose development executables retain the original self-launch behavior.
bool GetCrFilesHelperExecutable(base::FilePath* executable);
std::vector<uint8_t> CreateFileAccessBookmark(const base::FilePath& path);
base::ScopedClosureRunner ResolveFileAccessBookmark(const std::vector<uint8_t>& data);
void RememberFileAccess(const base::FilePath& path);
// Requests access only in a sandboxed macOS app. For a new destination, grants
// its nearest existing parent. Cancellation never proceeds with the operation.
void EnsureDirectoryAccess(views::Widget* parent, const base::FilePath& path,
                           bool write, base::OnceCallback<void(bool)> reply);
#if BUILDFLAG(IS_MAC)
bool IsCrFilesSandboxed();
void InitializeCrFilesBundle();
void RestoreFileAccess();
base::FilePath MacApplicationSupportDirectory();
base::FilePath MacCacheDirectory();
base::FilePath MacDownloadsDirectory();
base::FilePath MacUserHomeDirectory();
#endif
}  // namespace crfiles
#endif
