#include "src/ipc/operation_store.hxx"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace gravesentinel::ipc {
namespace {
using namespace std::chrono_literals;

std::string temp_path(std::string_view suffix = {}) {
  static std::atomic<unsigned long long> sequence{0};
  return "/tmp/gravesentinel-journal-test-" + std::to_string(::getpid()) + "-" +
         std::to_string(sequence.fetch_add(1)) + std::string(suffix) + ".jsonl";
}

void remove_path(const std::string &path) {
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
}

std::string read_text(const std::string &path) {
  constexpr std::uintmax_t maximum_bytes = 128U * 1024U * 1024U;
  const auto size = std::filesystem::file_size(path);
  if (size > maximum_bytes)
    throw std::runtime_error("operation store test file exceeds read bound");
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot open operation store test file");
  std::string contents(static_cast<std::size_t>(size), '\0');
  if (!contents.empty()) {
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (input.gcount() != static_cast<std::streamsize>(contents.size()))
      throw std::runtime_error("operation store test file is truncated");
  }
  return contents;
}

void write_text(const std::string &path, std::string_view contents,
                std::ios::openmode mode = std::ios::trunc) {
  std::ofstream output(path, std::ios::binary | mode);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::filesystem::path temp_directory(std::string_view suffix = {}) {
  auto path = std::filesystem::path(temp_path(suffix));
  path += ".directory";
  remove_path(path.string());
  if (!std::filesystem::create_directory(path))
    throw std::runtime_error("cannot create operation store test directory");
  return path;
}

void write_mode(const std::filesystem::path &path, std::string_view contents,
                mode_t mode) {
  write_text(path.string(), contents);
  if (::chmod(path.c_str(), mode) != 0)
    throw std::runtime_error("cannot set operation store test file mode");
}

std::filesystem::path compaction_path(const std::filesystem::path &journal,
                                      unsigned long long process,
                                      unsigned long long sequence) {
  return journal.string() + ".compact." + std::to_string(process) + "." +
         std::to_string(sequence);
}

std::size_t line_count(std::string_view contents) {
  return static_cast<std::size_t>(std::ranges::count(contents, '\n'));
}

template <typename Predicate>
std::optional<OperationView> wait_for_view(OperationStore &store,
                                           std::string_view operation_id,
                                           Predicate predicate) {
  for (int attempt = 0; attempt < 400; ++attempt) {
    auto view = store.get(operation_id);
    if (view && predicate(*view))
      return *view;
    std::this_thread::sleep_for(5ms);
  }
  return std::nullopt;
}

std::optional<OperationView> wait_for_terminal(OperationStore &store,
                                               std::string_view operation_id) {
  return wait_for_view(store, operation_id, [](const OperationView &view) {
    return view.state == OperationState::succeeded ||
           view.state == OperationState::failed ||
           view.state == OperationState::indeterminate;
  });
}

OperationStore::Executor successful_executor(std::string result = "ok") {
  return [result = std::move(result)](
             const OperationView &) -> std::expected<std::string, StoreError> {
    return result;
  };
}
} // namespace

TEST(OperationStoreTest, AcceptsDurableStopMethod) {
  const auto path = temp_path();
  remove_path(path);
  {
    OperationStore store(path, successful_executor("stopped"));

    const auto submitted = store.submit("req-stop", "SubmitStopContainer",
                                        "container-1", "Stop:container-1");

    ASSERT_TRUE(submitted) << submitted.error().message;
    const auto terminal = wait_for_terminal(store, submitted->operation_id);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(terminal->method, "SubmitStopContainer");
    EXPECT_EQ(terminal->state, OperationState::succeeded);
    EXPECT_EQ(terminal->result, "stopped");
  }
  remove_path(path);
}

TEST(OperationStoreTest,
     DeduplicatesSameRequestAndRejectsDifferentPayloadOrIdentity) {
  const auto path = temp_path();
  remove_path(path);
  std::atomic<int> calls{0};
  std::string operation_id;
  {
    OperationStore store(
        path,
        [&](const OperationView &) -> std::expected<std::string, StoreError> {
          ++calls;
          return std::string("ok");
        });
    auto submitted = store.submit("req-1", "SubmitCreateCheckpoint",
                                  "container-1", "Create:container-1");
    ASSERT_TRUE(submitted);
    operation_id = submitted->operation_id;
    EXPECT_EQ(operation_id.size(), 67U);
    EXPECT_EQ(operation_id.find('-', 3), std::string::npos);

    auto duplicate = store.submit("req-1", "SubmitCreateCheckpoint",
                                  "container-1", "Create:container-1");
    ASSERT_TRUE(duplicate);
    EXPECT_TRUE(duplicate->duplicate);
    EXPECT_EQ(operation_id, duplicate->operation_id);

    auto different_payload = store.submit("req-1", "SubmitCreateCheckpoint",
                                          "container-1", "Create:container-2");
    ASSERT_FALSE(different_payload);
    EXPECT_EQ(different_payload.error().code, StoreErrorCode::conflict);

    auto different_method = store.submit("req-1", "SubmitRestoreOrRestart",
                                         "container-1", "Create:container-1");
    ASSERT_FALSE(different_method);
    EXPECT_EQ(different_method.error().code, StoreErrorCode::conflict);

    auto final = wait_for_terminal(store, operation_id);
    ASSERT_TRUE(final);
    EXPECT_EQ(final->state, OperationState::succeeded);
  }
  EXPECT_EQ(calls.load(), 1);
  remove_path(path);
}

TEST(OperationStoreTest,
     ReopensTerminalOperationWithoutRerunAndKeepsDeterministicId) {
  const auto path = temp_path();
  remove_path(path);
  std::string operation_id;
  {
    OperationStore store(path, successful_executor("done"));
    auto submitted = store.submit("req-terminal", "SubmitCreateCheckpoint",
                                  "container-2", "payload");
    ASSERT_TRUE(submitted);
    operation_id = submitted->operation_id;
    auto final = wait_for_terminal(store, operation_id);
    ASSERT_TRUE(final);
    ASSERT_EQ(final->state, OperationState::succeeded);
  }
  {
    std::atomic<int> calls{0};
    OperationStore store(
        path,
        [&](const OperationView &) -> std::expected<std::string, StoreError> {
          ++calls;
          return std::string("unexpected");
        });
    auto recovered = store.get(operation_id);
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->state, OperationState::succeeded);
    EXPECT_EQ(recovered->result, "done");

    auto duplicate = store.submit("req-terminal", "SubmitCreateCheckpoint",
                                  "container-2", "payload");
    ASSERT_TRUE(duplicate);
    EXPECT_TRUE(duplicate->duplicate);
    EXPECT_EQ(duplicate->operation_id, operation_id);
    EXPECT_EQ(calls.load(), 0);
  }
  remove_path(path);
}

TEST(OperationStoreTest,
     RecoveryClassifiesBothRunningMethodsIndeterminateAndQueuedFailed) {
  const auto live_path = temp_path("-live");
  const auto crash_path = temp_path("-crash");
  remove_path(live_path);
  remove_path(crash_path);

  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  int entered = 0;
  bool release = false;
  OperationStoreOptions options;
  options.worker_count = 2;
  options.max_in_flight = 3;

  OperationStore live(
      live_path,
      [&](const OperationView &) -> std::expected<std::string, StoreError> {
        std::unique_lock lock(gate_mutex);
        ++entered;
        gate_condition.notify_all();
        gate_condition.wait(lock, [&] { return release; });
        return std::string("done");
      },
      options);
  auto create = live.submit("req-running-create", "SubmitCreateCheckpoint",
                            "container-a", "create");
  auto restore = live.submit("req-running-restore", "SubmitRestoreOrRestart",
                             "container-b", "restore");
  ASSERT_TRUE(create);
  ASSERT_TRUE(restore);
  {
    std::unique_lock lock(gate_mutex);
    ASSERT_TRUE(
        gate_condition.wait_for(lock, 2s, [&] { return entered == 2; }));
  }
  auto queued = live.submit("req-queued", "SubmitCreateCheckpoint",
                            "container-c", "queued");
  ASSERT_TRUE(queued);
  ASSERT_TRUE(std::filesystem::copy_file(
      live_path, crash_path,
      std::filesystem::copy_options::overwrite_existing));
  {
    std::lock_guard lock(gate_mutex);
    release = true;
  }
  gate_condition.notify_all();
  live.stop();

  auto recovered = OperationStore::create(
      crash_path,
      [](const OperationView &) -> std::expected<std::string, StoreError> {
        ADD_FAILURE() << "recovered operation must not execute";
        return std::string("unexpected");
      },
      options);
  ASSERT_TRUE(recovered) << recovered.error().message;

  for (const auto &id : {create->operation_id, restore->operation_id}) {
    auto view = (*recovered)->get(id);
    ASSERT_TRUE(view);
    EXPECT_EQ(view->state, OperationState::indeterminate);
    EXPECT_EQ(view->error_code, StoreErrorCode::execution_indeterminate);
    EXPECT_TRUE(view->indeterminate);
  }
  auto queued_view = (*recovered)->get(queued->operation_id);
  ASSERT_TRUE(queued_view);
  EXPECT_EQ(queued_view->state, OperationState::failed);
  EXPECT_EQ(queued_view->error_code, StoreErrorCode::interrupted);
  EXPECT_FALSE(queued_view->indeterminate);

  (*recovered)->stop();
  recovered->reset();
  auto recovered_again =
      OperationStore::create(crash_path, successful_executor());
  ASSERT_TRUE(recovered_again) << recovered_again.error().message;
  EXPECT_EQ((*recovered_again)->get(create->operation_id)->state,
            OperationState::indeterminate);
  EXPECT_EQ((*recovered_again)->get(queued->operation_id)->state,
            OperationState::failed);

  remove_path(live_path);
  remove_path(crash_path);
}

TEST(OperationStoreTest, AcceptsAndTruncatesOnlyAnUnterminatedFinalRecord) {
  const auto source_path = temp_path("-source");
  const auto torn_path = temp_path("-torn");
  const auto corrupt_path = temp_path("-corrupt");
  remove_path(source_path);
  remove_path(torn_path);
  remove_path(corrupt_path);

  std::string operation_id;
  {
    OperationStore store(source_path, successful_executor("complete"));
    auto submitted = store.submit("req-recovery", "SubmitCreateCheckpoint",
                                  "container", "payload");
    ASSERT_TRUE(submitted);
    operation_id = submitted->operation_id;
    ASSERT_TRUE(wait_for_terminal(store, operation_id));
  }
  const auto valid_journal = read_text(source_path);
  ASSERT_FALSE(valid_journal.empty());
  ASSERT_TRUE(std::filesystem::copy_file(source_path, torn_path));
  write_text(torn_path, "{\"request_id\":", std::ios::app);

  auto torn = OperationStore::create(torn_path, successful_executor());
  ASSERT_TRUE(torn) << torn.error().message;
  auto recovered = (*torn)->get(operation_id);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->state, OperationState::succeeded);
  torn->reset();
  const auto repaired = read_text(torn_path);
  ASSERT_FALSE(repaired.empty());
  EXPECT_EQ(repaired.back(), '\n');
  EXPECT_EQ(repaired.find("{\"request_id\":"), std::string::npos);

  const auto first_newline = valid_journal.find('\n');
  ASSERT_NE(first_newline, std::string::npos);
  write_text(corrupt_path, valid_journal + "{not-json}\n" +
                               valid_journal.substr(0, first_newline + 1));
  auto corrupt = OperationStore::create(corrupt_path, successful_executor());
  ASSERT_FALSE(corrupt);
  EXPECT_EQ(corrupt.error().code, StoreErrorCode::journal_corrupt);

  remove_path(source_path);
  remove_path(torn_path);
  remove_path(corrupt_path);
}

TEST(OperationStoreTest, ReportsUnsafeJournalAsAStartupError) {
  const auto path = temp_path("-unsafe-mode");
  remove_path(path);
  write_text(path, {});
  ASSERT_EQ(::chmod(path.c_str(), 0644), 0);

  auto created = OperationStore::create(path, successful_executor());
  ASSERT_FALSE(created);
  EXPECT_EQ(created.error().code, StoreErrorCode::journal_corrupt);
  EXPECT_THROW(
      { OperationStore store(path, successful_executor()); },
      std::runtime_error);

  remove_path(path);
}

TEST(OperationStoreTest,
     RejectsUnexpectedSchemaAndConflictingDuplicateOperationRecords) {
  const auto source_path = temp_path("-schema-source");
  const auto schema_path = temp_path("-schema");
  const auto conflict_path = temp_path("-conflict");
  remove_path(source_path);
  remove_path(schema_path);
  remove_path(conflict_path);

  {
    OperationStore store(source_path, successful_executor());
    auto submitted = store.submit("req-schema", "SubmitCreateCheckpoint",
                                  "container", "payload");
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(wait_for_terminal(store, submitted->operation_id));
  }
  const auto journal = read_text(source_path);
  const auto first_newline = journal.find('\n');
  const auto last_start = journal.rfind('\n', journal.size() - 2);
  ASSERT_NE(first_newline, std::string::npos);
  ASSERT_NE(last_start, std::string::npos);

  auto first = nlohmann::json::parse(journal.substr(0, first_newline));
  first["unexpected"] = true;
  write_text(schema_path,
             first.dump() + "\n" + journal.substr(first_newline + 1));
  auto schema = OperationStore::create(schema_path, successful_executor());
  ASSERT_FALSE(schema);
  EXPECT_EQ(schema.error().code, StoreErrorCode::journal_corrupt);

  auto conflicting = nlohmann::json::parse(
      journal.substr(last_start + 1, journal.size() - last_start - 2));
  conflicting["method"] = "SubmitRestoreOrRestart";
  write_text(conflict_path, journal + conflicting.dump() + "\n");
  auto conflict = OperationStore::create(conflict_path, successful_executor());
  ASSERT_FALSE(conflict);
  EXPECT_EQ(conflict.error().code, StoreErrorCode::journal_corrupt);

  remove_path(source_path);
  remove_path(schema_path);
  remove_path(conflict_path);
}

TEST(OperationStoreTest, BoundsRegistryWithoutForgettingDedupHistory) {
  const auto path = temp_path();
  remove_path(path);
  OperationStoreOptions options;
  options.max_records = 1;
  options.max_journal_entries = 4;
  options.worker_count = 1;

  OperationStore store(path, successful_executor(), options);
  auto first = store.submit("req-one", "SubmitCreateCheckpoint", "container",
                            "payload-one");
  ASSERT_TRUE(first);
  ASSERT_TRUE(wait_for_terminal(store, first->operation_id));

  auto full = store.submit("req-two", "SubmitCreateCheckpoint", "container",
                           "payload-two");
  ASSERT_FALSE(full);
  EXPECT_EQ(full.error().code, StoreErrorCode::resource_exhausted);

  auto duplicate = store.submit("req-one", "SubmitCreateCheckpoint",
                                "container", "payload-one");
  ASSERT_TRUE(duplicate);
  EXPECT_TRUE(duplicate->duplicate);
  EXPECT_EQ(duplicate->operation_id, first->operation_id);
  remove_path(path);
}

TEST(OperationStoreTest, InFlightLimitIncludesRunningOperations) {
  const auto path = temp_path();
  remove_path(path);
  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  bool entered = false;
  bool release = false;
  OperationStoreOptions options;
  options.max_in_flight = 1;
  options.worker_count = 1;

  OperationStore store(
      path,
      [&](const OperationView &) -> std::expected<std::string, StoreError> {
        std::unique_lock lock(gate_mutex);
        entered = true;
        gate_condition.notify_all();
        gate_condition.wait(lock, [&] { return release; });
        return std::string("done");
      },
      options);
  auto running = store.submit("req-running", "SubmitCreateCheckpoint",
                              "container", "running");
  ASSERT_TRUE(running);
  {
    std::unique_lock lock(gate_mutex);
    ASSERT_TRUE(gate_condition.wait_for(lock, 2s, [&] { return entered; }));
  }
  auto rejected = store.submit("req-rejected", "SubmitCreateCheckpoint",
                               "container", "rejected");
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, StoreErrorCode::resource_exhausted);
  {
    std::lock_guard lock(gate_mutex);
    release = true;
  }
  gate_condition.notify_all();
  ASSERT_TRUE(wait_for_terminal(store, running->operation_id));
  remove_path(path);
}

TEST(OperationStoreTest, RecoversEveryStableExecutorErrorCodeExactly) {
  constexpr StoreErrorCode codes[]{
      StoreErrorCode::invalid_argument,   StoreErrorCode::not_found,
      StoreErrorCode::already_exists,     StoreErrorCode::resource_exhausted,
      StoreErrorCode::conflict,           StoreErrorCode::permission_denied,
      StoreErrorCode::capture_failed,     StoreErrorCode::storage_error,
      StoreErrorCode::corrupted_snapshot, StoreErrorCode::incompatible_runtime,
      StoreErrorCode::restore_failed,     StoreErrorCode::cold_start_failed,
      StoreErrorCode::deadline_exceeded,  StoreErrorCode::unavailable,
      StoreErrorCode::internal,
  };

  for (const auto code : codes) {
    const auto path = temp_path("-stable-error");
    remove_path(path);
    std::string operation_id;
    {
      OperationStore store(
          path,
          [code](
              const OperationView &) -> std::expected<std::string, StoreError> {
            return std::unexpected(StoreError(code, "stable failure"));
          });
      auto submitted = store.submit(
          "req-stable-error", "SubmitCreateCheckpoint", "container", "payload");
      ASSERT_TRUE(submitted);
      operation_id = submitted->operation_id;
      auto final = wait_for_terminal(store, operation_id);
      ASSERT_TRUE(final);
      EXPECT_EQ(final->state, OperationState::failed);
      EXPECT_EQ(final->error_code, code);
    }

    auto reopened = OperationStore::create(path, successful_executor());
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto recovered = (*reopened)->get(operation_id);
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered->state, OperationState::failed);
    EXPECT_EQ(recovered->error_code, code);
    reopened->reset();
    remove_path(path);
  }
}

TEST(OperationStoreTest, PreservesStructuredExecutorIndeterminateFailure) {
  const auto path = temp_path();
  remove_path(path);
  std::string operation_id;
  {
    OperationStore store(
        path,
        [](const OperationView &) -> std::expected<std::string, StoreError> {
          return std::unexpected(
              StoreError(StoreErrorCode::execution_indeterminate,
                         "runtime outcome requires reconciliation", true));
        });
    auto submitted = store.submit("req-uncertain", "SubmitRestoreOrRestart",
                                  "container", "payload");
    ASSERT_TRUE(submitted);
    operation_id = submitted->operation_id;
    auto final = wait_for_terminal(store, operation_id);
    ASSERT_TRUE(final);
    EXPECT_EQ(final->state, OperationState::indeterminate);
    EXPECT_EQ(final->error_code, StoreErrorCode::execution_indeterminate);
    EXPECT_TRUE(final->indeterminate);
  }

  auto reopened = OperationStore::create(path, successful_executor());
  ASSERT_TRUE(reopened) << reopened.error().message;
  auto recovered = (*reopened)->get(operation_id);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->state, OperationState::indeterminate);
  EXPECT_EQ(recovered->error_code, StoreErrorCode::execution_indeterminate);
  EXPECT_TRUE(recovered->indeterminate);
  remove_path(path);
}

TEST(OperationStoreTest, ExecutesIndependentOperationsConcurrently) {
  const auto path = temp_path();
  remove_path(path);
  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  int active = 0;
  int maximum_active = 0;
  bool release = false;
  OperationStoreOptions options;
  options.worker_count = 2;
  options.max_in_flight = 2;

  OperationStore store(
      path,
      [&](const OperationView &) -> std::expected<std::string, StoreError> {
        std::unique_lock lock(gate_mutex);
        ++active;
        maximum_active = std::max(maximum_active, active);
        gate_condition.notify_all();
        gate_condition.wait(lock, [&] { return release; });
        --active;
        return std::string("done");
      },
      options);
  auto first =
      store.submit("req-concurrent-1", "SubmitCreateCheckpoint", "a", "one");
  auto second =
      store.submit("req-concurrent-2", "SubmitRestoreOrRestart", "b", "two");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);

  bool ran_concurrently = false;
  {
    std::unique_lock lock(gate_mutex);
    ran_concurrently =
        gate_condition.wait_for(lock, 2s, [&] { return active == 2; });
    release = true;
  }
  gate_condition.notify_all();
  EXPECT_TRUE(ran_concurrently);
  EXPECT_EQ(maximum_active, 2);
  ASSERT_TRUE(wait_for_terminal(store, first->operation_id));
  ASSERT_TRUE(wait_for_terminal(store, second->operation_id));
  remove_path(path);
}

TEST(OperationStoreTest, StopDrainsQueuedOperations) {
  const auto path = temp_path();
  remove_path(path);
  std::atomic<int> calls{0};
  OperationStoreOptions options;
  options.worker_count = 2;
  OperationStore store(
      path,
      [&](const OperationView &) -> std::expected<std::string, StoreError> {
        ++calls;
        std::this_thread::sleep_for(5ms);
        return std::string("done");
      },
      options);
  std::vector<std::string> operation_ids;
  for (int index = 0; index < 6; ++index) {
    auto submitted = store.submit("req-drain-" + std::to_string(index),
                                  "SubmitCreateCheckpoint", "container",
                                  "payload-" + std::to_string(index));
    ASSERT_TRUE(submitted);
    operation_ids.push_back(submitted->operation_id);
  }
  store.stop();
  EXPECT_EQ(calls.load(), 6);
  for (const auto &operation_id : operation_ids) {
    auto view = store.get(operation_id);
    ASSERT_TRUE(view);
    EXPECT_EQ(view->state, OperationState::succeeded);
  }
  remove_path(path);
}

TEST(OperationStoreTest, CompactsDuringRuntimeToStrictEntryBound) {
  const auto path = temp_path();
  remove_path(path);
  OperationStoreOptions options;
  options.max_records = 3;
  options.max_journal_entries = 4;
  options.worker_count = 1;
  options.max_journal_bytes = 1024U * 1024U;

  {
    OperationStore store(path, successful_executor(), options);
    for (int index = 0; index < 3; ++index) {
      auto submitted = store.submit("req-compact-" + std::to_string(index),
                                    "SubmitCreateCheckpoint", "container",
                                    "payload-" + std::to_string(index));
      ASSERT_TRUE(submitted);
      ASSERT_TRUE(wait_for_terminal(store, submitted->operation_id));
      EXPECT_LE(line_count(read_text(path)), options.max_journal_entries);
      EXPECT_LE(std::filesystem::file_size(path), options.max_journal_bytes);
    }
  }
  EXPECT_LE(line_count(read_text(path)), options.max_journal_entries);

  auto reopened = OperationStore::create(path, successful_executor(), options);
  ASSERT_TRUE(reopened) << reopened.error().message;
  reopened->reset();
  EXPECT_EQ(line_count(read_text(path)), options.max_records);
  remove_path(path);
}

TEST(OperationStoreTest,
     StartupRemovesOnlyExactSafeStaleCompactionFilesAndRetriesCollisions) {
  const auto directory = temp_directory("-stale-compaction");
  const auto journal = directory / "operations.jsonl";
  {
    OperationStore store(journal.string(), successful_executor());
  }

  const auto safe_first = compaction_path(journal, 123U, 456U);
  const auto safe_second = compaction_path(journal, 0U, 7U);
  write_mode(safe_first, "stale", 0600);
  write_mode(safe_second, "stale", 0600);

  const auto malformed =
      std::filesystem::path(journal.string() + ".compact.123");
  const auto extra_component =
      std::filesystem::path(journal.string() + ".compact.1.2.3");
  const auto nondigit =
      std::filesystem::path(journal.string() + ".compact.pid.1");
  const auto foreign = directory / "other.jsonl.compact.1.2";
  write_mode(malformed, "unknown", 0600);
  write_mode(extra_component, "unknown", 0600);
  write_mode(nondigit, "unknown", 0600);
  write_mode(foreign, "unknown", 0600);

  const auto unsafe_mode = compaction_path(journal, 10U, 20U);
  write_mode(unsafe_mode, "unsafe", 0644);
  const auto hard_link_source = directory / "hard-link-source";
  const auto hard_link = compaction_path(journal, 30U, 40U);
  write_mode(hard_link_source, "unsafe", 0600);
  std::filesystem::create_hard_link(hard_link_source, hard_link);
  const auto directory_candidate = compaction_path(journal, 50U, 60U);
  ASSERT_TRUE(std::filesystem::create_directory(directory_candidate));

  const auto symlink_target = directory / "symlink-target";
  write_mode(symlink_target, "target", 0600);
  const auto sequence_collision =
      compaction_path(journal, static_cast<unsigned long long>(::getpid()), 0U);
  std::filesystem::create_symlink(symlink_target, sequence_collision);

  auto reopened =
      OperationStore::create(journal.string(), successful_executor());
  ASSERT_TRUE(reopened) << reopened.error().message;
  reopened->reset();

  EXPECT_FALSE(std::filesystem::exists(safe_first));
  EXPECT_FALSE(std::filesystem::exists(safe_second));
  EXPECT_TRUE(std::filesystem::exists(malformed));
  EXPECT_TRUE(std::filesystem::exists(extra_component));
  EXPECT_TRUE(std::filesystem::exists(nondigit));
  EXPECT_TRUE(std::filesystem::exists(foreign));
  EXPECT_TRUE(std::filesystem::exists(unsafe_mode));
  EXPECT_TRUE(std::filesystem::exists(hard_link));
  EXPECT_TRUE(std::filesystem::is_directory(directory_candidate));
  EXPECT_TRUE(std::filesystem::is_symlink(
      std::filesystem::symlink_status(sequence_collision)));
  EXPECT_TRUE(std::filesystem::exists(symlink_target));
  remove_path(directory.string());
}

TEST(OperationStoreTest, BoundedStaleCleanupCompletesAcrossReopens) {
  const auto directory = temp_directory("-bounded-stale-compaction");
  const auto journal = directory / "operations.jsonl";
  std::string operation_id;
  {
    OperationStore store(journal.string(), successful_executor("persisted"));
    auto submitted = store.submit(
        "req-before-cleanup", "SubmitCreateCheckpoint", "container", "payload");
    ASSERT_TRUE(submitted);
    operation_id = submitted->operation_id;
    ASSERT_TRUE(wait_for_terminal(store, operation_id));
  }

  std::vector<std::filesystem::path> stale_files;
  for (unsigned long long sequence = 1U; sequence <= 3U; ++sequence) {
    stale_files.push_back(compaction_path(journal, 999U, sequence));
    write_mode(stale_files.back(), "stale", 0600);
  }
  const auto remaining = [&] {
    return static_cast<std::size_t>(
        std::ranges::count_if(stale_files, [](const auto &path) {
          return std::filesystem::exists(path);
        }));
  };

  OperationStoreOptions options;
  options.max_compaction_scan_entries = 16;
  options.max_compaction_cleanup_files = 1;
  options.max_compaction_name_attempts = 4;

  auto first =
      OperationStore::create(journal.string(), successful_executor(), options);
  ASSERT_FALSE(first);
  EXPECT_EQ(first.error().code, StoreErrorCode::resource_exhausted);
  EXPECT_EQ(remaining(), 2U);

  auto second =
      OperationStore::create(journal.string(), successful_executor(), options);
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().code, StoreErrorCode::resource_exhausted);
  EXPECT_EQ(remaining(), 1U);

  auto third =
      OperationStore::create(journal.string(), successful_executor(), options);
  ASSERT_TRUE(third) << third.error().message;
  EXPECT_EQ(remaining(), 0U);
  const auto recovered = (*third)->get(operation_id);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->state, OperationState::succeeded);
  EXPECT_EQ(recovered->result, "persisted");
  third->reset();
  remove_path(directory.string());
}

TEST(OperationStoreTest, CompactionCleanupScanBoundIsReopenSafe) {
  const auto directory = temp_directory("-compaction-scan-bound");
  const auto journal = directory / "operations.jsonl";
  {
    OperationStore store(journal.string(), successful_executor());
  }
  const auto unrelated = directory / "unrelated-entry";
  write_mode(unrelated, "preserve", 0600);

  OperationStoreOptions options;
  options.max_compaction_scan_entries = 1;
  auto bounded =
      OperationStore::create(journal.string(), successful_executor(), options);
  ASSERT_FALSE(bounded);
  EXPECT_EQ(bounded.error().code, StoreErrorCode::resource_exhausted);
  EXPECT_TRUE(std::filesystem::exists(unrelated));

  options.max_compaction_scan_entries = 8;
  auto reopened =
      OperationStore::create(journal.string(), successful_executor(), options);
  ASSERT_TRUE(reopened) << reopened.error().message;
  reopened->reset();
  EXPECT_TRUE(std::filesystem::exists(unrelated));
  remove_path(directory.string());
}

TEST(OperationStoreTest, CompactionNameRetriesAreBoundedAndReopenSafe) {
  const auto directory = temp_directory("-compaction-name-retry");
  const auto journal = directory / "operations.jsonl";
  const auto target = directory / "collision-target";
  write_mode(target, "target", 0600);
  const auto process = static_cast<unsigned long long>(::getpid());
  const auto first_collision = compaction_path(journal, process, 0U);
  const auto second_collision = compaction_path(journal, process, 1U);
  std::filesystem::create_symlink(target, first_collision);
  std::filesystem::create_symlink(target, second_collision);

  OperationStoreOptions options;
  options.max_compaction_scan_entries = 16;
  options.max_compaction_cleanup_files = 4;
  options.max_compaction_name_attempts = 2;

  auto exhausted =
      OperationStore::create(journal.string(), successful_executor(), options);
  ASSERT_FALSE(exhausted);
  EXPECT_EQ(exhausted.error().code, StoreErrorCode::resource_exhausted);
  EXPECT_TRUE(std::filesystem::is_symlink(
      std::filesystem::symlink_status(first_collision)));
  EXPECT_TRUE(std::filesystem::is_symlink(
      std::filesystem::symlink_status(second_collision)));

  ASSERT_TRUE(std::filesystem::remove(second_collision));
  auto reopened =
      OperationStore::create(journal.string(), successful_executor(), options);
  ASSERT_TRUE(reopened) << reopened.error().message;
  reopened->reset();
  EXPECT_TRUE(std::filesystem::is_symlink(
      std::filesystem::symlink_status(first_collision)));
  EXPECT_TRUE(std::filesystem::exists(target));
  remove_path(directory.string());
}

TEST(OperationStoreTest, TerminalPersistFailureCannotAppearSucceeded) {
  const auto path = temp_path();
  remove_path(path);
  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  bool entered = false;
  bool release = false;
  OperationStoreOptions options;
  options.worker_count = 1;

  OperationStore store(
      path,
      [&](const OperationView &) -> std::expected<std::string, StoreError> {
        std::unique_lock lock(gate_mutex);
        entered = true;
        gate_condition.notify_all();
        gate_condition.wait(lock, [&] { return release; });
        return std::string("side-effect-complete");
      },
      options);
  auto submitted = store.submit("req-persist-failure", "SubmitCreateCheckpoint",
                                "container", "payload");
  ASSERT_TRUE(submitted);
  {
    std::unique_lock lock(gate_mutex);
    ASSERT_TRUE(gate_condition.wait_for(lock, 2s, [&] { return entered; }));
  }
  ASSERT_TRUE(std::filesystem::remove(path));
  ASSERT_TRUE(std::filesystem::create_directory(path));
  {
    std::lock_guard lock(gate_mutex);
    release = true;
  }
  gate_condition.notify_all();

  auto final = wait_for_terminal(store, submitted->operation_id);
  ASSERT_TRUE(final);
  EXPECT_EQ(final->state, OperationState::indeterminate);
  EXPECT_NE(final->state, OperationState::succeeded);
  EXPECT_EQ(final->error_code, StoreErrorCode::journal_io);
  EXPECT_TRUE(final->indeterminate);

  auto rejected = store.submit("req-after-journal-failure",
                               "SubmitCreateCheckpoint", "container", "next");
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, StoreErrorCode::journal_io);
  store.stop();
  remove_path(path);
}
} // namespace gravesentinel::ipc
