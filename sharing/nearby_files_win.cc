// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "crFiles/sharing/nearby_files_win.h"

namespace crfiles::nearby {

PinnedDirectory::PinnedDirectory() = default;
PinnedDirectory::~PinnedDirectory() = default;
PinnedDirectory::PinnedDirectory(PinnedDirectory&&) = default;
PinnedDirectory& PinnedDirectory::operator=(PinnedDirectory&&) = default;

DirectoryLock::DirectoryLock() = default;
DirectoryLock::~DirectoryLock() = default;
DirectoryLock::DirectoryLock(DirectoryLock&&) = default;
DirectoryLock& DirectoryLock::operator=(DirectoryLock&&) = default;

}  // namespace crfiles::nearby
