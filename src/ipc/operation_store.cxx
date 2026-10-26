#include "src/ipc/operation_store.hxx"
#include "src/crypto/blake3.hxx"
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace gravesentinel::ipc {
namespace {
constexpr std::size_t digest_hex_size = 64;

class UniqueFd final {
public:
  explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
  ~UniqueFd() {
    if (fd_ >= 0)
      (void)::close(fd_);
  }
  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;
  UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {}
  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0)
        (void)::close(fd_);
      fd_ = other.release();
    }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] int release() noexcept {
    const int result = fd_;
    fd_ = -1;
    return result;
  }

private:
  int fd_;
};

struct ParsedRecord {
  OperationView view;
  std::string digest;
};

StoreError make_error(StoreErrorCode code, std::string message,
                      bool indeterminate = false) {
  return StoreError(code, std::move(message), indeterminate);
}

StoreError system_error(StoreErrorCode code, std::string_view action,
                        int error_number) {
  return make_error(
      code,
      std::string(action) + ": " +
          std::system_error(error_number, std::generic_category()).what());
}

std::string digest_of(const std::string &value) {
  const auto bytes = std::as_bytes(std::span(value.data(), value.size()));
  const auto digest = gravesentinel::ccm::blake3_hash(bytes);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const auto byte : digest.bytes())
    out << std::setw(2) << std::to_integer<unsigned>(byte);
  return out.str();
}

std::string make_id(const std::string &request_id) {
  return "op-" + digest_of(request_id);
}

bool terminal(OperationState state) {
  return state == OperationState::succeeded ||
         state == OperationState::failed ||
         state == OperationState::indeterminate;
}

std::expected<OperationState, StoreError> parse_state(std::string_view value) {
  if (value == "queued")
    return OperationState::queued;
  if (value == "running")
    return OperationState::running;
  if (value == "succeeded")
    return OperationState::succeeded;
  if (value == "failed")
    return OperationState::failed;
  if (value == "indeterminate")
    return OperationState::indeterminate;
  return std::unexpected(make_error(StoreErrorCode::journal_corrupt,
                                    "journal contains an unknown state"));
}

std::expected<StoreErrorCode, StoreError>
parse_error_code(std::string_view value) {
  if (value == "none")
    return StoreErrorCode::none;
  if (value == "invalid_argument")
    return StoreErrorCode::invalid_argument;
  if (value == "not_found")
    return StoreErrorCode::not_found;
  if (value == "already_exists")
    return StoreErrorCode::already_exists;
  if (value == "conflict")
    return StoreErrorCode::conflict;
  if (value == "resource_exhausted")
    return StoreErrorCode::resource_exhausted;
  if (value == "permission_denied")
    return StoreErrorCode::permission_denied;
  if (value == "capture_failed")
    return StoreErrorCode::capture_failed;
  if (value == "storage_error")
    return StoreErrorCode::storage_error;
  if (value == "corrupted_snapshot")
    return StoreErrorCode::corrupted_snapshot;
  if (value == "incompatible_runtime")
    return StoreErrorCode::incompatible_runtime;
  if (value == "restore_failed")
    return StoreErrorCode::restore_failed;
  if (value == "cold_start_failed")
    return StoreErrorCode::cold_start_failed;
  if (value == "deadline_exceeded")
    return StoreErrorCode::deadline_exceeded;
  if (value == "unavailable")
    return StoreErrorCode::unavailable;
  if (value == "journal_io")
    return StoreErrorCode::journal_io;
  if (value == "journal_corrupt")
    return StoreErrorCode::journal_corrupt;
  if (value == "executor_failed")
    return StoreErrorCode::executor_failed;
  if (value == "execution_indeterminate")
    return StoreErrorCode::execution_indeterminate;
  if (value == "interrupted")
    return StoreErrorCode::interrupted;
  if (value == "internal")
    return StoreErrorCode::internal;
  return std::unexpected(
      make_error(StoreErrorCode::journal_corrupt,
                 "journal contains an unknown stable error code"));
}

bool valid_digest(std::string_view value) {
  return value.size() == digest_hex_size &&
         std::ranges::all_of(value, [](char byte) {
           return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
         });
}

bool valid_json_string(std::string_view value) {
  try {
    (void)nlohmann::json(value).dump();
    return true;
  } catch (...) {
    return false;
  }
}

bool valid_method(std::string_view method) {
  return method == "SubmitCreateCheckpoint" ||
         method == "SubmitRestoreOrRestart" || method == "SubmitStopContainer";
}

std::expected<void, StoreError>
validate_identifier(std::string_view value, std::string_view field,
                    const OperationStoreOptions &options) {
  if (value.empty())
    return std::unexpected(
        make_error(StoreErrorCode::invalid_argument,
                   std::string(field) + " must not be empty"));
  if (value.size() > options.max_identifier_bytes)
    return std::unexpected(
        make_error(StoreErrorCode::invalid_argument,
                   std::string(field) + " exceeds the configured size limit"));
  if (value.find('\0') != std::string_view::npos || !valid_json_string(value))
    return std::unexpected(
        make_error(StoreErrorCode::invalid_argument,
                   std::string(field) + " is not a valid UTF-8 identifier"));
  return {};
}

std::expected<void, StoreError>
validate_options(std::string_view path, const OperationStoreOptions &options) {
  if (path.empty() || path.ends_with('/') || path == "." || path == "..")
    return std::unexpected(make_error(StoreErrorCode::invalid_argument,
                                      "journal path is invalid"));
  const auto slash = path.find_last_of('/');
  const auto basename =
      slash == std::string_view::npos ? path : path.substr(slash + 1);
  if (basename.empty() || basename == "." || basename == "..")
    return std::unexpected(make_error(StoreErrorCode::invalid_argument,
                                      "journal basename is invalid"));
  if (options.max_in_flight == 0 || options.max_records == 0 ||
      options.worker_count == 0 || options.worker_count > 64 ||
      options.max_identifier_bytes == 0 || options.max_request_bytes == 0 ||
      options.max_result_bytes == 0 || options.max_journal_bytes == 0 ||
      options.max_compaction_scan_entries == 0 ||
      options.max_compaction_cleanup_files == 0 ||
      options.max_compaction_name_attempts == 0)
    return std::unexpected(
        make_error(StoreErrorCode::invalid_argument,
                   "operation store limits must be nonzero"));
  if (options.max_journal_entries <= options.max_records)
    return std::unexpected(
        make_error(StoreErrorCode::invalid_argument,
                   "max_journal_entries must be greater than max_records"));
  return {};
}

OperationStoreOptions options_for_queue_limit(std::size_t queue_limit) {
  OperationStoreOptions options;
  options.max_in_flight = queue_limit;
  return options;
}

std::string parent_path(std::string_view path) {
  const auto slash = path.find_last_of('/');
  if (slash == std::string_view::npos)
    return ".";
  if (slash == 0)
    return "/";
  return std::string(path.substr(0, slash));
}

std::string basename(std::string_view path) {
  const auto slash = path.find_last_of('/');
  return std::string(slash == std::string_view::npos ? path
                                                     : path.substr(slash + 1));
}

bool decimal_component(std::string_view value) {
  return !value.empty() && std::ranges::all_of(value, [](const char byte) {
    return byte >= '0' && byte <= '9';
  });
}

bool stale_compaction_name(std::string_view name,
                           std::string_view journal_basename) {
  const auto prefix = std::string(journal_basename) + ".compact.";
  if (!name.starts_with(prefix))
    return false;
  const auto suffix = name.substr(prefix.size());
  const auto separator = suffix.find('.');
  return separator != std::string_view::npos &&
         suffix.find('.', separator + 1) == std::string_view::npos &&
         decimal_component(suffix.substr(0, separator)) &&
         decimal_component(suffix.substr(separator + 1));
}

std::expected<void, StoreError> close_checked(UniqueFd &fd,
                                              std::string_view description) {
  const int raw_fd = fd.release();
  if (raw_fd < 0)
    return {};
  if (::close(raw_fd) != 0)
    return std::unexpected(
        system_error(StoreErrorCode::journal_io, description, errno));
  return {};
}

std::expected<struct stat, StoreError> validate_regular_file(int fd) {
  struct stat status{};
  if (::fstat(fd, &status) != 0)
    return std::unexpected(system_error(
        StoreErrorCode::journal_io, "cannot stat operation journal", errno));
  if (!S_ISREG(status.st_mode))
    return std::unexpected(
        make_error(StoreErrorCode::journal_corrupt,
                   "operation journal is not a regular file"));
  if (status.st_uid != ::geteuid())
    return std::unexpected(make_error(StoreErrorCode::journal_corrupt,
                                      "operation journal owner is unsafe"));
  if (status.st_nlink != 1)
    return std::unexpected(
        make_error(StoreErrorCode::journal_corrupt,
                   "operation journal must have exactly one hard link"));
  if ((status.st_mode & static_cast<mode_t>(0777)) != static_cast<mode_t>(0600))
    return std::unexpected(make_error(StoreErrorCode::journal_corrupt,
                                      "operation journal mode must be 0600"));
  return status;
}

std::expected<void, StoreError> fsync_checked(int fd,
                                              std::string_view description) {
  for (;;) {
    if (::fsync(fd) == 0)
      return {};
    if (errno != EINTR)
      return std::unexpected(
          system_error(StoreErrorCode::journal_io, description, errno));
  }
}

std::expected<void, StoreError> fsync_parent(std::string_view path) {
  const auto parent = parent_path(path);
  UniqueFd directory(
      ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (directory.get() < 0)
    return std::unexpected(system_error(StoreErrorCode::journal_io,
                                        "cannot open journal parent directory",
                                        errno));
  struct stat status{};
  if (::fstat(directory.get(), &status) != 0)
    return std::unexpected(system_error(StoreErrorCode::journal_io,
                                        "cannot stat journal parent directory",
                                        errno));
  if (!S_ISDIR(status.st_mode))
    return std::unexpected(make_error(StoreErrorCode::journal_corrupt,
                                      "journal parent is not a directory"));
  return fsync_checked(directory.get(),
                       "cannot fsync journal parent directory");
}

std::expected<void, StoreError>
cleanup_stale_compaction_files(std::string_view journal_path,
                               const OperationStoreOptions &options) {
  const auto parent = parent_path(journal_path);
  const auto journal_basename = basename(journal_path);
  UniqueFd directory(
      ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (directory.get() < 0) {
    return std::unexpected(system_error(StoreErrorCode::journal_io,
                                        "cannot open journal parent directory",
                                        errno));
  }

  struct stat directory_status{};
  if (::fstat(directory.get(), &directory_status) != 0) {
    return std::unexpected(system_error(StoreErrorCode::journal_io,
                                        "cannot stat journal parent directory",
                                        errno));
  }
  if (!S_ISDIR(directory_status.st_mode)) {
    return std::unexpected(make_error(StoreErrorCode::journal_corrupt,
                                      "journal parent is not a directory"));
  }

  const int listing_fd = ::fcntl(directory.get(), F_DUPFD_CLOEXEC, 0);
  if (listing_fd < 0) {
    return std::unexpected(system_error(
        StoreErrorCode::journal_io,
        "cannot duplicate journal parent for compaction cleanup", errno));
  }
  DIR *const listing = ::fdopendir(listing_fd);
  if (listing == nullptr) {
    const int open_error = errno;
    (void)::close(listing_fd);
    return std::unexpected(system_error(
        StoreErrorCode::journal_io,
        "cannot list journal parent for compaction cleanup", open_error));
  }

  std::size_t scanned = 0;
  std::size_t removed = 0;
  std::optional<StoreError> failure;
  for (;;) {
    errno = 0;
    const dirent *const entry = ::readdir(listing);
    if (entry == nullptr) {
      if (errno != 0) {
        failure = system_error(StoreErrorCode::journal_io,
                               "cannot read journal parent directory", errno);
      }
      break;
    }

    const std::string_view name(entry->d_name);
    if (name == "." || name == "..")
      continue;
    if (scanned == options.max_compaction_scan_entries) {
      failure = make_error(StoreErrorCode::resource_exhausted,
                           "journal parent exceeds the configured compaction "
                           "cleanup scan limit");
      break;
    }
    ++scanned;
    if (!stale_compaction_name(name, journal_basename))
      continue;

    struct stat status{};
    if (::fstatat(directory.get(), entry->d_name, &status,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT)
        continue;
      failure = system_error(StoreErrorCode::journal_io,
                             "cannot inspect stale compacted journal", errno);
      break;
    }
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
        status.st_nlink != 1 ||
        (status.st_mode & static_cast<mode_t>(0777)) !=
            static_cast<mode_t>(0600)) {
      continue;
    }
    if (removed == options.max_compaction_cleanup_files) {
      failure = make_error(
          StoreErrorCode::resource_exhausted,
          "stale compacted journals exceed the configured cleanup limit");
      break;
    }

    for (;;) {
      if (::unlinkat(directory.get(), entry->d_name, 0) == 0) {
        ++removed;
        break;
      }
      if (errno == EINTR)
        continue;
      if (errno == ENOENT)
        break;
      failure = system_error(StoreErrorCode::journal_io,
                             "cannot remove stale compacted journal", errno);
      break;
    }
    if (failure)
      break;
  }

  const int close_error = ::closedir(listing);
  if (close_error != 0 && !failure) {
    failure =
        system_error(StoreErrorCode::journal_io,
                     "cannot close journal parent directory listing", errno);
  }
  if (removed != 0) {
    if (auto synced = fsync_checked(
            directory.get(),
            "cannot fsync journal parent after compaction cleanup");
        !synced) {
      return synced;
    }
  }
  if (failure)
    return std::unexpected(std::move(*failure));
  return {};
}

std::expected<void, StoreError> write_all(int fd, std::string_view data,
                                          std::string_view description) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const auto remaining = data.size() - offset;
    const auto chunk =
        std::min(remaining,
                 static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const auto written = ::write(fd, data.data() + offset, chunk);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return std::unexpected(
          system_error(StoreErrorCode::journal_io, description, errno));
    }
    if (written == 0)
      return std::unexpected(
          make_error(StoreErrorCode::journal_io,
                     std::string(description) + ": write returned zero"));
    offset += static_cast<std::size_t>(written);
  }
  return {};
}

std::expected<void, StoreError> truncate_checked(int fd, std::size_t length) {
  if (length > static_cast<std::size_t>(std::numeric_limits<off_t>::max()))
    return std::unexpected(make_error(StoreErrorCode::journal_corrupt,
                                      "journal length is not representable"));
  for (;;) {
    if (::ftruncate(fd, static_cast<off_t>(length)) == 0)
      return {};
    if (errno != EINTR)
      return std::unexpected(system_error(StoreErrorCode::journal_io,
                                          "cannot truncate torn journal tail",
                                          errno));
  }
}

std::expected<std::string, StoreError> record_line(const OperationView &view,
                                                   const std::string &digest) {
  try {
    nlohmann::json record{{"request_id", view.request_id},
                          {"operation_id", view.operation_id},
                          {"method", view.method},
                          {"container_id", view.container_id},
                          {"state", state_name(view.state)},
                          {"digest", digest},
                          {"result", view.result},
                          {"error_code", error_code_name(view.error_code)},
                          {"indeterminate", view.indeterminate}};
    return record.dump() + "\n";
  } catch (const std::exception &error) {
    return std::unexpected(
        make_error(StoreErrorCode::internal,
                   std::string("cannot serialize operation journal record: ") +
                       error.what()));
  }
}

bool has_base_schema(const nlohmann::json &record) {
  return record.contains("request_id") && record.contains("operation_id") &&
         record.contains("method") && record.contains("container_id") &&
         record.contains("state") && record.contains("digest") &&
         record.contains("result");
}

std::expected<ParsedRecord, StoreError>
parse_record(std::string_view line, const OperationStoreOptions &options) {
  try {
    const auto value = nlohmann::json::parse(line);
    if (!value.is_object() || !has_base_schema(value) ||
        (value.size() != 7 && value.size() != 9))
      return std::unexpected(
          make_error(StoreErrorCode::journal_corrupt,
                     "journal record does not match an accepted exact schema"));
    const bool structured = value.size() == 9;
    if (structured &&
        (!value.contains("error_code") || !value.contains("indeterminate")))
      return std::unexpected(
          make_error(StoreErrorCode::journal_corrupt,
                     "journal record does not match the structured schema"));
    if (!value.at("request_id").is_string() ||
        !value.at("operation_id").is_string() ||
        !value.at("method").is_string() ||
        !value.at("container_id").is_string() ||
        !value.at("state").is_string() || !value.at("digest").is_string() ||
        !value.at("result").is_string() ||
        (structured && (!value.at("error_code").is_string() ||
                        !value.at("indeterminate").is_boolean())))
      return std::unexpected(
          make_error(StoreErrorCode::journal_corrupt,
                     "journal record contains a value with the wrong type"));

    OperationView view;
    view.request_id = value.at("request_id").get<std::string>();
    view.operation_id = value.at("operation_id").get<std::string>();
    view.method = value.at("method").get<std::string>();
    view.container_id = value.at("container_id").get<std::string>();
    const auto state =
        parse_state(value.at("state").get_ref<const std::string &>());
    if (!state)
      return std::unexpected(state.error());
    view.state = *state;
    view.result = value.at("result").get<std::string>();
    const auto digest = value.at("digest").get<std::string>();

    if (auto valid =
            validate_identifier(view.request_id, "request_id", options);
        !valid)
      return std::unexpected(
          make_error(StoreErrorCode::journal_corrupt, valid.error().message));
    if (auto valid =
            validate_identifier(view.container_id, "container_id", options);
        !valid)
      return std::unexpected(
          make_error(StoreErrorCode::journal_corrupt, valid.error().message));
    if (!valid_method(view.method))
      return std::unexpected(make_error(StoreErrorCode::journal_corrupt,
                                        "journal method is not supported"));
    if (!valid_digest(digest))
      return std::unexpected(make_error(StoreErrorCode::journal_corrupt,
                                        "journal digest is invalid"));
    if (view.operation_id != make_id(view.request_id))
      return std::unexpected(
          make_error(StoreErrorCode::journal_corrupt,
                     "journal operation ID does not match its request ID"));
    if (view.result.size() > options.max_result_bytes ||
        !valid_json_string(view.result))
      return std::unexpected(make_error(StoreErrorCode::journal_corrupt,
                                        "journal result is invalid"));

    if (structured) {
      const auto parsed_code = parse_error_code(
          value.at("error_code").get_ref<const std::string &>());
      if (!parsed_code)
        return std::unexpected(parsed_code.error());
      view.error_code = *parsed_code;
      view.indeterminate = value.at("indeterminate").get<bool>();
    } else if (view.state == OperationState::failed) {
      view.error_code = StoreErrorCode::executor_failed;
    } else if (view.state == OperationState::indeterminate) {
      view.error_code = StoreErrorCode::execution_indeterminate;
      view.indeterminate = true;
    }

    if ((view.state == OperationState::queued ||
         view.state == OperationState::running) &&
        (!view.result.empty() || view.error_code != StoreErrorCode::none ||
         view.indeterminate))
      return std::unexpected(
          make_error(StoreErrorCode::journal_corrupt,
                     "nonterminal journal state contains terminal error data"));
    if (view.state == OperationState::succeeded &&
        (view.error_code != StoreErrorCode::none || view.indeterminate))
      return std::unexpected(
          make_error(StoreErrorCode::journal_corrupt,
                     "successful journal state contains error data"));
    if (view.state == OperationState::failed &&
        (view.error_code == StoreErrorCode::none || view.indeterminate))
      return std::unexpected(
          make_error(StoreErrorCode::journal_corrupt,
                     "failed journal state has inconsistent error data"));
    if (view.state == OperationState::indeterminate &&
        (view.error_code == StoreErrorCode::none || !view.indeterminate))
      return std::unexpected(make_error(
          StoreErrorCode::journal_corrupt,
          "indeterminate journal state has inconsistent error data"));

    return ParsedRecord{std::move(view), digest};
  } catch (const std::exception &error) {
    return std::unexpected(make_error(
        StoreErrorCode::journal_corrupt,
        std::string("cannot parse operation journal record: ") + error.what()));
  }
}

bool same_identity(const ParsedRecord &left, const ParsedRecord &right) {
  return left.view.operation_id == right.view.operation_id &&
         left.view.request_id == right.view.request_id &&
         left.view.method == right.view.method &&
         left.view.container_id == right.view.container_id &&
         left.digest == right.digest;
}

bool valid_transition(const OperationView &previous,
                      const OperationView &next) {
  if (previous.state == next.state)
    return previous == next;
  if (previous.state == OperationState::queued)
    return next.state == OperationState::running;
  if (previous.state == OperationState::running)
    return terminal(next.state);
  return false;
}

std::expected<void, StoreError> rename_checked(const std::string &source,
                                               const std::string &destination) {
  for (;;) {
    if (::rename(source.c_str(), destination.c_str()) == 0)
      return {};
    if (errno != EINTR)
      return std::unexpected(system_error(StoreErrorCode::journal_io,
                                          "cannot install compacted journal",
                                          errno));
  }
}
} // namespace

const char *state_name(OperationState state) noexcept {
  switch (state) {
  case OperationState::queued:
    return "queued";
  case OperationState::running:
    return "running";
  case OperationState::succeeded:
    return "succeeded";
  case OperationState::failed:
    return "failed";
  case OperationState::indeterminate:
    return "indeterminate";
  }
  return "failed";
}

const char *error_code_name(StoreErrorCode code) noexcept {
  switch (code) {
  case StoreErrorCode::none:
    return "none";
  case StoreErrorCode::invalid_argument:
    return "invalid_argument";
  case StoreErrorCode::not_found:
    return "not_found";
  case StoreErrorCode::already_exists:
    return "already_exists";
  case StoreErrorCode::conflict:
    return "conflict";
  case StoreErrorCode::resource_exhausted:
    return "resource_exhausted";
  case StoreErrorCode::permission_denied:
    return "permission_denied";
  case StoreErrorCode::capture_failed:
    return "capture_failed";
  case StoreErrorCode::storage_error:
    return "storage_error";
  case StoreErrorCode::corrupted_snapshot:
    return "corrupted_snapshot";
  case StoreErrorCode::incompatible_runtime:
    return "incompatible_runtime";
  case StoreErrorCode::restore_failed:
    return "restore_failed";
  case StoreErrorCode::cold_start_failed:
    return "cold_start_failed";
  case StoreErrorCode::deadline_exceeded:
    return "deadline_exceeded";
  case StoreErrorCode::unavailable:
    return "unavailable";
  case StoreErrorCode::journal_io:
    return "journal_io";
  case StoreErrorCode::journal_corrupt:
    return "journal_corrupt";
  case StoreErrorCode::executor_failed:
    return "executor_failed";
  case StoreErrorCode::execution_indeterminate:
    return "execution_indeterminate";
  case StoreErrorCode::interrupted:
    return "interrupted";
  case StoreErrorCode::internal:
    return "internal";
  }
  return "internal";
}

OperationStore::OperationStore(std::string path, Executor executor,
                               std::size_t queue_limit)
    : OperationStore(std::move(path), std::move(executor),
                     options_for_queue_limit(queue_limit), NoThrowTag{}) {
  if (startup_error_)
    throw std::runtime_error(
        std::string(error_code_name(startup_error_->code)) + ": " +
        startup_error_->message);
}

OperationStore::OperationStore(std::string path, Executor executor,
                               OperationStoreOptions options)
    : OperationStore(std::move(path), std::move(executor), options,
                     NoThrowTag{}) {
  if (startup_error_)
    throw std::runtime_error(
        std::string(error_code_name(startup_error_->code)) + ": " +
        startup_error_->message);
}

OperationStore::OperationStore(std::string path, Executor executor,
                               OperationStoreOptions options, NoThrowTag)
    : journal_path_(std::move(path)), executor_(std::move(executor)),
      options_(options) {
  auto initialized = initialize();
  if (!initialized)
    startup_error_ = std::move(initialized.error());
}

OperationStore::~OperationStore() { stop(); }

std::expected<std::unique_ptr<OperationStore>, StoreError>
OperationStore::create(std::string path, Executor executor,
                       OperationStoreOptions options) {
  try {
    auto store = std::unique_ptr<OperationStore>(new OperationStore(
        std::move(path), std::move(executor), options, NoThrowTag{}));
    if (store->startup_error_)
      return std::unexpected(*store->startup_error_);
    return store;
  } catch (const std::exception &error) {
    return std::unexpected(make_error(
        StoreErrorCode::internal,
        std::string("cannot initialize operation store: ") + error.what()));
  } catch (...) {
    return std::unexpected(make_error(StoreErrorCode::internal,
                                      "cannot initialize operation store"));
  }
}

std::expected<void, StoreError> OperationStore::initialize() {
  if (auto valid = validate_options(journal_path_, options_); !valid)
    return valid;
  if (auto cleaned = cleanup_stale_compaction_files(journal_path_, options_);
      !cleaned)
    return cleaned;
  if (auto recovered = recover(); !recovered)
    return recovered;

  try {
    workers_.reserve(options_.worker_count);
    for (std::size_t index = 0; index < options_.worker_count; ++index)
      workers_.emplace_back([this] { worker(); });
  } catch (const std::exception &error) {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_all();
    for (auto &thread : workers_) {
      if (thread.joinable())
        thread.join();
    }
    workers_.clear();
    return std::unexpected(make_error(
        StoreErrorCode::unavailable,
        std::string("cannot start operation workers: ") + error.what()));
  }
  return {};
}

std::expected<void, StoreError> OperationStore::recover() {
  bool created = false;
  UniqueFd journal(
      ::open(journal_path_.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
  if (journal.get() < 0 && errno == ENOENT) {
    journal = UniqueFd(
        ::open(journal_path_.c_str(),
               O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (journal.get() >= 0) {
      created = true;
    } else if (errno == EEXIST) {
      journal = UniqueFd(
          ::open(journal_path_.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
    }
  }
  if (journal.get() < 0)
    return std::unexpected(system_error(
        StoreErrorCode::journal_io, "cannot open operation journal", errno));
  const auto status = validate_regular_file(journal.get());
  if (!status)
    return std::unexpected(status.error());
  if (created) {
    if (auto synced =
            fsync_checked(journal.get(), "cannot fsync new operation journal");
        !synced)
      return synced;
    if (auto synced = fsync_parent(journal_path_); !synced)
      return synced;
  }
  if (status->st_size < 0)
    return std::unexpected(make_error(StoreErrorCode::journal_corrupt,
                                      "journal has a negative size"));
  const auto file_size = static_cast<std::uintmax_t>(status->st_size);
  if (file_size > options_.max_journal_bytes ||
      file_size > std::numeric_limits<std::size_t>::max())
    return std::unexpected(
        make_error(StoreErrorCode::resource_exhausted,
                   "operation journal exceeds the configured byte limit"));

  std::string contents(static_cast<std::size_t>(file_size), '\0');
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const auto remaining = contents.size() - offset;
    const auto chunk =
        std::min(remaining,
                 static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const auto count = ::pread(journal.get(), contents.data() + offset, chunk,
                               static_cast<off_t>(offset));
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return std::unexpected(system_error(
          StoreErrorCode::journal_io, "cannot read operation journal", errno));
    }
    if (count == 0)
      return std::unexpected(
          make_error(StoreErrorCode::journal_corrupt,
                     "operation journal changed while it was being read"));
    offset += static_cast<std::size_t>(count);
  }

  if (!contents.empty() && contents.back() != '\n') {
    const auto newline = contents.find_last_of('\n');
    const std::size_t complete_size =
        newline == std::string::npos ? 0 : newline + 1;
    if (auto truncated = truncate_checked(journal.get(), complete_size);
        !truncated)
      return truncated;
    if (auto synced =
            fsync_checked(journal.get(), "cannot fsync truncated journal");
        !synced)
      return synced;
    contents.resize(complete_size);
  }
  if (auto closed = close_checked(journal, "cannot close operation journal");
      !closed)
    return closed;

  std::size_t line_start = 0;
  while (line_start < contents.size()) {
    if (journal_entries_ >= options_.max_journal_entries)
      return std::unexpected(
          make_error(StoreErrorCode::resource_exhausted,
                     "operation journal exceeds the configured entry limit"));
    const auto newline = contents.find('\n', line_start);
    if (newline == std::string::npos)
      return std::unexpected(make_error(StoreErrorCode::journal_corrupt,
                                        "journal framing is invalid"));
    const auto line =
        std::string_view(contents).substr(line_start, newline - line_start);
    if (line.empty())
      return std::unexpected(make_error(StoreErrorCode::journal_corrupt,
                                        "journal contains an empty record"));
    auto parsed = parse_record(line, options_);
    if (!parsed)
      return std::unexpected(parsed.error());

    const auto request_mapping =
        request_to_operation_.find(parsed->view.request_id);
    if (request_mapping != request_to_operation_.end() &&
        request_mapping->second != parsed->view.operation_id)
      return std::unexpected(
          make_error(StoreErrorCode::journal_corrupt,
                     "request ID maps to conflicting operation IDs"));

    const auto operation = by_operation_.find(parsed->view.operation_id);
    if (operation == by_operation_.end()) {
      if (by_operation_.size() >= options_.max_records)
        return std::unexpected(make_error(
            StoreErrorCode::resource_exhausted,
            "recovered registry exceeds the configured record limit"));
      by_operation_.emplace(parsed->view.operation_id,
                            Record{parsed->view, parsed->digest});
      request_to_operation_.emplace(parsed->view.request_id,
                                    parsed->view.operation_id);
    } else {
      const ParsedRecord previous{operation->second.view,
                                  operation->second.digest};
      if (!same_identity(previous, *parsed) ||
          !valid_transition(previous.view, parsed->view))
        return std::unexpected(make_error(
            StoreErrorCode::journal_corrupt,
            "operation ID has conflicting or invalid journal records"));
      operation->second = Record{parsed->view, parsed->digest};
    }
    ++journal_entries_;
    line_start = newline + 1;
  }
  journal_bytes_ = contents.size();

  for (auto &[operation_id, record] : by_operation_) {
    (void)operation_id;
    if (record.view.state == OperationState::queued) {
      record.view.state = OperationState::failed;
      record.view.result = "interrupted_before_execution";
      record.view.error_code = StoreErrorCode::interrupted;
      record.view.indeterminate = false;
    } else if (record.view.state == OperationState::running) {
      record.view.state = OperationState::indeterminate;
      record.view.result = "interrupted_running_requires_reconciliation";
      record.view.error_code = StoreErrorCode::execution_indeterminate;
      record.view.indeterminate = true;
    }
  }
  return compact_locked();
}

std::expected<void, StoreError> OperationStore::compact_locked() {
  UniqueFd current(
      ::open(journal_path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (current.get() < 0) {
    if (errno != ENOENT || journal_bytes_ != 0 || journal_entries_ != 0)
      return std::unexpected(system_error(StoreErrorCode::journal_io,
                                          "cannot verify operation journal",
                                          errno));
  } else {
    const auto status = validate_regular_file(current.get());
    if (!status)
      return std::unexpected(status.error());
    if (status->st_size < 0 ||
        static_cast<std::uintmax_t>(status->st_size) != journal_bytes_)
      return std::unexpected(
          make_error(StoreErrorCode::journal_corrupt,
                     "operation journal changed outside the operation store"));
    if (auto closed =
            close_checked(current, "cannot close verified operation journal");
        !closed)
      return closed;
  }

  std::vector<std::string> operation_ids;
  operation_ids.reserve(by_operation_.size());
  for (const auto &[operation_id, record] : by_operation_) {
    (void)record;
    operation_ids.push_back(operation_id);
  }
  std::ranges::sort(operation_ids);

  std::string compacted;
  for (const auto &operation_id : operation_ids) {
    const auto &record = by_operation_.at(operation_id);
    auto line = record_line(record.view, record.digest);
    if (!line)
      return std::unexpected(line.error());
    if (line->size() > options_.max_journal_bytes - compacted.size())
      return std::unexpected(
          make_error(StoreErrorCode::resource_exhausted,
                     "canonical journal exceeds the configured byte limit"));
    compacted += *line;
  }

  std::string temporary;
  UniqueFd output;
  for (std::size_t attempt = 0; attempt < options_.max_compaction_name_attempts;
       ++attempt) {
    temporary = journal_path_ + ".compact." + std::to_string(::getpid()) + "." +
                std::to_string(compact_sequence_++);
    const int descriptor =
        ::open(temporary.c_str(),
               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor >= 0) {
      output = UniqueFd(descriptor);
      break;
    }
    if (errno != EEXIST) {
      return std::unexpected(system_error(StoreErrorCode::journal_io,
                                          "cannot create compacted journal",
                                          errno));
    }
  }
  if (output.get() < 0) {
    return std::unexpected(make_error(
        StoreErrorCode::resource_exhausted,
        "cannot allocate a unique compacted journal name within the configured "
        "attempt limit"));
  }
  const auto output_status = validate_regular_file(output.get());
  if (!output_status) {
    (void)::unlink(temporary.c_str());
    return std::unexpected(output_status.error());
  }
  if (auto written =
          write_all(output.get(), compacted, "cannot write compacted journal");
      !written) {
    (void)::unlink(temporary.c_str());
    return written;
  }
  if (auto synced =
          fsync_checked(output.get(), "cannot fsync compacted journal");
      !synced) {
    (void)::unlink(temporary.c_str());
    return synced;
  }
  if (auto closed = close_checked(output, "cannot close compacted journal");
      !closed) {
    (void)::unlink(temporary.c_str());
    return closed;
  }
  if (auto renamed = rename_checked(temporary, journal_path_); !renamed) {
    (void)::unlink(temporary.c_str());
    return renamed;
  }
  if (auto synced = fsync_parent(journal_path_); !synced)
    return synced;

  journal_entries_ = by_operation_.size();
  journal_bytes_ = compacted.size();
  return {};
}

std::expected<void, StoreError>
OperationStore::persist_locked(const Record &record, bool registered) {
  if (journal_error_)
    return std::unexpected(*journal_error_);
  auto line = record_line(record.view, record.digest);
  if (!line)
    return std::unexpected(line.error());
  if (line->size() > options_.max_journal_bytes)
    return std::unexpected(
        make_error(StoreErrorCode::resource_exhausted,
                   "journal record exceeds the configured byte limit"));

  const bool entry_limit_reached =
      journal_entries_ >= options_.max_journal_entries;
  const bool byte_limit_reached =
      journal_bytes_ > options_.max_journal_bytes - line->size();
  if (entry_limit_reached || byte_limit_reached) {
    auto compacted = compact_locked();
    if (!compacted)
      return compacted;
    if (registered)
      return {};
  }
  if (journal_entries_ >= options_.max_journal_entries ||
      journal_bytes_ > options_.max_journal_bytes - line->size())
    return std::unexpected(
        make_error(StoreErrorCode::resource_exhausted,
                   "operation journal has reached a configured limit"));

  UniqueFd journal(::open(journal_path_.c_str(),
                          O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW));
  if (journal.get() < 0)
    return std::unexpected(system_error(
        StoreErrorCode::journal_io, "cannot open operation journal", errno));
  const auto status = validate_regular_file(journal.get());
  if (!status)
    return std::unexpected(status.error());
  if (status->st_size < 0 ||
      static_cast<std::uintmax_t>(status->st_size) != journal_bytes_)
    return std::unexpected(
        make_error(StoreErrorCode::journal_corrupt,
                   "operation journal changed outside the operation store"));

  if (auto written =
          write_all(journal.get(), *line, "cannot append operation journal");
      !written)
    return written;
  if (auto synced =
          fsync_checked(journal.get(), "cannot fsync operation journal");
      !synced)
    return synced;

  if (auto closed = close_checked(journal, "cannot close operation journal");
      !closed)
    return closed;

  ++journal_entries_;
  journal_bytes_ += line->size();
  return {};
}

void OperationStore::poison_journal_locked(const StoreError &error) {
  if (!journal_error_)
    journal_error_ = error;
}

std::expected<SubmitResult, StoreError>
OperationStore::submit(std::string request_id, std::string method,
                       std::string container, std::string canonical) {
  if (auto valid = validate_identifier(request_id, "request_id", options_);
      !valid)
    return std::unexpected(valid.error());
  if (auto valid = validate_identifier(container, "container_id", options_);
      !valid)
    return std::unexpected(valid.error());
  if (!valid_method(method))
    return std::unexpected(make_error(StoreErrorCode::invalid_argument,
                                      "unsupported operation method"));
  if (canonical.size() > options_.max_request_bytes)
    return std::unexpected(
        make_error(StoreErrorCode::invalid_argument,
                   "canonical request exceeds the configured size limit"));

  const auto digest = digest_of(canonical);
  std::lock_guard lock(mutex_);
  if (auto mapping = request_to_operation_.find(request_id);
      mapping != request_to_operation_.end()) {
    const auto operation = by_operation_.find(mapping->second);
    if (operation == by_operation_.end())
      return std::unexpected(
          make_error(StoreErrorCode::internal,
                     "request registry contains a missing operation"));
    const auto &record = operation->second;
    if (record.digest != digest || record.view.method != method ||
        record.view.container_id != container)
      return std::unexpected(
          make_error(StoreErrorCode::conflict,
                     "request_id reused with a different request payload"));
    return SubmitResult{record.view.operation_id, true};
  }
  if (stopping_)
    return std::unexpected(make_error(StoreErrorCode::unavailable,
                                      "operation store is shutting down"));
  if (journal_error_)
    return std::unexpected(*journal_error_);
  if (by_operation_.size() >= options_.max_records)
    return std::unexpected(
        make_error(StoreErrorCode::resource_exhausted,
                   "operation registry has reached its record limit"));
  if (in_flight_ >= options_.max_in_flight)
    return std::unexpected(
        make_error(StoreErrorCode::resource_exhausted,
                   "operation in-flight limit has been reached"));

  const auto operation_id = make_id(request_id);
  if (by_operation_.contains(operation_id))
    return std::unexpected(
        make_error(StoreErrorCode::conflict,
                   "deterministic operation ID collides with another request"));
  Record record{OperationView{.operation_id = operation_id,
                              .request_id = request_id,
                              .method = method,
                              .container_id = container,
                              .state = OperationState::queued,
                              .result = {},
                              .error_code = StoreErrorCode::none,
                              .indeterminate = false},
                digest};
  if (auto persisted = persist_locked(record, false); !persisted) {
    poison_journal_locked(persisted.error());
    return std::unexpected(persisted.error());
  }
  by_operation_.emplace(operation_id, record);
  request_to_operation_.emplace(request_id, operation_id);
  queue_.push_back(operation_id);
  ++in_flight_;
  condition_.notify_one();
  return SubmitResult{operation_id, false};
}

std::expected<OperationView, StoreError>
OperationStore::get(std::string_view operation_id) const {
  std::lock_guard lock(mutex_);
  const auto operation = by_operation_.find(std::string(operation_id));
  if (operation == by_operation_.end())
    return std::unexpected(
        make_error(StoreErrorCode::not_found, "operation not found"));
  return operation->second.view;
}

std::optional<StoreError> OperationStore::startup_error() const {
  std::lock_guard lock(mutex_);
  return startup_error_;
}

bool OperationStore::accepting_submissions() const {
  std::lock_guard lock(mutex_);
  return !stopping_ && !startup_error_ && !journal_error_ &&
         by_operation_.size() < options_.max_records &&
         in_flight_ < options_.max_in_flight;
}

void OperationStore::worker() {
  for (;;) {
    std::string operation_id;
    Record snapshot;
    {
      std::unique_lock lock(mutex_);
      condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty() && stopping_)
        return;
      operation_id = std::move(queue_.front());
      queue_.pop_front();
      auto &record = by_operation_.at(operation_id);
      record.view.state = OperationState::running;
      record.view.result.clear();
      record.view.error_code = StoreErrorCode::none;
      record.view.indeterminate = false;
      if (auto persisted = persist_locked(record, true); !persisted) {
        poison_journal_locked(persisted.error());
        record.view.state = OperationState::failed;
        record.view.result = "journal_failed_before_execution";
        record.view.error_code = persisted.error().code;
        record.view.indeterminate = false;
        --in_flight_;
        continue;
      }
      snapshot = record;
    }

    std::expected<std::string, StoreError> result = std::unexpected(make_error(
        StoreErrorCode::unavailable, "operation executor is unavailable"));
    try {
      if (executor_)
        result = executor_(snapshot.view);
    } catch (const std::exception &error) {
      result = std::unexpected(make_error(
          StoreErrorCode::execution_indeterminate,
          std::string("operation executor threw: ") + error.what(), true));
    } catch (...) {
      result = std::unexpected(
          make_error(StoreErrorCode::execution_indeterminate,
                     "operation executor threw an unknown exception", true));
    }

    std::lock_guard lock(mutex_);
    auto &record = by_operation_.at(operation_id);
    if (result) {
      if (result->size() <= options_.max_result_bytes &&
          valid_json_string(*result)) {
        record.view.state = OperationState::succeeded;
        record.view.result = std::move(*result);
        record.view.error_code = StoreErrorCode::none;
        record.view.indeterminate = false;
      } else {
        record.view.state = OperationState::indeterminate;
        record.view.result = "executor_result_cannot_be_persisted";
        record.view.error_code = StoreErrorCode::execution_indeterminate;
        record.view.indeterminate = true;
      }
    } else {
      auto error = std::move(result.error());
      if (error.code == StoreErrorCode::none)
        error.code = error.indeterminate
                         ? StoreErrorCode::execution_indeterminate
                         : StoreErrorCode::executor_failed;
      record.view.state = error.indeterminate ? OperationState::indeterminate
                                              : OperationState::failed;
      if (error.message.size() <= options_.max_result_bytes &&
          valid_json_string(error.message)) {
        record.view.result = std::move(error.message);
      } else {
        record.view.result = "executor_error_message_cannot_be_persisted";
      }
      record.view.error_code = error.code;
      record.view.indeterminate = error.indeterminate;
    }

    auto persisted = persist_locked(record, true);
    if (!persisted) {
      poison_journal_locked(persisted.error());
      record.view.state = OperationState::indeterminate;
      record.view.result = "terminal_journal_persist_failed";
      record.view.error_code = persisted.error().code;
      record.view.indeterminate = true;
    }
    --in_flight_;
  }
}

void OperationStore::stop() {
  std::call_once(stop_once_, [this] {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_all();
    for (auto &thread : workers_) {
      if (thread.joinable())
        thread.join();
    }
    workers_.clear();
  });
}
} // namespace gravesentinel::ipc
