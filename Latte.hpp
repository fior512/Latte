#pragma once
#include <fstream>
#include <ios>
#pragma GCC optimize("O3")

#ifndef LATTE_DISABLE

  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <cmath>
  #include <cstdio>
  #include <cstring>
  #include <iomanip>
  #include <limits>
  #include <map>
  #include <memory>
  #include <mutex>
  #include <queue>
  #include <sstream>
  #include <string>
  #include <vector>


  #if defined(_MSC_VER)
    #include <intrin.h>
  #else
    #include <x86intrin.h>
  #endif

  #include <functional>
  #include <thread>

  #if defined(_MSC_VER)
    #include <windows.h>
  #else
    #include <unistd.h>
    #if defined(__linux__)
      #include <sys/syscall.h>
    #elif defined(__APPLE__)
      #include <pthread.h>
    #endif
  #endif

  #define LATTE_PULSE(id_str)                                    \
    do {                                                         \
      static thread_local Latte::RingBuffer* _l_rb = nullptr;    \
      static thread_local Latte::ThreadStorage* _l_ts = nullptr; \
      static thread_local uint64_t _l_last = 0;                  \
      if (__builtin_expect(!_l_rb, 0)) {                         \
        _l_ts = Latte::GetThreadStorage();                       \
        _l_rb = _l_ts->GetOrAdd(id_str);                         \
        _l_last = Latte::Intrinsic::RDTSC();                     \
      } else {                                                   \
        uint64_t _l_now = Latte::Intrinsic::RDTSC();             \
        _l_rb->push(                                             \
            _l_now - _l_last,                                    \
            _l_last,                                             \
            static_cast<uint8_t>(_l_ts->stack_ptr),              \
            Latte::Internal::CALIB_KEY_PULSE                     \
        );                                                       \
        _l_last = _l_now;                                        \
      }                                                          \
    } while (0)


  #define LATTE_FREQ(cycles_per_ns)                                          \
    do {                                                                     \
      struct timespec t1, t2;                                                \
      for (int _i = 0; _i < 1000000; ++_i)                                 \
        std::atomic_signal_fence(std::memory_order_seq_cst);               \
      clock_gettime(CLOCK_MONOTONIC_RAW, &t1);                               \
      uint64_t c1 = Latte::Intrinsic::RDTSC();                               \
      struct timespec start = t1;                                            \
      do { /*120ms*/                                                         \
        clock_gettime(CLOCK_MONOTONIC_RAW, &t2);                             \
        if ((t2.tv_sec - start.tv_sec) * 1000000000ULL +                     \
                (t2.tv_nsec - start.tv_nsec) >                               \
            120000000ULL)                                                    \
          break;                                                             \
      } while (1);                                                           \
      uint64_t c2 = Latte::Intrinsic::RDTSC();                               \
      clock_gettime(CLOCK_MONOTONIC_RAW, &t2);                               \
      double ns = (t2.tv_sec - t1.tv_sec) * 1e9 + (t2.tv_nsec - t1.tv_nsec); \
      cycles_per_ns = (ns > 0.0) ? (double)(c2 - c1) / ns : 1.0;             \
    } while (0)


namespace Latte {
using ID = const char*;
using Cycles = uint64_t;
constexpr size_t MAX_ACTIVE_SLOTS = 64;

constexpr size_t BUFFER_PWR = 16;
constexpr size_t MAX_SAMPLES = 1 << BUFFER_PWR;  // 65536
constexpr size_t BUFFER_MASK = MAX_SAMPLES - 1;

struct Intrinsic {
  __attribute__((always_inline)) static inline Cycles RDTSC() {
    return __rdtsc();
  }
  __attribute__((always_inline)) static inline Cycles RDTSCP() {
    unsigned int aux;
    return __rdtscp(&aux);
  }
  __attribute__((always_inline)) static inline Cycles LFENCE_RDTSCP() {
    _mm_lfence();
    unsigned int aux;
    return __rdtscp(&aux);
  }  // start
  __attribute__((always_inline)) static inline Cycles RDTSCP_LFENCE() {
    unsigned int aux;
    Cycles result = __rdtscp(&aux);
    _mm_lfence();
    return result;
  }  // stop
};



enum class Mode : uint8_t { Fast = 0, Mid = 1, Hard = 2 };

namespace Internal {
constexpr uint8_t CALIB_KEY_UNSET = 0xFF;
constexpr uint8_t CALIB_KEY_MIXED = 0xFE;
constexpr uint8_t CALIB_KEY_PULSE = 9;
constexpr size_t CALIB_KEY_COUNT = 10;

__attribute__((always_inline)) static inline uint8_t CalibKey(
    uint8_t start_mode, uint8_t stop_mode
) {
  return (start_mode < 3 && stop_mode < 3)
             ? static_cast<uint8_t>(start_mode * 3 + stop_mode)
             : CALIB_KEY_UNSET;
}



__attribute__((always_inline)) static inline void LFENCE() {
  #if defined(_MSC_VER)
  _mm_lfence();
  #else
  asm volatile("lfence" ::: "memory");
  #endif
}



// Calibration labels
// TODO: find an elegant replacement
inline constexpr char CALIB_FxF[] = "FxF";
inline constexpr char CALIB_FxM[] = "FxM";
inline constexpr char CALIB_FxH[] = "FxH";
inline constexpr char CALIB_MxF[] = "MxF";
inline constexpr char CALIB_MxM[] = "MxM";
inline constexpr char CALIB_MxH[] = "MxH";
inline constexpr char CALIB_HxF[] = "HxF";
inline constexpr char CALIB_HxM[] = "HxM";
inline constexpr char CALIB_HxH[] = "HxH";
inline constexpr char CALIB_PULSE[] = "PxP";

struct CleanResult {
  std::vector<double> values;  // sorted
  size_t outlier = 0;          // BUMED cleaned
  double cutoff = std::numeric_limits<double>::max();
};



inline CleanResult CleanData(const std::vector<double>& values) {
  CleanResult out;
  if (values.empty()) return out;

  std::vector<double> bucket_maxes;
  const size_t BUCKET_SIZE = 1000;

  for (size_t i = 0; i < values.size(); i += BUCKET_SIZE) {
    double b_max = 0;
    size_t end = std::min(i + BUCKET_SIZE, values.size());

    // Is bucket.size > 50%
    if ((end - i) < BUCKET_SIZE / 2) continue;

    for (size_t j = i; j < end; ++j) {
      if (values[j] > b_max) b_max = values[j];
    }
    bucket_maxes.push_back(b_max);
  }

  double cutoff = std::numeric_limits<double>::max();

  if (bucket_maxes.size() >= 4) {
    std::sort(bucket_maxes.begin(), bucket_maxes.end());

    const size_t n = bucket_maxes.size();
    const double q1 = bucket_maxes[n / 4];
    const double q3 = bucket_maxes[(n * 3) / 4];
    const double iqr = q3 - q1;

    cutoff = q3 + (3.0 * iqr);
    if (iqr == 0) cutoff = q3 * 1.5;
  } else if (!bucket_maxes.empty()) {
    cutoff =
        (*std::max_element(bucket_maxes.begin(), bucket_maxes.end())) * 1.5;
  }

  //BUMED filter
  out.values.reserve(values.size());
  for (double v : values) {
    if (v > cutoff)
      out.outlier++;
    else
      out.values.push_back(v);
  }

  if (out.values.empty()) {
    out.values = values;
    out.outlier = 0;
  }

  std::sort(out.values.begin(), out.values.end());
  out.cutoff = cutoff;
  return out;
}



inline double MedianFromSorted(const std::vector<double>& sorted) {
  if (sorted.empty()) return 0.0;
  const size_t n = sorted.size();
  return (n % 2 == 0) ? (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0
                      : sorted[n / 2];
}



inline uint64_t CurrentThreadId() {
  #if defined(__linux__)
  return static_cast<uint64_t>(::syscall(SYS_gettid));
  #elif defined(_MSC_VER)
  return static_cast<uint64_t>(::GetCurrentThreadId());
  #elif defined(__APPLE__)
  uint64_t tid = 0;
  pthread_threadid_np(nullptr, &tid);
  return tid;
  #else
  return static_cast<uint64_t>(
      std::hash<std::thread::id>{}(std::this_thread::get_id())
  );
  #endif
}



// OS id of process for DumpToJson
inline uint32_t CurrentProcessId() {
  #if defined(_MSC_VER)
  return static_cast<uint32_t>(::GetCurrentProcessId());
  #else
  return static_cast<uint32_t>(::getpid());
  #endif
}



}  // namespace Internal



struct alignas(64) RingBuffer {
  //durations
  Cycles duration[MAX_SAMPLES];
  Cycles start
      [MAX_SAMPLES];  // raw start timestamp (cycles, comparable via Manager::epoch)
  uint8_t depth
      [MAX_SAMPLES];  // number of Start()ed-but-not-yet-Stopped spans enclosing this sample
  size_t head = 0;

  // 0xFF: unset/unknown, 0xFE: mixed
  uint8_t calib_key = 0xFF;

  RingBuffer() { std::memset(duration, 0, sizeof(duration)); }

  __attribute__((always_inline)) inline void push(
      Cycles val, Cycles start_val, uint8_t depth_val, uint8_t key
  ) {
    if (calib_key == 0xFF)
      calib_key = key;
    else if (calib_key != key)
      calib_key = 0xFE;

    duration[head] = val;
    start[head] = start_val;
    depth[head] = depth_val;
    head = (head + 1) & BUFFER_MASK;
  }
};



struct ThreadStorage {
  ID stack_ids[MAX_ACTIVE_SLOTS];
  Cycles stack_starts[MAX_ACTIVE_SLOTS];
  uint8_t stack_modes[MAX_ACTIVE_SLOTS];  // Latte::Mode encoded
  size_t stack_ptr = 0;
  uint64_t tid = 0;  // OS thread id, captured at storage creation

  // for pointer comparisor
  std::map<ID, RingBuffer> history;
  __attribute__((always_inline)) inline RingBuffer* GetOrAdd(ID id) {
    return &history[id];
  }
};



struct Sample {
  Cycles duration;
  Cycles start;  // for DumpToJson
  uint8_t depth;
};



class Manager {
 public:
  std::mutex mutex;
  std::vector<ThreadStorage*> Threads;

  double cycles_per_ns = 1.0;  //1: undefined
  Cycles epoch = Intrinsic::RDTSC();

  static Manager& Get() {
    static Manager instance;
    return instance;
  }

  __attribute__((always_inline)) inline void EnsureCalibrated() {
    std::call_once(calibrate_once, [&]() { Calibrate(); });
  }

  __attribute__((always_inline)) inline Cycles CalibrationOffset(
      uint8_t key
  ) const {
    if (key >= Internal::CALIB_KEY_COUNT) return 0;
    if (!calib_valid[key]) return 0;
    return calib_offsets[key];
  }

  void Register(ThreadStorage* thread) {
    std::lock_guard<std::mutex> lock(mutex);
    Threads.push_back(thread);
  }

  std::vector<Cycles> ExtractRaw(ID id) {
    std::vector<Cycles> output;
    output.reserve(1024);

    std::lock_guard<std::mutex> lock(mutex);  // non blocking
    for (auto* thread : Threads) {            // over threadS
      auto it = thread->history.find(id);
      if (it == thread->history.end()) continue;

      for (auto& x : it->second.duration) {  //second is RingBuffer
        if (x > 0) output.push_back(x);
      }
    }
    return output;
  }

  std::map<ID, std::vector<Sample>> ExtractSamplesGlobal() {
    std::map<ID, std::vector<Sample>> global_buffer;
    std::lock_guard<std::mutex> lock(mutex);

    for (auto* thread : Threads) {
      for (auto& [id, ring] : thread->history) {
        std::vector<Sample>& buf_ = global_buffer[id];

        for (size_t i = 0; i < MAX_SAMPLES; ++i) {
          if (ring.duration[i] > 0)
            buf_.push_back({ring.duration[i], ring.start[i], ring.depth[i]});
        }
      }
    }
    return global_buffer;
  }

  void Calibrate();  //ctor bellow

 private:
  std::once_flag calibrate_once;
  std::array<Cycles, Internal::CALIB_KEY_COUNT> calib_offsets{};
  std::array<bool, Internal::CALIB_KEY_COUNT> calib_valid{};
};



inline ThreadStorage* GetThreadStorage() {
  static thread_local ThreadStorage* thread = nullptr;
  if (__builtin_expect(!thread, 0)) {
    thread = new ThreadStorage();
    thread->tid = Internal::CurrentThreadId();
    Manager::Get().Register(thread);
  }
  return thread;
}



namespace Internal {
inline RingBuffer* GetBuffer(ID id) { return GetThreadStorage()->GetOrAdd(id); }



}  // namespace Internal



template <Mode M, Cycles (*TimeFunc)()>
struct Recorder {
  __attribute__((always_inline)) static inline void Start(ID id) {
    ThreadStorage* thread = GetThreadStorage();
    if (__builtin_expect(thread->stack_ptr < MAX_ACTIVE_SLOTS, 1)) {
      thread->stack_starts[thread->stack_ptr] = TimeFunc();
      thread->stack_ids[thread->stack_ptr] = id;
      thread->stack_modes[thread->stack_ptr] = static_cast<uint8_t>(M);
      thread->stack_ptr++;
    }
  }

  __attribute__((always_inline)) static inline Cycles Stop(ID /*id*/) {
    Cycles end = TimeFunc();
    ThreadStorage* thread = GetThreadStorage();

    if (__builtin_expect(thread->stack_ptr > 0, 1)) {
      thread->stack_ptr--;

      const Cycles start_cycles = thread->stack_starts[thread->stack_ptr];
      Cycles elapsed = end - start_cycles;  // raw latency

      const uint8_t depth = static_cast<uint8_t>(thread->stack_ptr);
      const uint8_t start_mode = thread->stack_modes[thread->stack_ptr];
      const uint8_t stop_mode = static_cast<uint8_t>(M);
      const uint8_t key = Internal::CalibKey(start_mode, stop_mode);

      thread->history[thread->stack_ids[thread->stack_ptr]].push(
          elapsed, start_cycles, depth, key
      );
      return elapsed;
    }
    return 0;
  }
};



namespace Fast {
inline void Start(ID id) { Recorder<Mode::Fast, Intrinsic::RDTSC>::Start(id); }
inline void Stop(ID id) { Recorder<Mode::Fast, Intrinsic::RDTSC>::Stop(id); }
}  // namespace Fast



namespace Mid {
inline void Start(ID id) { Recorder<Mode::Mid, Intrinsic::RDTSCP>::Start(id); }
inline void Stop(ID id) { Recorder<Mode::Mid, Intrinsic::RDTSCP>::Stop(id); }
}  // namespace Mid



namespace Hard {
inline void Start(ID id) {
  Recorder<Mode::Hard, Intrinsic::LFENCE_RDTSCP>::Start(id);
}



inline void Stop(ID id) {
  Recorder<Mode::Hard, Intrinsic::RDTSCP_LFENCE>::Stop(id);
}
}  // namespace Hard



struct ModeAPI {
  void (*start)(ID);
  void (*stop)(ID);
  const char* name;
};



inline constexpr ModeAPI MODE_TABLE[3] = {
    {Fast::Start, Fast::Stop, "Fast"},
    {Mid::Start, Mid::Stop, "Mid"},
    {Hard::Start, Hard::Stop, "Hard"}
};



// FIFO
template <void (*StartF)(ID), void (*StopF)(ID)>
struct ScopeGuard {
  ID id_;
  __attribute__((always_inline)) explicit ScopeGuard(ID id) noexcept : id_(id) {
    StartF(id);
  }
  __attribute__((always_inline)) ~ScopeGuard() { StopF(id_); }
  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;
};



namespace Internal {
// id is the stringized expr
template <void (*StartF)(ID), void (*StopF)(ID), class F>
__attribute__((always_inline)) inline decltype(auto) TimedEval(
    ID id, F&& f
) {
  ScopeGuard<StartF, StopF> _g(id);
  return static_cast<F&&>(f)();
}
}  // namespace Internal



inline void Manager::Calibrate() {
  {
    LATTE_FREQ(cycles_per_ns);
  }
  constexpr int WARMUP_ITERS = 10000;  //naturally overwritten (ring)
  const int iters = (int)MAX_SAMPLES + WARMUP_ITERS;

  (void)GetThreadStorage();  // Force TLS init before sampling

  constexpr const char* CALIB_LABELS[3][3] = {
      {Internal::CALIB_FxF, Internal::CALIB_FxM, Internal::CALIB_FxH},
      {Internal::CALIB_MxF, Internal::CALIB_MxM, Internal::CALIB_MxH},
      {Internal::CALIB_HxF, Internal::CALIB_HxM, Internal::CALIB_HxH}
  };

  for (int start_mode = 0; start_mode < 3; ++start_mode) {
    for (int stop_mode = 0; stop_mode < 3; ++stop_mode) {
      const auto& start_api = MODE_TABLE[start_mode];
      const auto& stop_api = MODE_TABLE[stop_mode];
      const char* label = CALIB_LABELS[start_mode][stop_mode];

      for (int i = 0; i < iters; ++i) {
        Internal::LFENCE();
        start_api.start(label);
        stop_api.stop(label);
        Internal::LFENCE();
      }
    }
  }

  // PULSE SELF-OFFSET
  for (int i = 0; i < iters; ++i) {
    Internal::LFENCE();
    Latte::Fast::Start(Internal::CALIB_PULSE);
    LATTE_PULSE("InternalCalibPulse");
    Latte::Mid::Stop(Internal::CALIB_PULSE);
    Internal::LFENCE();
  }

  auto BUMED = [&](ID id) -> Cycles {  // Median(Min(Bucket[1'000] )): BUMED
    std::vector<Cycles> raw = ExtractRaw(id);
    if (raw.empty()) return 0;

    constexpr size_t BUCKET = 1000;
    const size_t full = (raw.size() / BUCKET) * BUCKET;
    if (full == 0) {
      return *std::min_element(raw.begin(), raw.end());
    }

    std::vector<Cycles> mins;
    mins.reserve(full / BUCKET);

    for (size_t i = 0; i < full; i += BUCKET) {
      Cycles m = std::numeric_limits<Cycles>::max();
      for (size_t j = i; j < i + BUCKET; ++j) {
        const Cycles v = raw[j];
        if (v > 0 && v < m) m = v;
      }
      if (m != std::numeric_limits<Cycles>::max()) mins.push_back(m);
    }

    if (mins.empty()) {
      return *std::min_element(raw.begin(), raw.end());
    }

    std::sort(mins.begin(), mins.end());
    const size_t n = mins.size();
    if (n & 1) return mins[n / 2];

    const unsigned __int128 a = mins[n / 2 - 1];
    const unsigned __int128 b = mins[n / 2];
    return (Cycles)((a + b + 1) / 2);
  };


  calib_offsets[Internal::CalibKey((uint8_t)Mode::Fast, (uint8_t)Mode::Fast)] =
      BUMED(Internal::CALIB_FxF);
  calib_offsets[Internal::CalibKey((uint8_t)Mode::Fast, (uint8_t)Mode::Mid)] =
      BUMED(Internal::CALIB_FxM);
  calib_offsets[Internal::CalibKey((uint8_t)Mode::Fast, (uint8_t)Mode::Hard)] =
      BUMED(Internal::CALIB_FxH);

  calib_offsets[Internal::CalibKey((uint8_t)Mode::Mid, (uint8_t)Mode::Fast)] =
      BUMED(Internal::CALIB_MxF);
  calib_offsets[Internal::CalibKey((uint8_t)Mode::Mid, (uint8_t)Mode::Mid)] =
      BUMED(Internal::CALIB_MxM);
  calib_offsets[Internal::CalibKey((uint8_t)Mode::Mid, (uint8_t)Mode::Hard)] =
      BUMED(Internal::CALIB_MxH);

  calib_offsets[Internal::CalibKey((uint8_t)Mode::Hard, (uint8_t)Mode::Fast)] =
      BUMED(Internal::CALIB_HxF);
  calib_offsets[Internal::CalibKey((uint8_t)Mode::Hard, (uint8_t)Mode::Mid)] =
      BUMED(Internal::CALIB_HxM);
  calib_offsets[Internal::CalibKey((uint8_t)Mode::Hard, (uint8_t)Mode::Hard)] =
      BUMED(Internal::CALIB_HxH);

  calib_offsets[Internal::CALIB_KEY_PULSE] = BUMED(Internal::CALIB_PULSE);

  for (size_t i = 0; i < Internal::CALIB_KEY_COUNT; ++i) {
    calib_valid[i] = true;
  }

  // cleanup calibration
  if (ThreadStorage* thread = GetThreadStorage()) {
    thread->history.erase(Internal::CALIB_FxF);
    thread->history.erase(Internal::CALIB_FxM);
    thread->history.erase(Internal::CALIB_FxH);
    thread->history.erase(Internal::CALIB_MxF);
    thread->history.erase(Internal::CALIB_MxM);
    thread->history.erase(Internal::CALIB_MxH);
    thread->history.erase(Internal::CALIB_HxF);
    thread->history.erase(Internal::CALIB_HxM);
    thread->history.erase(Internal::CALIB_HxH);
    thread->history.erase(Internal::CALIB_PULSE);
    thread->history.erase("InternalCalibPulse");
  }
}


inline std::vector<double> ToNs(const std::vector<Cycles>& samples) {
  Manager& mgr = Manager::Get();
  mgr.EnsureCalibrated();
  std::vector<double> out;
  out.reserve(samples.size());
  for (Cycles c : samples) out.push_back((double)c / mgr.cycles_per_ns);
  return out;
}

class SnapshotResult {
 public:
  SnapshotResult() = default;
  explicit SnapshotResult(std::vector<Cycles> samples)
      : samples_(std::move(samples)) {}

  std::vector<double> to_ns() const { return ToNs(samples_); }

  operator const std::vector<Cycles>&() const { return samples_; }
  bool empty() const { return samples_.empty(); }
  size_t size() const { return samples_.size(); }
  Cycles operator[](size_t i) const { return samples_[i]; }
  auto begin() const { return samples_.begin(); }
  auto end() const { return samples_.end(); }

 private:
  std::vector<Cycles> samples_;
};


inline SnapshotResult Snapshot(ID id) {
  return SnapshotResult(Manager::Get().ExtractRaw(id));
}


inline std::string FormatTime(double ns) {
  std::stringstream ss;
  ss << std::fixed << std::setprecision(2);
  if (ns < 1000.0)
    ss << ns << " ns";
  else if (ns < 1e6)
    ss << (ns / 1e3) << " us";
  else if (ns < 1e9)
    ss << (ns / 1e6) << " ms";
  else if (ns < 60e9)
    ss << (ns / 1e9) << " s";
  else
    ss << (ns / 60e9) << " min";

  return ss.str();
}



namespace Parameter {
enum Unit { Cycle, Time };
enum Data { Raw, Calibrated };
}  // namespace Parameter


inline Internal::CleanResult DataClean(const std::vector<double>& values) {
  return Internal::CleanData(values);
}


inline void DumpToStream(
    std::ostream& oss,
    Parameter::Unit unit = Parameter::Cycle,
    Parameter::Data data_mode = Parameter::Raw
) {
  Manager& mgr = Manager::Get();
  if (unit == Parameter::Time || data_mode == Parameter::Calibrated) {
    mgr.EnsureCalibrated();
  }

  struct Series {
    std::vector<double> values;
    uint8_t calib_key = Internal::CALIB_KEY_UNSET;
  };

  std::map<ID, Series> global_buffer;

  {  // TODO: modernize thread safety (at least fast extraction)
    std::lock_guard<std::mutex> lock(mgr.mutex);
    for (auto* thread : mgr.Threads) {
      for (auto& [id, ring] : thread->history) {
        Series& buf_ = global_buffer[id];

        if (buf_.calib_key == Internal::CALIB_KEY_UNSET)
          buf_.calib_key = ring.calib_key;
        else if (buf_.calib_key != ring.calib_key)
          buf_.calib_key = Internal::CALIB_KEY_MIXED;

        for (auto& elapsed_time : ring.duration) {
          if (elapsed_time > 0) buf_.values.push_back((double)elapsed_time);
        }
      }
    }
  }


  auto FormatLarge = [](double val) {
    const char* units[] = {"", "K", "M", "B", "T"};
    int unit_idx = 0;
    while (val >= 1000.0 && unit_idx < 4) {
      val /= 1000.0;
      unit_idx++;
    }
    std::ostringstream ss;
    if (unit_idx == 0)
      ss << std::fixed << std::setprecision(0) << val;
    else
      ss << std::fixed << std::setprecision(2) << val << " " << units[unit_idx];
    return ss.str();
  };

  auto ToDisp = [&](double cycles) {
    return (unit == Parameter::Time) ? FormatTime(cycles / mgr.cycles_per_ns)
                                     : FormatLarge(cycles);
  };

  // Column Widths
  const int C1 = 20;
  const int C2 = 9;
  const int C3 = 10;
  const int C4 = 10;
  const int C5 = 10;
  const int C6 = 8;
  const int C7 = 10;
  const int C8 = 10;
  const int C9 = 10;
  const int C_BY = 10;

  const int COL_COUNT = 10;
  const int TABLE_WIDTH = C1 + C2 + C3 + C4 + C5 + C6 + C7 + C8 + C9 + C_BY +
                          (3 * (COL_COUNT - 1) + 2);
  const std::string line(TABLE_WIDTH, '-');
  const std::string d_line(TABLE_WIDTH, '=');

  const char* LIGHT_GRAY = "\033[90m";
  const char* COLOR_RESET = "\033[0m";
  auto gray = [&](const std::string& s) {
    return std::string(LIGHT_GRAY) + s + COLOR_RESET;
  };

  auto write_row = [&](const std::vector<std::string>& cells) {
    oss << gray("|") << " ";
    for (size_t i = 0; i < cells.size(); ++i) {
      if (i > 0) oss << gray(" | ");
      oss << cells[i];
    }
    oss << " " << gray("|") << "\n";
  };
  auto col = [](const std::string& s, int width, bool left = false) {
    std::ostringstream ss;
    if (left)
      ss << std::left << std::setw(width) << s;
    else
      ss << std::right << std::setw(width) << s;
    std::string res = ss.str();
    return (res.size() > (size_t)width) ? res.substr(0, width) : res;
  };

  oss << "\n" << gray("#") << gray(d_line) << gray("#") << "\n";
  std::string title =
      "LATTE TELEMETRY [" +
      std::string((unit == Parameter::Time) ? "TIME" : "CYCLES") + "][" +
      std::string((data_mode == Parameter::Calibrated) ? "CAL" : "RAW") + "]";
  write_row({col(title, TABLE_WIDTH - 2, true)});
  oss << gray("#") << gray(d_line) << gray("#") << "\n";


  // removed self-measured overhead
  if (data_mode == Parameter::Calibrated) {
    auto off_str = [&](uint8_t sm, uint8_t em) -> std::string {
      const uint8_t k = Internal::CalibKey(sm, em);
      return ToDisp((double)mgr.CalibrationOffset(k));
    };

    auto off_pulse_str = [&]() -> std::string {
      return ToDisp((double)mgr.CalibrationOffset(Internal::CALIB_KEY_PULSE));
    };

    constexpr int MW = 14;
    auto mcol = [&](const std::string& s, int w, bool left = false) {
      std::ostringstream ss;
      if (left)
        ss << std::left << std::setw(w) << s;
      else
        ss << std::right << std::setw(w) << s;
      std::string res = ss.str();
      return (res.size() > (size_t)w) ? res.substr(0, w) : res;
    };

    const auto F = (uint8_t)Mode::Fast;
    const auto M = (uint8_t)Mode::Mid;
    const auto H = (uint8_t)Mode::Hard;

    const std::string END(82, ' ');

    write_row({col("SELF-OFFSET H[Start] x W[Stop]", TABLE_WIDTH - 2, true)});
    write_row(
        {mcol("", 10, true) + mcol("F", MW) + mcol("M", MW) + mcol("H", MW) +
         END}
    );
    write_row(
        {mcol("F", 10, true) + mcol(off_str(F, F), MW) +
         mcol(off_str(F, M), MW) + mcol(off_str(F, H), MW) + END}
    );
    write_row(
        {mcol("M", 10, true) + mcol(off_str(M, F), MW) +
         mcol(off_str(M, M), MW) + mcol(off_str(M, H), MW) + END}
    );
    write_row(
        {mcol("H", 10, true) + mcol(off_str(H, F), MW) +
         mcol(off_str(H, M), MW) + mcol(off_str(H, H), MW) + END}
    );
    write_row(
        {mcol("PULSE", 10, true) + mcol(off_pulse_str(), MW) + mcol("", MW) +
         mcol("", MW) + END}
    );
    oss << gray("|") << gray(line) << gray("|") << "\n";
  }
  write_row(
      {col("COMPONENT", C1, true),
       col("SAMPLES", C2),
       col("AVG", C3),
       col("MEDIAN", C4),
       col("STD DEV", C5),
       col("SKEW", C6),
       col("MIN", C7),
       col("MAX", C8),
       col("RANGE", C9),
       col("OUTLIER", C_BY)}
  );

  oss << gray("|") << gray(line) << gray("|") << "\n";
  for (auto& [id, series] : global_buffer) {
    if (series.values.empty()) continue;

    std::vector<double> adjusted;  // noise filtering
    adjusted.reserve(series.values.size());

    const double off = (data_mode == Parameter::Calibrated)
                           ? (double)mgr.CalibrationOffset(series.calib_key)
                           : 0.0;

    for (double v : series.values) {
      double x = v - off;
      if (x < 0.0) x = 0.0;
      adjusted.push_back(x);
    }

    Internal::CleanResult clean = Internal::CleanData(adjusted);
    std::vector<double>& clean_values = clean.values;
    size_t outlier_count = clean.outlier;

    const size_t n = clean_values.size();
    if (n == 0) continue;

    double sum = 0;
    for (double v : clean_values) sum += v;
    const double avg = sum / (double)n;
    const double median = Internal::MedianFromSorted(clean_values);

    double var_sum = 0, skew_sum = 0;
    for (double v : clean_values) {
      double d = v - avg;
      var_sum += d * d;
      skew_sum += (d * d * d);
    }

    const double std_dev = std::sqrt(var_sum / (double)n);
    const double skew =
        (n > 1 && std_dev > 1e-9)
            ? (skew_sum / (double)n) / (std_dev * std_dev * std_dev)
            : 0.0;

    std::ostringstream sk;
    sk << std::fixed << std::setprecision(2) << skew;

    const std::string component_name =
        (id != nullptr) ? std::string(id) : std::string("<null-id>");

    write_row(
        {col(component_name, C1, true),
         col(std::to_string(n), C2),
         col(ToDisp(avg), C3),
         col(ToDisp(median), C4),
         col(ToDisp(std_dev), C5),
         col(sk.str(), C6),
         col(ToDisp(clean_values.front()), C7),
         col(ToDisp(clean_values.back()), C8),
         col(ToDisp(clean_values.back() - clean_values.front()), C9),
         col(std::to_string(outlier_count), C_BY)}
    );
  }

  oss << gray("#") << gray(d_line) << gray("#") << std::endl;
}



inline void DumpToJson(const std::string& path) {
  Manager& mgr = Manager::Get();
  mgr.EnsureCalibrated();

  struct Track {
    ID id;
    uint64_t tid;
    std::vector<Sample> samples;
  };
  std::vector<Track> tracks;
  {
    std::lock_guard<std::mutex> lock(mgr.mutex);
    for (auto* thread : mgr.Threads) {
      for (auto& [id, ring] : thread->history) {
        Track track{id, thread->tid, {}};
        track.samples.reserve(MAX_SAMPLES);

        for (size_t k = 0; k < MAX_SAMPLES; ++k) {
          const size_t i = (ring.head + k) & BUFFER_MASK;

          if (ring.duration[i] > 0) {
            track.samples.push_back(
                {ring.duration[i], ring.start[i], ring.depth[i]}
            );
          }
        }
        if (!track.samples.empty()) {
          tracks.push_back(std::move(track));
        }
      }
    }
  }

  size_t total = 0;
  for (auto& t : tracks) total += t.samples.size();

  // K-way merge of sorted tracks by start time: O(N log k), k = #tracks
  struct Cursor {
    Cycles start;
    uint32_t track;
    uint32_t idx;
  };
  auto later = [](const Cursor& a, const Cursor& b) {
    return a.start > b.start;
  };
  std::priority_queue<Cursor, std::vector<Cursor>, decltype(later)> heap(later);
  for (uint32_t t = 0; t < tracks.size(); ++t)
    heap.push({tracks[t].samples[0].start, t, 0});

  std::string out;
  out.reserve(total * 128 + 16);
  out += "[\n";

  const double inv_cpns = 1.0 / mgr.cycles_per_ns;
  const uint32_t pid = Internal::CurrentProcessId();
  char row[256];
  while (!heap.empty()) {
    Cursor c = heap.top();
    heap.pop();
    const Track& t = tracks[c.track];
    const Sample& s = t.samples[c.idx];

    const double start_ns = (double)(s.start - mgr.epoch) * inv_cpns;
    const double dur_ns = (double)s.duration * inv_cpns;
    const int len = snprintf(
        row,
        sizeof(row),
        "  {\"name\": \"%s\", \"ph\": \"X\", \"pid\": %u, \"tid\": %llu, "
        "\"ts\": %.3f, \"dur\": %.3f,"
        " \"component\": \"%s\", \"sample_index\": %u, \"depth\": %u, "
        "\"start_ns\": %.2f, \"duration_ns\": %.2f}",
        t.id,
        pid,
        (unsigned long long)t.tid,
        start_ns / 1e3,
        dur_ns / 1e3,
        t.id,
        c.idx,
        (unsigned)s.depth,
        start_ns,
        dur_ns
    );
    out.append(row, (size_t)len);
    out += (--total > 0) ? ",\n" : "\n";

    if (c.idx + 1 < t.samples.size())
      heap.push({t.samples[c.idx + 1].start, c.track, c.idx + 1});
  }
  out += "]\n";

  std::ofstream file(path, std::ios::binary);
  file.write(out.data(), (std::streamsize)out.size());
}

}  // namespace Latte



  #define LATTE_CALIBRATE()                     \
    do {                                        \
      Latte::Manager::Get().EnsureCalibrated(); \
    } while (0)


  #define LATTE_CONCAT_(a, b) a##b
  #define LATTE_CONCAT(a, b) LATTE_CONCAT_(a, b)

  #define LATTE_RAII_TAG_(mode) LATTE_RAII_TAG__##mode
  #define LATTE_RAII_TAG(mode) LATTE_RAII_TAG_(mode)
  #define LATTE_RAII_TAG__ Fast
  #define LATTE_RAII_TAG__Fast Fast
  #define LATTE_RAII_TAG__Mid Mid
  #define LATTE_RAII_TAG__Hard Hard

  #define LATTE_RAII(mode)                    \
    ::Latte::ScopeGuard<                      \
        ::Latte::LATTE_RAII_TAG(mode)::Start, \
        ::Latte::LATTE_RAII_TAG(mode)::Stop>  \
    LATTE_CONCAT(_latte_raii_, __LINE__) {    \
      __func__                                \
    }

  #define LATTE_FIELD(expr)                                                  \
    ::Latte::Internal::TimedEval<::Latte::Fast::Start, ::Latte::Fast::Stop>( \
        #expr, [&]() -> decltype(auto) { return (expr); }                    \
    )  //transfer output


#else  // DEFINED(LATTE_DISABLE)
  #include <cstdint>
  #include <ostream>
  #include <string>
  #include <vector>

  #define LATTE_PULSE(id_str) \
    do {                      \
    } while (0)

  #define LATTE_FREQ(cycles_per_ns) \
    do {                            \
    } while (0)

  #define LATTE_CALIBRATE() \
    do {                    \
    } while (0)

  #define LATTE_RAII(mode) \
    do {                   \
    } while (0)  // statement position only

  #define LATTE_FIELD(expr) (expr)

namespace Latte {
using ID = const char*;
using Cycles = uint64_t;

namespace Fast {
inline void Start(ID) {}
inline void Stop(ID) {}
}  // namespace Fast

namespace Mid {
inline void Start(ID) {}
inline void Stop(ID) {}
}  // namespace Mid

namespace Hard {
inline void Start(ID) {}
inline void Stop(ID) {}
}  // namespace Hard

namespace Parameter {
enum Unit { Cycle, Time };
enum Data { Raw, Calibrated };
}  // namespace Parameter



namespace Internal {
struct CleanResult {
  std::vector<double> values;
  size_t outlier = 0;
  double cutoff = 0.0;
};
}  // namespace Internal


template <void (*)(ID), void (*)(ID)>
struct ScopeGuard {
  explicit ScopeGuard(ID) noexcept {}
};

struct SnapshotResult {
  SnapshotResult() = default;
  std::vector<double> to_ns() const { return {}; }
  bool empty() const { return true; }
  size_t size() const { return 0; }
  const Cycles* begin() const { return nullptr; }
  const Cycles* end() const { return nullptr; }
};

inline SnapshotResult Snapshot(ID) { return {}; }
inline std::vector<double> ToNs(const std::vector<Cycles>&) { return {}; }
inline std::string FormatTime(double) { return ""; }

inline Internal::CleanResult DataClean(const std::vector<double>&) {
  return {};
}

inline void DumpToStream(
    std::ostream&,
    Parameter::Unit = Parameter::Cycle,
    Parameter::Data = Parameter::Raw
) {}

inline void DumpToJson(const std::string& path) { (void)path; }

}  // namespace Latte

#endif  // LATTE_DISABLE
