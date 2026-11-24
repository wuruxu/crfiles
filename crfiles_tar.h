// Copyright 2026 The Chromium Authors
#ifndef CRFILES_TAR_H_
#define CRFILES_TAR_H_
#include <cstdint>
#include <string_view>
#include "base/files/file.h"
#include "base/files/file_path.h"
namespace crfiles {
class TarOutput {
 public:
  virtual ~TarOutput() = default;
  virtual base::FilePath Entry(const base::FilePath& relative, bool directory) = 0;
  virtual bool AddBytes(uint64_t bytes) = 0;
  // A visitor can leave file invalid to consume this entry without writing it.
  virtual bool OpenFile(const base::FilePath& relative, uint64_t size, base::File& file);
  virtual bool Link(const base::FilePath& relative, const base::FilePath& target, bool symbolic) = 0;
  virtual bool FinishLinks() = 0;
};
// Runs only in the archive helper. Links are created after all regular files.
bool ExtractTar(base::File input, std::string_view format, TarOutput& output);
bool ExtractZstdFile(base::File input, const base::FilePath& name, TarOutput& output);
}
#endif
