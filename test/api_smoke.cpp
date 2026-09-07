// g++ -O3 -march=native -std=c++17 api_smoke.cpp -lpthread
//
// Exercises every public API once, at trivial iteration counts. Replaces
// speedtest.cpp (a benchmark harness) and testcase.cpp (a market-maker
// simulation): both only ran as compile+run+exit-0 smoke checks in CI,
// so their compute cost bought nothing CI actually verified. This asserts
// real postconditions instead, for a fraction of the CI time.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
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

static int add(int a, int b) { return a + b; }

int main() {
  // Manual Start/Stop, all 3 modes.
  Latte::Fast::Start("api_fast");
  Latte::Fast::Stop("api_fast");
  Latte::Mid::Start("api_mid");
  Latte::Mid::Stop("api_mid");
  Latte::Hard::Start("api_hard");
  Latte::Hard::Stop("api_hard");
  CHECK(Latte::Snapshot("api_fast").size() == 1);
  CHECK(Latte::Snapshot("api_mid").size() == 1);
  CHECK(Latte::Snapshot("api_hard").size() == 1);

  // LATTE_RAII: default mode and an explicit mode.
  { LATTE_RAII(); }
  { LATTE_RAII(Hard); }
  CHECK(Latte::Snapshot("main").size() == 2);

  // LATTE_FIELD: runs expr and returns its value unchanged.
  CHECK(LATTE_FIELD(add(1, 2)) == 3);

  // LATTE_PULSE: first call primes, second measures.
  for (int i = 0; i < 2; ++i) LATTE_PULSE("api_pulse");
  CHECK(Latte::Snapshot("api_pulse").size() == 1);

  // LATTE_FREQ / manual cycles-to-ns translation.
  double cycles_per_ns = 0.0;
  LATTE_FREQ(cycles_per_ns);
  CHECK(cycles_per_ns > 0.0);

  // LATTE_CALIBRATE, Snapshot/ToNs/.to_ns(), FormatTime, DataClean.
  LATTE_CALIBRATE();
  auto raw = Latte::Snapshot("api_fast");
  auto ns_free = Latte::ToNs(raw);
  auto ns_method = raw.to_ns();
  CHECK(ns_free.size() == raw.size());
  CHECK(ns_method == ns_free);
  CHECK(!Latte::FormatTime(ns_free.empty() ? 0.0 : ns_free[0]).empty());
  CHECK(Latte::DataClean({1.0, 2.0, 3.0}).values.size() == 3);

  // DumpToStream: both Unit x Data combinations actually used in the docs.
  std::ostringstream oss;
  Latte::DumpToStream(oss, Latte::Parameter::Time, Latte::Parameter::Raw);
  Latte::DumpToStream(oss, Latte::Parameter::Cycle, Latte::Parameter::Calibrated);
  CHECK(!oss.str().empty());

  // DumpToJson: well-formed, non-empty.
  const std::string json_path = "api_smoke_dump.json";
  Latte::DumpToJson(json_path);
  {
    std::ifstream f(json_path);
    CHECK(f.good());
    std::string content(
        (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()
    );
    CHECK(!content.empty());
  }
  std::remove(json_path.c_str());

  std::cout << "api_smoke: " << g_checks << " checks passed\n";
  return 0;
}
