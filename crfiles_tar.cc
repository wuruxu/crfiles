// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_tar.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include "base/containers/span.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "third_party/lzma_sdk/src/C/7zCrc.h"
#include "third_party/lzma_sdk/src/C/Xz.h"
#include "third_party/lzma_sdk/src/C/XzCrc64.h"
#include "third_party/zlib/zlib.h"
namespace crfiles {
namespace {
constexpr uint64_t kMaximumFileBytes = 16ull * 1024 * 1024 * 1024;
constexpr uint64_t kMaximumStreamBytes = kMaximumFileBytes + 1024 * 1024 * 1024;
constexpr size_t kMaximumHeaders = 100000;
constexpr size_t kMaximumMetadata = 1024 * 1024;
constexpr size_t kMaximumAllMetadata = 64 * 1024 * 1024;

// Bounded buffers feed the TAR reader directly; no intermediate .tar file.
class TarInput {
 public:
  TarInput(base::File file, std::string_view format);
  ~TarInput();
  bool Read(base::span<uint8_t> destination) {
    while (!destination.empty()) {
      if (available_.empty() && (!Fill() || available_.empty())) return false;
      const auto count = std::min(destination.size(), available_.size());
      destination.first(count).copy_from(available_.first(count));
      destination = destination.subspan(count);
      available_ = available_.subspan(count);
    }
    return true;
  }
  bool Finish() {
    // Drain the compression footer, validating CRCs and rejecting nonzero
    // trailing data instead of publishing a truncated or corrupt archive.
    do {
      if (!std::ranges::all_of(available_, [](uint8_t value) { return value == 0; })) return false;
      available_ = {};
      if (!Fill()) return false;
    } while (!available_.empty());
    return ended_;
  }
 private:
  bool Fill();
  bool ReadInput() {
    if (!encoded_.empty() || eof_) return true;
    auto count = file_.ReadAtCurrentPos(input_);
    if (!count) return false;
    encoded_ = base::span(input_).first(*count);
    eof_ = *count == 0;
    return true;
  }
  base::File file_;
  bool gzip_ = false, xz_ = false, initialized_ = false;
  bool eof_ = false, ended_ = false, gzip_member_done_ = false;
  uint64_t decoded_ = 0;
  std::array<uint8_t, 65536> input_, buffer_;
  base::span<uint8_t> encoded_, available_;
  z_stream gzip_state_{};
  CXzUnpacker xz_state_{};
  ISzAlloc allocator_{};
};
TarInput::TarInput(base::File file, std::string_view format) : file_(std::move(file)) {
  gzip_ = format == ".tar.gz" || format == ".tgz";
  xz_ = format == ".tar.xz";
  if (gzip_) initialized_ = inflateInit2(&gzip_state_, MAX_WBITS + 16) == Z_OK;
  else if (xz_) {
    [[maybe_unused]] static const bool tables = [] { CrcGenerateTable(); Crc64GenerateTable(); return true; }();
    // Reject oversized dictionaries requested by untrusted XZ headers.
    allocator_.Alloc = [](ISzAllocPtr, size_t size) -> void* {
      return size <= 128 * 1024 * 1024 ? std::malloc(size) : nullptr;
    };
    allocator_.Free = [](ISzAllocPtr, void* pointer) { std::free(pointer); };
    XzUnpacker_Construct(&xz_state_, &allocator_);
    XzUnpacker_Init(&xz_state_);
    initialized_ = true;
  } else initialized_ = format == ".tar";
}
TarInput::~TarInput() {
  if (initialized_ && gzip_) inflateEnd(&gzip_state_);
  if (initialized_ && xz_) XzUnpacker_Free(&xz_state_);
}
bool TarInput::Fill() {
  if (!initialized_) return false;
  if (ended_) return true;
  size_t produced = 0;
  if (!gzip_ && !xz_) {
    auto count = file_.ReadAtCurrentPos(buffer_);
    if (!count) return false;
    produced = *count;
    ended_ = produced == 0;
  } else while (!produced && !ended_) {
    if (!ReadInput()) return false;
    size_t consumed = 0;
    if (gzip_) {
      if (gzip_member_done_) {
        if (eof_) { ended_ = true; break; }
        if (inflateReset2(&gzip_state_, MAX_WBITS + 16) != Z_OK) return false;
        gzip_member_done_ = false;
      }
      gzip_state_.next_in = encoded_.data();
      gzip_state_.avail_in = static_cast<uInt>(encoded_.size());
      gzip_state_.next_out = buffer_.data();
      gzip_state_.avail_out = static_cast<uInt>(buffer_.size());
      const int result = inflate(&gzip_state_, Z_NO_FLUSH);
      consumed = encoded_.size() - gzip_state_.avail_in;
      produced = buffer_.size() - gzip_state_.avail_out;
      if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) return false;
      gzip_member_done_ = result == Z_STREAM_END;
    } else {
      consumed = encoded_.size(); produced = buffer_.size();
      ECoderStatus status = CODER_STATUS_NOT_FINISHED;
      if (XzUnpacker_Code(&xz_state_, buffer_.data(), &produced, encoded_.data(), &consumed,
          eof_, CODER_FINISH_ANY, &status) != SZ_OK) return false;
      if (eof_ && !consumed && !produced) {
        if (!XzUnpacker_IsStreamWasFinished(&xz_state_)) return false;
        ended_ = true;
      }
    }
    encoded_ = encoded_.subspan(consumed);
    if (!consumed && !produced && !ended_ && !gzip_member_done_) return false;
  }
  if (produced > kMaximumStreamBytes - decoded_) return false;
  decoded_ += produced;
  available_ = base::span(buffer_).first(produced);
  return true;
}

std::optional<uint64_t> Number(base::span<const uint8_t> field) {
  uint64_t value = 0;
  if (field.front() & 0x80) {
    // GNU base-256 extension; negative sizes are never meaningful here.
    if (field.front() & 0x40) return std::nullopt;
    value = field.front() & 0x7f;
    for (uint8_t byte : field.subspan(size_t{1})) {
      if (value > (std::numeric_limits<uint64_t>::max() - byte) / 256) return std::nullopt;
      value = value * 256 + byte;
    }
    return value;
  }
  bool ended = false, digit = false;
  for (uint8_t byte : field) {
    if (byte == 0 || byte == ' ') { if (digit) ended = true; continue; }
    if (ended || byte < '0' || byte > '7' || value > (std::numeric_limits<uint64_t>::max() - (byte - '0')) / 8)
      return std::nullopt;
    value = value * 8 + byte - '0'; digit = true;
  }
  return value;
}
std::string Field(base::span<const uint8_t> field) {
  const auto end = std::ranges::find(field, uint8_t{0});
  return std::string(field.begin(), end);
}
bool Decimal(std::string_view text, uint64_t& value) {
  return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; }) &&
      base::StringToUint64(text, &value);
}
using Attributes = std::map<std::string, std::string>;
bool Pax(std::string_view data, Attributes& attributes) {
  while (!data.empty()) {
    const size_t space = data.find(' ');
    uint64_t length = 0;
    if (space == std::string_view::npos || !Decimal(data.substr(0, space), length) ||
        length > data.size() || length <= space + 2) return false;
    auto record = data.substr(space + 1, static_cast<size_t>(length) - space - 1);
    if (record.back() != '\n') return false;
    record.remove_suffix(1);
    const size_t equals = record.find('=');
    if (equals == 0 || equals == std::string_view::npos || record.find('\0') != std::string_view::npos) return false;
    const auto key = record.substr(0, equals), value = record.substr(equals + 1);
    // Sparse layouts and special filesystem metadata are unsupported.
    if (key.starts_with("GNU.sparse.") || key == "SCHILY.realsize" ||
        (key == "SCHILY.filetype" && value != "file")) return false;
    if (key == "path" || key == "size" || key == "linkpath") attributes[std::string(key)] = std::string(value);
    data.remove_prefix(static_cast<size_t>(length));
  }
  return true;
}
bool Padding(TarInput& input, uint64_t size) {
  std::array<uint8_t, 512> padding;
  return input.Read(base::span(padding).first(static_cast<size_t>((512 - size % 512) % 512)));
}
}

bool ExtractTar(base::File file, std::string_view format, TarOutput& output) {
  TarInput input(std::move(file), format);
  std::array<uint8_t, 512> header;
  std::array<uint8_t, 65536> buffer;
  Attributes global, local;
  std::optional<std::string> long_name;
  std::optional<std::string> long_link;
  size_t metadata_bytes = 0;
  bool pending_metadata = false;
  for (size_t entries = 0; entries < kMaximumHeaders; ++entries) {
    if (!input.Read(header)) return false;
    auto bytes = base::span(header);
    if (std::ranges::all_of(header, [](uint8_t c) { return c == 0; })) {
      if (pending_metadata || !input.Read(header) ||
          !std::ranges::all_of(header, [](uint8_t c) { return c == 0; })) return false;
      return input.Finish() && output.FinishLinks();
    }
    const auto checksum = Number(bytes.subspan(size_t{148}, size_t{8}));
    uint64_t sum = 0;
    int64_t signed_sum = 0;
    for (size_t i = 0; i < header.size(); ++i) {
      const uint8_t value = i >= 148 && i < 156 ? ' ' : header[i];
      sum += value; signed_sum += value < 128 ? value : static_cast<int>(value) - 256;
    }
    if (!checksum || (*checksum != sum && (signed_sum < 0 || *checksum != static_cast<uint64_t>(signed_sum)))) return false;
    auto size = Number(bytes.subspan(size_t{124}, size_t{12}));
    if (!size || *size > kMaximumFileBytes) return false;
    const auto magic = Field(bytes.subspan(size_t{257}, size_t{6}));
    if (!magic.empty() && magic != "ustar" && magic != "ustar ") return false;
    const char type = static_cast<char>(header[156]);
    if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
      if (*size > kMaximumMetadata || *size > kMaximumAllMetadata - metadata_bytes) return false;
      metadata_bytes += static_cast<size_t>(*size);
      std::string data(static_cast<size_t>(*size), '\0');
      if (!input.Read(base::as_writable_byte_span(data)) || !Padding(input, *size)) return false;
      if (type == 'L' || type == 'K') {
        if (!data.empty() && data.back() == '\0') data.pop_back();
        if (data.empty() || data.find('\0') != std::string::npos) return false;
        (type == 'L' ? long_name : long_link) = std::move(data);
      } else if (!Pax(data, type == 'g' ? global : local)) return false;
      if (type != 'g') pending_metadata = true;
      continue;
    }
    // Sparse files and special device entries remain unsupported.
    if (type != '\0' && type != '0' && type != '5' && type != '1' && type != '2') return false;
    const bool directory = type == '5';
    std::string name = Field(bytes.first(size_t{100}));
    if (magic == "ustar") {
      auto prefix = Field(bytes.subspan(size_t{345}, size_t{155}));
      if (!prefix.empty()) name = prefix + "/" + name;
    }
    if (long_name) name = *long_name;
    std::string link = long_link.value_or(Field(bytes.subspan(size_t{157}, size_t{100})));
    auto attributes = global;
    for (const auto& [key, value] : local) {
      if (value.empty()) attributes.erase(key);
      else attributes[key] = value;
    }
    if (auto found = attributes.find("path"); found != attributes.end() && !found->second.empty()) name = found->second;
    if (auto found = attributes.find("linkpath"); found != attributes.end() && !found->second.empty()) link = found->second;
    if (auto found = attributes.find("size"); found != attributes.end() && !found->second.empty()) {
      uint64_t value;
      if (!Decimal(found->second, value) || value > kMaximumFileBytes) return false;
      size = value;
    }
    local.clear(); long_name.reset(); long_link.reset(); pending_metadata = false;
    if (directory && *size) return false;
    if (!base::IsStringUTF8(name) || name.find('\0') != std::string::npos) return false;
    std::string_view normalized_name(name);
    while (normalized_name.starts_with("./")) normalized_name.remove_prefix(2);
    // `tar -cf archive.tar .` contains a harmless root-directory entry.
    if (directory && (normalized_name.empty() || normalized_name == ".")) continue;
    if (type == '1' || type == '2') {
      if (*size || link.empty() || !base::IsStringUTF8(link) || link.find('\0') != std::string::npos ||
          !output.Link(base::FilePath::FromUTF8Unsafe(normalized_name), base::FilePath::FromUTF8Unsafe(link), type == '2')) return false;
      continue;
    }
    const auto target = output.Entry(base::FilePath::FromUTF8Unsafe(normalized_name), directory);
    if (target.empty()) return false;
    if (directory) continue;
    base::File destination(target, base::File::FLAG_CREATE | base::File::FLAG_WRITE);
    if (!destination.IsValid()) return false;
    uint64_t remaining = *size;
    while (remaining) {
      const auto count = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
      auto chunk = base::span(buffer).first(count);
      if (!input.Read(chunk) || !output.AddBytes(count) || !destination.WriteAtCurrentPosAndCheck(chunk)) return false;
      remaining -= count;
    }
    if (!Padding(input, *size)) return false;
  }
  return false;
}
}
