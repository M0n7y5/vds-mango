// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "unique_fd.hh"
#include "vds_fs.hh"

namespace {

constexpr mode_t kVdsFileMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
constexpr mode_t kLockFileMode = S_IRUSR | S_IWUSR;
constexpr int kDbLockRetryBudgetMs = 100;
constexpr int kDbLockRetrySleepMs = 5;

std::filesystem::path db_directory(const std::filesystem::path &db_path) {
  const std::filesystem::path directory = db_path.parent_path();
  if (directory.empty()) {
    return ".";
  }
  return directory;
}

void chmod_db_file_if_exists(const std::filesystem::path &db_path) {
  if (::chmod(db_path.c_str(), kVdsFileMode) < 0 && errno != ENOENT) {
    throw std::runtime_error("failed to chmod " + db_path.string() + ": " +
                             std::strerror(errno));
  }
}

class LinuxFileLock final : public vds::FileLock {
public:
  explicit LinuxFileLock(vds::UniqueFd fd) : fd_(std::move(fd)) {}

private:
  vds::UniqueFd fd_;
};

} // namespace

namespace vds {

void ensure_parent_directory(const std::filesystem::path &path) {
  const std::filesystem::path directory = path.parent_path();
  if (!directory.empty()) {
    std::filesystem::create_directories(directory);
  }
}

std::unique_ptr<FileLock> lock_file(const std::filesystem::path &path) {
  /*
   * flock needs only an open descriptor, so the lock is a file nobody else
   * can open: on the directory any user could hold it and keep vdsd down.
   */
  std::filesystem::path lock = path;
  lock += ".lock";
  UniqueFd fd(::open(lock.c_str(),
                     O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                     kLockFileMode));
  if (!fd || ::fchmod(fd.get(), kLockFileMode) < 0) {
    throw std::runtime_error("failed to open DB lock " + lock.string() +
                             ": " + std::strerror(errno));
  }
  // Bound the wait so a stuck locker cannot stall the epoll loop.
  int waited_ms = 0;
  while (::flock(fd.get(), LOCK_EX | LOCK_NB) < 0) {
    if (errno == EINTR) {
      continue;
    }
    if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
        waited_ms < kDbLockRetryBudgetMs) {
      timespec sleep_for{};
      sleep_for.tv_nsec =
          static_cast<long>(kDbLockRetrySleepMs) * 1000000L;
      if (::nanosleep(&sleep_for, nullptr) < 0 && errno != EINTR) {
        throw std::runtime_error("failed to lock " + lock.string() + ": " +
                                 std::strerror(errno));
      }
      waited_ms += kDbLockRetrySleepMs;
      continue;
    }
    throw std::runtime_error("failed to lock " + lock.string() + ": " +
                             std::strerror(errno));
  }
  chmod_db_file_if_exists(path);
  return std::make_unique<LinuxFileLock>(std::move(fd));
}

std::string read_file_if_exists(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    if (!error) {
      return {};
    }
    throw std::runtime_error("failed to stat " + path.string() + ": " +
                             error.message());
  }

  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("failed to open " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

void write_file_atomic(const std::filesystem::path &path,
                       std::string_view text) {
  const std::filesystem::path tmp_path = path.string() + ".tmp";
  {
    UniqueFd fd(::open(tmp_path.c_str(),
                       O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kVdsFileMode));
    if (!fd) {
      throw std::runtime_error("failed to open " + tmp_path.string() + ": " +
                               std::strerror(errno));
    }
    if (::fchmod(fd.get(), kVdsFileMode) < 0) {
      throw std::runtime_error("failed to chmod " + tmp_path.string() + ": " +
                               std::strerror(errno));
    }
    const char *data = text.data();
    std::size_t size = text.size();
    while (size > 0) {
      const ssize_t written = ::write(fd.get(), data, size);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error("failed to write " + tmp_path.string() + ": " +
                                 std::strerror(errno));
      }
      if (written == 0) {
        throw std::runtime_error("write returned zero for " +
                                 tmp_path.string());
      }
      data += written;
      size -= static_cast<std::size_t>(written);
    }
    if (::fsync(fd.get()) < 0) {
      throw std::runtime_error("failed to fsync " + tmp_path.string() + ": " +
                               std::strerror(errno));
    }
  }
  if (::rename(tmp_path.c_str(), path.c_str()) < 0) {
    throw std::runtime_error("failed to rename " + tmp_path.string() + " to " +
                             path.string() + ": " + std::strerror(errno));
  }
  const std::filesystem::path directory = db_directory(path);
  const UniqueFd directory_fd(
      ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!directory_fd) {
    throw std::runtime_error(std::string("failed to open DB directory for "
                                         "fsync: ") +
                             std::strerror(errno));
  }
  if (::fsync(directory_fd.get()) < 0) {
    throw std::runtime_error(std::string("failed to fsync DB directory: ") +
                             std::strerror(errno));
  }
}

} // namespace vds
