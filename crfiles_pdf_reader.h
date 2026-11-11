// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_CRFILES_PDF_READER_H_
#define CRFILES_CRFILES_PDF_READER_H_

#include <string_view>
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "libcr/theme_mode.h"

namespace crfiles {
bool IsPdfReaderFile(const base::FilePath& path, std::string_view mime = {});
// UI thread. Each request relaunches crFiles in a separate PDF reader process. Reports
// launch failure or abnormal exit on the UI thread; never kills the reader
// when crFiles exits, so pending document edits can still be saved.
void OpenCrFilesPdf(const base::FilePath& path, base::OnceClosure on_error);
// UI thread. Existing readers follow the host's next explicit theme change.
void UpdateCrFilesPdfReaderTheme(libcr::ThemeMode mode);
}  // namespace crfiles
#endif
