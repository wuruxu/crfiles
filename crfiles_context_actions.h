// Copyright 2026 The Chromium Authors
#ifndef CRFILES_CONTEXT_ACTIONS_H_
#define CRFILES_CONTEXT_ACTIONS_H_
#include <string>
#include <string_view>
#include <optional>
#include <cstdint>
#include <vector>
#include <atomic>
#include <memory>
#include "base/files/file_path.h"
#include "ui/gfx/image/image_skia.h"
namespace crfiles {
bool IsValidEntryName(std::u16string_view name);
struct FileApplication { std::string id, name; bool is_default = false; gfx::ImageSkia icon; };
// Worker-only system association lookup and launch.
std::vector<FileApplication> FileApplications(const base::FilePath& path);
// Requires MayBlock and WithBaseSyncPrimitives task traits: the macOS launcher
// waits for /usr/bin/open to exit, with a bounded timeout.
bool OpenFileWith(const base::FilePath& path, const std::string& application);
bool OpenFolderTerminal(const base::FilePath& path);
std::optional<uint64_t> FileSelectionSize(std::vector<base::FilePath> paths);
// Properties-only progress. The UI polls these values and cancels on close.
struct PropertiesSizeProgress {
  std::atomic_bool canceled{false};
  std::atomic_bool incomplete{false};
  std::atomic_bool done{false};
  std::atomic<uint64_t> bytes{0};
};
void CalculatePropertiesSize(std::vector<base::FilePath> paths,
                             std::shared_ptr<PropertiesSizeProgress> progress);
}
#endif
