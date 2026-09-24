// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_windows.h"
#include <windows.h>
#include <shellapi.h>
#include <oleidl.h>
#include <string>
namespace crfiles {
namespace {
class ClipboardLock {
 public:
  ClipboardLock() : open_(OpenClipboard(nullptr) != FALSE) {}
  ~ClipboardLock() { if (open_) CloseClipboard(); }
  bool open() const { return open_; }
 private:
  bool open_;
};
UINT DropEffectFormat() { return RegisterClipboardFormatW(L"Preferred DropEffect"); }
}
bool HasWindowsFileClipboard() { return IsClipboardFormatAvailable(CF_HDROP) != FALSE; }
WindowsFileClipboard ReadWindowsFileClipboard() {
  WindowsFileClipboard result;
  ClipboardLock lock;
  if (!lock.open()) return result;
  auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
  if (!drop) return result;
  const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
  if (count > 100000) return result;
  for (UINT i = 0; i < count; ++i) {
    const UINT size = DragQueryFileW(drop, i, nullptr, 0);
    if (!size || size > 32767) { result.paths.clear(); return result; }
    std::wstring name(size + 1, L'\0');
    if (DragQueryFileW(drop, i, name.data(), size + 1) != size) { result.paths.clear(); return result; }
    name.resize(size);
    base::FilePath path(name);
    if (!path.IsAbsolute() || path.ReferencesParent()) { result.paths.clear(); return result; }
    result.paths.push_back(std::move(path));
  }
  auto effect = GetClipboardData(DropEffectFormat());
  if (effect && GlobalSize(effect) >= sizeof(DWORD)) {
    if (const void* data = GlobalLock(effect)) {
      const DWORD value = *static_cast<const DWORD*>(data);
      result.move = value == DROPEFFECT_MOVE;
      GlobalUnlock(effect);
    }
  }
  return result;
}
void SetWindowsClipboardMove(bool move) {
  ClipboardLock lock;
  if (!lock.open() || !IsClipboardFormatAvailable(CF_HDROP)) return;
  auto memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
  if (!memory) return;
  if (void* data = GlobalLock(memory)) {
    const DWORD effect = move ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    *static_cast<DWORD*>(data) = effect;
    GlobalUnlock(memory);
    if (SetClipboardData(DropEffectFormat(), memory)) return;
  }
  GlobalFree(memory);
}
}
