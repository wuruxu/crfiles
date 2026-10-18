// Copyright 2026 The Chromium Authors
#ifndef CRFILES_TAGS_VIEW_H_
#define CRFILES_TAGS_VIEW_H_
#include <memory>
#include <vector>
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "crFiles/crfiles_tags.h"
namespace views { class View; class Widget; }
namespace crfiles {
void ShowFileTagEditor(views::Widget* parent, std::vector<base::FilePath> paths,
                       base::RepeatingClosure changed = {});
using OpenTagCallback = base::RepeatingCallback<void(int64_t, std::string)>;
std::unique_ptr<views::View> CreateTagsPropertyView(std::vector<base::FilePath> paths, OpenTagCallback open_tag);
}
#endif
