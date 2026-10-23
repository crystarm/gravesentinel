#pragma once
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <functional>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gravesentinel::ipc {
enum class OperationState { queued, running, succeeded, failed, indeterminate };
struct OperationView {
  std::string operation_id;
  std::string request_id;
  std::string method;
  std::string container_id;
  OperationState state;
  std::string result;
};
struct SubmitResult {
  std::string operation_id;
  bool duplicate;
};
struct StoreError {
  std::string message;
};
class OperationStore final {
public:
  using Executor = std::function<std::expected<std::string, StoreError>(
      const OperationView &)>;
  OperationStore(std::string journal_path, Executor executor,
                 std::size_t queue_limit = 64);
  ~OperationStore();
  OperationStore(const OperationStore &) = delete;
  OperationStore &operator=(const OperationStore &) = delete;
  std::expected<SubmitResult, StoreError> submit(std::string request_id,
                                                 std::string method,
                                                 std::string container_id,
                                                 std::string canonical_request);
  std::expected<OperationView, StoreError>
  get(std::string_view operation_id) const;
  void stop();

private:
  struct Record {
    OperationView view;
    std::string digest;
    std::string canonical_request;
  };
  bool persist_locked(const Record &record);
  void recover();
  void worker();
  std::string journal_path_;
  Executor executor_;
  std::size_t queue_limit_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool stopping_{false};
  std::unordered_map<std::string, Record> by_operation_;
  std::unordered_map<std::string, std::string> request_to_operation_;
  std::vector<std::string> queue_;
  std::thread worker_;
};
const char *state_name(OperationState state) noexcept;
} // namespace gravesentinel::ipc
