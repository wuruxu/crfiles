// Copyright 2026 The Chromium Authors
#ifndef CRFILES_CRFILES_WINDOWS_H_
#define CRFILES_CRFILES_WINDOWS_H_
#include <vector>
#include "base/files/file_path.h"
namespace crfiles {
struct WindowsFileClipboard {
  std::vector<base::FilePath> paths;
  bool move = false;
};
// UI-thread clipboard helpers. No ownership is taken from the clipboard.
bool HasWindowsFileClipboard();
WindowsFileClipboard ReadWindowsFileClipboard();
void SetWindowsClipboardMove(bool move);
}
#endif
