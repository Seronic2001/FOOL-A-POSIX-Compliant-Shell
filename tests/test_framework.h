#pragma once

// Minimal single-header test framework for FOOL.
//
// Usage:
//   #include "test_framework.h"
//
//   TEST(parser, splits_on_semicolons) {
//     ParseResult r = parse_input("a; b");
//     CHECK(r.ok());
//     CHECK_EQ(r.commands.size(), 3);  // a, b, sentinel
//   }
//
//   int main() { return run_all_tests(); }
//
// Each TEST registers itself with a static initializer; failures abort the
// current test (via longjmp) and testing continues with the next one.

#include <csetjmp>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace testfw {

// Compares values without triggering -Wsign-compare when one side is a
// signed literal and the other an unsigned size (e.g. vec.size() == 3).
template <typename A, typename B>
bool equal_values(const A& a, const B& b) {
  if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
    using C = std::common_type_t<A, B>;
    return static_cast<C>(a) == static_cast<C>(b);
  } else {
    return a == b;
  }
}

struct TestCase {
  const char* suite;
  const char* name;
  void (*fn)(void);
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

inline int& failures() {
  static int f = 0;
  return f;
}

inline int& current_checks() {
  static int c = 0;
  return c;
}

inline jmp_buf& jump_target() {
  static jmp_buf jb;
  return jb;
}

inline int run_all_tests() {
  int total = 0;
  for (const TestCase& tc : registry()) {
    ++total;
    current_checks() = 0;
    if (setjmp(jump_target()) == 0) {
      tc.fn();
      std::printf("[ PASS ] %s.%s\n", tc.suite, tc.name);
    } else {
      ++failures();
      std::printf("[ FAIL ] %s.%s\n", tc.suite, tc.name);
    }
  }
  std::printf("\n%d tests, %d failures\n", total, failures());
  return failures() == 0 ? 0 : 1;
}

}  // namespace testfw

#define TEST(suite_name, test_name)                                        \
  static void suite_name##_##test_name##_impl(void);                       \
  static const bool suite_name##_##test_name##_registered = [] {           \
    testfw::registry().push_back(                                          \
        {#suite_name, #test_name, &suite_name##_##test_name##_impl});      \
    return true;                                                           \
  }();                                                                     \
  static void suite_name##_##test_name##_impl(void)

// Abort the current test on failure. CHECK-style macros use a jump so one
// failing assertion doesn't mask later ones with cascading crashes.
#define TEST_FAIL_MSG(msg)                                                 \
  do {                                                                     \
    std::printf("    %s:%d: %s\n", __FILE__, __LINE__, (msg).c_str());     \
    std::longjmp(testfw::jump_target(), 1);                                \
  } while (0)

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      TEST_FAIL_MSG(std::string("CHECK failed: ") + #cond);                \
    }                                                                      \
    ++testfw::current_checks();                                            \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    auto va = (a);                                                         \
    auto vb = (b);                                                         \
    if (!testfw::equal_values(va, vb)) {                                   \
      TEST_FAIL_MSG(std::string("CHECK_EQ failed: ") + #a + " == " + #b);  \
    }                                                                      \
    ++testfw::current_checks();                                            \
  } while (0)

// Assert-style alias: same soft-fail semantics (test aborts via longjmp,
// remaining tests still run).
#define ASSERT_EQ(a, b) CHECK_EQ(a, b)

#define CHECK_STR_EQ(a, b)                                                 \
  do {                                                                     \
    std::string va_ = (a);                                                 \
    std::string vb_ = (b);                                                 \
    if (va_ != vb_) {                                                      \
      TEST_FAIL_MSG("strings differ:\n      got:  \"" + va_ +              \
                    "\"\n      want: \"" + vb_ + "\"");                    \
    }                                                                      \
    ++testfw::current_checks();                                            \
  } while (0)
