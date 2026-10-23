#include "src/ipc/operation_store.hxx"
#include <atomic>
#include <filesystem>
#include <gtest/gtest.h>
#include <thread>
#include <unistd.h>

namespace gravesentinel::ipc {
namespace {
std::string temp_path() {
  return "/tmp/gravesentinel-journal-" + std::to_string(::getpid()) + ".jsonl";
}
} // namespace
TEST(OperationStoreTest, DeduplicatesSameRequestAndRejectsDifferentPayload) {
  const auto path = temp_path();
  std::filesystem::remove(path);
  std::atomic<int> calls{0};
  {
    OperationStore store(
        path,
        [&](const OperationView &) -> std::expected<std::string, StoreError> {
          ++calls;
          return std::string("ok");
        });
    auto a = store.submit("req-1", "SubmitCreateCheckpoint", "container-1",
                          "Create:container-1");
    ASSERT_TRUE(a);
    auto duplicate = store.submit("req-1", "SubmitCreateCheckpoint",
                                  "container-1", "Create:container-1");
    ASSERT_TRUE(duplicate);
    EXPECT_TRUE(duplicate->duplicate);
    EXPECT_EQ(a->operation_id, duplicate->operation_id);
    EXPECT_FALSE(store.submit("req-1", "SubmitCreateCheckpoint", "container-1",
                              "Create:container-2"));
    for (int i = 0; i < 100; ++i) {
      auto v = store.get(a->operation_id);
      if (v && (v->state == OperationState::succeeded ||
                v->state == OperationState::failed))
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto final = store.get(a->operation_id);
    ASSERT_TRUE(final);
    EXPECT_EQ(final->state, OperationState::succeeded);
  }
  EXPECT_EQ(calls.load(), 1);
  std::filesystem::remove(path);
}
TEST(OperationStoreTest, ReopensJournalAndRetainsTerminalOperation) {
  const auto path = temp_path();
  std::filesystem::remove(path);
  std::string id;
  {
    OperationStore store(
        path,
        [](const OperationView &) -> std::expected<std::string, StoreError> {
          return std::string("done");
        });
    auto s = store.submit("req-2", "SubmitCreateCheckpoint", "c-2", "payload");
    ASSERT_TRUE(s);
    id = s->operation_id;
    for (int i = 0; i < 100; ++i) {
      auto v = store.get(id);
      if (v && v->state == OperationState::succeeded)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  {
    OperationStore store(
        path,
        [](const OperationView &) -> std::expected<std::string, StoreError> {
          ADD_FAILURE() << "recovered operation must not rerun";
          return std::string{};
        });
    auto v = store.get(id);
    ASSERT_TRUE(v);
    EXPECT_EQ(v->state, OperationState::succeeded);
    auto duplicate =
        store.submit("req-2", "SubmitCreateCheckpoint", "c-2", "payload");
    ASSERT_TRUE(duplicate);
    EXPECT_TRUE(duplicate->duplicate);
  }
  std::filesystem::remove(path);
}
} // namespace gravesentinel::ipc
