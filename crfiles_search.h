// Copyright 2026 The Chromium Authors
#ifndef CRFILES_SEARCH_H_
#define CRFILES_SEARCH_H_
#include <cstdint>
#include <map>
#include <memory>
#include "base/functional/callback.h"
#include "base/process/process.h"
#include "crFiles/crfiles_search.mojom.h"
#include "mojo/public/cpp/bindings/remote.h"
namespace crfiles {
inline constexpr char kSearchProcessSwitch[] = "crfiles-search-process";
int RunSearchProcess();
// UI-sequence connection shared by all search tabs; no process before first use.
class SearchConnection {
 public:
  SearchConnection();
  ~SearchConnection();
  static std::shared_ptr<SearchConnection> Get();
  uint64_t Start(mojom::SearchOptionsPtr options, base::RepeatingClosure disconnected);
  void Next(uint64_t job, mojom::SearchService::NextCallback callback);
  void Pause(uint64_t job, bool paused);
  void Cancel(uint64_t job);
 private:
  bool Connect();
  void Disconnected();
  base::Process process_;
  mojo::Remote<mojom::SearchService> service_;
  uint64_t next_job_ = 0;
  std::map<uint64_t, base::RepeatingClosure> jobs_;
};
}
#endif
