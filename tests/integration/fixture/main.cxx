#include "tests/integration/fixture/counter_fixture.hxx"

#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
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

struct CounterRecord {
  std::string instance_tag;
  std::uint64_t counter{};
};

struct FileState {
  std::uint64_t records{};
  std::uint64_t counter{};
  std::string instance_tag;
};

[[nodiscard]] std::expected<CounterRecord, std::string>
parse_counter_record(std::string_view line) {
  constexpr std::string_view tag_prefix = "instance_tag=";
  constexpr std::string_view counter_separator = " counter=";
  if (!line.starts_with(tag_prefix)) {
    return std::unexpected("record does not start with instance_tag");
  }

  const auto separator = line.find(counter_separator, tag_prefix.size());
  if (separator == line.npos || separator == tag_prefix.size()) {
    return std::unexpected("record has no instance tag or counter");
  }
  const auto counter_text = line.substr(separator + counter_separator.size());
  std::uint64_t counter{};
  const auto parsed = std::from_chars(
      counter_text.data(), counter_text.data() + counter_text.size(), counter);
  if (counter_text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != counter_text.data() + counter_text.size()) {
    return std::unexpected("record counter is not an unsigned integer");
  }

  return CounterRecord{
      .instance_tag = std::string(
          line.substr(tag_prefix.size(), separator - tag_prefix.size())),
      .counter = counter,
  };
}

[[nodiscard]] std::expected<FileState, std::string>
append_and_read_state(std::string_view state_file,
                      std::string_view instance_tag, std::uint64_t counter) {
  {
    std::ofstream output{std::string(state_file),
                         std::ios::out | std::ios::app};
    if (!output.is_open()) {
      return std::unexpected("could not open state file for append: " +
                             std::string(state_file));
    }
    output << "instance_tag=" << instance_tag << " counter=" << counter << '\n';
    if (!output) {
      return std::unexpected("could not write state file: " +
                             std::string(state_file));
    }
    output.close();
    if (output.fail()) {
      return std::unexpected("could not close state file after writing: " +
                             std::string(state_file));
    }
  }

  std::ifstream input{std::string(state_file)};
  if (!input.is_open()) {
    return std::unexpected("could not open state file for reading: " +
                           std::string(state_file));
  }

  FileState state;
  std::string line;
  while (std::getline(input, line)) {
    if (input.eof()) {
      return std::unexpected("state file ends with an incomplete record: " +
                             std::string(state_file));
    }
    const auto record = parse_counter_record(line);
    if (!record) {
      return std::unexpected("invalid state file record " +
                             std::to_string(state.records + 1U) + ": " +
                             record.error());
    }
    if (state.records == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected("state file record count overflow");
    }
    ++state.records;
    state.counter = record->counter;
    state.instance_tag = record->instance_tag;
  }
  if (!input.eof()) {
    return std::unexpected("could not read state file: " +
                           std::string(state_file));
  }
  if (state.records == 0U || state.counter != counter ||
      state.instance_tag != instance_tag) {
    return std::unexpected("state file append verification failed: " +
                           std::string(state_file));
  }
  return state;
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
            << " interval_ms=" << options->interval.count()
            << " state_file=" << options->state_file
            << " instance_tag=" << options->instance_tag << std::endl;

  int exit_status = EXIT_SUCCESS;
  while (!stop_requested.load(std::memory_order_relaxed)) {
    observe_cpu(cpu_mask);
    const auto current_counter = counter.load(std::memory_order_relaxed);
    const auto file_state = append_and_read_state(
        options->state_file, options->instance_tag, current_counter);
    if (!file_state) {
      std::cerr << "fixture-error: " << file_state.error() << std::endl;
      exit_status = EXIT_FAILURE;
      stop_requested.store(true, std::memory_order_relaxed);
      break;
    }

    std::cout << "counter=" << current_counter << " pid=" << ::getpid()
              << " nonce=" << instance_nonce
              << " cpu_mask=" << cpu_mask.load(std::memory_order_relaxed)
              << " workers=" << options->worker_threads
              << " digest=" << work_digest.load(std::memory_order_relaxed)
              << " file_records=" << file_state->records
              << " file_counter=" << file_state->counter
              << " instance_tag=" << options->instance_tag << std::endl;
    std::this_thread::sleep_for(options->interval);
  }

  for (auto &worker : workers) {
    worker.join();
  }
  std::cerr << "fixture-stop counter="
            << counter.load(std::memory_order_relaxed) << std::endl;
  return exit_status;
}
