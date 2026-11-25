// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_TRASH_H_
#define CRFILES_TRASH_H_
#include "base/files/file_path.h"
namespace crfiles {
// Private crFiles trash storage on all platforms.
base::FilePath TrashRoot();
// Blocking operations; callers must run on a file worker.
bool TrashHasItems();
bool MoveToTrash(const base::FilePath& path);
bool RestoreFromTrash(const base::FilePath& path);
// Permanently delete crFiles-owned trash entries under the shared trash lock.
bool EmptyTrash();
}
#endif
