#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "formats/inflate.hpp"

namespace mapkern {

using Bytes = std::vector<uint8_t>;
using SharedBytes = std::shared_ptr<const Bytes>;

class ByteSource {
 public:
  virtual ~ByteSource() = default;
  virtual Bytes read(uint64_t offset, size_t length) = 0;
  virtual uint64_t size() = 0;
  virtual std::string name() const = 0;
};

class FileSource : public ByteSource {
 public:
  explicit FileSource(std::string path);
  ~FileSource() override;
  Bytes read(uint64_t offset, size_t length) override;
  uint64_t size() override;
  std::string name() const override { return path_; }
  bool ok() const { return file_ != nullptr; }

 private:
  std::string path_;
  std::FILE* file_ = nullptr;
  uint64_t size_ = 0;
  std::mutex mutex_;
};

class MemorySource : public ByteSource {
 public:
  explicit MemorySource(SharedBytes data, std::string name = "memory") : data_(std::move(data)), name_(std::move(name)) {}
  Bytes read(uint64_t offset, size_t length) override;
  uint64_t size() override { return data_->size(); }
  std::string name() const override { return name_; }

 private:
  SharedBytes data_;
  std::string name_;
};

inline std::filesystem::path fsPath(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

std::FILE* openFile(const std::string& path, const char* mode);
std::optional<Bytes> readFile(const std::string& path);
bool writeFileAtomic(const std::string& path, const uint8_t* data, size_t size);

}
