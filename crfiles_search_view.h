// Copyright 2026 The Chromium Authors
#ifndef CRFILES_SEARCH_VIEW_H_
#define CRFILES_SEARCH_VIEW_H_
#include <memory>
#include "base/files/file_path.h"
#include "base/functional/callback.h"
namespace views { class View; }
namespace crfiles {
enum class SearchPageState { kReady, kRunning, kPaused, kComplete, kCanceled, kFailed };
std::unique_ptr<views::View> CreateSearchPage(const base::FilePath& root,
    base::RepeatingCallback<void(base::FilePath, bool)> open,
    base::RepeatingCallback<void(base::FilePath)> reveal,
    base::RepeatingClosure state_changed);
SearchPageState GetSearchPageState(views::View* page);
void RefreshSearchPage(views::View* page);
void StopSearchPage(views::View* page);
}
#endif
