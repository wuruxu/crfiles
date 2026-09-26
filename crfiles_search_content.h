// Copyright 2026 The Chromium Authors
#ifndef CRFILES_SEARCH_CONTENT_H_
#define CRFILES_SEARCH_CONTENT_H_
#include <atomic>
#include <string>
#include <string_view>
#include "base/files/file_path.h"
namespace crfiles {
// One worker owns parsing. Controls are updated by the child's Mojo IO thread.
struct SearchControl {
  std::atomic_bool canceled{false};
  std::atomic_bool paused{false};
  bool Interrupted() const { return canceled.load() || paused.load(); }
};
// Returns a matching text/metadata fragment. Unsupported or unreadable inputs
// increment skipped. Work is bounded and cooperative; parsing calls may finish
// before observing pause. The caller retries the file after a pause.
std::string SearchFileContent(const base::FilePath& path, const std::u16string& folded_query,
                             const SearchControl& control, bool* skipped);
std::string SearchFragment(std::string_view value, const std::u16string& folded_query);
}
#endif
