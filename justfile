CXX := "g++"
CXXFLAGS := "-O3 -march=native -std=c++17 -Wall -Wextra"
LDFLAGS := "-lpthread"
INCLUDE := "-I."

BIN_DIR := "bin"
TEST_DIR := "test"
BENCH_DIR := "bench"

# Caliper/Likwid install prefix (bench/caliper_bench.cpp, bench/annot_bench.cpp)
CALIPER_PREFIX := env_var("HOME") + "/.local"
# Tracy source (client is header + TracyClient.cpp, no install needed)
TRACY_PREFIX := env_var("HOME") + "/.local/src/tracy"

default: all

all: setup api-smoke accuracy sanity sanity-disabled
  @echo " All binaries compiled successfully"

setup:
  @mkdir -p {{BIN_DIR}}
  @echo "Created {{BIN_DIR}}/ directory"

api-smoke: setup
  @echo "-> Compiling api_smoke..."
  {{CXX}} {{CXXFLAGS}} {{INCLUDE}} {{TEST_DIR}}/api_smoke.cpp -o {{BIN_DIR}}/api_smoke {{LDFLAGS}}
  @echo "Built {{BIN_DIR}}/api_smoke"

# Manual benchmark harness, not part of `all`/`run`/CI: timing numbers are
# too noisy on shared runners (see .github/workflows/ci.yml header comment).
speedtest: setup
  @echo "-> Compiling speedtest..."
  {{CXX}} {{CXXFLAGS}} {{INCLUDE}} {{TEST_DIR}}/speedtest.cpp -o {{BIN_DIR}}/speedtest {{LDFLAGS}}
  @echo "Built {{BIN_DIR}}/speedtest"

accuracy: setup
  @echo "-> Compiling accuracy.cpp ..."
  {{CXX}} {{CXXFLAGS}} {{INCLUDE}} {{TEST_DIR}}/accuracy.cpp -o {{BIN_DIR}}/accuracy {{LDFLAGS}}

sanity: setup
  @echo "-> Compiling sanity.cpp ..."
  {{CXX}} {{CXXFLAGS}} {{INCLUDE}} {{TEST_DIR}}/sanity.cpp -o {{BIN_DIR}}/sanity {{LDFLAGS}}
  @echo "Built {{BIN_DIR}}/sanity"

sanity-disabled: setup
  @echo "-> Compiling sanity.cpp [LATTE_DISABLE] ..."
  {{CXX}} {{CXXFLAGS}} -DLATTE_DISABLE {{INCLUDE}} {{TEST_DIR}}/sanity.cpp -o {{BIN_DIR}}/sanity_disabled {{LDFLAGS}}
  @echo "Built {{BIN_DIR}}/sanity_disabled"



run-api-smoke: api-smoke
  @echo "-> Running api_smoke.cpp ..."
  @./{{BIN_DIR}}/api_smoke

run-speed: speedtest
  @echo "-> Running speedtest.cpp ..."
  @./{{BIN_DIR}}/speedtest

run-accuracy: accuracy
  @echo "-> Running accuracy.cpp ..."
  @./{{BIN_DIR}}/accuracy

run-sanity: sanity sanity-disabled
  @echo "-> Running sanity.cpp ..."
  @./{{BIN_DIR}}/sanity
  @echo "-> Running sanity.cpp [LATTE_DISABLE] ..."
  @./{{BIN_DIR}}/sanity_disabled

run: run-api-smoke run-accuracy run-sanity

bench-caliper: setup
  @echo "-> Compiling caliper_bench.cpp [Caliper: {{CALIPER_PREFIX}}]..."
  {{CXX}} {{CXXFLAGS}} {{INCLUDE}} -I{{CALIPER_PREFIX}}/include {{BENCH_DIR}}/caliper_bench.cpp -o {{BIN_DIR}}/caliper_bench -L{{CALIPER_PREFIX}}/lib -lcaliper -Wl,-rpath,{{CALIPER_PREFIX}}/lib {{LDFLAGS}}
  @echo "Built {{BIN_DIR}}/caliper_bench"

run-bench-caliper: bench-caliper
  @echo "-> Run 1/3: Caliper inactive"
  @./{{BIN_DIR}}/caliper_bench
  @echo "-> Run 2/3: CALI_CONFIG=runtime-report"
  @CALI_CONFIG=runtime-report ./{{BIN_DIR}}/caliper_bench
  @echo "-> Run 3/3: CALI_CONFIG=event-trace"
  @CALI_CONFIG=event-trace ./{{BIN_DIR}}/caliper_bench

# annot_bench: Latte vs Caliper vs Likwid vs Tracy vs chrono (Google Benchmark)
# Requires: libcaliper, liblikwid, libbenchmark in ~/.local, tracy source in ~/.local/src/tracy
bench-annot: setup
  @echo "-> Compiling annot_bench.cpp [Caliper + Likwid + Tracy + Google Benchmark]..."
  {{CXX}} {{CXXFLAGS}} -pthread {{INCLUDE}} -I{{CALIPER_PREFIX}}/include -I{{TRACY_PREFIX}}/public -DLIKWID_PERFMON -DTRACY_ENABLE {{BENCH_DIR}}/annot_bench.cpp {{TRACY_PREFIX}}/public/TracyClient.cpp -o {{BIN_DIR}}/annot_bench -L{{CALIPER_PREFIX}}/lib -lcaliper -llikwid -lbenchmark -Wl,-rpath,{{CALIPER_PREFIX}}/lib
  @echo "Built {{BIN_DIR}}/annot_bench"

# Full matrix: Part A (overhead) + Part B (measurement error).
# NOTE: event-trace writes a .cali trace in the CWD (removed here after R3).
# NOTE: for the realistic Tracy number, run tracy-capture while the client listens:
#   tracy-capture -o trace.tracy   (the client binds 127.0.0.1:8086 itself)
run-bench-annot: bench-annot
  @echo "-> R1: all inactive"
  @./{{BIN_DIR}}/annot_bench
  @echo "-> R2: CALI_CONFIG=runtime-report"
  @CALI_CONFIG=runtime-report ./{{BIN_DIR}}/annot_bench
  @echo "-> R3: CALI_CONFIG=event-trace (caliper cases only)"
  @CALI_CONFIG=event-trace ./{{BIN_DIR}}/annot_bench --benchmark_filter=caliper_ --benchmark_min_time=0.3s
  @rm -f ./*.cali
  @echo "-> R4: likwid-perfctr -m -g CLOCK -c 3"
  @{{CALIPER_PREFIX}}/bin/likwid-perfctr -m -g CLOCK -c 3 ./{{BIN_DIR}}/annot_bench
  @echo "-> E1: error analysis (truth / latte / chrono)"
  @./{{BIN_DIR}}/annot_bench --error
  @echo "-> E2: error analysis + CALI_CONFIG=runtime-report"
  @CALI_CONFIG=runtime-report ./{{BIN_DIR}}/annot_bench --error
  @echo "-> E3: error analysis under likwid-perfctr"
  @{{CALIPER_PREFIX}}/bin/likwid-perfctr -m -g CLOCK -c 3 ./{{BIN_DIR}}/annot_bench --error

clean:
  @rm -rf {{BIN_DIR}}
  @echo "Cleaned {{BIN_DIR}}/"

rebuild: clean all

check:
  @echo "-> Checking Latte.hpp syntax..."
  {{CXX}} {{CXXFLAGS}} -fsyntax-only Latte.hpp
  {{CXX}} {{CXXFLAGS}} -DLATTE_DISABLE -fsyntax-only Latte.hpp
  @echo "Header syntax is valid (enabled and LATTE_DISABLE)"

info:
  @echo "Compiler: {{CXX}}"
  @{{CXX}} --version | head -n1
  @echo "Flags: {{CXXFLAGS}} {{LDFLAGS}}"
  @echo "Architecture: $(uname -m)"
  @echo "CPU Info:"
  @lscpu | grep "Model name" || echo "  (lscpu not available)"

help:
  @just --list
