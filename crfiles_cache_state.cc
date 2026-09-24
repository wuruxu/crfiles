// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_cache_state.h"
#include <algorithm>
#include <map>
#include <utility>
#include "base/no_destructor.h"
#include "base/files/file_util.h"
#include "base/synchronization/lock.h"
#include "build/build_config.h"
namespace crfiles {
namespace {
using Key = std::pair<base::FilePath, bool>;
struct State {
  base::Lock lock;
  std::map<Key, std::weak_ptr<FileCacheEpoch>> observed;
  std::map<uint64_t, std::vector<base::FilePath>> mutations;
  uint64_t next = 0;
};
State& CacheState() { static base::NoDestructor<State> state; return *state; }
bool Affected(const Key& key, const std::vector<base::FilePath>& paths) {
  return std::ranges::any_of(paths, [&](const auto& changed) {
#if BUILDFLAG(IS_WIN)
    // Invalidate conservatively across case aliases without merging cache keys:
    // Windows also supports directories with case-sensitive names.
    const auto left = changed.NormalizePathSeparators().GetComponents();
    const auto right = key.first.NormalizePathSeparators().GetComponents();
    const auto common = std::min(left.size(), right.size());
    for (size_t i = 0; i < common; ++i)
      if (!base::FilePath::CompareEqualIgnoreCase(left[i], right[i])) return false;
    return left.size() <= right.size() || key.second;
#else
    return changed == key.first || changed.IsParent(key.first) ||
        (key.second && key.first.IsParent(changed));
#endif
  });
}
}
std::shared_ptr<FileCacheEpoch> ObserveFileCachePath(const base::FilePath& path, bool directory) {
  auto& state = CacheState(); base::AutoLock guard(state.lock);
  if (state.observed.size() >= 2048)
    std::erase_if(state.observed, [](const auto& item) { return item.second.expired(); });
  const Key key{path.StripTrailingSeparators(), directory};
  auto& weak = state.observed[key];
  if (auto existing = weak.lock()) return existing;
  auto epoch = std::make_shared<FileCacheEpoch>();
  for (const auto& [id, paths] : state.mutations) if (Affected(key, paths)) ++epoch->writers;
  weak = epoch;
  return epoch;
}
ScopedFileCacheMutation::ScopedFileCacheMutation(std::vector<base::FilePath> paths) {
  std::erase_if(paths, [](const auto& path) { return path.empty(); });
  // Mutation scopes run on file workers. Observe both the spelling supplied by
  // the caller and its existing canonical ancestor (including new destinations).
  const auto original = paths;
  for (const auto& path : original) {
    auto ancestor = path;
    std::vector<base::FilePath> suffix;
    while (!ancestor.empty()) {
      auto canonical = base::MakeAbsoluteFilePath(ancestor);
      if (!canonical.empty()) {
        for (auto part = suffix.rbegin(); part != suffix.rend(); ++part) canonical = canonical.Append(*part);
        paths.push_back(std::move(canonical)); break;
      }
      if (ancestor == ancestor.DirName()) break;
      suffix.push_back(ancestor.BaseName()); ancestor = ancestor.DirName();
    }
  }
  for (auto& path : paths) path = path.StripTrailingSeparators();
  std::ranges::sort(paths); paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  auto& state = CacheState(); base::AutoLock guard(state.lock);
  id_ = ++state.next;
  std::erase_if(state.observed, [](const auto& item) { return item.second.expired(); });
  for (const auto& [key, weak] : state.observed) if (Affected(key, paths)) {
    if (auto epoch = weak.lock()) { ++epoch->writers; ++epoch->revision; }
  }
  state.mutations.emplace(id_, std::move(paths));
}
ScopedFileCacheMutation::~ScopedFileCacheMutation() {
  auto& state = CacheState(); base::AutoLock guard(state.lock);
  const auto found = state.mutations.find(id_);
  if (found == state.mutations.end()) return;
  for (const auto& [key, weak] : state.observed) if (Affected(key, found->second)) {
    if (auto epoch = weak.lock()) { ++epoch->revision; --epoch->writers; }
  }
  state.mutations.erase(found);
}
}
