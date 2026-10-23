#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gravesentinel::ipc {
enum class OperationState { queued, running, succeeded, failed, indeterminate };

enum class StoreErrorCode {
  none,
  invalid_argument,
  not_found,
  already_exists,
  conflict,
  resource_exhausted,
  permission_denied,
  capture_failed,
  storage_error,
  corrupted_snapshot,
  incompatible_runtime,
  restore_failed,
  cold_start_failed,
  deadline_exceeded,
  unavailable,
  journal_io,
  journal_corrupt,
  executor_failed,
  execution_indeterminate,
  interrupted,
  internal,
};

struct StoreError {
  std::string message;
  StoreErrorCode code{StoreErrorCode::internal};
  bool indeterminate{false};

  StoreError() = default;
  StoreError(std::string message_value)
      : message(std::move(message_value)),
        code(StoreErrorCode::executor_failed) {}
  StoreError(StoreErrorCode code_value, std::string message_value,
             bool indeterminate_value = false)
      : message(std::move(message_value)), code(code_value),
        indeterminate(indeterminate_value) {}
};

struct OperationView {
  std::string operation_id;
  std::string request_id;
  std::string method;
  std::string container_id;
  OperationState state;
  std::string result;
  StoreErrorCode error_code{StoreErrorCode::none};
  bool indeterminate{false};

  bool operator==(const OperationView &) const = default;
};

struct SubmitResult {
  std::string operation_id;
  bool duplicate;
};

struct OperationStoreOptions {
  std::size_t max_in_flight{64};
  std::size_t max_records{4096};
  std::size_t worker_count{4};
  std::size_t max_journal_entries{8192};
  std::size_t max_journal_bytes{64U * 1024U * 1024U};
  std::size_t max_identifier_bytes{1024};
  std::size_t max_request_bytes{1024U * 1024U};
  std::size_t max_result_bytes{64U * 1024U};
};

class OperationStore final {
public:
  using Executor = std::function<std::expected<std::string, StoreError>(
      const OperationView &)>;

  OperationStore(std::string journal_path, Executor executor,
                 std::size_t queue_limit = 64);
  OperationStore(std::string journal_path, Executor executor,
                 OperationStoreOptions options);
  ~OperationStore();
  OperationStore(const OperationStore &) = delete;
  OperationStore &operator=(const OperationStore &) = delete;

  [[nodiscard]] static std::expected<std::unique_ptr<OperationStore>,
                                     StoreError>
  create(std::string journal_path, Executor executor,
         OperationStoreOptions options = {});

  [[nodiscard]] std::expected<SubmitResult, StoreError>
  submit(std::string request_id, std::string method, std::string container_id,
         std::string canonical_request);
  [[nodiscard]] std::expected<OperationView, StoreError>
  get(std::string_view operation_id) const;
  [[nodiscard]] std::optional<StoreError> startup_error() const;
  [[nodiscard]] bool accepting_submissions() const;
  void stop();

private:
  struct Record {
    OperationView view;
    std::string digest;
  };
  struct NoThrowTag {};

  OperationStore(std::string journal_path, Executor executor,
                 OperationStoreOptions options, NoThrowTag);

  [[nodiscard]] std::expected<void, StoreError> initialize();
  [[nodiscard]] std::expected<void, StoreError> recover();
  [[nodiscard]] std::expected<void, StoreError>
  persist_locked(const Record &record, bool registered);
  [[nodiscard]] std::expected<void, StoreError> compact_locked();
  void poison_journal_locked(const StoreError &error);
  void worker();

  std::string journal_path_;
  Executor executor_;
  OperationStoreOptions options_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::once_flag stop_once_;
  bool stopping_{false};
  std::optional<StoreError> startup_error_;
  std::optional<StoreError> journal_error_;
  std::unordered_map<std::string, Record> by_operation_;
  std::unordered_map<std::string, std::string> request_to_operation_;
  std::deque<std::string> queue_;
  std::size_t in_flight_{0};
  std::size_t journal_entries_{0};
  std::size_t journal_bytes_{0};
  std::vector<std::thread> workers_;
};

[[nodiscard]] const char *state_name(OperationState state) noexcept;
[[nodiscard]] const char *error_code_name(StoreErrorCode code) noexcept;
} // namespace gravesentinel::ipc
