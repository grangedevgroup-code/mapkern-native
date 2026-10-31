#include "formats/bytes.hpp"

#include <filesystem>
#include <optional>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mapkern {

std::FILE* openFile(const std::string& path, const char* mode) {
#ifdef _WIN32
  const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  std::wstring wide(static_cast<size_t>(n > 0 ? n : 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), n);
  std::wstring wmode;
  for (const char* m = mode; *m; m++) wmode += static_cast<wchar_t>(*m);
  return _wfopen(wide.c_str(), wmode.c_str());
#else
  return std::fopen(path.c_str(), mode);
#endif
}

static int seek64(std::FILE* f, uint64_t offset) {
#ifdef _WIN32
  return _fseeki64(f, static_cast<int64_t>(offset), SEEK_SET);
#else
  return fseeko(f, static_cast<off_t>(offset), SEEK_SET);
#endif
}

FileSource::FileSource(std::string path) : path_(std::move(path)) {
  file_ = openFile(path_, "rb");
  if (file_) {
#ifdef _WIN32
    _fseeki64(file_, 0, SEEK_END);
    size_ = static_cast<uint64_t>(_ftelli64(file_));
#else
    fseeko(file_, 0, SEEK_END);
    size_ = static_cast<uint64_t>(ftello(file_));
#endif
  }
}

FileSource::~FileSource() {
  if (file_) std::fclose(file_);
}

Bytes FileSource::read(uint64_t offset, size_t length) {
  std::lock_guard lock(mutex_);
  if (!file_) throw FormatError("Could not open " + path_);
  if (offset >= size_) return {};
  length = static_cast<size_t>(std::min<uint64_t>(length, size_ - offset));
  Bytes out(length);
  if (seek64(file_, offset) != 0) throw FormatError("Could not seek in " + path_);
  const size_t got = std::fread(out.data(), 1, length, file_);
  out.resize(got);
  return out;
}

uint64_t FileSource::size() { return size_; }

Bytes MemorySource::read(uint64_t offset, size_t length) {
  if (offset >= data_->size()) return {};
  const size_t n = static_cast<size_t>(std::min<uint64_t>(length, data_->size() - offset));
  return Bytes(data_->begin() + static_cast<ptrdiff_t>(offset), data_->begin() + static_cast<ptrdiff_t>(offset + n));
}

std::optional<Bytes> readFile(const std::string& path) {
  std::FILE* f = openFile(path, "rb");
  if (!f) return std::nullopt;
  Bytes out;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}

bool writeFileAtomic(const std::string& path, const uint8_t* data, size_t size) {
  const std::string tmp = path + ".tmp";
  std::FILE* f = openFile(tmp, "wb");
  if (!f) return false;
  const bool ok = std::fwrite(data, 1, size, f) == size;
  std::fclose(f);
  if (!ok) {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(tmp), ec);
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(path), ec);
  if (ec) {
    std::filesystem::remove(std::filesystem::u8path(path), ec);
    std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(path), ec);
  }
  return !ec;
}

}
