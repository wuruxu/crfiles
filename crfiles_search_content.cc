// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_search_content.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <string_view>
#include <vector>
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/i18n/case_conversion.h"
#include "base/no_destructor.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "third_party/pdfium/public/cpp/fpdf_scopers.h"
#include "third_party/pdfium/public/fpdf_text.h"
#include "third_party/pdfium/public/fpdfview.h"
#include "third_party/zlib/zlib.h"
namespace crfiles {
std::string SearchFragment(std::string_view value, const std::u16string& query) {
  auto text = base::UTF8ToUTF16(value);
  auto folded = base::i18n::ToLower(text);
  const auto position = folded.find(query);
  if (position == std::u16string::npos) return {};
  const size_t start = std::min(text.size(), position > 70 ? position - 70 : 0);
  text = text.substr(start, std::min<size_t>(280, text.size() - start));
  for (auto& c : text) if (c < u' ') c = u' ';
  return base::UTF16ToUTF8(text);
}
namespace {
constexpr size_t kMetadataLimit = 4 * 1024 * 1024;
std::string DecodeText(std::string_view data) {
  if (data.starts_with("\xff\xfe") || data.starts_with("\xfe\xff")) {
    const bool little = data.starts_with("\xff\xfe");
    std::u16string decoded;
    for (size_t i = 2; i + 1 < data.size(); i += 2) {
      const auto a = static_cast<unsigned char>(data[i]), b = static_cast<unsigned char>(data[i + 1]);
      decoded.push_back(static_cast<char16_t>(little ? a | (b << 8) : (a << 8) | b));
    }
    return base::UTF16ToUTF8(decoded);
  }
  if (data.starts_with("\xef\xbb\xbf")) data.remove_prefix(3);
  if (data.find('\0') != std::string_view::npos || !base::IsStringUTF8(data)) return {};
  return std::string(data);
}
std::string Latin1(std::string_view data) {
  std::u16string value;
  value.reserve(data.size());
  for (unsigned char c : data) value.push_back(c);
  return base::UTF16ToUTF8(value);
}
std::string Inflate(std::string_view data, size_t limit, bool* skipped) {
  std::string result(limit, '\0');
  uLongf size = result.size();
  if (uncompress(reinterpret_cast<Bytef*>(result.data()), &size,
      reinterpret_cast<const Bytef*>(data.data()), data.size()) != Z_OK) { *skipped = true; return {}; }
  result.resize(size); return result;
}
// Bounds-checked TIFF reader for EXIF IFDs, including camera, lens, time and GPS.
// Field names use standardized metadata identifiers rather than UI sentences.
class ExifReader {
 public:
  explicit ExifReader(std::string_view data) : data_(data), little_(data.starts_with("II")) {}
  std::vector<std::string> Read() {
    if (data_.size() < 8 || (!data_.starts_with("II") && !data_.starts_with("MM")) || U16(2) != 42) return {};
    Ifd(U32(4), 0, false); return std::move(fields_);
  }
 private:
  bool Fits(size_t offset, size_t count) const { return offset <= data_.size() && count <= data_.size() - offset; }
  uint32_t UInt(size_t offset, size_t count) const {
    if (!Fits(offset, count)) return 0;
    uint32_t value = 0;
    for (size_t i = 0; i < count; ++i) {
      const auto b = static_cast<unsigned char>(data_[offset + (little_ ? count - i - 1 : i)]);
      value = (value << 8) | b;
    }
    return value;
  }
  uint16_t U16(size_t offset) const { return static_cast<uint16_t>(UInt(offset, 2)); }
  uint32_t U32(size_t offset) const { return UInt(offset, 4); }
  std::string Tag(uint16_t tag, bool gps) const {
    if (gps) {
      switch (tag) {
        case 1: return "GPS.LatitudeRef"; case 2: return "GPS.Latitude";
        case 3: return "GPS.LongitudeRef"; case 4: return "GPS.Longitude";
        case 5: return "GPS.AltitudeRef"; case 6: return "GPS.Altitude";
        case 7: return "GPS.TimeStamp"; case 29: return "GPS.DateStamp";
        default: return {};
      }
    }
    switch (tag) {
      case 0x02bc: return "XMP"; case 0x010e: return "EXIF.ImageDescription"; case 0x010f: return "EXIF.Make";
      case 0x0110: return "EXIF.Model"; case 0x0131: return "EXIF.Software";
      case 0x0132: return "EXIF.DateTime"; case 0x013b: return "EXIF.Artist";
      case 0x8298: return "EXIF.Copyright"; case 0x9003: return "EXIF.DateTimeOriginal";
      case 0x9004: return "EXIF.DateTimeDigitized"; case 0x9286: return "EXIF.UserComment";
      case 0xa433: return "EXIF.LensMake"; case 0xa434: return "EXIF.LensModel";
      case 0x829a: return "EXIF.ExposureTime"; case 0x829d: return "EXIF.FNumber";
      case 0x8827: return "EXIF.ISO"; case 0x920a: return "EXIF.FocalLength";
      case 0x9c9b: return "EXIF.XPTitle"; case 0x9c9c: return "EXIF.XPComment";
      case 0x9c9d: return "EXIF.XPAuthor"; case 0x9c9e: return "EXIF.XPKeywords";
      case 0x9c9f: return "EXIF.XPSubject"; default: return {};
    }
  }
  void Ifd(size_t offset, int depth, bool gps) {
    if (depth > 4 || !Fits(offset, 2) || !visited_.insert(offset).second) return;
    const size_t count = std::min<size_t>(U16(offset), 512);
    for (size_t i = 0; i < count && fields_.size() < 256; ++i) {
      const size_t entry = offset + 2 + 12 * i;
      if (!Fits(entry, 12)) return;
      const uint16_t tag = U16(entry), type = U16(entry + 2);
      const uint32_t items = U32(entry + 4);
      if (!gps && (tag == 0x8769 || tag == 0x8825)) { Ifd(U32(entry + 8), depth + 1, tag == 0x8825); continue; }
      const auto name = Tag(tag, gps);
      if (name.empty() || items == 0 || items > 65536) continue;
      size_t unit = type == 3 ? 2 : type == 4 || type == 9 ? 4 : type == 5 || type == 10 ? 8 : 1;
      const size_t bytes = unit * items;
      const size_t location = bytes <= 4 ? entry + 8 : U32(entry + 8);
      if (!Fits(location, bytes)) continue;
      std::string value;
      if (type == 2 || type == 7 || tag == 0x02bc || (tag >= 0x9c9b && tag <= 0x9c9f)) {
        auto raw = data_.substr(location, std::min<size_t>(bytes, 2048));
        if (tag >= 0x9c9b && tag <= 0x9c9f) value = DecodeText(std::string("\xff\xfe") + std::string(raw));
        else if (tag == 0x9286 && raw.size() >= 8 && raw.starts_with("UNICODE")) {
          raw.remove_prefix(8);
          value = DecodeText(std::string(little_ ? "\xff\xfe" : "\xfe\xff") + std::string(raw));
        } else {
          if (tag == 0x9286 && raw.size() >= 8 && raw.starts_with("ASCII")) raw.remove_prefix(8);
          const auto zero = raw.find('\0');
          value = DecodeText(raw.substr(0, zero));
        }
      } else {
        for (size_t n = 0; n < std::min<size_t>(items, 4); ++n) {
          if (!value.empty()) value += ", ";
          if (type == 5 || type == 10) {
            const auto numerator = U32(location + n * unit), denominator = U32(location + n * unit + 4);
            const double divisor = type == 10 ? static_cast<int32_t>(denominator) : static_cast<double>(denominator);
            const double dividend = type == 10 ? static_cast<int32_t>(numerator) : static_cast<double>(numerator);
            if (divisor) value += base::NumberToString(dividend / divisor);
          } else if (unit <= 4) value += base::NumberToString(UInt(location + n * unit, unit));
        }
      }
      if (!value.empty()) fields_.push_back(name + ": " + value);
    }
  }
  std::string_view data_;
  bool little_;
  std::set<size_t> visited_;
  std::vector<std::string> fields_;
};
std::string MatchExif(std::string_view data, const std::u16string& query) {
  for (const auto& field : ExifReader(data).Read()) {
    auto match = SearchFragment(field, query); if (!match.empty()) return match;
  }
  return {};
}
bool ReadAt(base::File& file, uint64_t offset, size_t size, std::string* out) {
  if (size > kMetadataLimit || offset > static_cast<uint64_t>(INT64_MAX)) return false;
  out->resize(size);
  auto read = file.Read(static_cast<int64_t>(offset), base::as_writable_byte_span(*out));
  return read && *read == size;
}
uint32_t Big32(std::string_view s) {
  if (s.size() < 4) return 0;
  uint32_t value = 0; for (size_t i = 0; i < 4; ++i) value = (value << 8) | static_cast<unsigned char>(s[i]);
  return value;
}
uint64_t Big64(std::string_view s) {
  if (s.size() < 8) return 0;
  uint64_t value = 0; for (size_t i = 0; i < 8; ++i) value = (value << 8) | static_cast<unsigned char>(s[i]);
  return value;
}
std::string VideoTag(std::string_view tag) {
  if (tag == "\xa9" "nam" || tag == "titl") return "Video.Title";
  if (tag == "\xa9" "des" || tag == "desc" || tag == "ldes" || tag == "dscp") return "Video.Description";
  if (tag == "\xa9" "ART" || tag == "\xa9" "aut" || tag == "auth") return "Video.Author";
  if (tag == "\xa9" "day" || tag == "date" || tag == "yrrc") return "Video.Date";
  if (tag == "\xa9" "mak") return "Video.CameraMake";
  if (tag == "\xa9" "mod") return "Video.CameraModel";
  if (tag == "\xa9" "xyz") return "Video.Location";
  if (tag == "\xa9" "cmt") return "Video.Comment";
  if (tag == "\xa9" "cpy" || tag == "cprt") return "Video.Copyright";
  if (tag == "\xa9" "too") return "Video.Software";
  if (tag == "keyw") return "Video.Keywords";
  return {};
}
std::string VideoText(std::string_view data, uint32_t type) {
  if (data.size() > 4096) data = data.substr(0, 4096);
  if (type == 2) {
    if (data.size() < 2 || data.size() % 2) return {};
    std::u16string decoded;
    for (size_t i = 0; i + 1 < data.size(); i += 2)
      decoded.push_back((static_cast<unsigned char>(data[i]) << 8) | static_cast<unsigned char>(data[i + 1]));
    if (!decoded.empty() && decoded.front() == 0xfeff) decoded.erase(decoded.begin());
    return base::UTF16ToUTF8(decoded);
  }
  if (type != 0 && type != 1) return {};
  const auto end = data.find('\0');
  data = data.substr(0, end);
  if (data.empty() || !base::IsStringUTF8(data)) return {};
  return std::string(data);
}
class VideoMetadataSearch {
 public:
  VideoMetadataSearch(base::File& file, const std::u16string& query,
                      const SearchControl& control, bool* skipped)
      : file_(file), query_(query), control_(control), skipped_(skipped),
        deadline_(base::TimeTicks::Now() + base::Seconds(2)) {}
  std::string Search(uint64_t length) { return Scan(0, length, 0, false, {}); }

 private:
  struct Box {
    uint64_t payload = 0, end = 0;
    std::string type;
  };
  bool ReadBox(uint64_t offset, uint64_t end, Box* box) {
    if (end - offset < 8) return false;
    std::string header;
    if (!ReadAt(*file_, offset, 8, &header)) return false;
    box->type = header.substr(4, 4);
    uint64_t size = Big32(header);
    uint64_t header_size = 8;
    if (size == 1) {
      if (end - offset < 16 || !ReadAt(*file_, offset + 8, 8, &header)) return false;
      size = Big64(header); header_size = 16;
    } else if (size == 0) {
      size = end - offset;
    }
    if (size < header_size || size > end - offset) return false;
    box->payload = offset + header_size;
    box->end = offset + size;
    return true;
  }
  bool ReadPayload(uint64_t start, uint64_t end, size_t limit, std::string* data) {
    const uint64_t size = end - start;
    if (size > limit || size > kMetadataLimit - metadata_bytes_) { *skipped_ = true; return false; }
    if (!ReadAt(*file_, start, static_cast<size_t>(size), data)) { *skipped_ = true; return false; }
    metadata_bytes_ += static_cast<size_t>(size);
    return true;
  }
  void ReadKeys(const Box& box) {
    std::string data;
    if (!ReadPayload(box.payload, box.end, 64 * 1024, &data) || data.size() < 8) return;
    const uint32_t count = Big32(std::string_view(data).substr(4, 4));
    if (count > 256) { *skipped_ = true; return; }
    size_t offset = 8;
    keys_.clear();
    for (uint32_t i = 0; i < count; ++i) {
      if (offset > data.size() || data.size() - offset < 8) { *skipped_ = true; return; }
      const size_t size = Big32(std::string_view(data).substr(offset, 4));
      if (size < 8 || size > data.size() - offset) { *skipped_ = true; return; }
      const auto key = std::string_view(data).substr(offset + 8, size - 8);
      keys_.push_back(base::IsStringUTF8(key) ? std::string(key) : std::string());
      offset += size;
    }
  }
  std::string ItemName(std::string_view type) const {
    auto name = VideoTag(type);
    if (!name.empty()) return name;
    const uint32_t index = Big32(type);
    if (index > 0 && index <= keys_.size()) return keys_[index - 1];
    return {};
  }
  std::string MatchValue(const Box& box, const std::string& name, bool data_box) {
    std::string data;
    if (!ReadPayload(box.payload, box.end, 64 * 1024, &data)) return {};
    std::string value;
    if (data_box) {
      if (data.size() < 8) return {};
      const uint32_t type = Big32(data) & 0xffffff;
      std::string_view payload = std::string_view(data).substr(8);
      if (type == 0 && (payload.starts_with(std::string_view("Exif\0\0", 6)) ||
                        payload.starts_with("II") || payload.starts_with("MM"))) {
        if (payload.starts_with(std::string_view("Exif\0\0", 6))) payload.remove_prefix(6);
        return MatchExif(payload, *query_);
      }
      value = VideoText(payload, type);
    } else {
      // QuickTime user-data strings may start with a 16-bit length and language.
      std::string_view text = data;
      if (data.size() >= 4 && ((static_cast<size_t>(static_cast<unsigned char>(data[0])) << 8) |
          static_cast<unsigned char>(data[1])) == data.size() - 4)
        text.remove_prefix(4);
      value = VideoText(text, 0);
    }
    return value.empty() ? std::string() : SearchFragment(name + ": " + value, *query_);
  }
  std::string MatchDates(const Box& box) {
    std::string header;
    if (box.end - box.payload < 4 || !ReadPayload(box.payload, box.payload + 4, 4, &header)) {
      *skipped_ = true; return {};
    }
    const unsigned char version = static_cast<unsigned char>(header.front());
    if (version > 1) { *skipped_ = true; return {}; }
    const size_t width = version == 1 ? 8 : 4;
    if (box.end - box.payload < 4 + 2 * width) { *skipped_ = true; return {}; }
    std::string dates;
    if (!ReadPayload(box.payload + 4, box.payload + 4 + 2 * width, 16, &dates)) return {};
    for (size_t index = 0; index < 2; ++index) {
      const auto bytes = std::string_view(dates).substr(index * width, width);
      const uint64_t seconds = version == 1 ? Big64(bytes) : Big32(bytes);
      // ISO BMFF uses seconds since 1904-01-01 UTC. Zero is unspecified;
      // reject dates beyond year 9999 before converting or multiplying.
      if (seconds == 0) continue;
      if (seconds > 255485145599ULL) { *skipped_ = true; continue; }
      const auto time = base::Time::UnixEpoch() + base::Seconds(static_cast<int64_t>(seconds) - 2082844800LL);
      base::Time::Exploded exploded;
      time.UTCExplode(&exploded);
      const auto value = base::StringPrintf("%04d-%02d-%02d %02d:%02d:%02d UTC",
          exploded.year, exploded.month, exploded.day_of_month, exploded.hour, exploded.minute, exploded.second);
      auto match = SearchFragment("Video." + box.type + (index == 0 ? ".CreateDate: " : ".ModifyDate: ") + value, *query_);
      if (!match.empty()) return match;
    }
    return {};
  }
  std::string Scan(uint64_t start, uint64_t end, int depth, bool in_ilst,
                   const std::string& item_name) {
    for (uint64_t offset = start; offset < end && !control_->Interrupted(); ) {
      if (base::TimeTicks::Now() >= deadline_ || ++boxes_ > 4096) { *skipped_ = true; break; }
      Box box;
      if (!ReadBox(offset, end, &box)) { *skipped_ = true; break; }
      offset = box.end;
      if (box.type == "mvhd" || box.type == "tkhd" || box.type == "mdhd") {
        auto match = MatchDates(box);
        if (!match.empty()) return match;
        continue;
      }
      if (box.type == "keys" && depth <= 3) { ReadKeys(box); continue; }
      if (box.type == "Exif" || box.type == "exif") {
        std::string data;
        if (ReadPayload(box.payload, box.end, 64 * 1024, &data)) {
          std::string_view exif(data);
          if (exif.starts_with(std::string_view("Exif\0\0", 6))) exif.remove_prefix(6);
          auto match = MatchExif(exif, *query_);
          if (!match.empty()) return match;
        }
        continue;
      }
      if (box.type == "data" && !item_name.empty()) {
        auto match = MatchValue(box, item_name, true);
        if (!match.empty()) return match;
        continue;
      }
      if (depth >= 6) continue;
      if (in_ilst) {
        const auto name = ItemName(box.type);
        if (!name.empty()) {
          auto match = Scan(box.payload, box.end, depth + 1, false, name);
          if (!match.empty()) return match;
        }
      } else if (box.type == "moov" || box.type == "udta" || box.type == "trak" ||
                 box.type == "mdia" || box.type == "meta" || box.type == "ilst") {
        uint64_t child_start = box.payload;
        if (box.type == "meta") {
          // ISO FullBox metadata has version/flags; Android/QuickTime files
          // also use an unversioned container starting directly with hdlr.
          std::string prefix;
          if (box.end - child_start < 4 || !ReadAt(*file_, child_start, 4, &prefix)) {
            *skipped_ = true; continue;
          }
          if (Big32(prefix) == 0) {
            child_start += 4;
          } else {
            Box first_child;
            if (!ReadBox(child_start, box.end, &first_child) ||
                (first_child.type != "hdlr" && first_child.type != "keys" && first_child.type != "ilst")) {
              *skipped_ = true; continue;
            }
          }
        }
        std::vector<std::string> parent_keys;
        if (box.type == "meta") { parent_keys = std::move(keys_); keys_.clear(); }
        auto match = Scan(child_start, box.end, depth + 1, box.type == "ilst", {});
        if (box.type == "meta") keys_ = std::move(parent_keys);
        if (!match.empty()) return match;
      } else if (depth > 0) {
        const auto name = VideoTag(box.type);
        if (!name.empty()) {
          auto match = MatchValue(box, name, false);
          if (!match.empty()) return match;
        }
      }
    }
    return {};
  }
  raw_ref<base::File> file_;
  raw_ref<const std::u16string> query_;
  raw_ref<const SearchControl> control_;
  raw_ptr<bool> skipped_;
  base::TimeTicks deadline_;
  size_t boxes_ = 0, metadata_bytes_ = 0;
  std::vector<std::string> keys_;
};
std::string ImageMetadata(base::File& file, std::string_view signature, const std::u16string& query,
                          const SearchControl& control, bool* skipped) {
  const auto deadline = base::TimeTicks::Now() + base::Seconds(2);
  const int64_t file_size = file.GetLength();
  if (file_size < 0) { *skipped = true; return {}; }
  const uint64_t length = static_cast<uint64_t>(file_size);
  std::string data, header;
  size_t metadata_bytes = 0, decoded_bytes = 0;
  if (signature.starts_with("\xff\xd8")) {
    uint64_t offset = 2;
    for (int segment = 0; segment < 4096 && offset + 4 <= length && !control.Interrupted() && base::TimeTicks::Now() < deadline; ++segment) {
      if (!ReadAt(file, offset, 2, &header) || static_cast<unsigned char>(header[0]) != 0xff) break;
      const auto marker = static_cast<unsigned char>(header[1]);
      if (marker == 0xff) { ++offset; continue; }
      if (marker == 0xda || marker == 0xd9) break;  // Never read JPEG pixels.
      if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8)) { offset += 2; continue; }
      if (!ReadAt(file, offset + 2, 2, &header)) break;
      const size_t size = (static_cast<unsigned char>(header[0]) << 8) | static_cast<unsigned char>(header[1]);
      if (size < 2 || offset + 2 + size > length) break;
      if (marker == 0xe1 || marker == 0xfe) {
        metadata_bytes += size;
        if (metadata_bytes > kMetadataLimit || !ReadAt(file, offset + 4, size - 2, &data)) { *skipped = true; break; }
        auto match = data.starts_with(std::string_view("Exif\0\0", 6)) ? MatchExif(std::string_view(data).substr(6), query) :
            SearchFragment(marker == 0xfe ? "JPEG.Comment: " + data : "XMP: " + data, query);
        if (!match.empty()) return match;
      }
      offset += 2 + size;
    }
  } else if (signature.starts_with("\x89PNG")) {
    uint64_t offset = 8;
    for (int chunk = 0; chunk < 65536 && offset + 12 <= length && !control.Interrupted() && base::TimeTicks::Now() < deadline; ++chunk) {
      if (!ReadAt(file, offset, 8, &header)) break;
      const uint64_t size = Big32(header);
      const auto type = header.substr(4, 4);
      if (size > length - offset - 12 || type == "IEND") break;
      if (type == "eXIf" || type == "tEXt" || type == "zTXt" || type == "iTXt") {
        if (size > kMetadataLimit - metadata_bytes || !ReadAt(file, offset + 8, size, &data)) { *skipped = true; break; }
        metadata_bytes += size;
        if (type == "eXIf") { auto match = MatchExif(data, query); if (!match.empty()) return match; }
        else {
          const auto zero = data.find('\0');
          if (zero != std::string::npos) {
            std::string text;
            if (type == "tEXt") text = data.substr(zero + 1);
            else if (type == "zTXt" && zero + 2 <= data.size() && data[zero + 1] == 0)
              text = Inflate(std::string_view(data).substr(zero + 2), kMetadataLimit - decoded_bytes, skipped);
            else if (type == "iTXt" && zero + 3 <= data.size()) {
              const auto language = data.find('\0', zero + 3);
              const auto translated = language == std::string::npos ? language : data.find('\0', language + 1);
              if (translated != std::string::npos && data[zero + 2] == 0)
                text = data[zero + 1] == 0 ? data.substr(translated + 1) : Inflate(std::string_view(data).substr(translated + 1), kMetadataLimit - decoded_bytes, skipped);
            }
            if (text.size() > kMetadataLimit - decoded_bytes) { *skipped = true; break; }
            decoded_bytes += text.size();
            if (type != "iTXt") text = Latin1(text);
            auto match = SearchFragment("PNG." + Latin1(data.substr(0, zero)) + ": " + text, query);
            if (!match.empty()) return match;
          }
        }
      }
      offset += size + 12;  // Skip IDAT without allocating/decoding pixel data.
    }
  } else if (signature.starts_with("II") || signature.starts_with("MM")) {
    if (length > kMetadataLimit || !ReadAt(file, 0, length, &data)) { *skipped = true; return {}; }
    return MatchExif(data, query);
  } else if (signature.starts_with("RIFF") && length >= 12) {
    uint64_t offset = 12;
    for (int chunk = 0; chunk < 4096 && offset + 8 <= length && !control.Interrupted() && base::TimeTicks::Now() < deadline; ++chunk) {
      if (!ReadAt(file, offset, 8, &header)) break;
      const auto type = header.substr(0, 4);
      uint32_t size = 0;
      for (int i = 7; i >= 4; --i) size = (size << 8) | static_cast<unsigned char>(header[i]);
      if (size > length - offset - 8) break;
      if (type == "EXIF" || type == "XMP ") {
        if (size > kMetadataLimit - metadata_bytes || !ReadAt(file, offset + 8, size, &data)) { *skipped = true; break; }
        metadata_bytes += size;
        std::string_view exif = data;
        if (exif.starts_with(std::string_view("Exif\0\0", 6))) exif.remove_prefix(6);
        auto match = type == "EXIF" ? MatchExif(exif, query) : SearchFragment("XMP: " + data, query);
        if (!match.empty()) return match;
      }
      offset += 8 + static_cast<uint64_t>(size) + (size & 1);
    }
  }
  if (base::TimeTicks::Now() >= deadline) *skipped = true;
  return {};
}
std::string PdfText(const base::FilePath& path, const std::u16string& query, const SearchControl& control, bool* skipped) {
  // Search process has one parsing sequence and no preview renderer.
  static const bool initialized = [] { FPDF_LIBRARY_CONFIG config = {}; config.version = 2; FPDF_InitLibraryWithConfig(&config); return true; }();
  (void)initialized;
  std::string bytes;
  if (!base::ReadFileToStringWithMaxSize(path, &bytes, 64 * 1024 * 1024) || control.Interrupted()) { *skipped = true; return {}; }
  ScopedFPDFDocument document(FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr));
  if (!document) { *skipped = true; return {}; }
  const auto deadline = base::TimeTicks::Now() + base::Seconds(3);
  const int pages = FPDF_GetPageCount(document.get());
  for (int index = 0; index < std::min(pages, 512) && !control.Interrupted(); ++index) {
    if (base::TimeTicks::Now() >= deadline) { *skipped = true; break; }
    ScopedFPDFPage page(FPDF_LoadPage(document.get(), index));
    if (!page) { *skipped = true; continue; }
    ScopedFPDFTextPage text(FPDFText_LoadPage(page.get()));
    if (!text) { *skipped = true; continue; }
    const int count = FPDFText_CountChars(text.get());
    if (count < 0 || count > 1024 * 1024) { *skipped = true; continue; }
    std::vector<unsigned short> buffer(static_cast<size_t>(count) + 1);
    const int read = FPDFText_GetText(text.get(), 0, count, buffer.data());
    if (read <= 0 || static_cast<size_t>(read) > buffer.size()) continue;
    std::u16string decoded;
    for (int i = 0; i < read - 1; ++i) decoded.push_back(static_cast<char16_t>(buffer[i]));
    auto match = SearchFragment(base::UTF16ToUTF8(decoded), query);
    if (!match.empty()) return "PDF " + base::NumberToString(index + 1) + ": " + match;
  }
  if (pages > 512) *skipped = true;
  return {};
}
}
std::string SearchFileContent(const base::FilePath& path, const std::u16string& query,
                             const SearchControl& control, bool* skipped) {
  if (control.Interrupted()) return {};
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) { *skipped = true; return {}; }
  std::string signature;
  const int64_t length = file.GetLength();
  if (length < 0 || !ReadAt(file, 0, std::min<int64_t>(16, length), &signature)) { *skipped = true; return {}; }
  if (signature.starts_with("%PDF-") || path.MatchesExtension(FILE_PATH_LITERAL(".pdf"))) return PdfText(path, query, control, skipped);
  if (path.MatchesExtension(FILE_PATH_LITERAL(".mp4")) || path.MatchesExtension(FILE_PATH_LITERAL(".mov")) ||
      path.MatchesExtension(FILE_PATH_LITERAL(".m4v")))
    return VideoMetadataSearch(file, query, control, skipped).Search(static_cast<uint64_t>(length));
  if (signature.starts_with("\xff\xd8") || signature.starts_with("\x89PNG") || signature.starts_with("II") || signature.starts_with("MM") ||
      (signature.starts_with("RIFF") && signature.size() >= 12 && signature.substr(8, 4) == "WEBP"))
    return ImageMetadata(file, signature, query, control, skipped);
  if (length > 16 * 1024 * 1024) { *skipped = true; return {}; }
  std::string data;
  std::array<uint8_t, 32768> chunk;
  int64_t offset = 0;
  while (!control.Interrupted()) {
    const auto read = file.Read(offset, chunk);
    if (!read) { *skipped = true; return {}; }
    if (*read == 0) break;
    if (data.size() + *read > 16 * 1024 * 1024) { *skipped = true; return {}; }
    data.append(reinterpret_cast<const char*>(chunk.data()), *read); offset += *read;
  }
  if (control.Interrupted()) return {};
  auto decoded = DecodeText(data);
  if (decoded.empty() && !data.empty()) { *skipped = true; return {}; }
  return SearchFragment(decoded, query);
}
}
