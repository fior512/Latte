// g++ -O2 -std=c++17 sanity.cpp -lpthread            (checks the instrumented build)
// g++ -O2 -std=c++17 -DLATTE_DISABLE sanity.cpp -lpthread  (checks the no-op build)
//
// Pass/fail checks for edge cases that are easy to get wrong: unmatched
// Stop(), unknown IDs, ring-buffer wraparound, concurrent registration, and
// LATTE_DISABLE actually compiling down to a true no-op.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../Latte.hpp"

static int g_checks = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #cond \
                << "\n";                                                   \
      std::exit(1);                                                        \
    }                                                                      \
  } while (0)


#ifndef LATTE_DISABLE

static void busy_spin(int iters) {
  volatile uint64_t sink = 0;
  for (int i = 0; i < iters; ++i) sink += i;
  (void)sink;
}


int main() {
  // Unknown/never-recorded ID must yield an empty snapshot, not a crash.
  CHECK(Latte::Snapshot("never_used_component").empty());

  // A Stop() with no matching Start() must not crash or corrupt state.
  Latte::Fast::Stop("orphan_stop");
  Latte::Mid::Stop("orphan_stop");
  Latte::Hard::Stop("orphan_stop");
  CHECK(Latte::Snapshot("orphan_stop").empty());

  // Basic Start/Stop must actually record positive-duration samples.
  for (int i = 0; i < 50; ++i) {
    Latte::Hard::Start("sanity_component");
    busy_spin(1000);
    Latte::Hard::Stop("sanity_component");
  }
  auto samples = Latte::Snapshot("sanity_component");
  CHECK(!samples.empty());
  for (auto v : samples) CHECK(v > 0);

  // Ring buffer must survive wraparound (push well past MAX_SAMPLES) without crashing,
  // and exposed samples must stay bounded by the buffer capacity.
  for (int i = 0; i < 70000; ++i) {
    Latte::Fast::Start("wraparound_component");
    Latte::Fast::Stop("wraparound_component");
  }
  auto wrapped = Latte::Snapshot("wraparound_component");
  CHECK(wrapped.size() <= 65536);

  LATTE_CALIBRATE();

  // ToNs must translate the whole snapshot through the backend-calibrated
  // frequency: one double per sample, positive, monotonic in the input.
  {
    auto ns_samples = Latte::ToNs(samples);
    CHECK(ns_samples.size() == samples.size());
    for (double v : ns_samples) CHECK(v > 0.0);

    std::vector<Latte::Cycles> probe = {1, 2, 3};
    auto probe_ns = Latte::ToNs(probe);
    CHECK(probe_ns.size() == 3);
    CHECK(probe_ns[0] > 0.0);
    CHECK(probe_ns[1] > probe_ns[0]);
    CHECK(probe_ns[2] > probe_ns[1]);
    CHECK(Latte::ToNs({}).empty());

    // Fluent form must agree with the free function.
    auto ns_method = Latte::Snapshot("sanity_component").to_ns();
    CHECK(ns_method.size() == samples.size());
    CHECK(ns_method == ns_samples);
  }

  // DumpToStream must not throw/crash on both raw and calibrated views.
  std::ostringstream oss;
  Latte::DumpToStream(oss, Latte::Parameter::Time, Latte::Parameter::Raw);
  Latte::DumpToStream(
      oss, Latte::Parameter::Cycle, Latte::Parameter::Calibrated
  );
  CHECK(!oss.str().empty());

  // Multiple threads registering and recording concurrently must not corrupt
  // Manager's thread list or drop samples (validates Register()'s mutex-
  // protected path). Respect the documented contract: only read (Snapshot)
  // after all worker threads have joined.
  constexpr int THREADS = 8;
  constexpr int ITERS_PER_THREAD = 2000;
  std::vector<std::thread> workers;
  std::vector<std::string> ids(THREADS);
  for (int t = 0; t < THREADS; ++t) {
    ids[t] = "thread_component_" + std::to_string(t);
  }
  for (int t = 0; t < THREADS; ++t) {
    workers.emplace_back([id = ids[t].c_str()]() {
      for (int i = 0; i < ITERS_PER_THREAD; ++i) {
        Latte::Mid::Start(id);
        busy_spin(50);
        Latte::Mid::Stop(id);
      }
    });
  }
  for (auto& w : workers) w.join();

  for (int t = 0; t < THREADS; ++t) {
    auto s = Latte::Snapshot(ids[t].c_str());
    CHECK(s.size() == static_cast<size_t>(ITERS_PER_THREAD));
  }

  // Depth must equal the number of currently-open spans, for both Start/Stop
  // and LATTE_PULSE (a pulse fired from inside N nested spans is not itself
  // a stack entry, but must still report depth N).
  Latte::Hard::Start("depth_outer");  // depth 0
  Latte::Mid::Start("depth_middle");  // depth 1
  Latte::Fast::Start("depth_inner");  // depth 2
  // LATTE_PULSE is keyed by call site (each source line owns its own static
  // state), so it must be called repeatedly from the same line to produce a
  // sample: the first call only primes it, the second measures the gap.
  for (int i = 0; i < 2; ++i) {
    LATTE_PULSE("depth_pulse");  // depth 3 (3 spans open: outer, middle, inner)
  }
  Latte::Fast::Stop("depth_inner");
  Latte::Mid::Stop("depth_middle");
  Latte::Hard::Stop("depth_outer");

  {
    auto samples = Latte::Manager::Get().ExtractSamplesGlobal();
    CHECK(samples.at("depth_outer").back().depth == 0);
    CHECK(samples.at("depth_middle").back().depth == 1);
    CHECK(samples.at("depth_inner").back().depth == 2);
    CHECK(samples.at("depth_pulse").back().depth == 3);

    // start must be non-decreasing across a sequential (non-wrapped) series.
    for (size_t i = 1; i < samples.at("sanity_component").size(); ++i) {
      CHECK(
          samples.at("sanity_component")[i].start >=
          samples.at("sanity_component")[i - 1].start
      );
    }
  }

  // DumpToJson must include the new depth/start_ns fields, and produce a
  // well-formed, non-empty JSON array. Written to the cwd rather than bin/ -
  // this test is also built directly with g++ (sanitizer CI jobs), where
  // bin/ (created by the justfile's `setup` recipe) doesn't exist.
  const std::string json_path = "sanity_dump.json";
  Latte::DumpToJson(json_path);
  {
    std::ifstream f(json_path);
    CHECK(f.good());
    std::string content(
        (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()
    );
    CHECK(!content.empty());
    size_t first = content.find_first_not_of(" \t\r\n");
    size_t last = content.find_last_not_of(" \t\r\n");
    CHECK(
        first != std::string::npos && content[first] == '[' &&
        content[last] == ']'
    );
    CHECK(content.find("\"depth\"") != std::string::npos);
    CHECK(content.find("\"start_ns\"") != std::string::npos);
    CHECK(content.find("\"duration_ns\"") != std::string::npos);
    // Start/Stop spans and LATTE_PULSE share one serialization schema: every
    // sample is a Chrome Trace complete event (ph: "X") with no per-row
    // discriminator. Pulses must not be emitted as instant events (ph: "i").
    // The recording thread's OS id must be preserved in every row (no tid: 0).
    CHECK(content.find("\"ph\": \"X\"") != std::string::npos);
    CHECK(content.find("\"ph\": \"i\"") == std::string::npos);
    CHECK(content.find("\"kind\"") == std::string::npos);
    CHECK(content.find("\"tid\": ") != std::string::npos);
    CHECK(content.find("\"tid\": 0") == std::string::npos);
    CHECK(content.find("\"pid\": ") != std::string::npos);
    CHECK(content.find("\"pid\": 0") == std::string::npos);
  }
  std::remove(json_path.c_str());

  // DumpToJson to a directory that doesn't exist must fail gracefully: no
  // crash/throw, and no file materializes.
  const std::string bad_path = "sanity_dump_missing_dir/sanity_dump.json";
  std::remove(bad_path.c_str());
  Latte::DumpToJson(bad_path);
  CHECK(!std::ifstream(bad_path).good());

  // LATTE_RAII: Start on construction, Stop on scope exit -> exactly one
  // sample per call, regardless of mode. Id is __func__ ("main" here).
  {
    auto before = Latte::Manager::Get().ExtractSamplesGlobal()["main"].size();
    {
      LATTE_RAII();
      busy_spin(100);
    }
    auto after = Latte::Manager::Get().ExtractSamplesGlobal()["main"].size();
    CHECK(after == before + 1);
  }
  {
    auto before = Latte::Manager::Get().ExtractSamplesGlobal()["main"].size();
    {
      LATTE_RAII(Hard);
      busy_spin(100);
    }
    auto after = Latte::Manager::Get().ExtractSamplesGlobal()["main"].size();
    CHECK(after == before + 1);
  }

  // LATTE_RAII must still Stop() on an early return, keeping the per-thread
  // stack balanced (the failure mode manual Start/Stop can't guard against).
  // Inside a lambda, __func__ is the closure's call operator name
  // ("operator()"), not the enclosing function's - that's the id recorded.
  auto raii_early_return = [](bool early) -> int {
    LATTE_RAII(Mid);
    if (early) return 1;
    busy_spin(10);
    return 2;
  };
  {
    const size_t depth_before = Latte::GetThreadStorage()->stack_ptr;
    auto before =
        Latte::Manager::Get().ExtractSamplesGlobal()["operator()"].size();
    CHECK(raii_early_return(true) == 1);
    CHECK(raii_early_return(false) == 2);
    CHECK(Latte::GetThreadStorage()->stack_ptr == depth_before);
    auto after =
        Latte::Manager::Get().ExtractSamplesGlobal()["operator()"].size();
    CHECK(after == before + 2);
  }

  // Nested LATTE_RAII spans across different modes must pop in LIFO order
  // and leave the stack balanced.
  {
    const size_t depth_before = Latte::GetThreadStorage()->stack_ptr;
    {
      LATTE_RAII(Hard);
      {
        LATTE_RAII(Mid);
        {
          LATTE_RAII();
          busy_spin(10);
        }
      }
    }
    CHECK(Latte::GetThreadStorage()->stack_ptr == depth_before);
  }

  // LATTE_FIELD must run expr, return its value, and record under the
  // callee expr, not the caller. The id is the stringized literal, so
  // Snapshot() resolves it by address.
  {
    auto add = [](int a, int b) { return a + b; };
    const size_t before = Latte::Snapshot("add(19, 23)").size();
    const size_t caller_before = Latte::Snapshot("main").size();
    int out = LATTE_FIELD(add(19, 23));
    CHECK(out == 42);
    CHECK(Latte::Snapshot("add(19, 23)").size() == before + 1);
    CHECK(Latte::Snapshot("main").size() == caller_before);
  }

  // LATTE_FIELD preserves value category: an lvalue expr comes back as a
  // reference to the same object, not a copy.
  {
    int v = 7;
    int& r = LATTE_FIELD(v);
    r = 9;
    CHECK(v == 9);
    CHECK(&r == &v);
  }

  // A parenthesized expr must keep its full text as id. Truncating at the
  // first '(' used to yield an empty id here.
  {
    int a = 1, b = 2, c = 3;
    CHECK(LATTE_FIELD((a + b) * c) == 9);
    CHECK(Latte::Snapshot("(a + b) * c").size() == 1);
  }

  // Two call sites sharing one expr text share one id, so their samples
  // land in the same series.
  {
    std::vector<int> v{1, 2, 3};
    LATTE_FIELD(v.size());
    LATTE_FIELD(v.size());
    CHECK(Latte::Snapshot("v.size()").size() == 2);
  }

  std::cout << "sanity (enabled build): " << g_checks << " checks passed\n";
  return 0;
}


#else  // LATTE_DISABLE

int main() {
  // In a disabled build every entry point must be a true no-op: it must
  // compile with the same call sites as the enabled build (this is itself
  // part of the check) and must never touch state or crash.
  Latte::Fast::Start("x");
  Latte::Fast::Stop("x");
  Latte::Mid::Start("x");
  Latte::Mid::Stop("x");
  Latte::Hard::Start("x");
  Latte::Hard::Stop("x");
  LATTE_PULSE("x");
  LATTE_CALIBRATE();
  {
    LATTE_RAII();
  }
  {
    LATTE_RAII(Hard);
  }
  CHECK(LATTE_FIELD(1 + 1) == 2);

  CHECK(Latte::Snapshot("x").empty());
  CHECK(Latte::Snapshot("x").to_ns().empty());
  CHECK(Latte::ToNs({1, 2}).empty());
  CHECK(Latte::FormatTime(123.0).empty());
  CHECK(Latte::DataClean({1.0, 2.0, 3.0}).values.empty());

  std::ostringstream oss;
  Latte::DumpToStream(oss);
  CHECK(oss.str().empty());

  std::cout << "sanity (disabled build): " << g_checks << " checks passed\n";
  return 0;
}


#endif
