// Minimal check macros for the fleet tests (no test framework dependency).
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace ftest {
inline int& Failures() {
  static int n = 0;
  return n;
}
inline int& Checks() {
  static int n = 0;
  return n;
}
}  // namespace ftest

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++ftest::Checks();                                                           \
    if (!(cond)) {                                                               \
      ++ftest::Failures();                                                       \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                            \
  } while (0)

#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    ++ftest::Checks();                                                                              \
    const auto va_ = (a);                                                                           \
    const auto vb_ = (b);                                                                           \
    if (!(va_ == vb_)) {                                                                            \
      ++ftest::Failures();                                                                          \
      std::printf("  FAIL %s:%d: %s == %s (%s vs %s)\n", __FILE__, __LINE__, #a, #b,                \
                  ftest::Str(va_).c_str(), ftest::Str(vb_).c_str());                                \
    }                                                                                               \
  } while (0)

namespace ftest {
inline std::string Str(const std::string& s) { return "\"" + s + "\""; }
inline std::string Str(const char* s) { return s ? Str(std::string(s)) : "null"; }
template <typename T>
std::string Str(const T& v) {
  return std::to_string(v);
}
inline int Finish(const char* name) {
  std::printf("%s: %d checks, %d failures\n", name, Checks(), Failures());
  return Failures() == 0 ? 0 : 1;
}
}  // namespace ftest

#define TEST(name) static void name()
#define RUN(name)                           \
  do {                                      \
    std::printf("[ RUN ] %s\n", #name);     \
    const int before_ = ftest::Failures();  \
    name();                                 \
    std::printf("[ %s ] %s\n", ftest::Failures() == before_ ? " OK " : "FAIL", #name); \
  } while (0)
