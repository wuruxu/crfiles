// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_text_thumbnail.h"
#include <algorithm>
#include <array>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/strings/string_util.h"
#include "base/strings/string_view_util.h"
#include "base/strings/utf_string_conversion_utils.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
#include "libcr/font_fallback_linux.h"
#endif
#include "skia/ext/font_utils.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkBitmap.h"

namespace crfiles {
namespace {
bool Stopped(const std::shared_ptr<std::atomic_bool>& canceled) {
  return canceled && canceled->load(std::memory_order_relaxed);
}
sk_sp<SkTypeface> MatchTextFallback(SkFontMgr* manager, SkUnichar character) {
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  return libcr::MatchFallbackTypeface(*manager, SkFontStyle(), character);
#else
  return manager->matchFamilyStyleCharacter(nullptr, SkFontStyle(), nullptr, 0, character);
#endif
}
using FallbackFaces = std::map<SkUnichar, sk_sp<SkTypeface>>;
// Use worker-local Skia fonts and paint, not Views/Canvas text caches shared
// with the UI. This is a small plain-text thumbnail, not a rich text renderer.
void DrawLine(SkCanvas& canvas, std::u16string_view text, const SkFont& primary,
              SkFontMgr* manager, FallbackFaces& fallbacks, float baseline, SkColor color) {
  SkPaint paint; paint.setAntiAlias(true); paint.setColor(color);
  float x = 12;
  canvas.save(); canvas.clipRect(SkRect::MakeXYWH(12, baseline - 9, 104, 11));
  for (size_t i = 0; i < text.size() && x < 116; ++i) {
    base_icu::UChar32 character;
    if (!base::ReadUnicodeCharacter(text, &i, &character)) break;
    SkFont font = primary;
    auto glyph = font.unicharToGlyph(character);
    if (!glyph) {
      auto [found, inserted] = fallbacks.try_emplace(character);
      if (inserted) {
        for (const auto& [codepoint, face] : fallbacks) {
          if (face && face->unicharToGlyph(character)) { found->second = face; break; }
        }
        if (!found->second) found->second = MatchTextFallback(manager, character);
      }
      if (found->second) { font.setTypeface(found->second); glyph = font.unicharToGlyph(character); }
      // A missing fallback is not fatal: use the primary font's replacement
      // character (or its .notdef glyph), preserving the rest of the preview.
      else glyph = font.unicharToGlyph(0xfffd);
    }
    canvas.drawSimpleText(&glyph, sizeof(glyph), SkTextEncoding::kGlyphID, x, baseline, font, paint);
    x += std::max(1.f, font.getWidth(glyph));
  }
  canvas.restore();
}
std::optional<std::u16string> DecodeText(std::string bytes, bool truncated) {
  std::u16string text;
  const bool little = bytes.starts_with("\xff\xfe");
  const bool big = bytes.starts_with("\xfe\xff");
  if (little || big) {
    if (bytes.size() % 2 && !truncated) return std::nullopt;
    for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
      const auto first = static_cast<unsigned char>(bytes[i]);
      const auto second = static_cast<unsigned char>(bytes[i + 1]);
      text.push_back(static_cast<char16_t>(little ? first | (second << 8) : (first << 8) | second));
    }
    if (truncated && !text.empty() && text.back() >= 0xd800 && text.back() <= 0xdbff) text.pop_back();
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] >= 0xd800 && text[i] <= 0xdbff) {
        if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff) return std::nullopt;
      } else if (text[i] >= 0xdc00 && text[i] <= 0xdfff) return std::nullopt;
    }
  } else {
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.erase(0, 3);
    // A bounded read may end in the middle of a UTF-8 code point. Only trim
    // the incomplete suffix; malformed input anywhere else remains a failure.
    for (int trim = 0; truncated && trim < 3 && !bytes.empty() && !base::IsStringUTF8(bytes); ++trim) bytes.pop_back();
    if (!base::IsStringUTF8(bytes)) return std::nullopt;
    text = base::UTF8ToUTF16(bytes);
  }
  for (char16_t c : text)
    if ((c < 0x20 && c != u'\t' && c != u'\r' && c != u'\n') || (c >= 0x7f && c <= 0x9f)) return std::nullopt;
  return text;
}
}
bool IsTextThumbnailPath(const base::FilePath& path) {
  if (base::EqualsCaseInsensitiveASCII(path.BaseName().AsUTF8Unsafe(), "Makefile")) return true;
  constexpr std::array extensions = {FILE_PATH_LITERAL(".conf"), FILE_PATH_LITERAL(".md"),
      FILE_PATH_LITERAL(".txt"), FILE_PATH_LITERAL(".bat"), FILE_PATH_LITERAL(".sh"),
      FILE_PATH_LITERAL(".ini"), FILE_PATH_LITERAL(".json"), FILE_PATH_LITERAL(".yaml"),
      FILE_PATH_LITERAL(".yml"), FILE_PATH_LITERAL(".log"), FILE_PATH_LITERAL(".cfg"),
      FILE_PATH_LITERAL(".toml"), FILE_PATH_LITERAL(".csv"), FILE_PATH_LITERAL(".xml"),
      FILE_PATH_LITERAL(".markdown"), FILE_PATH_LITERAL(".bash"), FILE_PATH_LITERAL(".zsh"),
      FILE_PATH_LITERAL(".cmd"), FILE_PATH_LITERAL(".ps1"), FILE_PATH_LITERAL(".patch"),
      FILE_PATH_LITERAL(".cc"), FILE_PATH_LITERAL(".h"), FILE_PATH_LITERAL(".desktop"),
      FILE_PATH_LITERAL(".cpp"), FILE_PATH_LITERAL(".c")};
  return std::ranges::any_of(extensions, [&](const auto* extension) { return path.MatchesExtension(extension); });
}
gfx::ImageSkia LoadTextThumbnail(const base::FilePath& path, int edge,
    const std::shared_ptr<std::atomic_bool>& canceled) {
  if (edge <= 0 || edge > 128 || Stopped(canceled)) return {};
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  base::File::Info info;
  if (!file.IsValid() || !file.GetInfo(&info) || info.is_directory) return {};
  std::vector<uint8_t> buffer(16 * 1024);
  const auto count = file.ReadAtCurrentPos(buffer);
  if (!count || Stopped(canceled)) return {};
  const auto data = base::as_string_view(base::span(buffer).first(*count));
  const auto text = DecodeText(std::string(data), info.size > static_cast<int64_t>(*count));
  if (!text || Stopped(canceled)) return {};
  SkBitmap bitmap;
  if (!bitmap.tryAllocN32Pixels(edge, edge)) return {};
  bitmap.eraseColor(SK_ColorTRANSPARENT);
  SkCanvas canvas(bitmap);
  canvas.scale(edge / 128.f, edge / 128.f);
  SkPaint paper; paper.setAntiAlias(true); paper.setColor(SkColorSetRGB(250, 250, 247));
  canvas.drawRoundRect(SkRect::MakeXYWH(5, 2, 118, 124), 6, 6, paper);
  paper.setStyle(SkPaint::kStroke_Style); paper.setStrokeWidth(1); paper.setColor(SkColorSetRGB(185, 189, 195));
  canvas.drawRoundRect(SkRect::MakeXYWH(5, 2, 118, 124), 6, 6, paper);
  const auto manager = skia::DefaultFontMgr();
  if (!manager) return {};
#if BUILDFLAG(IS_WIN)
  auto typeface = manager->matchFamilyStyle("Consolas", SkFontStyle());
#else
  auto typeface = manager->matchFamilyStyle("monospace", SkFontStyle());
#endif
  if (!typeface) typeface = skia::DefaultTypeface();
  if (!typeface) return {};
  const SkFont font(std::move(typeface), 9);
  FallbackFaces fallbacks;
  size_t position = 0;
  for (int row = 0; row < 5 && position < text->size(); ++row) {
    if (Stopped(canceled)) return {};
    auto end = text->find_first_of(u"\r\n", position);
    if (end == std::u16string::npos) end = text->size();
    std::u16string line;
    for (size_t i = position; i < end && line.size() < 128; ++i) {
      if ((*text)[i] == u'\t') line.append(4 - line.size() % 4, u' ');
      else line.push_back((*text)[i]);
    }
    if (!line.empty() && line.back() >= 0xd800 && line.back() <= 0xdbff) line.pop_back();
    DrawLine(canvas, line, font, manager.get(), fallbacks, 16 + row * 10, SkColorSetRGB(45, 49, 55));
    position = end;
    if (position < text->size() && (*text)[position++] == u'\r' && position < text->size() && (*text)[position] == u'\n') ++position;
  }
  if (Stopped(canceled)) return {};
  const auto label = base::ToUpperASCII(base::FilePath(path.Extension()).AsUTF16Unsafe());
  DrawLine(canvas, label, font, manager.get(), fallbacks, 121, SkColorSetRGB(102, 107, 115));
  bitmap.setImmutable();
  auto image = gfx::ImageSkia::CreateFromBitmap(bitmap, 1.f);
  image.MakeThreadSafe(); return image;
}
}
