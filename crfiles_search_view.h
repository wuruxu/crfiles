// Copyright 2026 The Chromium Authors
#ifndef CRFILES_SEARCH_VIEW_H_
#define CRFILES_SEARCH_VIEW_H_
#include <memory>
#include <cstdint>
#include <string>
#include <vector>
#include "base/files/file_path.h"
#include "base/functional/callback.h"
namespace views { class View; }
namespace crfiles {
struct SearchScope {
  // An explicit empty scope means no files, never the default directory.
  bool multiple = false;
  std::vector<base::FilePath> paths;
  std::u16string title;
  int64_t tag_id = 0;
};
enum class SearchPageState { kReady, kRunning, kPaused, kComplete, kCanceled, kFailed };
std::unique_ptr<views::View> CreateSearchPage(const base::FilePath& root,
    base::RepeatingCallback<void(base::FilePath, bool)> open,
    base::RepeatingCallback<void(base::FilePath)> reveal,
    base::RepeatingClosure state_changed, SearchScope scope = {});
SearchPageState GetSearchPageState(views::View* page);
void RefreshSearchPage(views::View* page);
void StopSearchPage(views::View* page);
}
#endif
