# OSS Survey — GTest infra

## Need
C++ unit test framework with assertion / matcher / parametric test support;
must work on Windows with no system install.

## Candidates considered

| Library | License | Pros | Cons | Decision |
|---|---|---|---|---|
| GoogleTest | BSD-3 | de-facto C++ standard; matchers (gmock); CMake-friendly | larger than Catch2 | **chosen** |
| Catch2 v3 | BSL-1.0 | header-only available; BDD style | less ubiquitous in CI; matcher API smaller | not chosen |
| doctest | MIT | very fast compile; header-only | smaller ecosystem | not chosen |

## Decision
Use **GoogleTest** via CMake `FetchContent` (no system install needed,
Windows friendly).

## Helper matchers
Reviewed gmock matchers; no need for a third-party bit-level matcher
library — gmock's `EXPECT_EQ` + bitmask intermediates suffice for the
~18 test cases in scope.
