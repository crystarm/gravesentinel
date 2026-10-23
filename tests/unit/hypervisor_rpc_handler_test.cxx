#include "src/hypervisor/rpc_handler.hxx"

#include "src/ipc/codec.hxx"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <gtest/gtest.h>
#include <unistd.h>

namespace gravesentinel::hypervisor {
namespace {

using namespace std::chrono_literals;
using ccm::CheckpointErrorDetail;
using ccm::CheckpointResult;
using ccm::ContainerId;
using ccm::ContainerRegistration;
using ccm::OperationId;
using ccm::RequestId;
using ccm::RuntimeContainerRef;
using ccm::RuntimeSideEffect;
using ccm::StableErrorCode;
using ccm::StorageSideEffect;

[[nodiscard]] nlohmann::json request(std::string method, std::string request_id,
                                     nlohmann::json parameters) {
  return {{"version", ipc::protocol_version},
          {"method", std::move(method)},
          {"request_id", std::move(request_id)},
          {"params", std::move(parameters)}};
}

[[nodiscard]] nlohmann::json complete_registration_parameters() {
  return {
      {"container_id", "container-1"},
      {"runtime_container", "runtime-1"},
      {"checkpoint",
       {{"interval_seconds", 60},
        {"keep_last", 3U},
        {"container_cpu", 0U},
        {"checkpoint_cpu", 1U}}},
      {"cold_start",
       {{"image_reference", "registry.example/worker:latest"},
        {"argv", {"worker", "", "--threads", "2"}},
        {"environment",
         {{{"name", "WORKER_MODE"}, {"value", "steady"}},
          {{"name", "API_TOKEN"}, {"value", "registration-secret"}}}},
        {"mounts",
         {{{"source", "/srv/input"},
           {"target", "/input"},
           {"read_only", true}}}},
        {"runtime_options", {"--log-level=warning"}}}},
      {"runtime_fingerprint",
       {{"runtime_name", "podman"},
        {"runtime_version", "5.4.2"},
        {"runtime_build_origin", "Debian 13"},
        {"architecture", "amd64"},
        {"kernel_release", "6.12.0"},
        {"criu_version", "4.1.1"},
        {"oci_runtime_name", "crun"},
        {"oci_runtime_version", "1.21"},
        {"oci_runtime_commit", "abcdef"},
        {"oci_runtime_features", "+CRIU +SECCOMP"},
        {"cgroup_version", "v2"},
        {"cgroup_manager", "systemd"},
        {"cgroup_controllers", "cpu memory pids"},
        {"privilege_mode", "rootful"},
        {"user_namespace_mode", "host"},
        {"capability_profile", "default"},
        {"selinux_mode", "disabled"},
        {"apparmor_profile", "host-default"},
        {"seccomp_profile", "default"},
        {"command_profile", "podman-criu-rootful-v1"}}},
  };
}

[[nodiscard]] CheckpointErrorDetail service_error(
    const StableErrorCode code, std::string diagnostic,
    const StorageSideEffect storage_side_effect = StorageSideEffect::none,
    const RuntimeSideEffect runtime_side_effect = RuntimeSideEffect::none) {
  return CheckpointErrorDetail{
      .code = code,
      .diagnostic = std::move(diagnostic),
      .retryable = false,
      .correlation_id = std::nullopt,
      .storage_side_effect = storage_side_effect,
      .runtime_side_effect = runtime_side_effect,
  };
}

class FakeCheckpointService final : public ccm::ICheckpointService {
public:
  [[nodiscard]] CheckpointResult<void>
  register_container(const ContainerRegistration &registration,
                     const RequestId &request_id) override {
    const std::lock_guard lock(mutex_);
    ++registration_calls_;
    registration_ = registration;
    registration_request_ = request_id.value();
    return registration_result_;
  }

  [[nodiscard]] CheckpointResult<void>
  unregister_container(const ContainerId &container_id,
                       const RequestId &request_id) override {
    const std::lock_guard lock(mutex_);
    unregistered_container_ = container_id.value();
    unregister_request_ = request_id.value();
    return unregister_result_;
  }

  [[nodiscard]] CheckpointResult<void>
  create_checkpoint(const ContainerId &container_id,
                    const RequestId &request_id,
                    const OperationId &operation_id) override {
    const std::lock_guard lock(mutex_);
    created_container_ = container_id.value();
    create_request_ = request_id.value();
    create_operation_ = operation_id.value();
    return create_result_;
  }

  [[nodiscard]] CheckpointResult<void>
  restore_or_restart(const ContainerId &container_id,
                     const RequestId &request_id,
                     const OperationId &operation_id) override {
    const std::lock_guard lock(mutex_);
    restored_container_ = container_id.value();
    restore_request_ = request_id.value();
    restore_operation_ = operation_id.value();
    return restore_result_;
  }

  [[nodiscard]] CheckpointResult<void> shutdown() override { return {}; }

  void set_registration_result(CheckpointResult<void> result) {
    const std::lock_guard lock(mutex_);
    registration_result_ = std::move(result);
  }

  void set_unregister_result(CheckpointResult<void> result) {
    const std::lock_guard lock(mutex_);
    unregister_result_ = std::move(result);
  }

  void set_create_result(CheckpointResult<void> result) {
    const std::lock_guard lock(mutex_);
    create_result_ = std::move(result);
  }

  void set_restore_result(CheckpointResult<void> result) {
    const std::lock_guard lock(mutex_);
    restore_result_ = std::move(result);
  }

  [[nodiscard]] std::optional<ContainerRegistration> registration() const {
    const std::lock_guard lock(mutex_);
    return registration_;
  }

  [[nodiscard]] int registration_calls() const {
    const std::lock_guard lock(mutex_);
    return registration_calls_;
  }

  [[nodiscard]] std::string registration_request() const {
    const std::lock_guard lock(mutex_);
    return registration_request_;
  }

  [[nodiscard]] std::string unregistered_container() const {
    const std::lock_guard lock(mutex_);
    return unregistered_container_;
  }

  [[nodiscard]] std::string create_request() const {
    const std::lock_guard lock(mutex_);
    return create_request_;
  }

  [[nodiscard]] std::string create_operation() const {
    const std::lock_guard lock(mutex_);
    return create_operation_;
  }

  [[nodiscard]] std::string restore_request() const {
    const std::lock_guard lock(mutex_);
    return restore_request_;
  }

  [[nodiscard]] std::string restore_operation() const {
    const std::lock_guard lock(mutex_);
    return restore_operation_;
  }

private:
  mutable std::mutex mutex_;
  CheckpointResult<void> registration_result_{};
  CheckpointResult<void> unregister_result_{};
  CheckpointResult<void> create_result_{};
  CheckpointResult<void> restore_result_{};
  std::optional<ContainerRegistration> registration_;
  int registration_calls_{};
  std::string registration_request_;
  std::string unregistered_container_;
  std::string unregister_request_;
  std::string created_container_;
  std::string create_request_;
  std::string create_operation_;
  std::string restored_container_;
  std::string restore_request_;
  std::string restore_operation_;
};

[[nodiscard]] std::string temporary_journal_path() {
  static std::atomic<unsigned long long> sequence{0};
  return (std::filesystem::temp_directory_path() /
          ("gravesentinel-rpc-handler-" + std::to_string(::getpid()) + "-" +
           std::to_string(++sequence) + ".jsonl"))
      .string();
}

class HypervisorRpcHandlerTest : public ::testing::Test {
protected:
  void SetUp() override {
    journal_path_ = temporary_journal_path();
    ipc::OperationStoreOptions options;
    options.worker_count = 1;
    auto store = ipc::OperationStore::create(
        journal_path_, make_operation_executor(service_), options);
    ASSERT_TRUE(store) << store.error().message;
    store_ = std::move(*store);
  }

  void TearDown() override {
    if (store_)
      store_->stop();
    store_.reset();
    std::error_code ignored;
    std::filesystem::remove(journal_path_, ignored);
  }

  [[nodiscard]] nlohmann::json
  wait_for_terminal(RpcHandler &handler, const std::string &operation_id) {
    for (int attempt = 0; attempt < 200; ++attempt) {
      auto result =
          handler(request("GetOperation", "poll-" + std::to_string(attempt),
                          {{"operation_id", operation_id}}));
      if (result.is_object() && result.contains("state") &&
          (result.at("state") == "succeeded" ||
           result.at("state") == "failed" ||
           result.at("state") == "indeterminate")) {
        return result;
      }
      std::this_thread::sleep_for(5ms);
    }
    return nullptr;
  }

  FakeCheckpointService service_;
  std::string journal_path_;
  std::unique_ptr<ipc::OperationStore> store_;
};

TEST_F(HypervisorRpcHandlerTest,
       ReconstructsEveryRegistrationFieldAndRevalidatesWithCore) {
  auto parameters = complete_registration_parameters();
  const auto decoded = decode_container_registration(parameters);
  ASSERT_TRUE(decoded);
  ASSERT_TRUE(decoded->cold_start);
  EXPECT_EQ(decoded->cold_start->argv,
            (std::vector<std::string>{"worker", "", "--threads", "2"}));
  EXPECT_EQ(decoded->cold_start->environment.size(), 2U);
  EXPECT_EQ(decoded->cold_start->environment[1].name, "API_TOKEN");
  EXPECT_EQ(decoded->cold_start->environment[1].value, "registration-secret");
  ASSERT_EQ(decoded->cold_start->mounts.size(), 1U);
  EXPECT_EQ(decoded->cold_start->mounts[0].source, "/srv/input");
  EXPECT_EQ(decoded->cold_start->mounts[0].target, "/input");
  EXPECT_TRUE(decoded->cold_start->mounts[0].read_only);
  EXPECT_EQ(decoded->cold_start->runtime_options,
            (std::vector<std::string>{"--log-level=warning"}));
  ASSERT_TRUE(decoded->runtime_fingerprint);
  EXPECT_EQ(decoded->runtime_fingerprint->runtime_build_origin, "Debian 13");
  EXPECT_EQ(decoded->runtime_fingerprint->command_profile,
            "podman-criu-rootful-v1");

  RpcHandler handler(service_, *store_);
  const auto rejected =
      handler(request("Register", "register-unsupported", parameters));
  EXPECT_EQ(rejected, nlohmann::json({{"ok", false}}));
  EXPECT_EQ(service_.registration_calls(), 0);
  EXPECT_EQ(rejected.dump().find("registration-secret"), std::string::npos);

  parameters["cold_start"]["mounts"] = nlohmann::json::array();
  parameters["cold_start"]["runtime_options"] = nlohmann::json::array();
  const auto expected = decode_container_registration(parameters);
  ASSERT_TRUE(expected);
  ASSERT_TRUE(ccm::validate_container_registration(*expected));

  const auto registered =
      handler(request("Register", "register-valid", parameters));
  EXPECT_TRUE(registered.is_object());
  EXPECT_TRUE(registered.empty());
  const auto captured = service_.registration();
  ASSERT_TRUE(captured);
  EXPECT_EQ(*captured, *expected);
  EXPECT_EQ(service_.registration_request(), "register-valid");
}

TEST_F(HypervisorRpcHandlerTest,
       ReturnsOnlyNormalizerSafeSuccessAndErrorForms) {
  RpcHandler handler(service_, *store_);

  const auto unregistered = handler(request("Unregister", "unregister-ok",
                                            {{"container_id", "container-1"}}));
  EXPECT_TRUE(unregistered.is_object());
  EXPECT_TRUE(unregistered.empty());
  EXPECT_EQ(service_.unregistered_container(), "container-1");

  service_.set_unregister_result(std::unexpected(
      service_error(StableErrorCode::permission_denied,
                    "secret=/run/credentials/scheduler-token")));
  const auto denied = handler(request("Unregister", "unregister-failed",
                                      {{"container_id", "container-1"}}));
  EXPECT_EQ(denied, nlohmann::json({{"ok", false}}));
  EXPECT_EQ(denied.dump().find("secret"), std::string::npos);
  EXPECT_EQ(denied.dump().find("credentials"), std::string::npos);

  const auto missing = handler(request(
      "GetOperation", "get-missing", {{"operation_id", "operation-missing"}}));
  EXPECT_EQ(missing, nlohmann::json({{"ok", false}}));
}

TEST_F(HypervisorRpcHandlerTest, ReportsQueueSaturationFromOperationStore) {
  RpcHandler handler(service_, *store_);
  EXPECT_EQ(handler(request("GetHealth", "health-available",
                            nlohmann::json::object()))
                .at("queue"),
            "available");

  store_->stop();
  store_.reset();
  std::error_code ignored;
  std::filesystem::remove(journal_path_, ignored);

  ipc::OperationStoreOptions options;
  options.max_records = 1;
  options.worker_count = 1;
  auto store = ipc::OperationStore::create(
      journal_path_, make_operation_executor(service_), options);
  ASSERT_TRUE(store) << store.error().message;
  store_ = std::move(*store);
  RpcHandler bounded_handler(service_, *store_);

  const auto submitted =
      bounded_handler(request("SubmitCreateCheckpoint", "health-operation",
                              {{"container_id", "container-1"}}));
  ASSERT_TRUE(submitted.contains("operation_id"));
  const auto terminal = wait_for_terminal(
      bounded_handler, submitted.at("operation_id").get<std::string>());
  ASSERT_FALSE(terminal.is_null());
  EXPECT_EQ(bounded_handler(request("GetHealth", "health-saturated",
                                    nlohmann::json::object()))
                .at("queue"),
            "saturated");
  EXPECT_EQ(bounded_handler(request("SubmitCreateCheckpoint", "health-rejected",
                                    {{"container_id", "container-2"}})),
            nlohmann::json({{"ok", false}, {"error_code", "server_busy"}}));
}

TEST_F(HypervisorRpcHandlerTest, PreservesEveryStableOperationErrorCode) {
  struct ExpectedError {
    StableErrorCode code;
    const char *name;
  };
  constexpr ExpectedError errors[]{
      {StableErrorCode::invalid_argument, "invalid_argument"},
      {StableErrorCode::not_found, "not_found"},
      {StableErrorCode::already_exists, "already_exists"},
      {StableErrorCode::resource_exhausted, "resource_exhausted"},
      {StableErrorCode::conflict, "conflict"},
      {StableErrorCode::permission_denied, "permission_denied"},
      {StableErrorCode::capture_failed, "capture_failed"},
      {StableErrorCode::storage_error, "storage_error"},
      {StableErrorCode::corrupted_snapshot, "corrupted_snapshot"},
      {StableErrorCode::incompatible_runtime, "incompatible_runtime"},
      {StableErrorCode::restore_failed, "restore_failed"},
      {StableErrorCode::cold_start_failed, "cold_start_failed"},
      {StableErrorCode::deadline_exceeded, "deadline_exceeded"},
      {StableErrorCode::unavailable, "unavailable"},
      {StableErrorCode::internal, "internal"},
  };

  RpcHandler handler(service_, *store_);
  for (std::size_t index = 0; index < std::size(errors); ++index) {
    service_.set_create_result(std::unexpected(
        service_error(errors[index].code, "private diagnostic")));
    const auto request_id = "mapped-error-" + std::to_string(index);
    const auto submission =
        handler(request("SubmitCreateCheckpoint", request_id,
                        {{"container_id", "container-1"}}));
    ASSERT_TRUE(submission.contains("operation_id")) << errors[index].name;
    const auto operation = wait_for_terminal(
        handler, submission.at("operation_id").get<std::string>());
    ASSERT_FALSE(operation.is_null()) << errors[index].name;
    EXPECT_EQ(operation.at("state"), "failed") << errors[index].name;
    EXPECT_EQ(operation.at("error_code"), errors[index].name);
  }
}

TEST_F(HypervisorRpcHandlerTest,
       SubmitsDeduplicatesAndHidesOperationResultsAndDiagnostics) {
  RpcHandler handler(service_, *store_);
  const auto submission =
      handler(request("SubmitCreateCheckpoint", "create-request",
                      {{"container_id", "container-1"}}));
  ASSERT_TRUE(submission.is_object());
  ASSERT_EQ(submission.size(), 2U);
  ASSERT_TRUE(submission.at("operation_id").is_string());
  EXPECT_FALSE(submission.at("duplicate").get<bool>());
  const auto operation_id = submission.at("operation_id").get<std::string>();

  const auto duplicate =
      handler(request("SubmitCreateCheckpoint", "create-request",
                      {{"container_id", "container-1"}}));
  EXPECT_EQ(duplicate.at("operation_id"), operation_id);
  EXPECT_TRUE(duplicate.at("duplicate").get<bool>());

  const auto succeeded = wait_for_terminal(handler, operation_id);
  ASSERT_FALSE(succeeded.is_null());
  EXPECT_EQ(succeeded, nlohmann::json({{"operation_id", operation_id},
                                       {"state", "succeeded"},
                                       {"error_code", nullptr}}));
  EXPECT_EQ(service_.create_request(), "create-request");
  EXPECT_EQ(service_.create_operation(), operation_id);
  EXPECT_FALSE(succeeded.contains("result"));
  EXPECT_FALSE(succeeded.contains("diagnostic"));

  service_.set_create_result(std::unexpected(
      service_error(StableErrorCode::storage_error,
                    "password=definite-storage-secret; /private/checkpoints")));
  const auto failed_submission =
      handler(request("SubmitCreateCheckpoint", "failed-create-request",
                      {{"container_id", "container-1"}}));
  ASSERT_TRUE(failed_submission.contains("operation_id"));
  const auto failed = wait_for_terminal(
      handler, failed_submission.at("operation_id").get<std::string>());
  ASSERT_FALSE(failed.is_null());
  EXPECT_EQ(failed.at("state"), "failed");
  EXPECT_EQ(failed.at("error_code"), "storage_error");
  EXPECT_EQ(failed.size(), 3U);
  EXPECT_EQ(failed.dump().find("password"), std::string::npos);
  EXPECT_EQ(failed.dump().find("private"), std::string::npos);

  service_.set_restore_result(std::unexpected(service_error(
      StableErrorCode::incompatible_runtime, "token=uncertain-runtime-secret",
      StorageSideEffect::none, RuntimeSideEffect::incarnation_may_exist)));
  const auto uncertain_submission =
      handler(request("SubmitRestoreOrRestart", "uncertain-restore-request",
                      {{"container_id", "container-1"}}));
  ASSERT_TRUE(uncertain_submission.contains("operation_id"));
  const auto uncertain = wait_for_terminal(
      handler, uncertain_submission.at("operation_id").get<std::string>());
  ASSERT_FALSE(uncertain.is_null());
  EXPECT_EQ(uncertain.at("state"), "indeterminate");
  EXPECT_EQ(uncertain.at("error_code"), "incompatible_runtime");
  EXPECT_EQ(uncertain.size(), 3U);
  EXPECT_EQ(uncertain.dump().find("token"), std::string::npos);
  EXPECT_EQ(service_.restore_request(), "uncertain-restore-request");
  EXPECT_EQ(service_.restore_operation(),
            uncertain_submission.at("operation_id").get<std::string>());
}

} // namespace
} // namespace gravesentinel::hypervisor
