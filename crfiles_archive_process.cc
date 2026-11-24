// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_file_access.h"
#include "crFiles/crfiles_archive.h"
#include "crFiles/crfiles_tar.h"
#include "crFiles/crfiles_archive.mojom.h"
#include <optional>
#include <array>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "base/check.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/task/bind_post_task.h"
#include "base/task/thread_pool.h"
#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/memory_mapped_file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/thread.h"
#include "crFiles/crfiles_context_actions.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/invitation.h"
#include "third_party/lzma_sdk/google/seven_zip_reader.h"
#include "third_party/unrar/google/unrar_wrapper.h"
#include "third_party/zlib/google/zip_reader.h"
#include "third_party/zlib/google/zip.h"
#include "base/functional/bind.h"
namespace crfiles {
namespace {
constexpr uint64_t kMaximumBytes = 16ull * 1024 * 1024 * 1024;
constexpr size_t kMaximumEntries = 100000;
// The staging tree starts empty. Links are deferred until regular writes end.
// Treat both slash styles as separators on every platform.
class Output : public TarOutput {
 public:
  Output(base::FilePath root, mojo::PendingRemote<mojom::ArchiveProgressListener> listener);
  ~Output() override;
  base::FilePath Entry(const base::FilePath& relative, bool directory) override {
    if (++entries_ > kMaximumEntries) return {};
    auto text = relative.AsUTF16Unsafe();
    for (auto& ch : text) if (ch == u'\\') ch = u'/';
    while (!text.empty() && text.back() == u'/') text.pop_back();
    if (text.empty() || text.front() == u'/' || text.find(u':') != std::u16string::npos) return {};
    base::FilePath target = root_;
    size_t start = 0;
    while (start < text.size()) {
      const auto end = text.find(u'/', start);
      auto component = std::u16string_view(text).substr(start, end == std::u16string::npos ? end : end - start);
      if (!IsValidEntryName(component)) return {};
      target = target.Append(base::FilePath::FromUTF16Unsafe(component));
      if (symbolic_links_.contains(target) || hard_links_.contains(target)) return {};
      if (end == std::u16string::npos) break;
      start = end + 1;
    }
    if (!root_.IsParent(target) || !base::CreateDirectory(directory ? target : target.DirName())) return {};
    return target;
  }
  bool AddBytes(uint64_t bytes) override {
    if (bytes > kMaximumBytes - bytes_) return false;
    bytes_ += bytes;
    if (base::TimeTicks::Now() - last_report_ >= base::Milliseconds(100)) {
      listener_->OnProgress(bytes_); last_report_ = base::TimeTicks::Now();
    }
    return true;
  }
  bool Link(const base::FilePath& relative, const base::FilePath& target, bool symbolic) override {
    const auto path = Entry(relative, false);
    if (path.empty() || base::PathExists(path) || base::IsLink(path)) return false;
    if (symbolic) {
      symbolic_links_.emplace(path, target);
      return true;
    }
    // Hard-link names are rooted at the archive root, never the link's parent.
    auto text = target.AsUTF16Unsafe();
    for (auto& ch : text) if (ch == u'\\') ch = u'/';
    while (text.starts_with(u"./")) text.erase(0, 2);
    base::FilePath resolved = root_;
    size_t start = 0;
    while (start < text.size()) {
      const size_t end = text.find(u'/', start);
      const auto component = std::u16string_view(text).substr(start, end == std::u16string::npos ? end : end - start);
      if (!IsValidEntryName(component) || component.find(u':') != std::u16string_view::npos) return false;
      resolved = resolved.Append(base::FilePath::FromUTF16Unsafe(component));
      if (end == std::u16string::npos) break;
      start = end + 1;
    }
    if (resolved == root_) return false;
    hard_links_.emplace(path, resolved);
    return true;
  }
  bool FinishLinks() override {
    // Resolve forward references and chains without recursion. Erasing resolved
    // entries bounds the work; cycles and missing targets fail explicitly.
    while (!hard_links_.empty()) {
      std::vector<base::FilePath> chain;
      std::set<base::FilePath> seen;
      auto target = hard_links_.begin()->first;
      while (hard_links_.contains(target)) {
        if (!seen.insert(target).second) return false;
        chain.push_back(target);
        target = hard_links_.at(target);
      }
      base::File::Info info;
      if (symbolic_links_.contains(target) || !base::GetFileInfo(target, &info) || info.is_directory || info.is_symbolic_link) return false;
      for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
#if BUILDFLAG(IS_WIN)
        if (!CreateHardLinkW(it->value().c_str(), target.value().c_str(), nullptr)) return false;
#else
        if (link(target.value().c_str(), it->value().c_str()) != 0) return false;
#endif
        hard_links_.erase(*it);
      }
    }
    // Nothing writes through a symlink: create even dangling links only after
    // all regular files and hard links have been written and validated.
    for (const auto& [path, target] : symbolic_links_) {
#if BUILDFLAG(IS_WIN)
      const auto resolved = target.IsAbsolute() ? target : path.DirName().Append(target);
      DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
      if (base::DirectoryExists(resolved)) flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
      if (!CreateSymbolicLinkW(path.value().c_str(), target.value().c_str(), flags)) return false;
#else
      if (!base::CreateSymbolicLink(target, path)) return false;
#endif
    }
    return true;
  }
  bool RequirePassword() { needs_password = true; return false; }
  bool needs_password = false;
  void Finish() { listener_->OnProgress(bytes_); }
  base::FilePath ScratchRoot() const { return root_.DirName(); }
 private:
  base::FilePath root_;
  std::map<base::FilePath, base::FilePath> symbolic_links_, hard_links_;
  mojo::Remote<mojom::ArchiveProgressListener> listener_;
  uint64_t bytes_ = 0;
  size_t entries_ = 0;
  base::TimeTicks last_report_;
};
Output::Output(base::FilePath root, mojo::PendingRemote<mojom::ArchiveProgressListener> listener)
    : root_(std::move(root)), listener_(std::move(listener)) {}
Output::~Output() = default;
class ZipWriter : public zip::WriterDelegate {
 public:
  ZipWriter(base::File file, Output& output);
  ~ZipWriter() override;
  bool PrepareOutput() override { return file_.IsValid(); }
  bool WriteBytes(base::span<const uint8_t> data) override {
    return output_->AddBytes(data.size()) && file_.WriteAtCurrentPosAndCheck(data);
  }
  void SetTimeModified(const base::Time& time) override { file_.SetTimes(time, time); }
 private:
  base::File file_;
  raw_ref<Output> output_;
};
ZipWriter::ZipWriter(base::File file, Output& output) : file_(std::move(file)), output_(output) {}
ZipWriter::~ZipWriter() = default;
bool ExtractZip(base::File file, const std::string& password, Output& output) {
  zip::ZipReader reader;
  if (!reader.OpenFromPlatformFile(file.GetPlatformFile())) return false;
  reader.SetPassword(password);
  while (const auto* entry = reader.Next()) {
    if (entry->is_unsafe || entry->uses_aes_encryption) return false;
    if (entry->is_encrypted && password.empty()) return output.RequirePassword();
    const auto target = output.Entry(entry->path, entry->is_directory);
    if (target.empty()) return false;
    if (entry->is_directory) continue;
    ZipWriter writer(base::File(target, base::File::FLAG_CREATE | base::File::FLAG_WRITE), output);
    if (!reader.ExtractCurrentEntry(&writer, kMaximumBytes))
      return entry->is_encrypted ? output.RequirePassword() : false;
  }
  return reader.ok();
}
// Archive browsing has no filesystem root. Only the selected regular member
// can write, through a file handle created by the parent process.
class BrowseOutput : public TarOutput {
 public:
  BrowseOutput(int ordinal, const base::FilePath& member, base::File output)
      : ordinal_(ordinal), member_(member), output_(std::move(output)) {}

  base::FilePath Entry(const base::FilePath& path, bool directory) override {
    return Record(path, 0, directory, false) ? path : base::FilePath();
  }
  bool OpenFile(const base::FilePath& path, uint64_t size, base::File& file) override {
    if (!Record(path, size, false, false)) return false;
    if (count_ - 1 == ordinal_) {
      file = std::move(output_);
      selected = file.IsValid();
      return selected;
    }
    return true;
  }
  bool Link(const base::FilePath& path, const base::FilePath&, bool) override {
    return Record(path, 0, false, true);
  }
  bool FinishLinks() override { return true; }
  bool AddBytes(uint64_t count) override {
    if (count > kMaximumBytes - bytes_) return false;
    bytes_ += count;
    return true;
  }
  bool Zip(base::File& archive, const std::string& password) {
    zip::ZipReader reader;
    if (!reader.OpenFromPlatformFile(archive.GetPlatformFile())) return false;
    reader.SetPassword(password);
    while (const auto* entry = reader.Next()) {
      if (entry->is_unsafe || entry->original_size < 0) return false;
      if (!Record(entry->path, entry->original_size, entry->is_directory, entry->is_symbolic_link)) return false;
      if (count_ - 1 != ordinal_) continue;
      if (!output_.IsValid() || entry->is_directory || entry->is_symbolic_link || entry->uses_aes_encryption) return false;
      if (entry->is_encrypted && password.empty()) { needs_password = true; return false; }
      PreviewWriter writer(&output_);
      selected = reader.ExtractCurrentEntry(&writer, kMaximumBytes);
      needs_password = !selected && entry->is_encrypted;
      return selected;
    }
    return reader.ok();
  }
  bool selected = false;
  bool needs_password = false;
  std::vector<mojom::ArchiveMemberPtr> members;
 private:
  class PreviewWriter : public zip::FileWriterDelegate {
   public:
    using FileWriterDelegate::FileWriterDelegate;
    void SetPosixFilePermissions(int) override {}
  };
  bool Record(const base::FilePath& path, uint64_t size, bool directory, bool link) {
    auto name = path.AsUTF16Unsafe();
    std::ranges::replace(name, u'\\', u'/');
    if (name.empty() || name.front() == u'/' || name.find(u':') != name.npos ||
        ++count_ > static_cast<int>(kMaximumEntries) || name.size() > 4096) return false;
    base::FilePath relative;
    for (const auto& part : base::FilePath::FromUTF16Unsafe(name).GetComponents()) {
      if (part == FILE_PATH_LITERAL(".")) continue;
      base::FilePath component(part);
      if (!IsValidEntryName(component.AsUTF16Unsafe())) return false;
      relative = relative.Append(component);
    }
    if (relative.empty() || (count_ - 1 == ordinal_ && relative != member_)) return false;
    if (ordinal_ < 0) {
      name_units_ += name.size();
      if (name_units_ > 4 * 1024 * 1024) return false;
      members.push_back(mojom::ArchiveMember::New(relative, size, directory, link));
    }
    return true;
  }
  int ordinal_;
  const base::FilePath member_;
  int count_ = 0;
  size_t name_units_ = 0;
  uint64_t bytes_ = 0;
  base::File output_;
};

class SevenZipOutput : public seven_zip::Delegate {
 public:
  explicit SevenZipOutput(Output& output);
  ~SevenZipOutput() override;
  void OnOpenError(seven_zip::Result) override { ok = false; }
  base::File OnTempFileRequest() override {
    if (!scratch_.CreateUniqueTempDirUnderPath(output_->ScratchRoot())) { ok = false; return {}; }
    return base::File(scratch_.GetPath().AppendASCII("solid"),
        base::File::FLAG_CREATE | base::File::FLAG_READ | base::File::FLAG_WRITE);
  }
  bool OnEntry(const seven_zip::EntryInfo& entry, base::span<uint8_t>& bytes) override {
    target_ = {};
    constexpr size_t kMaxSingleEntryMmap = 2ull * 1024 * 1024 * 1024;
    if (entry.is_encrypted || entry.file_size > kMaxSingleEntryMmap || !output_->AddBytes(entry.file_size)) return ok = false;
    const auto target = output_->Entry(entry.file_path, false);
    if (target.empty()) return ok = false;
    base::File file(target, base::File::FLAG_CREATE | base::File::FLAG_READ | base::File::FLAG_WRITE);
    if (!file.IsValid()) return ok = false;
    target_ = target;
    bytes = {};
    if (entry.file_size) {
      mapping_.emplace();
      if (!mapping_->Initialize(std::move(file), {0, entry.file_size}, base::MemoryMappedFile::READ_WRITE_EXTEND)) {
        mapping_.reset();
        base::DeleteFile(target); return ok = false;
      }
      bytes = mapping_->mutable_bytes();
    }
    return true;
  }
  bool OnDirectory(const seven_zip::EntryInfo& entry) override {
    return ok = !output_->Entry(entry.file_path, true).empty();
  }
  bool EntryDone(seven_zip::Result result, const seven_zip::EntryInfo& entry) override {
    mapping_.reset();
    // Entry() validates and counts a new archive entry. Reuse the path here
    // rather than registering the same file again for cleanup or timestamps.
    const auto target = std::exchange(target_, base::FilePath());
    if (result != seven_zip::Result::kSuccess) {
      if (!target.empty()) base::DeleteFile(target);
      return ok = false;
    }
    if (!entry.last_modified_time.is_null()) {
      if (!target.empty()) {
        base::File file(target, base::File::FLAG_OPEN | base::File::FLAG_WRITE_ATTRIBUTES);
        if (file.IsValid()) file.SetTimes(entry.last_modified_time, entry.last_modified_time);
      }
    }
    return ok = true;
  }
  bool ok = true;
 private:
  raw_ref<Output> output_;
  base::FilePath target_;
  base::ScopedTempDir scratch_;
  std::optional<base::MemoryMappedFile> mapping_;
};
SevenZipOutput::SevenZipOutput(Output& output) : output_(output) {}
SevenZipOutput::~SevenZipOutput() = default;
// RarReader extracts into a reusable scratch file before reporting the entry
// name. Bound the bytes at the writer, not by the untrusted header size.
class RarWriter : public third_party_unrar::RarWriterDelegate {
 public:
  RarWriter(base::File file, Output& output, bool& ok);
  ~RarWriter() override;
  bool Write(base::span<const uint8_t> data) override {
    if (!*ok_) return false;
    if (fresh_) {
      if (file_.Seek(base::File::FROM_BEGIN, 0) != 0 || !file_.SetLength(0)) return *ok_ = false;
      fresh_ = false;
    }
    return *ok_ = output_->AddBytes(data.size()) && file_.WriteAtCurrentPosAndCheck(data);
  }
  void Close() override { fresh_ = true; }
 private:
  base::File file_;
  raw_ref<Output> output_;
  raw_ref<bool> ok_;
  bool fresh_ = true;
};
RarWriter::RarWriter(base::File file, Output& output, bool& ok) : file_(std::move(file)), output_(output), ok_(ok) {}
RarWriter::~RarWriter() = default;
bool ExtractRar(base::File input, const std::string& password, Output& output) {
  base::ScopedTempDir temporary;
  if (!temporary.CreateUniqueTempDirUnderPath(output.ScratchRoot())) return false;
  auto scratch = temporary.GetPath().AppendASCII("entry");
  base::File file(scratch, base::File::FLAG_CREATE | base::File::FLAG_READ | base::File::FLAG_WRITE);
  if (!file.IsValid()) return false;
  bool writer_ok = true;
  third_party_unrar::RarReader reader;
  reader.SetPassword(password);
  reader.SetWriterDelegate(std::make_unique<RarWriter>(file.Duplicate(), output, writer_ok));
  const bool opened = reader.Open(std::make_unique<third_party_unrar::FileReader>(std::move(input)), std::move(file));
  if (reader.HeadersEncrypted() && (password.empty() || reader.HeaderDecryptionFailed())) return output.RequirePassword();
  if (!opened || reader.IsVolume()) return false;
  while (reader.ExtractNextEntry()) {
    const auto& entry = reader.current_entry();
    if (entry.is_encrypted && (password.empty() || !entry.contents_valid)) return output.RequirePassword();
    if (!writer_ok || !entry.contents_valid || reader.HasWriteError() || reader.HasExtractionError()) return false;
    const auto target = output.Entry(entry.file_path, entry.is_directory);
    if (target.empty()) return false;
    if (!entry.is_directory) {
      // Never overwrite a previously extracted entry, even with duplicate names.
      base::File destination(target, base::File::FLAG_CREATE | base::File::FLAG_WRITE);
      if (!destination.IsValid()) return false;
      base::File source(scratch, base::File::FLAG_OPEN | base::File::FLAG_READ);
      if (!source.IsValid() || source.GetLength() < 0 || static_cast<uint64_t>(source.GetLength()) != entry.file_size) return false;
      std::array<uint8_t, 65536> buffer;
      while (true) {
        const auto count = source.ReadAtCurrentPos(buffer);
        if (!count) return false;
        if (!*count) break;
        if (!destination.WriteAtCurrentPosAndCheck(base::span(buffer).first(*count))) return false;
      }
    }
  }
  return writer_ok && !reader.HasWriteError() && !reader.HasExtractionError();
}
class CompressionFileAccessor : public zip::FileAccessor {
 public:
  CompressionFileAccessor(base::FilePath root, std::shared_ptr<std::atomic_bool> canceled);
  ~CompressionFileAccessor() override;
  bool Open(zip::Paths paths, std::vector<base::File>* files) override {
    for (const auto& path : paths) {
      if (canceled_->load() || path.IsAbsolute() || path.ReferencesParent()) return false;
      const auto absolute = root_.Append(path);
      if (base::DirectoryExists(absolute)) files->emplace_back();
      else files->emplace_back(absolute, base::File::FLAG_OPEN | base::File::FLAG_READ);
    }
    return true;
  }
  bool List(const base::FilePath& path, std::vector<base::FilePath>* files,
      std::vector<base::FilePath>* subdirs) override {
    if (canceled_->load() || path.IsAbsolute() || path.ReferencesParent()) return false;
    base::FileEnumerator entries(root_.Append(path), false,
        base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES, base::FilePath::StringType(),
        base::FileEnumerator::FolderSearchPolicy::ALL, base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
    for (auto entry = entries.Next(); !entry.empty(); entry = entries.Next()) {
      if (canceled_->load()) return false;
      (entries.GetInfo().IsDirectory() ? subdirs : files)->push_back(path.Append(entry.BaseName()));
    }
    return entries.GetError() == base::File::FILE_OK;
  }
  bool GetInfo(const base::FilePath& path, Info* info) override {
    if (canceled_->load() || path.IsAbsolute() || path.ReferencesParent()) return false;
    base::File::Info file_info;
    if (!base::GetFileInfo(root_.Append(path), &file_info)) return false;
    info->is_directory = file_info.is_directory;
    info->last_modified = file_info.last_modified;
    current_path = path;
    return true;
  }
  base::FilePath current_path;
 private:
  base::FilePath root_;
  std::shared_ptr<std::atomic_bool> canceled_;
};
CompressionFileAccessor::CompressionFileAccessor(base::FilePath root, std::shared_ptr<std::atomic_bool> canceled)
    : root_(std::move(root)), canceled_(std::move(canceled)) {}
CompressionFileAccessor::~CompressionFileAccessor() = default;
class ArchiveDecoder : public mojom::ArchiveDecoder {
 public:
  ArchiveDecoder(mojo::PendingReceiver<mojom::ArchiveDecoder> receiver, base::OnceClosure quit);
  ~ArchiveDecoder() override;
  void Browse(base::File archive, const std::string& format, int32_t ordinal,
      const base::FilePath& member, base::File output, const std::string& password, BrowseCallback reply) override {
    const bool valid = archive.IsValid() && (ordinal == -1 ? !output.IsValid() : ordinal >= 0 && output.IsValid());
    BrowseOutput visitor(ordinal, member, std::move(output));
    bool ok = false;
    if (valid && format == ".zip") ok = visitor.Zip(archive, password);
    else if (valid && (format == ".tar" || format == ".tar.gz" || format == ".tgz" ||
             format == ".tar.xz" || format == ".tar.zst" || format == ".tzst"))
      ok = ExtractTar(std::move(archive), format, visitor);
    ok = ok && (ordinal < 0 || visitor.selected);
    if (!ok) visitor.members.clear();
    std::move(reply).Run(ok, visitor.needs_password, std::move(visitor.members));
  }
  void Compress(const base::FilePath& source_directory,
      const std::vector<base::FilePath>& relative_sources, const base::FilePath& destination,
      const std::vector<uint8_t>& source_bookmark, const std::vector<uint8_t>& output_bookmark,
      mojo::PendingRemote<mojom::ArchiveProgressListener> listener, CompressCallback callback) override {
    auto source_grant = ResolveFileAccessBookmark(source_bookmark);
    auto output_grant = ResolveFileAccessBookmark(output_bookmark);
    if (!source_directory.IsAbsolute() || !destination.IsAbsolute() || relative_sources.empty() ||
        !base::DirectoryExists(source_directory) || base::PathExists(destination)) {
      std::move(callback).Run(false); return;
    }
    for (const auto& source : relative_sources) {
      if (source.IsAbsolute() || source.BaseName() != source || !IsValidEntryName(source.AsUTF16Unsafe())) {
        std::move(callback).Run(false); return;
      }
    }
    if (compression_progress_.is_bound()) { std::move(callback).Run(false); return; }
    compression_progress_.Bind(std::move(listener));
    // Keep the child's IPC sequence responsive while zlib is working. On
    // parent disconnect the service is destroyed and cancels the worker.
    auto report = base::BindPostTaskToCurrentDefault(base::BindRepeating(
        &ArchiveDecoder::CompressionProgress, weak_.GetWeakPtr()));
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE, base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
        base::BindOnce([](base::FilePath root, std::vector<base::FilePath> sources, base::FilePath target,
            base::ScopedClosureRunner source_grant, base::ScopedClosureRunner output_grant,
            std::shared_ptr<std::atomic_bool> canceled,
            base::RepeatingCallback<void(uint64_t, uint64_t, base::FilePath)> progress) {
          if (canceled->load()) return false;
          CompressionFileAccessor accessor(root, canceled);
          zip::ZipParams params;
          params.src_dir = root; params.src_files = sources; params.dest_file = target; params.recursive = true;
          params.file_accessor = &accessor;
          params.progress_period = base::Milliseconds(100);
          params.progress_callback = base::BindRepeating(
              [](std::shared_ptr<std::atomic_bool> stopped, const base::FilePath& current_path,
                  base::RepeatingCallback<void(uint64_t, uint64_t, base::FilePath)> notify,
                  const zip::Progress& update) {
                if (stopped->load() || update.errors) return false;
                notify.Run(static_cast<uint64_t>(std::max<int64_t>(0, update.bytes)),
                    static_cast<uint64_t>(std::max(0, update.files)) + static_cast<uint64_t>(std::max(0, update.directories)), current_path);
                return true;
              }, canceled, std::cref(accessor.current_path), std::move(progress));
          return zip::Zip(params) && !canceled->load();
        }, source_directory, relative_sources, destination, std::move(source_grant),
            std::move(output_grant), compression_canceled_, std::move(report)),
        base::BindOnce(&ArchiveDecoder::CompressionDone, weak_.GetWeakPtr(), std::move(callback)));
  }
  void Extract(base::File archive, const std::string& format, const base::FilePath& single_file_name, const base::FilePath& staging,
      const std::string& password, const std::vector<uint8_t>& staging_bookmark, mojo::PendingRemote<mojom::ArchiveProgressListener> listener,
      ExtractCallback callback) override {
    auto access_grant = ResolveFileAccessBookmark(staging_bookmark);
    bool success = false;
    bool needs_password = false;
    if (archive.IsValid() && staging.IsAbsolute() && base::DirectoryExists(staging)) {
      Output output(staging, std::move(listener));
      if (format == ".zip") success = ExtractZip(std::move(archive), password, output);
      else if (format == ".rar") success = ExtractRar(std::move(archive), password, output);
      else if (format == ".zst") {
        if (IsValidEntryName(single_file_name.AsUTF16Unsafe()))
          success = ExtractZstdFile(std::move(archive), single_file_name, output);
      } else if (format == ".tar" || format == ".tar.gz" || format == ".tgz" || format == ".tar.xz" ||
                 format == ".tar.zst" || format == ".tzst")
        success = ExtractTar(std::move(archive), format, output);
      else if (format == ".7z") {
        SevenZipOutput delegate(output);
        auto reader = seven_zip::SevenZipReader::Create(std::move(archive), delegate);
        if (reader) { reader->Extract(); success = delegate.ok; }
      }
      needs_password = output.needs_password;
      output.Finish();
    }
    std::move(callback).Run(success, needs_password);
  }
 private:
  void CompressionProgress(uint64_t bytes, uint64_t entries, base::FilePath path) {
    if (compression_progress_.is_bound()) compression_progress_->OnCompressionProgress(bytes, entries, path);
  }
  void CompressionDone(CompressCallback callback, bool success) {
    std::move(callback).Run(success);
    compression_progress_.reset();
  }
  mojo::Receiver<mojom::ArchiveDecoder> receiver_;
  mojo::Remote<mojom::ArchiveProgressListener> compression_progress_;
  std::shared_ptr<std::atomic_bool> compression_canceled_ = std::make_shared<std::atomic_bool>(false);
  base::WeakPtrFactory<ArchiveDecoder> weak_{this};
};
ArchiveDecoder::ArchiveDecoder(mojo::PendingReceiver<mojom::ArchiveDecoder> receiver, base::OnceClosure quit)
    : receiver_(this, std::move(receiver)) { receiver_.set_disconnect_handler(std::move(quit)); }
ArchiveDecoder::~ArchiveDecoder() { compression_canceled_->store(true); }
}
int RunArchiveProcess() {
  base::SingleThreadTaskExecutor executor(base::MessagePumpType::IO);
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("crFilesArchive");
  base::Thread ipc_thread("crFilesArchiveIPC");
  CHECK(ipc_thread.StartWithOptions(base::Thread::Options(base::MessagePumpType::IO, 0)));
  mojo::core::ScopedIPCSupport ipc(ipc_thread.task_runner(), mojo::core::ScopedIPCSupport::ShutdownPolicy::CLEAN);
  auto endpoint = mojo::PlatformChannel::RecoverPassedEndpointFromCommandLine(*base::CommandLine::ForCurrentProcess());
  if (!endpoint.is_valid()) { base::ThreadPoolInstance::Get()->Shutdown(); return 1; }
  auto pipe = mojo::IncomingInvitation::AcceptIsolated(std::move(endpoint));
  if (!pipe.is_valid()) { base::ThreadPoolInstance::Get()->Shutdown(); return 1; }
  base::RunLoop loop;
  {
    ArchiveDecoder service(mojo::PendingReceiver<mojom::ArchiveDecoder>(std::move(pipe)), loop.QuitClosure());
    loop.Run();
  }
  base::ThreadPoolInstance::Get()->Shutdown();
  return 0;
}
}
