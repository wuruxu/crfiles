// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_search.h"
#include "crFiles/crfiles_search_content.h"
#include <deque>
#include <map>
#include <memory>
#include "base/check.h"
#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/i18n/case_conversion.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/invitation.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#else
#include <sys/stat.h>
#endif
namespace crfiles {
namespace {
bool Linked(const base::FilePath& path) {
#if BUILDFLAG(IS_WIN)
  const DWORD attributes = GetFileAttributesW(path.value().c_str());
  return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  return base::IsLink(path);
#endif
}
struct SearchJob {
  explicit SearchJob(mojom::SearchOptionsPtr value) : options(std::move(value)) {}
  mojom::SearchOptionsPtr options;
  SearchControl control;
  bool initialized = false, failed = false, done = false, limited = false;
  std::u16string query;
  std::deque<std::pair<base::FilePath, bool>> directories;
  std::unique_ptr<base::FileEnumerator> enumerator;
  base::FilePath pending;
  bool enumerating_root = false, inherited_hidden = false;
  uint64_t scanned = 0, skipped = 0, matches = 0;
};
mojom::SearchBatchPtr ReadBatch(const std::shared_ptr<SearchJob>& job) {
  auto result = mojom::SearchBatch::New();
  if (job->control.canceled.load()) { result->done = true; return result; }
  if (!job->initialized) {
    job->initialized = true;
    auto root = base::FilePath::FromUTF8Unsafe(job->options->root);
    job->query = base::i18n::ToLower(base::UTF8ToUTF16(job->options->query));
    job->failed = !root.IsAbsolute() || root.ReferencesParent() || Linked(root) || !base::DirectoryExists(root) ||
        job->options->query.size() > 4096 || job->options->extensions.size() > 32 ||
        (job->options->mode == mojom::SearchMode::kContent && job->query.empty());
    for (auto& extension : job->options->extensions) {
      extension = base::ToLowerASCII(extension);
      if (extension.size() < 2 || extension.size() > 32 || extension.front() != '.' ||
          extension.find_first_not_of(".abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) job->failed = true;
    }
    if (!job->failed) job->directories.emplace_back(root, false);
    else job->done = true;
  }
  const auto deadline = base::TimeTicks::Now() + base::Milliseconds(40);
  size_t visited = 0;
  while (!job->done && !job->control.Interrupted() && result->hits.size() < 48 && visited < 256 && base::TimeTicks::Now() < deadline) {
    if (job->scanned >= 1000000 || job->matches >= 10000) { job->limited = true; job->done = true; break; }
    if (!job->enumerator) {
      if (job->directories.empty()) { job->done = true; break; }
      auto [directory, hidden_directory] = job->directories.front(); job->directories.pop_front();
      job->inherited_hidden = hidden_directory;
      if (Linked(directory)) { ++job->skipped; continue; }
      job->enumerating_root = directory == base::FilePath::FromUTF8Unsafe(job->options->root);
      job->enumerator = std::make_unique<base::FileEnumerator>(directory, false,
          base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES | base::FileEnumerator::SHOW_SYM_LINKS,
          base::FilePath::StringType(), base::FileEnumerator::FolderSearchPolicy::ALL,
          base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
    }
    auto path = job->pending.empty() ? job->enumerator->Next() : job->pending;
    if (path.empty()) {
      if (job->enumerator->GetError() != base::File::FILE_OK) {
        ++job->skipped;
        if (job->enumerating_root) { job->failed = true; job->done = true; }
      }
      job->enumerator.reset(); continue;
    }
    const auto info = job->enumerator->GetInfo();
    if (Linked(path)) { job->pending.clear(); ++job->scanned; ++job->skipped; ++visited; continue; }
    std::string match;
    const auto filename = base::ToLowerASCII(path.BaseName().AsUTF8Unsafe());
    bool hidden = job->inherited_hidden || filename.starts_with(".");
#if BUILDFLAG(IS_WIN)
    hidden |= (info.find_data().dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
#endif
    if (hidden && !job->options->show_hidden) {
      job->pending.clear(); ++job->scanned; ++visited; continue;
    }
    bool type_matches = job->options->extensions.empty();
    for (const auto& type : job->options->extensions)
      if (!info.IsDirectory() && filename.ends_with(type)) type_matches = true;
    bool skipped = false;
#if BUILDFLAG(IS_POSIX)
    // Never open named pipes, devices or sockets while searching content.
    if (!info.IsDirectory() && !S_ISREG(info.stat().st_mode)) {
      job->pending.clear(); ++job->scanned; ++job->skipped; ++visited; continue;
    }
#endif
    if (type_matches && (!job->options->hidden_only || hidden)) {
      if (job->options->mode == mojom::SearchMode::kPath) {
        if (job->query.empty()) match = path.AsUTF8Unsafe();
        else match = SearchFragment(path.AsUTF8Unsafe(), job->query);
      } else if (!info.IsDirectory()) match = SearchFileContent(path, job->query, job->control, &skipped);
    }
    if (job->control.Interrupted()) { job->pending = path; break; }
    job->pending.clear(); ++job->scanned; ++visited;
    if (skipped) ++job->skipped;
    if (info.IsDirectory()) {
      if (job->directories.size() < 10000) job->directories.emplace_back(path, hidden);
      else { job->limited = true; ++job->skipped; }
    }
    if (!match.empty()) {
      auto hit = mojom::SearchHit::New();
      hit->hidden = hidden;
      hit->path = path.AsUTF8Unsafe(); hit->directory = info.IsDirectory(); hit->size = info.GetSize();
      hit->detail = std::move(match); result->hits.push_back(std::move(hit)); ++job->matches;
    }
  }
  result->scanned = job->scanned; result->skipped = job->skipped;
  result->done = job->done || job->control.canceled.load(); result->paused = job->control.paused.load();
  result->failed = job->failed; result->limited = job->limited;
  return result;
}
class SearchService : public mojom::SearchService {
 public:
  SearchService(mojo::PendingReceiver<mojom::SearchService> receiver, base::OnceClosure quit)
      : receiver_(this, std::move(receiver)), worker_(base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock(), base::TaskPriority::USER_VISIBLE})) {
    receiver_.set_disconnect_handler(std::move(quit));
  }
  ~SearchService() override { for (auto& [id, job] : jobs_) job->control.canceled = true; }
  void Start(uint64_t id, mojom::SearchOptionsPtr options) override {
    Cancel(id);
    if (jobs_.size() < 64) jobs_[id] = std::make_shared<SearchJob>(std::move(options));
  }
  void Next(uint64_t id, NextCallback callback) override {
    auto found = jobs_.find(id);
    if (found == jobs_.end()) { auto batch = mojom::SearchBatch::New(); batch->failed = true; batch->done = true; std::move(callback).Run(std::move(batch)); return; }
    worker_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(&ReadBatch, found->second), std::move(callback));
  }
  void SetPaused(uint64_t id, bool paused) override { auto found = jobs_.find(id); if (found != jobs_.end()) found->second->control.paused = paused; }
  void Cancel(uint64_t id) override {
    auto found = jobs_.find(id);
    if (found != jobs_.end()) { found->second->control.canceled = true; jobs_.erase(found); }
  }
 private:
  mojo::Receiver<mojom::SearchService> receiver_;
  scoped_refptr<base::SequencedTaskRunner> worker_;
  std::map<uint64_t, std::shared_ptr<SearchJob>> jobs_;
};
}
int RunSearchProcess() {
  base::SingleThreadTaskExecutor executor(base::MessagePumpType::IO);
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("crFilesSearch");
  base::Thread ipc_thread("crFilesSearchIPC");
  CHECK(ipc_thread.StartWithOptions(base::Thread::Options(base::MessagePumpType::IO, 0)));
  mojo::core::ScopedIPCSupport ipc(ipc_thread.task_runner(), mojo::core::ScopedIPCSupport::ShutdownPolicy::CLEAN);
  auto endpoint = mojo::PlatformChannel::RecoverPassedEndpointFromCommandLine(*base::CommandLine::ForCurrentProcess());
  if (!endpoint.is_valid()) { base::ThreadPoolInstance::Get()->Shutdown(); return 1; }
  auto pipe = mojo::IncomingInvitation::AcceptIsolated(std::move(endpoint));
  if (!pipe.is_valid()) { base::ThreadPoolInstance::Get()->Shutdown(); return 1; }
  base::RunLoop loop;
  {
    SearchService service(mojo::PendingReceiver<mojom::SearchService>(std::move(pipe)), loop.QuitClosure());
    loop.Run();
  }
  base::ThreadPoolInstance::Get()->Shutdown();
  return 0;
}
}
