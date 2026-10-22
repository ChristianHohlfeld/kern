#include "kern/core/base.h"

#include <cstdarg>
#include <cstdio>

#if defined(__wasm__)
#define KERN_ENV(name) __attribute__((import_module("kern_env"), import_name(#name)))
extern "C" {
KERN_ENV(now) double kern_env_now();
KERN_ENV(log) void kern_env_log(int level, const char* text, unsigned length);
}
#else
#include <chrono>
static double kern_env_now() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
static void kern_env_log(int level, const char* text, unsigned length) {
  std::fprintf(level >= 2 ? stderr : stdout, "%.*s\n", static_cast<int>(length), text);
}
#endif

namespace kern {

namespace {
char g_error[1024] = {0};
}

bool fail(const char* format, ...) {
  va_list args;
  va_start(args, format);
  std::vsnprintf(g_error, sizeof g_error, format, args);
  va_end(args);
  return false;
}

const char* last_error() { return g_error; }
void clear_error() { g_error[0] = 0; }

void log(LogLevel level, const char* format, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, format);
  const int n = std::vsnprintf(buffer, sizeof buffer, format, args);
  va_end(args);
  if (n > 0) kern_env_log(static_cast<int>(level), buffer, static_cast<unsigned>(min_of<int>(n, sizeof buffer - 1)));
}

f64 now_ms() { return kern_env_now(); }

}  // namespace kern
