// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "crFiles/crfiles_file_access.h"
#include "crFiles/sharing/connect_client.h"

#include <algorithm>
#include <array>
#include "base/base64.h"
#include "crFiles/crfiles_theme.h"
#include "crFiles/grit/crfiles_strings.h"
#include "ui/base/resource/resource_bundle.h"
#include <optional>
#include <map>
#include <utility>
#include <vector>
#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/files/file.h"
#include "crypto/hash.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/path_service.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include "base/base_paths_win.h"
#endif
#include "net/base/elements_upload_data_stream.h"
#include "net/base/io_buffer.h"
#include "net/base/ip_address.h"
#include "net/base/isolation_info.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/base/network_handle.h"
#include "net/base/upload_bytes_element_reader.h"
#include "net/base/upload_file_element_reader.h"
#include "net/base/upload_progress.h"
#include "net/cert/cert_status_flags.h"
#include "net/cert/cert_verifier.h"
#include "net/cert/cert_verify_result.h"
#include "net/cert/x509_certificate.h"
#include "net/cookies/cookie_store.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/proxy_resolution/proxy_config_service_fixed.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/redirect_info.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "url/gurl.h"

namespace crfiles {
namespace {
std::string ComputerProfile() {
  const auto preferences = GetNearbySharingPreferences();
  base::DictValue profile;
  profile.Set("name", preferences.name.empty() ? "crPhotos" : preferences.name);
  std::string pixels;
  if (preferences.custom_avatar.size() == 49152 &&
      base::Base64Decode(preferences.custom_avatar, &pixels) && pixels.size() == 96 * 96 * 4) {
    profile.Set("avatar_format", "rgba96");
    profile.Set("avatar", preferences.custom_avatar);
  } else {
    static constexpr auto ids = std::to_array<int>({
        IDR_CRFILES_AVATAR_0, IDR_CRFILES_AVATAR_1, IDR_CRFILES_AVATAR_2,
        IDR_CRFILES_AVATAR_3, IDR_CRFILES_AVATAR_4, IDR_CRFILES_AVATAR_5,
        IDR_CRFILES_AVATAR_6, IDR_CRFILES_AVATAR_7, IDR_CRFILES_AVATAR_8});
    const int index = std::clamp(preferences.avatar, 0, 8);
    auto png = ui::ResourceBundle::GetSharedInstance().GetRawDataResource(ids[index]);
    profile.Set("avatar_format", "png");
    profile.Set("avatar", base::Base64Encode(png));
  }
  return base::WriteJson(profile).value_or("{}");
}

struct VerifiedPhones {
  base::Lock lock;
  std::map<std::string, base::TimeTicks> endpoints;
  uint64_t generation = 0;
};
VerifiedPhones& PhoneVerification() {
  static base::NoDestructor<VerifiedPhones> cache;
  return *cache;
}
std::string VerificationKey(const ConnectPeer& peer) {
  return peer.id + "|" + peer.address + ":" + base::NumberToString(peer.port);
}
uint64_t VerificationGeneration(const ConnectPeer& peer, bool revoke, bool local_change = false) {
  auto& cache = PhoneVerification();
  base::AutoLock lock(cache.lock);
  if (revoke) {
    if (local_change) ++cache.generation;
    std::erase_if(cache.endpoints, [&](const auto& entry) { return entry.first.starts_with(peer.id + "|"); });
  }
  return cache.generation;
}
void RememberVerifiedPhone(const ConnectPeer& peer, uint64_t generation) {
  auto& cache = PhoneVerification();
  base::AutoLock lock(cache.lock);
  // A request started before local revocation cannot restore the badge.
  if (generation != cache.generation) return;
  const auto now = base::TimeTicks::Now();
  std::erase_if(cache.endpoints, [&](const auto& entry) { return entry.second <= now; });
  const auto key = VerificationKey(peer);
  if (!cache.endpoints.contains(key) && cache.endpoints.size() >= 128) return;
  cache.endpoints[key] = now + base::Minutes(2);
}

bool HexId(const std::string& id) {
  return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}
std::string NormalizePin(std::string pin) {
  std::erase_if(pin, [](char c) { return c == ':' || c == ' ' || c == '\n' || c == '\r'; });
  pin = base::ToUpperASCII(pin);
  if (pin.size() != 64 || pin.find_first_not_of("0123456789ABCDEF") != std::string::npos) return {};
  return pin;
}
bool TokenValid(const std::string& token) {
  return token.size() == 43 && std::ranges::all_of(token, [](char c) {
    return base::IsAsciiAlphaNumeric(c) || c == '-' || c == '_';
  });
}
bool EndpointValid(const ConnectPeer& peer) {
  net::IPAddress address;
  if (!peer.port || !HexId(peer.id) || !address.AssignFromIPLiteral(peer.address) || !address.IsIPv4()) return false;
  const auto& bytes = address.bytes();
  return bytes[0] == 10 || (bytes[0] == 172 && (bytes[1] & 0xf0) == 16) ||
      (bytes[0] == 192 && bytes[1] == 168) || (bytes[0] == 169 && bytes[1] == 254);
}
base::FilePath CredentialFile(const std::string& id) {
  if (!HexId(id)) return {};
  [[maybe_unused]] base::FilePath home;
#if BUILDFLAG(IS_MAC)
  auto directory = IsCrFilesSandboxed() ? MacApplicationSupportDirectory().AppendASCII("connect") :
      base::GetHomeDir().AppendASCII(".config").AppendASCII("crFiles").AppendASCII("connect");
#elif BUILDFLAG(IS_WIN)
  if (!base::PathService::Get(base::DIR_LOCAL_APP_DATA, &home)) return {};
  auto directory = home.AppendASCII("crFiles").AppendASCII("connect");
#else
  if (!base::PathService::Get(base::DIR_HOME, &home)) return {};
  auto directory = home.AppendASCII(".config").AppendASCII("crFiles").AppendASCII("connect");
#endif
  if (base::IsLink(directory) || !base::CreateDirectory(directory)) return {};
#if BUILDFLAG(IS_POSIX)
  if (!base::SetPosixFilePermissions(directory, 0700)) return {};
#endif
  return directory.AppendASCII(id + ".json");
}

// Dedicated context, exact certificate pin. This runs during TLS verification,
// before HTTP headers/upload (pairing code or bearer token) can be transmitted.
class PinVerifier : public net::CertVerifier {
 public:
  PinVerifier(std::string host, std::string pin) : host_(std::move(host)), pin_(std::move(pin)) {}
  int Verify(const RequestParams& params, net::CertVerifyResult* result,
             net::CompletionOnceCallback, std::unique_ptr<Request>* request,
             const net::NetLogWithSource&) override {
    request->reset();
    result->Reset();
    result->verified_cert = params.certificate();
    if (params.hostname() == host_ && params.certificate() &&
        base::HexEncode(net::X509Certificate::CalculateFingerprint256(params.certificate()->cert_buffer())) == pin_)
      return net::OK;
    result->cert_status = net::CERT_STATUS_AUTHORITY_INVALID;
    return net::ERR_CERT_AUTHORITY_INVALID;
  }
  void Verify2QwacBinding(const std::string&, const std::string&,
      const scoped_refptr<net::X509Certificate>&,
      base::OnceCallback<void(const scoped_refptr<net::X509Certificate>&)> callback,
      const net::NetLogWithSource&) override { std::move(callback).Run(nullptr); }
  void SetConfig(const Config&) override {}
  void AddObserver(Observer*) override {}
  void RemoveObserver(Observer*) override {}
 private:
  std::string host_;
  std::string pin_;
};

net::NetworkTrafficAnnotationTag Annotation() {
  return net::DefineNetworkTrafficAnnotation("crphotos_connect_pairing", R"(
    semantics {
      sender: "crPhotos Connect"
      description: "Pairs with a user-selected phone, archives its accessible media locally and sends selected photos and videos to it."
      trigger: "The user pairs a phone. While sharing is enabled, available paired phones are synchronized automatically."
      data: "Pairing code or saved bearer token and user-selected media, sent only after certificate pin validation."
      destination: OTHER
      destination_other: "User-selected private IPv4 HTTPS server on the local network."
    }
    policy {
      cookies_allowed: NO
      setting: "Disabling nearby sharing or revoking the phone pairing stops automatic synchronization."
      policy_exception_justification: "Standalone crPhotos application."
    })");
}

class HttpRequest : public net::URLRequest::Delegate {
 public:
  using Done = base::OnceCallback<void(int, int, std::string)>;
  HttpRequest(const ConnectPeer& peer, const std::string& pin, const std::string& token,
              const std::string& path, const std::string& body, Done done,
              base::FilePath destination = {}, bool thumbnail = false,
              base::FilePath source = {}, int64_t source_size = 0,
              base::Time source_modified = {}, base::RepeatingCallback<void(int64_t)> progress = {})
      : done_(std::move(done)), destination_(std::move(destination)), thumbnail_(thumbnail), buffer_(base::MakeRefCounted<net::IOBufferWithSize>(16384)) {
    net::URLRequestContextBuilder builder;
    builder.DisableHttpCache(); builder.SetCookieStore(nullptr);
    builder.SetSpdyAndQuicEnabled(false, false);
    builder.set_proxy_config_service(std::make_unique<net::ProxyConfigServiceFixed>(
        net::ProxyConfigWithAnnotation::CreateDirect()));
    builder.SetCertVerifier(std::make_unique<PinVerifier>(peer.address, pin));
    context_ = builder.Build();
    request_ = context_->CreateRequest(GURL("https://" + peer.address + ":" +
        base::NumberToString(peer.port) + path), net::DEFAULT_PRIORITY, this, Annotation(),
        net::handles::kInvalidNetworkHandle);
    request_->SetLoadFlags(net::LOAD_DISABLE_CACHE);
    // This adds LOAD_DO_NOT_SAVE_COOKIES, which must not be cleared by a
    // subsequent SetLoadFlags() call (URLRequest enforces this with CHECK).
    request_->set_disallow_credentials(); request_->set_send_client_certs(false);
    request_->set_isolation_info(net::IsolationInfo::CreateTransient(std::nullopt));
    net::HttpRequestHeaders headers;
    headers.SetHeader("Connection", "close");
    if (!token.empty()) headers.SetHeader("Authorization", "Bearer " + token);
    if (!source.empty()) {
      request_->set_method("POST");
      headers.SetHeader("Content-Type", "application/octet-stream");
      std::vector<std::unique_ptr<net::UploadElementReader>> readers;
      readers.push_back(std::make_unique<net::UploadFileElementReader>(
          base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock()}).get(),
          source, 0, source_size, source_modified));
      request_->set_upload(std::make_unique<net::ElementsUploadDataStream>(std::move(readers), 0));
      progress_ = std::move(progress);
    } else if (!body.empty()) {
      request_->set_method("POST");
      headers.SetHeader("Content-Type", "application/json");
      std::vector<std::unique_ptr<net::UploadElementReader>> readers;
      readers.push_back(net::UploadOwnedBytesElementReader::CreateWithString(body));
      request_->set_upload(std::make_unique<net::ElementsUploadDataStream>(std::move(readers), 0));
    }
    request_->SetExtraRequestHeaders(headers);
  }
  ~HttpRequest() override {
    output_.Close();
    if (owns_output_ && !complete_) base::DeleteFile(destination_);
  }
  int64_t bytes() const { return bytes_; }
  const std::string& sha256() const { return sha256_; }
  void Start() {
    if (!destination_.empty()) {
      output_.Initialize(destination_, base::File::FLAG_CREATE | base::File::FLAG_WRITE);
      if (!output_.IsValid()) { Finish(net::ERR_FILE_EXISTS); return; }
      owns_output_ = true;
    }
    timeout_.Start(FROM_HERE, base::Seconds(20), base::BindOnce(&HttpRequest::Finish,
        base::Unretained(this), net::ERR_TIMED_OUT));
    if (progress_) upload_timer_.Start(FROM_HERE, base::Milliseconds(250),
        base::BindRepeating(&HttpRequest::UploadProgress, base::Unretained(this)));
    request_->Start();
  }
 private:
  void UploadProgress() {
    const auto position = request_->GetUploadProgress().position();
    if (position > upload_position_) { upload_position_ = position; timeout_.Reset(); }
    progress_.Run(static_cast<int64_t>(position));
  }
  void OnReceivedRedirect(net::URLRequest*, const net::RedirectInfo&, bool* defer) override {
    *defer = true; Finish(net::ERR_UNSAFE_REDIRECT);
  }
  void OnAuthRequired(net::URLRequest* request, const net::AuthChallengeInfo&) override { request->CancelAuth(); }
  void OnSSLCertificateError(net::URLRequest*, int, const net::SSLInfo&, bool) override {
    Finish(net::ERR_CERT_AUTHORITY_INVALID);
  }
  void OnResponseStarted(net::URLRequest*, int error) override {
    if (error != net::OK) { Finish(error); return; }
    upload_timer_.Stop();
    status_ = request_->GetResponseCode();
    if (status_ != 200) { Finish(net::OK); return; }
    std::string mime;
    request_->GetMimeType(&mime);
    if (mime != (destination_.empty() ? "application/json" : thumbnail_ ? "image/jpeg" : "application/octet-stream")) { Finish(net::ERR_INVALID_RESPONSE); return; }
    Read();
  }
  void Read() {
    while (done_) {
      int bytes = request_->Read(buffer_.get(), buffer_->size());
      if (bytes == net::ERR_IO_PENDING || !Consume(bytes)) break;
    }
  }
  void OnReadCompleted(net::URLRequest*, int bytes) override { if (done_ && Consume(bytes)) Read(); }
  bool Consume(int bytes) {
    if (bytes <= 0) { Finish(bytes); return false; }
    if (!destination_.empty()) {
      const int64_t limit = thumbnail_ ? 2 * 1024 * 1024 : INT64_C(128) * 1024 * 1024 * 1024;
      if (bytes_ > limit - bytes) { Finish(net::ERR_FILE_TOO_BIG); return false; }
      const auto data = buffer_->span().first(static_cast<size_t>(bytes));
      if (!output_.WriteAtCurrentPosAndCheck(data)) { Finish(net::ERR_FILE_NO_SPACE); return false; }
      hash_.Update(data); bytes_ += bytes;
      // Inactivity timeout, not a total-duration limit on large videos.
      timeout_.Reset();
      return true;
    }
    if (body_.size() + static_cast<size_t>(bytes) > 2 * 1024 * 1024) {
      Finish(net::ERR_FILE_TOO_BIG); return false;
    }
    body_.append(buffer_->data(), bytes); return true;
  }
  void Finish(int error) {
    if (!done_) return;
    if (!destination_.empty() && error == net::OK && status_ == 200) {
      if (!bytes_ || !output_.Flush()) error = net::ERR_FAILED;
      else {
        std::array<uint8_t, crypto::hash::kSha256Size> digest;
        hash_.Finish(digest); sha256_ = base::HexEncode(digest); complete_ = true;
      }
    }
    output_.Close();
    auto done = std::move(done_);
    upload_timer_.Stop(); timeout_.Stop(); request_->Cancel();
    std::move(done).Run(error, status_, std::move(body_));
  }
  base::RepeatingCallback<void(int64_t)> progress_;
  base::RepeatingTimer upload_timer_;
  uint64_t upload_position_ = 0;
  Done done_;
  base::FilePath destination_;
  bool thumbnail_ = false;
  bool complete_ = false;
  bool owns_output_ = false;
  base::File output_;
  int64_t bytes_ = 0;
  std::string sha256_;
  crypto::hash::Hasher hash_{crypto::hash::kSha256};
  scoped_refptr<net::IOBufferWithSize> buffer_;
  std::unique_ptr<net::URLRequestContext> context_;
  std::unique_ptr<net::URLRequest> request_;
  base::OneShotTimer timeout_;
  int status_ = 0;
  std::string body_;
};
}  // namespace

class ConnectClient::Backend {
 public:
  explicit Backend(Callback callback) : callback_(std::move(callback)) {}
  void Request(ConnectRequest command) {
    credential_timer_.Stop();
    last_http_status_ = 0; last_net_error_ = 0;
    ++generation_; job_.reset(); command_ = std::move(command);
    pin_.clear(); token_.clear(); saved_ = false;
    verification_generation_ = VerificationGeneration(command_.peer,
        command_.action == ConnectAction::kForget || command_.action == ConnectAction::kImport, true);
    if (!EndpointValid(command_.peer)) { Reply(ConnectStatus::kInvalidInput); return; }
    file_ = CredentialFile(command_.peer.id);
    if (file_.empty() || base::IsLink(file_)) { Reply(ConnectStatus::kStorageError); return; }
    if (command_.action == ConnectAction::kForget) {
      Reply(base::DeleteFile(file_) ? ConnectStatus::kForgotten : ConnectStatus::kStorageError); return;
    }
    if (base::PathExists(file_)) {
      std::string json;
      if (!base::ReadFileToStringWithMaxSize(file_, &json, 16384)) { Reply(ConnectStatus::kStorageError); return; }
      auto data = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
      const auto* pin = data ? data->FindString("certificate_sha256") : nullptr;
      const auto* token = data ? data->FindString("token") : nullptr;
      if (!pin || !token || NormalizePin(*pin).empty() || !TokenValid(*token) ||
          base::ToLowerASCII(pin->substr(0, 32)) != command_.peer.id) { Reply(ConnectStatus::kStorageError); return; }
      pin_ = NormalizePin(*pin); token_ = *token; saved_ = true;
    }
    if (command_.action == ConnectAction::kImport) {
      pin_ = NormalizePin(command_.fingerprint); saved_ = false;
      if (pin_.empty() || base::ToLowerASCII(pin_.substr(0, 32)) != command_.peer.id ||
          !TokenValid(command_.token) || command_.client_id.empty() || command_.client_id.size() > 80) {
        Reply(ConnectStatus::kInvalidInput); return;
      }
      token_ = command_.token;
      // Prove the phone owns the offered certificate and accepts this grant
      // before replacing any existing desktop credential.
      Fetch("/v1/profile", ComputerProfile(), false); return;
    }
    if (command_.action == ConnectAction::kLoad) {
      Reply(saved_ ? ConnectStatus::kSaved : ConnectStatus::kNeedsPairing); return;
    }
    if (command_.action == ConnectAction::kPair) {
      // A saved identity cannot be silently replaced by a new form or mDNS hint.
      if (saved_) { Reply(ConnectStatus::kSaved); return; }
      pin_ = NormalizePin(command_.fingerprint);
      if (pin_.empty() || base::ToLowerASCII(pin_.substr(0, 32)) != command_.peer.id ||
          command_.code.size() != 6 || command_.code.find_first_not_of("0123456789") != std::string::npos) {
        Reply(ConnectStatus::kInvalidInput); return;
      }
      base::DictValue body;
      body.Set("code", command_.code); body.Set("name", "crPhotos desktop");
      Fetch("/v1/pair", base::WriteJson(body).value_or(""), true);
    } else if (saved_ && (command_.action == ConnectAction::kReceive || command_.action == ConnectAction::kUpload)) {
      if (!base::StartsWith(command_.path, "/v1/receive/") || command_.path.size() > 256 ||
          command_.body.size() > 128 * 1024 ||
          (command_.action == ConnectAction::kUpload && (command_.source.empty() ||
           command_.source_size <= 0 || command_.source_size > INT64_C(16) * 1024 * 1024 * 1024))) {
        Reply(ConnectStatus::kInvalidInput); return;
      }
      Fetch(command_.path, command_.body, false);
    } else if (saved_ && (command_.action == ConnectAction::kList || command_.action == ConnectAction::kDownload)) {
      if (!base::StartsWith(command_.path, "/v1/media") || command_.path.size() > 4096 ||
          (command_.action == ConnectAction::kDownload && command_.destination.empty())) {
        Reply(ConnectStatus::kInvalidInput); return;
      }
      if (command_.action == ConnectAction::kList && command_.path.find("cursor=") == std::string::npos)
        Fetch("/v1/profile", ComputerProfile(), false);
      else Fetch(command_.path, "", false);
    } else if (saved_) Fetch("/v1/profile", ComputerProfile(), false);
    else Reply(ConnectStatus::kNeedsPairing);
  }
 private:
  void Reply(ConnectStatus status, int count = 0) {
    if (status == ConnectStatus::kConnected && saved_)
      RememberVerifiedPhone(command_.peer, verification_generation_);
    else if (status == ConnectStatus::kNeedsPairing || status == ConnectStatus::kUnauthorized ||
             status == ConnectStatus::kCertificateMismatch || status == ConnectStatus::kStorageError ||
             status == ConnectStatus::kNetworkError)
      VerificationGeneration(command_.peer, true);
    ConnectResult result;
    credential_timer_.Stop();
    result.http_status = last_http_status_; result.net_error = last_net_error_;
    result.status = status; result.fingerprint = pin_; result.saved = saved_; result.media_count = count;
    command_.code.clear(); command_.fingerprint.clear(); command_.token.clear();
    callback_.Run(std::move(result));
  }
  void CheckCredential() {
    std::string json;
    const bool read = !base::IsLink(file_) && base::ReadFileToStringWithMaxSize(file_, &json, 16384);
    auto data = read ? base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC) : std::nullopt;
    const auto* token = data ? data->FindString("token") : nullptr;
    if (token && *token == token_) return;
    ++generation_; job_.reset(); Reply(ConnectStatus::kNeedsPairing);
  }
  void Fetch(const std::string& path, const std::string& body, bool pairing) {
    profile_request_ = path == "/v1/profile";
    if (command_.action == ConnectAction::kList || command_.action == ConnectAction::kDownload ||
        command_.action == ConnectAction::kReceive || command_.action == ConnectAction::kUpload)
      credential_timer_.Start(FROM_HERE, base::Seconds(1), this, &Backend::CheckCredential);
    job_ = std::make_unique<HttpRequest>(command_.peer, pin_, pairing ? "" : token_, path, body,
        base::BindOnce(&Backend::Completed, weak_.GetWeakPtr(), generation_, pairing),
        command_.action == ConnectAction::kDownload ? command_.destination : base::FilePath(), command_.thumbnail,
        command_.action == ConnectAction::kUpload ? command_.source : base::FilePath(),
        command_.source_size, command_.source_modified,
        base::BindRepeating(&Backend::UploadProgress, weak_.GetWeakPtr()));
    job_->Start();
  }
  void UploadProgress(int64_t bytes) {
    ConnectResult result; result.status = ConnectStatus::kUploading; result.bytes = bytes;
    callback_.Run(std::move(result));
  }
  void Completed(uint64_t generation, bool pairing, int error, int status, std::string body) {
    // Delete the request/context only after URLRequest's delegate stack unwinds.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&Backend::Process, weak_.GetWeakPtr(), generation, pairing, error, status, std::move(body)));
  }
  void Process(uint64_t generation, bool pairing, int error, int status, std::string body) {
    if (generation != generation_) return;
    credential_timer_.Stop();
    last_http_status_ = status; last_net_error_ = error;
    ConnectResult download;
    download.bytes = job_->bytes(); download.sha256 = job_->sha256(); download.http_status = status;
    job_.reset();
    if (command_.action == ConnectAction::kImport && (error != net::OK || status != 200))
      LOG(WARNING) << "Connect QR phone verification failed: net_error=" << error << " http_status=" << status;
    if (error == net::ERR_CERT_AUTHORITY_INVALID) { Reply(ConnectStatus::kCertificateMismatch); return; }
    if (error != net::OK) { Reply(ConnectStatus::kNetworkError); return; }
    if (status == 401) {
      if (command_.action != ConnectAction::kImport && command_.action != ConnectAction::kList &&
          command_.action != ConnectAction::kDownload && command_.action != ConnectAction::kReceive &&
          command_.action != ConnectAction::kUpload && !base::DeleteFile(file_)) { Reply(ConnectStatus::kStorageError); return; }
      saved_ = false; token_.clear(); Reply(ConnectStatus::kUnauthorized); return;
    }
    if (status == 403) { Reply(pairing ? ConnectStatus::kPairRejected : ConnectStatus::kUnauthorized); return; }
    if (profile_request_ && (status == 200 || status == 404 || status == 405)) {
      // Older phones do not expose profiles. Keep their existing connection flow.
      Fetch(command_.action == ConnectAction::kList ? command_.path : "/v1/media?limit=100&type=all", "", false);
      return;
    }
    if (status != 200) { Reply(ConnectStatus::kNetworkError); return; }
    if (saved_) RememberVerifiedPhone(command_.peer, verification_generation_);
    if (command_.action == ConnectAction::kDownload) {
      download.status = ConnectStatus::kConnected; download.saved = saved_;
      callback_.Run(std::move(download)); return;
    }
    if (command_.action == ConnectAction::kList || command_.action == ConnectAction::kReceive ||
        command_.action == ConnectAction::kUpload) {
      ConnectResult result; result.status = ConnectStatus::kConnected; result.saved = saved_;
      result.body = std::move(body); callback_.Run(std::move(result)); return;
    }
    auto json = base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC);
    if (!json) { Reply(ConnectStatus::kInvalidResponse); return; }
    if (pairing) {
      const auto* id = json->FindString("device_id");
      const auto* pin = json->FindString("certificate_sha256");
      const auto* token = json->FindString("token");
      const auto* client = json->FindString("client_id");
      if (!id || *id != command_.peer.id || !pin || NormalizePin(*pin) != pin_ ||
          !token || !TokenValid(*token) || !client || client->size() > 80 || client->empty()) {
        Reply(ConnectStatus::kInvalidResponse); return;
      }
      base::DictValue credentials;
      credentials.Set("device_id", *id); credentials.Set("certificate_sha256", pin_);
      credentials.Set("token", *token); credentials.Set("client_id", *client);
      const auto serialized = base::WriteJson(credentials);
      if (!serialized || !base::ImportantFileWriter::WriteFileAtomically(file_, *serialized)) {
        Reply(ConnectStatus::kStorageError); return;
      }
#if BUILDFLAG(IS_POSIX)
      if (!base::SetPosixFilePermissions(file_, 0600)) {
        base::DeleteFile(file_); Reply(ConnectStatus::kStorageError); return;
      }
#endif
      token_ = *token; saved_ = true;
      Fetch("/v1/profile", ComputerProfile(), false);
    } else {
      const auto* items = json->FindList("items");
      if (!items || items->size() > 100 || !json->FindDict("access") || !json->FindString("catalog_version")) {
        Reply(ConnectStatus::kInvalidResponse); return;
      }
      if (command_.action == ConnectAction::kImport) {
        base::DictValue credentials;
        credentials.Set("device_id", command_.peer.id); credentials.Set("certificate_sha256", pin_);
        credentials.Set("token", token_); credentials.Set("client_id", command_.client_id);
        auto serialized = base::WriteJson(credentials);
        if (!serialized || !base::ImportantFileWriter::WriteFileAtomically(file_, *serialized)) {
          Reply(ConnectStatus::kStorageError); return;
        }
#if BUILDFLAG(IS_POSIX)
        if (!base::SetPosixFilePermissions(file_, 0600)) {
          base::DeleteFile(file_); Reply(ConnectStatus::kStorageError); return;
        }
#endif
        saved_ = true;
      }
      Reply(ConnectStatus::kConnected, static_cast<int>(items->size()));
    }
  }
  bool profile_request_ = false;
  Callback callback_;
  ConnectRequest command_;
  base::FilePath file_;
  std::string pin_;
  std::string token_;
  bool saved_ = false;
  uint64_t generation_ = 0;
  uint64_t verification_generation_ = 0;
  int last_http_status_ = 0;
  int last_net_error_ = 0;
  base::RepeatingTimer credential_timer_;
  std::unique_ptr<HttpRequest> job_;
  base::WeakPtrFactory<Backend> weak_{this};
};

bool ConnectClient::IsVerifiedPeer(const ConnectPeer& peer, base::TimeDelta minimum_validity) {
  auto& cache = PhoneVerification();
  base::AutoLock lock(cache.lock);
  const auto found = cache.endpoints.find(VerificationKey(peer));
  return found != cache.endpoints.end() && found->second > base::TimeTicks::Now() + minimum_validity;
}

ConnectClient::ConnectClient(Callback callback) : callback_(std::move(callback)) {
  started_ = thread_.StartWithOptions(base::Thread::Options(base::MessagePumpType::IO, 0));
  if (started_) thread_.task_runner()->PostTask(FROM_HERE,
      base::BindOnce(&ConnectClient::StartOnIO, base::Unretained(this), callback_));
}
ConnectClient::~ConnectClient() {
  if (!started_) return;
  thread_.task_runner()->PostTask(FROM_HERE, base::BindOnce(&ConnectClient::StopOnIO, base::Unretained(this)));
  thread_.Stop();
}
void ConnectClient::Request(ConnectRequest request) {
  if (!started_) { ConnectResult result; result.status = ConnectStatus::kNetworkError; callback_.Run(std::move(result)); return; }
  thread_.task_runner()->PostTask(FROM_HERE,
      base::BindOnce(&ConnectClient::RequestOnIO, base::Unretained(this), std::move(request)));
}
void ConnectClient::StartOnIO(Callback callback) { backend_ = std::make_unique<Backend>(std::move(callback)); }
void ConnectClient::RequestOnIO(ConnectRequest request) { backend_->Request(std::move(request)); }
void ConnectClient::StopOnIO() { backend_.reset(); }
}  // namespace crfiles
