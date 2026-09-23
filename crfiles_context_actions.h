// Copyright 2026 The Chromium Authors
#ifndef CRFILES_CONTEXT_ACTIONS_H_
#define CRFILES_CONTEXT_ACTIONS_H_
#include <string>
#include <optional>
#include <cstdint>
#include <vector>
#include "base/files/file_path.h"
namespace crfiles {
struct FileApplication { std::string id, name; bool is_default = false; };
// Worker-only system association lookup and launch.
std::vector<FileApplication> FileApplications(const base::FilePath& path);
bool OpenFileWith(const base::FilePath& path, const std::string& application);
bool OpenFolderTerminal(const base::FilePath& path);
std::optional<uint64_t> FileSelectionSize(std::vector<base::FilePath> paths);
}
#endif
