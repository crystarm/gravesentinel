#include "tests/integration/fixture/counter_fixture.hxx"

#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::atomic_bool stop_requested{false};
static_assert(std::atomic_bool::is_always_lock_free);

extern "C" void handle_signal(int) {
  stop_requested.store(true, std::memory_order_relaxed);
}

void observe_cpu(std::atomic<std::uint64_t> &cpu_mask) {
  const int cpu = ::sched_getcpu();
  if (cpu >= 0 && cpu < 64) {
    cpu_mask.fetch_or(std::uint64_t{1} << static_cast<unsigned>(cpu),
                      std::memory_order_relaxed);
  }
}

void run_worker(std::atomic<std::uint64_t> &counter,
                std::atomic<std::uint64_t> &work_digest,
                std::atomic<std::uint64_t> &cpu_mask,
                std::chrono::milliseconds interval, std::size_t worker_index) {
  std::uint64_t state =
      0x9E3779B97F4A7C15ULL ^ static_cast<std::uint64_t>(worker_index + 1U);
  while (!stop_requested.load(std::memory_order_relaxed)) {
    observe_cpu(cpu_mask);
    for (std::size_t iteration = 0; iteration < 50'000; ++iteration) {
      state ^= state << 13U;
      state ^= state >> 7U;
      state ^= state << 17U;
    }
    work_digest.fetch_xor(state, std::memory_order_relaxed);
    counter.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(interval);
  }
}

} // namespace

int main(int argc, char **argv) {
  std::vector<std::string_view> arguments;
  if (argc > 1) {
    arguments.reserve(static_cast<std::size_t>(argc - 1));
  }
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  const auto options =
      gravesentinel::ccm::test::parse_counter_options(arguments);
  if (!options) {
    std::cerr << "fixture-error: " << options.error() << '\n'
              << gravesentinel::ccm::test::counter_fixture_usage() << '\n';
    return EXIT_FAILURE;
  }
  if (options->show_help) {
    std::cout << gravesentinel::ccm::test::counter_fixture_usage() << '\n';
    return EXIT_SUCCESS;
  }

  static_cast<void>(std::signal(SIGINT, handle_signal));
  static_cast<void>(std::signal(SIGTERM, handle_signal));

  const auto instance_nonce =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  std::atomic<std::uint64_t> counter{0};
  std::atomic<std::uint64_t> work_digest{0};
  std::atomic<std::uint64_t> cpu_mask{0};
  std::vector<std::thread> workers;
  workers.reserve(options->worker_threads);
  for (std::size_t index = 0; index < options->worker_threads; ++index) {
    workers.emplace_back(run_worker, std::ref(counter), std::ref(work_digest),
                         std::ref(cpu_mask), options->interval, index);
  }

  std::cerr << "fixture-start pid=" << ::getpid()
            << " workers=" << options->worker_threads
            << " interval_ms=" << options->interval.count() << std::endl;

  while (!stop_requested.load(std::memory_order_relaxed)) {
    observe_cpu(cpu_mask);
    std::cout << "counter=" << counter.load(std::memory_order_relaxed)
              << " pid=" << ::getpid() << " nonce=" << instance_nonce
              << " cpu_mask=" << cpu_mask.load(std::memory_order_relaxed)
              << " workers=" << options->worker_threads
              << " digest=" << work_digest.load(std::memory_order_relaxed)
              << std::endl;
    std::this_thread::sleep_for(options->interval);
  }

  for (auto &worker : workers) {
    worker.join();
  }
  std::cerr << "fixture-stop counter="
            << counter.load(std::memory_order_relaxed) << std::endl;
  return EXIT_SUCCESS;
}
