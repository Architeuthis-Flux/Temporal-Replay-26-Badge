#line 1 "/Users/kevinsanto/Documents/GitHub/Temporal-Badge/firmware/BadgeTest.h"
// BadgeTest.h — Serial-triggered self-test framework
//
// Serial commands:
//   test      — run all tests (C + Python)
//   test:c    — C-level bridge tests only
//   test:py   — Python-level tests only
//
// Output format (machine-parseable):
//   [test] BEGIN
//   [test] c:uid ................. PASS
//   [test] c:server_url .......... FAIL (expected "https://...", got "")
//   [test] C: 5/6 passed
//   [test] TOTAL: 11/12 passed
//   [test] END

#pragma once

// Run the specified test suite. Pass "all", "c", or "py".
void runTests(const char* suite);
