// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "crFiles/sharing/phone_sender.h"
#include "libcr/svg/svg_decoder.h"
#include "crFiles/crfiles_history.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/values.h"
#include "crFiles/grit/crfiles_strings.h"
#include "crFiles/sharing/connect_client.h"
#include "crypto/hash.h"

namespace crfiles {
namespace {
using Stage = NearbySharing::Stage;
struct SendFile {
  base::FilePath path;
  int64_t size;
  base::Time modified;
  std::string mime;
  std::string sha256;
};
std::string Mime(const base::FilePath& path) {
  const std::pair<const char*, const char*> types[] = {
      {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"png", "image/png"},
      {"svg", "image/svg+xml"}, {"webp", "image/webp"}, {"gif", "image/gif"}, {"heic", "image/heic"},
      {"heif", "image/heif"}, {"avif", "image/avif"}, {"mp4", "video/mp4"},
      {"mov", "video/quicktime"}, {"m4v", "video/mp4"}, {"webm", "video/webm"},
      {"mkv", "video/x-matroska"}, {"3gp", "video/3gpp"}};
  for (const auto& [ext, mime] : types) {
    if (path.MatchesFinalExtension(base::FilePath::FromUTF8Unsafe(std::string(".") + ext).value())) return mime;
  }
  return {};
}
std::optional<std::vector<SendFile>> Prepare(std::vector<base::FilePath> paths,
                                           const base::RepeatingCallback<bool()>& canceled) {
  if (paths.empty() || paths.size() > 128) return std::nullopt;
  std::vector<SendFile> files;
  int64_t total = 0;
  std::array<uint8_t, 256 * 1024> buffer;
  for (const auto& path : paths) {
    const std::string name = path.BaseName().AsUTF8Unsafe();
    const std::string mime = Mime(path);
    if (canceled.Run() || mime.empty() || name.empty() || name.starts_with(".") ||
        name.size() > 200 || name.find_first_of("/\\") != std::string::npos ||
        std::ranges::any_of(name, [](unsigned char c) { return c < 32 || c == 127; }) ||
        !base::IsStringUTF8(name) || base::IsLink(path)) return std::nullopt;
    base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
    base::File::Info before, after;
    if (!file.IsValid() || !file.GetInfo(&before) || before.is_directory || before.size <= 0 ||
        before.size > INT64_C(16) * 1024 * 1024 * 1024) return std::nullopt;
    if (libcr::IsSvgPath(path) && !libcr::ValidateSvgFile(file, before.size)) return std::nullopt;
    total += before.size;
    if (total > INT64_C(64) * 1024 * 1024 * 1024) return std::nullopt;
    crypto::hash::Hasher hash(crypto::hash::kSha256);
    int64_t read = 0;
    while (read < before.size) {
      if (canceled.Run()) return std::nullopt;
      const auto count = file.ReadAtCurrentPos(base::span(buffer).first(
          static_cast<size_t>(std::min<int64_t>(buffer.size(), before.size - read))));
      if (!count || !*count) return std::nullopt;
      hash.Update(base::span(buffer).first(*count)); read += *count;
    }
    if (!file.GetInfo(&after) || after.size != before.size || after.last_modified != before.last_modified)
      return std::nullopt;
    std::array<uint8_t, crypto::hash::kSha256Size> digest;
    hash.Finish(digest);
    files.push_back({path, before.size, before.last_modified, mime, base::HexEncode(digest)});
  }
  return files;
}

// Coalesce progress on the IO thread; the worker consumes it without calling
// UI code. A terminal response always follows the last upload progress update.
struct ReplySlot {
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<ConnectResult> result;
};
class PhoneSender {
 public:
  PhoneSender(ConnectPeer peer, base::RepeatingCallback<bool()> canceled,
              base::RepeatingCallback<void(PhoneSendProgress)> report)
      : peer_(std::move(peer)), canceled_(std::move(canceled)), report_(std::move(report)) {
    LOG(INFO) << "[crPhotos phone send] starting_client";
    NewClient();
    LOG(INFO) << "[crPhotos phone send] client_started";
  }
  void Run(const std::vector<base::FilePath>& paths) {
    Report(Stage::kConnecting, IDS_CRFILES_PHONE_SEND_PREPARING);
    ConnectRequest load; load.action = ConnectAction::kLoad;
    auto credentials = Exchange(std::move(load));
    if (!credentials) { Finish(Stage::kCanceled); return; }
    if (credentials->status != ConnectStatus::kSaved) { Error(*credentials); return; }
    LOG(INFO) << "[crPhotos phone send] credentials_loaded; preparing_files=" << paths.size();
    auto files = Prepare(paths, canceled_);
    if (!files || canceled_.Run()) {
      Finish(canceled_.Run() ? Stage::kCanceled : Stage::kError, IDS_CRFILES_PHONE_SEND_ERROR); return;
    }
    base::ListValue list;
    for (const auto& file : *files) {
      progress_.total += file.size;
      base::DictValue item; item.Set("name", file.path.BaseName().AsUTF8Unsafe());
      item.Set("size", base::NumberToString(file.size)); item.Set("mime", file.mime); item.Set("sha256", file.sha256);
      list.Append(std::move(item));
    }
    base::DictValue offer; offer.Set("files", std::move(list));
    LOG(INFO) << "[crPhotos phone send] prepared; files=" << files->size()
              << " bytes=" << progress_.total << "; sending_offer";
    auto response = Control("/v1/receive/offer", base::WriteJson(offer).value_or(""));
    const auto deadline = base::TimeTicks::Now() + base::Minutes(2);
    for (;;) {
      auto body = Decode(response);
      if (!body) return;
      const auto& state = *body->FindString("state");
      if (state == "pending" && progress_.stage != Stage::kWaiting)
        LOG(INFO) << "[crPhotos phone send] offer_received_by_phone; waiting_for_phone_approval";
      if (state == "accepted" && body->FindInt("completed") == 0) break;
      if (state != "pending") { Finish(Stage::kError, IDS_CRFILES_PHONE_SEND_ERROR); return; }
      Report(Stage::kWaiting, IDS_CRFILES_PHONE_SEND_WAITING);
      const auto next = base::TimeTicks::Now() + base::Seconds(1);
      while (base::TimeTicks::Now() < next && !canceled_.Run()) {
        std::unique_lock lock(replies_->mutex);
        replies_->ready.wait_for(lock, std::chrono::milliseconds(100));
      }
      if (canceled_.Run() || base::TimeTicks::Now() >= deadline) { Finish(Stage::kCanceled); return; }
      response = Control("/v1/receive/" + session_);
    }
    LOG(INFO) << "[crPhotos phone send] phone_accepted; starting_upload";
    for (size_t index = 0; index < files->size(); ++index) {
      const auto& file = (*files)[index];
      FileOperationLog log("send", "menu", file.path.AsUTF8Unsafe(), peer_.name + "/" + file.path.BaseName().AsUTF8Unsafe());
      if (!log.valid()) { Finish(Stage::kError, IDS_CRFILES_PHONE_SEND_ERROR); return; }
      Report(Stage::kTransferring);
      ConnectRequest upload; upload.action = ConnectAction::kUpload;
      upload.path = "/v1/receive/" + session_ + "/file/" + base::NumberToString(index);
      upload.source = file.path; upload.source_size = file.size; upload.source_modified = file.modified;
      response = Exchange(std::move(upload));
      // Recover a lost acknowledgement by checking this pinned session once.
      // Never retry file bytes: the phone may already have published the file.
      if (response && response->status == ConnectStatus::kNetworkError)
        response = Control("/v1/receive/" + session_);
      auto body = Decode(response);
      if (!body) return;
      if (body->FindInt("completed") != static_cast<int>(index + 1) ||
          *body->FindString("state") != (index + 1 == files->size() ? "complete" : "accepted")) {
        Finish(Stage::kError, IDS_CRFILES_PHONE_SEND_ERROR); return;
      }
      log.Succeed();
      sent_ += file.size; progress_.transferred = sent_;
      Report(Stage::kTransferring);
    }
    Finish(Stage::kComplete);
  }
 private:
  void NewClient() {
    client_ = std::make_unique<ConnectClient>(base::BindRepeating(
        [](std::shared_ptr<ReplySlot> replies, ConnectResult result) {
          std::lock_guard lock(replies->mutex);
          replies->result = std::move(result); replies->ready.notify_one();
        }, replies_));
  }
  void Report(Stage stage, int message_id = 0) {
    if (progress_.stage != stage || progress_.message_id != message_id)
      LOG(INFO) << "[crPhotos phone send] stage=" << static_cast<int>(stage)
                << " message_id=" << message_id << " transferred=" << progress_.transferred
                << " total=" << progress_.total;
    progress_.stage = stage; progress_.message_id = message_id; report_.Run(progress_);
  }
  std::optional<ConnectResult> TakeReply() {
    std::unique_lock lock(replies_->mutex);
    if (!replies_->result) replies_->ready.wait_for(lock, std::chrono::milliseconds(100));
    return std::exchange(replies_->result, std::nullopt);
  }
  std::optional<ConnectResult> Exchange(ConnectRequest request) {
    if (canceled_.Run()) return std::nullopt;
    request.peer = peer_;
    const auto action = request.action;
    const auto started = base::TimeTicks::Now();
    auto next_diagnostic = started + base::Seconds(10);
    client_->Request(std::move(request));
    while (!canceled_.Run()) {
      if (base::TimeTicks::Now() >= next_diagnostic) {
        LOG(INFO) << "[crPhotos phone send] awaiting_response; action=" << static_cast<int>(action)
                  << " elapsed_seconds=" << (base::TimeTicks::Now() - started).InSeconds();
        next_diagnostic = base::TimeTicks::Now() + base::Seconds(10);
      }
      auto result = TakeReply();
      if (!result) continue;
      if (result->status != ConnectStatus::kUploading) return result;
      progress_.transferred = std::min(progress_.total, sent_ + static_cast<uint64_t>(std::max<int64_t>(0, result->bytes)));
      Report(Stage::kTransferring);
    }
    return std::nullopt;
  }
  std::optional<ConnectResult> Control(std::string path, std::string body = {}) {
    ConnectRequest request; request.action = ConnectAction::kReceive;
    request.path = std::move(path); request.body = std::move(body);
    return Exchange(std::move(request));
  }
  void Finish(Stage stage, int message_id = 0) {
    client_.reset();
    if (stage != Stage::kComplete && !session_.empty()) {
      // Join the old IO thread before clearing its replies, so stale upload
      // callbacks cannot satisfy this separate cancellation request.
      {
        std::lock_guard lock(replies_->mutex);
        replies_->result.reset();
      }
      NewClient();
      // Bound the remote cancel wait; the phone has its own expiry fallback.
      ConnectRequest request; request.action = ConnectAction::kReceive; request.peer = peer_;
      request.path = "/v1/receive/" + session_ + "/cancel"; request.body = "{}";
      client_->Request(std::move(request));
      const auto deadline = base::TimeTicks::Now() + base::Seconds(2);
      while (base::TimeTicks::Now() < deadline) {
        auto reply = TakeReply();
        if (reply && reply->status != ConnectStatus::kUploading) break;
      }
    }
    client_.reset();
    // Only acknowledged/published bytes count toward the final result.
    progress_.transferred = sent_;
    Report(stage, stage == Stage::kError ? message_id : 0);
  }
  void Error(const ConnectResult& result) {
    LOG(WARNING) << "Connect send failed: status=" << static_cast<int>(result.status)
                 << " http_status=" << result.http_status << " net_error=" << result.net_error;
    Finish(Stage::kError, result.status == ConnectStatus::kNeedsPairing ? IDS_CRFILES_PHONE_SEND_PAIR
        : result.status == ConnectStatus::kCertificateMismatch ? IDS_CRFILES_CONNECT_CERT_ERROR
        : result.status == ConnectStatus::kUnauthorized ? IDS_CRFILES_CONNECT_AUTH_ERROR
        : IDS_CRFILES_PHONE_SEND_ERROR);
  }
  std::optional<base::DictValue> Decode(const std::optional<ConnectResult>& result) {
    if (!result) { Finish(Stage::kCanceled); return std::nullopt; }
    if (result->status != ConnectStatus::kConnected) { Error(*result); return std::nullopt; }
    auto body = base::JSONReader::ReadDict(result->body, base::JSON_PARSE_RFC);
    const auto* id = body ? body->FindString("id") : nullptr;
    const auto* state = body ? body->FindString("state") : nullptr;
    if (!id || id->size() != 36 || id->find_first_not_of("0123456789abcdef-") != std::string::npos ||
        !state || (!session_.empty() && *id != session_)) {
      Finish(Stage::kError, IDS_CRFILES_PHONE_SEND_ERROR); return std::nullopt;
    }
    session_ = *id; progress_.verified = true;
    if (*state == "canceled" || *state == "rejected") { Finish(Stage::kCanceled); return std::nullopt; }
    return body;
  }
  ConnectPeer peer_;
  base::RepeatingCallback<bool()> canceled_;
  base::RepeatingCallback<void(PhoneSendProgress)> report_;
  std::shared_ptr<ReplySlot> replies_ = std::make_shared<ReplySlot>();
  std::unique_ptr<ConnectClient> client_;
  PhoneSendProgress progress_;
  uint64_t sent_ = 0;
  std::string session_;
};
}  // namespace
void VerifyDiscoveredPhone(const ConnectPeer& peer, base::RepeatingCallback<bool()> canceled) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  auto replies = std::make_shared<ReplySlot>();
  ConnectClient client(base::BindRepeating(
      [](std::shared_ptr<ReplySlot> replies, ConnectResult result) {
        std::lock_guard lock(replies->mutex);
        replies->result = std::move(result); replies->ready.notify_one();
      }, replies));
  if (canceled.Run()) return;
  ConnectRequest request;
  request.peer = peer;
  request.action = ConnectAction::kList;
  request.path = "/v1/media?limit=1&type=all";
  client.Request(std::move(request));
  const auto deadline = base::TimeTicks::Now() + base::Seconds(21);
  while (!canceled.Run() && base::TimeTicks::Now() < deadline) {
    std::unique_lock lock(replies->mutex);
    if (!replies->result) replies->ready.wait_for(lock, std::chrono::milliseconds(100));
    if (replies->result) return;
  }
}

void SendFilesToPhone(const ConnectPeer& peer, const std::vector<base::FilePath>& paths,
                      base::RepeatingCallback<bool()> canceled,
                      base::RepeatingCallback<void(PhoneSendProgress)> report) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  PhoneSender(peer, std::move(canceled), std::move(report)).Run(paths);
}
}  // namespace crfiles
