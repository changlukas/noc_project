# Multi-instance DPI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lift the 5-singleton invariant in `cosim/c/cmodel_dpi.cpp` so the DPI layer can host N instances per shell, using a chandle-based ABI with typed handle + registry validation.

**Architecture:** SV side gains `input chandle ctx_i` on each wrap; `tb_top.sv` owns lifecycle in one `initial` block. C side replaces 5 `unique_ptr` singletons with a per-handle `HandleBlock` (typed control block) tracked in a process-wide `unordered_set<HandleBlock*>` registry. Every DPI handler validates `ctx` via registry-membership lookup before deref.

**Tech Stack:** C++17, SystemVerilog (Verilator), GoogleTest, CMake, yaml-cpp.

**Spec:** `docs/superpowers/specs/2026-06-09-multi-instance-dpi-design.md`

---

### Task 1: Foundation — types, macros, new error code

**Files:**
- Create: `cosim/c/handle_block.hpp`
- Modify: `cosim/c/dpi_boundary_macros.h`
- Modify: `cosim/c/cmodel_dpi.h`

- [ ] **Step 1: Create `handle_block.hpp` with types + registry decl**

```cpp
// cosim/c/handle_block.hpp
// Typed handle block + process-wide registry for multi-instance DPI.
#ifndef COSIM_HANDLE_BLOCK_HPP
#define COSIM_HANDLE_BLOCK_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

namespace ni::cmodel::cosim {

enum class ShellType : uint32_t {
    Master       = 0x4D415354u,  // 'MAST'
    Slave        = 0x534C4156u,  // 'SLAV'
    Nmu          = 0x4E4D5520u,  // 'NMU '
    Nsu          = 0x4E535520u,  // 'NSU '
    ChannelModel = 0x434D4D20u,  // 'CMM '
};

enum class HandleState { Live };  // closed handles are removed from registry

struct HandleBlock {
    uint32_t    magic;
    ShellType   type;
    HandleState state;
    std::string name;
    std::unique_ptr<void, void (*)(void*)> adapter;  // type-erased
};

extern std::unordered_set<HandleBlock*> g_handle_registry;

}  // namespace ni::cmodel::cosim

#endif  // COSIM_HANDLE_BLOCK_HPP
```

- [ ] **Step 2: Add `DPI_BOUNDARY_BEGIN_R / END_R` + first-error-wins to existing macros**

Replace the existing macro block in `cosim/c/dpi_boundary_macros.h:27-36` and append the new return-variant. Final content of that section:

```cpp
// First-error-wins: skip overwrite if a prior error is already latched.
#define DPI_SET_ERR_IF_CLEAR(code_expr, msg_expr)                              \
    do {                                                                        \
        int prior = ni::cmodel::cosim::g_dpi_error_code.load();                \
        if (prior == CMODEL_DPI_OK) {                                          \
            ni::cmodel::cosim::g_dpi_error_code.store(code_expr);              \
            ni::cmodel::cosim::g_dpi_error_msg = (msg_expr);                   \
        }                                                                       \
    } while (0)

#define DPI_BOUNDARY_BEGIN(fn_name) try
#define DPI_BOUNDARY_END(fn_name)                                              \
    catch (const std::exception& e) {                                          \
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_GENERIC,                           \
                             std::string(#fn_name ": ") + e.what());          \
    }                                                                           \
    catch (...) {                                                              \
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_UNKNOWN,                           \
                             std::string(#fn_name) + ": ...");                \
    }

#define DPI_BOUNDARY_BEGIN_R(fn_name, fail_value)                              \
    auto _dpi_fail_value = (fail_value); try
#define DPI_BOUNDARY_END_R(fn_name)                                            \
    catch (const std::exception& e) {                                          \
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_GENERIC,                           \
                             std::string(#fn_name ": ") + e.what());          \
        return _dpi_fail_value;                                                \
    }                                                                           \
    catch (...) {                                                              \
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_UNKNOWN,                           \
                             std::string(#fn_name) + ": ...");                \
        return _dpi_fail_value;                                                \
    }
```

- [ ] **Step 3: Add new error enum value in `cosim/c/cmodel_dpi.h`**

Replace lines 19-27 with the extended enum:

```cpp
typedef enum {
    CMODEL_DPI_OK = 0,
    CMODEL_DPI_ERR_GENERIC = 1,
    CMODEL_DPI_ERR_NOT_INITIALIZED = 2,
    CMODEL_DPI_ERR_HERMETIC_VIOLATION = 3,
    CMODEL_DPI_ERR_BACKPRESSURE = 4,
    CMODEL_DPI_ERR_INJECT_BAD_MODE = 5,
    CMODEL_DPI_ERR_REINIT_FORBIDDEN = 6,
    CMODEL_DPI_ERR_UNKNOWN = 99
} cmodel_dpi_error_e;
```

- [ ] **Step 4: Build to verify no regressions**

Run:
```
cmake --build build
```
Expected: succeeds. Existing tests not affected (types are additive).

- [ ] **Step 5: Commit**

```
git add cosim/c/handle_block.hpp cosim/c/dpi_boundary_macros.h cosim/c/cmodel_dpi.h
git commit -m "feat(cosim): add HandleBlock types and return-variant DPI boundary macros"
```

---

### Task 2: Registry + `validate_handle` + new test scaffolding

**Files:**
- Modify: `cosim/c/cmodel_dpi.cpp` (registry def + validate_handle impl)
- Modify: `cosim/c/dpi_boundary_macros.h` (REQUIRE_HANDLE macro)
- Create: `c_model/tests/cosim/test_cmodel_dpi.cpp`
- Modify: `c_model/tests/cosim/CMakeLists.txt`

- [ ] **Step 1: Create the test file with scaffolding + first negative cases**

```cpp
// c_model/tests/cosim/test_cmodel_dpi.cpp
// Single ordered TEST_F walking the DPI session state machine.
// Each negative assertion calls check_and_clear_error() to drain the latch.
#include "cmodel_dpi.h"
#include "handle_block.hpp"
#include <atomic>
#include <gtest/gtest.h>
#include <string>

namespace ni::cmodel::cosim {
extern std::atomic<int> g_dpi_error_code;
extern std::string      g_dpi_error_msg;
}  // namespace ni::cmodel::cosim

namespace {

void check_and_clear_error(int expected_code) {
    const char* msg = nullptr;
    int code = cmodel_check_error(&msg);
    EXPECT_EQ(code, expected_code) << "msg: " << (msg ? msg : "<null>");
    ni::cmodel::cosim::g_dpi_error_code.store(CMODEL_DPI_OK);
    ni::cmodel::cosim::g_dpi_error_msg.clear();
}

class CmodelDpiLifecycleTest : public ::testing::Test {};

TEST_F(CmodelDpiLifecycleTest, walk_session_state_machine) {
    // Case: *_create before init → ERR_NOT_INITIALIZED.
    void* h0 = cmodel_channel_model_create("cm_pre_init");
    EXPECT_EQ(h0, nullptr);
    check_and_clear_error(CMODEL_DPI_ERR_NOT_INITIALIZED);

    // Body extended by Tasks 3-10. See per-task "Final test body so far" blocks.
}

}  // namespace
```

(`cmodel_channel_model_create` is declared in `cmodel_dpi.h` and defined in Task 5; this test compiles but `cmodel_channel_model_create` is undefined at this point. The test target will not link until Task 5 lands. Mark the test as `EXCLUDE_FROM_ALL` here and remove that property in Task 5 — see Step 7 below.)

- [ ] **Step 2: Register the test in CMake**

`add_cmodel_test()` (defined in `c_model/tests/CMakeLists.txt:13-20`) only compiles `${name}.cpp` and links `gtest_main` — it does **not** pull in `cosim/c/cmodel_dpi.cpp`, and the c_model build does not normally see `svdpi.h`. The new test target needs explicit additions for both. Append to `c_model/tests/cosim/CMakeLists.txt`:

```cmake
add_cmodel_test(test_cmodel_dpi)
target_sources(test_cmodel_dpi PRIVATE
    ${CMAKE_SOURCE_DIR}/../cosim/c/cmodel_dpi.cpp)
target_link_libraries(test_cmodel_dpi PRIVATE yaml-cpp::yaml-cpp)
target_include_directories(test_cmodel_dpi PRIVATE
    ${CMAKE_SOURCE_DIR}/../cosim/c
    $ENV{VERILATOR_ROOT}/include)
# Excluded from default build until Task 5 defines cmodel_channel_model_create;
# Task 5 removes this property.
set_target_properties(test_cmodel_dpi PROPERTIES EXCLUDE_FROM_ALL TRUE)
```

The `VERILATOR_ROOT` env var must be set in the shell that runs the build (already required for `cosim/verilator/Makefile`; see `cosim/verilator/obj_dir/Vtb_top.mk:14`).

- [ ] **Step 3: Add registry definition + `validate_handle` impl in `cmodel_dpi.cpp`**

In the `namespace ni::cmodel::cosim` block at the top of `cmodel_dpi.cpp` (after the existing `g_dpi_error_msg` definition), add:

```cpp
std::unordered_set<HandleBlock*> g_handle_registry;

// Note: g_session_state is declared in Task 3 (added then to avoid forward ref).
// Reference here is forward-declared via the same file's later definition.
extern SessionState g_session_state;

HandleBlock* validate_handle(void* ctx, ShellType expected, const char* fn_name) {
    // State-first check: pre-init → NOT_INITIALIZED per spec state-transition table.
    if (g_session_state == SessionState::Uninitialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED,
                             std::string(fn_name) + ": session not initialized");
        return nullptr;
    }
    // Registry membership — avoids garbage void* deref SIGSEGV.
    // Post-finalize handles fail here (registry was emptied by finalize) →
    // ERR_HERMETIC_VIOLATION, consistent with the spec test matrix.
    if (!g_handle_registry.count(static_cast<HandleBlock*>(ctx))) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_HERMETIC_VIOLATION,
                             std::string(fn_name) + ": ctx not in registry");
        return nullptr;
    }
    auto* h = static_cast<HandleBlock*>(ctx);
    if (h->magic != static_cast<uint32_t>(expected)) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_HERMETIC_VIOLATION,
                             std::string(fn_name) + ": magic mismatch");
        return nullptr;
    }
    if (h->type != expected) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_HERMETIC_VIOLATION,
                             std::string(fn_name) + ": type mismatch");
        return nullptr;
    }
    if (h->state != HandleState::Live) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_HERMETIC_VIOLATION,
                             std::string(fn_name) + ": handle not live");
        return nullptr;
    }
    return h;
}
```

Add `#include "handle_block.hpp"` to the includes block.

- [ ] **Step 4: Add `REQUIRE_HANDLE` macro to `dpi_boundary_macros.h`**

Append after the existing `REQUIRE_ADAPTER` macro. Canonical form — introduces `_h` into the caller scope; the caller does its own `static_cast` to the typed adapter pointer:

```cpp
// REQUIRE_HANDLE — used by ctx-taking handlers. validate_handle sets the
// error latch and returns null on failure; this macro then returns from the
// void handler. Caller pulls the typed adapter with:
//     auto* nmu = static_cast<NmuShellAdapter*>(_h->adapter.get());
#define REQUIRE_HANDLE(ctx, expected_type, fn_name)                            \
    auto* _h = ni::cmodel::cosim::validate_handle((ctx), (expected_type),      \
                                                  (fn_name));                  \
    if (!_h) return
```

- [ ] **Step 5: Build the c_model library**

`cmodel_channel_model_create` won't link until Task 5 defines it. Verify the rest of the test target compiles by building anything-but-this-test:

```
cmake --build build
```
Expected: succeeds. The `test_cmodel_dpi` target was registered `EXCLUDE_FROM_ALL` in Step 2 so it won't break the default build until Task 5 removes that property.

- [ ] **Step 6: Commit**

```
git add cosim/c/cmodel_dpi.cpp cosim/c/dpi_boundary_macros.h \
        c_model/tests/cosim/test_cmodel_dpi.cpp \
        c_model/tests/cosim/CMakeLists.txt
git commit -m "feat(cosim): add handle registry, validate_handle, REQUIRE_HANDLE macro"
```

---

### Task 3: Session state machine — `cmodel_init` retry + REINIT guard

**Files:**
- Modify: `cosim/c/cmodel_dpi.cpp`
- Modify: `c_model/tests/cosim/test_cmodel_dpi.cpp`

- [ ] **Step 1: Add session state + globals in `cmodel_dpi.cpp`**

In the `namespace ni::cmodel::cosim` block at top, add:

```cpp
enum class SessionState { Uninitialized, Initialized, Finalized };
SessionState g_session_state = SessionState::Uninitialized;
std::size_t  g_ever_created_master = 0;  // bumped on each cmodel_master_create

// Cached scenario + original YAML path — read by *_create handlers in Tasks 6-9.
// Scenario struct lacks a `path` field so the literal path string is stashed
// separately. Both are immutable after cmodel_init success.
ni::cmodel::axi::Scenario g_scenario;
std::string               g_scenario_yaml_path;
```

(`g_scoreboard` already exists at line 55 — `unique_ptr<ni::cmodel::axi::Scoreboard>` — keep it. Step 2 below moves its construction to here instead of being created inside `cmodel_init`'s current adapter-construction block.)

- [ ] **Step 2: Rewrite `cmodel_init` body for state-machine compliance**

Replace `cosim/c/cmodel_dpi.cpp:61-142` (the entire `cmodel_init` function) with:

```cpp
extern "C" void cmodel_init(const char* scenario_yaml_path) {
    using namespace ni::cmodel::cosim;
    if (g_session_state == SessionState::Initialized ||
        g_session_state == SessionState::Finalized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_REINIT_FORBIDDEN,
                             "cmodel_init: session already initialized or finalized");
        return;
    }
    // Retry from UNINITIALIZED: clear prior latch before parsing.
    g_dpi_error_code.store(CMODEL_DPI_OK);
    g_dpi_error_msg.clear();

    DPI_BOUNDARY_BEGIN(cmodel_init) {
        auto scenario = ni::cmodel::axi::load_scenario(std::string(scenario_yaml_path));
        g_scenario = std::move(scenario);
        g_scenario_yaml_path = scenario_yaml_path;
        g_scoreboard = std::make_unique<ni::cmodel::axi::Scoreboard>();

        // ====== Begin existing singleton-construction block (kept until Task 10) ======
        // Preserve verbatim lines 76-139 of the pre-refactor cmodel_init:
        // channel_adapter / master_adapter / slave_adapter / nmu_adapter /
        // nsu_adapter construction + scoreboard-callback wiring on the master.
        // (The original block also called `make_unique<Scoreboard>()` — DELETE
        //  that line; g_scoreboard is now created above.)
        // ====== End singleton-construction block ======

        g_session_state = SessionState::Initialized;
    }
    DPI_BOUNDARY_END(cmodel_init);
}
```

(Preserve the existing singleton construction; it gets cleaned up in Task 10. The state-machine guard and retry-clear are the new behavior here.)

- [ ] **Step 3: Add test cases to `walk_session_state_machine`**

Insert at the start of the `TEST_F` body (before the garbage-ctx case that's `#if 0`'d):

```cpp
// Case: cmodel_init on bad YAML → state stays UNINITIALIZED, retry allowed.
cmodel_init("/nonexistent/path/to/scenario.yaml");
check_and_clear_error(CMODEL_DPI_ERR_GENERIC);

// Case: cmodel_init twice (both successful) → second rejected.
const char* good_yaml = std::getenv("CMODEL_TEST_SCENARIO_YAML");
ASSERT_NE(good_yaml, nullptr) << "set CMODEL_TEST_SCENARIO_YAML to a valid scenario";
cmodel_init(good_yaml);
check_and_clear_error(CMODEL_DPI_OK);
cmodel_init(good_yaml);
check_and_clear_error(CMODEL_DPI_ERR_REINIT_FORBIDDEN);
```

Append to `c_model/tests/cosim/CMakeLists.txt` after the existing `add_cmodel_test(test_cmodel_dpi)` block. The path uses `sim/test_patterns/AX4-BAS-001_single_write_no_read/scenario.yaml` (verified to exist at repo root):

```cmake
set_tests_properties(test_cmodel_dpi PROPERTIES
    ENVIRONMENT "CMODEL_TEST_SCENARIO_YAML=${CMAKE_SOURCE_DIR}/../sim/test_patterns/AX4-BAS-001_single_write_no_read/scenario.yaml")
```

(`set_tests_properties` is the right name once gtest_discover_tests has registered the ctest cases. If the test name discovered by gtest includes a fixture prefix like `CmodelDpiLifecycleTest.walk_session_state_machine`, set the env var on that specific test name instead.)

- [ ] **Step 4: Build + run**

```
cmake --build build --target test_cmodel_dpi
# (target was EXCLUDE_FROM_ALL; explicit --target opts in)
ctest --test-dir build -R test_cmodel_dpi --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**

```
git add cosim/c/cmodel_dpi.cpp c_model/tests/cosim/test_cmodel_dpi.cpp \
        c_model/tests/cosim/CMakeLists.txt
git commit -m "feat(cosim): add session state machine, REINIT_FORBIDDEN guard, retry-clear"
```

---

### Task 4: `cmodel_finalize` any-state idempotent

**Files:**
- Modify: `cosim/c/cmodel_dpi.cpp`
- Modify: `c_model/tests/cosim/test_cmodel_dpi.cpp`

- [ ] **Step 1: Rewrite `cmodel_finalize` body**

Replace `cosim/c/cmodel_dpi.cpp:144-154`:

```cpp
extern "C" void cmodel_finalize(void) {
    using namespace ni::cmodel::cosim;
    DPI_BOUNDARY_BEGIN(cmodel_finalize) {
        if (g_session_state != SessionState::Initialized) {
            return;  // no-op from UNINITIALIZED or FINALIZED
        }
        // Destroy each handle block. unique_ptr<void, deleter> in HandleBlock
        // ensures the type-erased adapter is properly deleted.
        for (HandleBlock* h : g_handle_registry) {
            delete h;
        }
        g_handle_registry.clear();

        // Preserve existing singleton resets until Task 10 removes the path.
        g_channel_adapter.reset();
        g_master_adapter.reset();
        g_slave_adapter.reset();
        g_nmu_adapter.reset();
        g_nsu_adapter.reset();
        g_scoreboard.reset();
        g_ever_created_master = 0;

        g_session_state = SessionState::Finalized;
    }
    DPI_BOUNDARY_END(cmodel_finalize);
}
```

- [ ] **Step 2: Add finalize-from-UNINITIALIZED no-op test**

Only this one finalize case can be added now — the post-init finalize tests must live AFTER per-shell create tests (Tasks 5-9 append to the INITIALIZED block). Add the post-init finalize cases in Task 10.

Insert at the very start of the TEST_F body, before any `cmodel_init` call (need to add this BEFORE the existing `*_create before init` case from Task 2; this means reordering the test body):

```cpp
// Case: cmodel_finalize from UNINITIALIZED → no-op (no error, state unchanged).
cmodel_finalize();
check_and_clear_error(CMODEL_DPI_OK);
```

The full INITIALIZED-block finalize cases (idempotent twice + post-finalize REINIT_FORBIDDEN + stale-ctx HERMETIC_VIOLATION) are added in Task 10.

- [ ] **Step 3: Build + run + commit**

```
cmake --build build --target test_cmodel_dpi
# (target was EXCLUDE_FROM_ALL; explicit --target opts in)
ctest --test-dir build -R test_cmodel_dpi --output-on-failure
git add cosim/c/cmodel_dpi.cpp c_model/tests/cosim/test_cmodel_dpi.cpp
git commit -m "feat(cosim): cmodel_finalize idempotent across all session states"
```

---

### Task 5: ChannelModel migration — `*_create` + ctx-taking cycle ops + SV wrap + tb_top

**Files:**
- Modify: `cosim/c/cmodel_dpi.h`
- Modify: `cosim/c/cmodel_dpi.cpp`
- Modify: `cosim/sv/channel_model_wrap.sv`
- Modify: `cosim/sv/tb_top.sv`
- Modify: `c_model/tests/cosim/test_cmodel_dpi.cpp`

- [ ] **Step 1: Update DPI header signatures for ChannelModel**

Replace `cosim/c/cmodel_dpi.h:39-45` with:

```cpp
void* cmodel_channel_model_create(const char* name);
void  cmodel_channel_model_set_inputs(void* ctx, svBit req_in_valid,
                                      svBitVecVal* req_in_flit, svBit req_in_credit_return,
                                      svBit rsp_in_valid, svBitVecVal* rsp_in_flit,
                                      svBit rsp_in_credit_return);
void  cmodel_channel_model_tick(void* ctx);
void  cmodel_channel_model_get_outputs(void* ctx, svBit* req_out_valid,
                                       svBitVecVal* req_out_flit,
                                       svBit* req_out_credit_return, svBit* rsp_out_valid,
                                       svBitVecVal* rsp_out_flit,
                                       svBit* rsp_out_credit_return);
```

- [ ] **Step 2: Update `cmodel_dpi.cpp` ChannelModel handlers**

Replace `cosim/c/cmodel_dpi.cpp:242-284` with:

```cpp
extern "C" void* cmodel_channel_model_create(const char* name) {
    using namespace ni::cmodel::cosim;
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED,
                             "cmodel_channel_model_create: not initialized");
        return nullptr;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_channel_model_create, nullptr) {
        auto adapter = std::make_unique<ChannelModelShellAdapter>();
        adapter->init();
        auto* h = new HandleBlock{
            static_cast<uint32_t>(ShellType::ChannelModel),
            ShellType::ChannelModel,
            HandleState::Live,
            std::string(name),
            std::unique_ptr<void, void (*)(void*)>(
                adapter.release(),
                [](void* p) { delete static_cast<ChannelModelShellAdapter*>(p); })
        };
        g_handle_registry.insert(h);
        return static_cast<void*>(h);
    }
    DPI_BOUNDARY_END_R(cmodel_channel_model_create);
}

extern "C" void cmodel_channel_model_set_inputs(void* ctx, svBit req_in_valid,
                                                svBitVecVal* req_in_flit,
                                                svBit req_in_credit_return,
                                                svBit rsp_in_valid,
                                                svBitVecVal* rsp_in_flit,
                                                svBit rsp_in_credit_return) {
    DPI_BOUNDARY_BEGIN(cmodel_channel_model_set_inputs) {
        REQUIRE_HANDLE(ctx, ni::cmodel::cosim::ShellType::ChannelModel,
                       "cmodel_channel_model_set_inputs");
        auto* cm = static_cast<ChannelModelShellAdapter*>(_h->adapter.get());
        ChannelModelInputs in{};
        in.req_in_valid = static_cast<bool>(req_in_valid);
        in.req_in_flit  = unpack_flit(req_in_flit);
        in.req_in_credit_return = static_cast<bool>(req_in_credit_return);
        in.rsp_in_valid = static_cast<bool>(rsp_in_valid);
        in.rsp_in_flit  = unpack_flit(rsp_in_flit);
        in.rsp_in_credit_return = static_cast<bool>(rsp_in_credit_return);
        cm->set_inputs(in);
    }
    DPI_BOUNDARY_END(cmodel_channel_model_set_inputs);
}

extern "C" void cmodel_channel_model_tick(void* ctx) {
    DPI_BOUNDARY_BEGIN(cmodel_channel_model_tick) {
        REQUIRE_HANDLE(ctx, ni::cmodel::cosim::ShellType::ChannelModel,
                       "cmodel_channel_model_tick");
        auto* cm = static_cast<ChannelModelShellAdapter*>(_h->adapter.get());
        cm->tick();
    }
    DPI_BOUNDARY_END(cmodel_channel_model_tick);
}

extern "C" void cmodel_channel_model_get_outputs(void* ctx, svBit* req_out_valid,
                                                 svBitVecVal* req_out_flit,
                                                 svBit* req_out_credit_return,
                                                 svBit* rsp_out_valid,
                                                 svBitVecVal* rsp_out_flit,
                                                 svBit* rsp_out_credit_return) {
    DPI_BOUNDARY_BEGIN(cmodel_channel_model_get_outputs) {
        REQUIRE_HANDLE(ctx, ni::cmodel::cosim::ShellType::ChannelModel,
                       "cmodel_channel_model_get_outputs");
        auto* cm = static_cast<ChannelModelShellAdapter*>(_h->adapter.get());
        ChannelModelOutputs out{};
        cm->get_outputs(out);
        *req_out_valid = static_cast<svBit>(out.req_out_valid);
        pack_flit(out.req_out_flit, req_out_flit);
        *req_out_credit_return = static_cast<svBit>(out.req_out_credit_return);
        *rsp_out_valid = static_cast<svBit>(out.rsp_out_valid);
        pack_flit(out.rsp_out_flit, rsp_out_flit);
        *rsp_out_credit_return = static_cast<svBit>(out.rsp_out_credit_return);
    }
    DPI_BOUNDARY_END(cmodel_channel_model_get_outputs);
}
```

- [ ] **Step 3: Update `channel_model_wrap.sv` to take `chandle ctx_i`**

Modify `cosim/sv/channel_model_wrap.sv` module port list — add `input chandle ctx_i,` near the top of the port list. Update each DPI import to take `chandle ctx` as first arg. Update each DPI call site in `always_ff` to pass `ctx_i`.

(Exact pattern: see `cosim/sv/channel_model_wrap.sv` `import "DPI-C"` blocks and the `always_ff` calls. Insert `chandle ctx_i,` as first port; insert `input chandle ctx,` as first arg of each import; pass `ctx_i` as first arg of each call.)

- [ ] **Step 4: Update `tb_top.sv` — declare cm_ctx, call create**

In `cosim/sv/tb_top.sv`, after line 47 (the existing `import` for `cmodel_init`), add:

```sv
import "DPI-C" context function chandle cmodel_channel_model_create(input string name);

chandle cm_ctx;
```

Then update the existing `initial begin` (lines 55-61) to:

```sv
initial begin
    if (!$value$plusargs("scenario=%s", scenario_path)) begin
        $display("ERROR: +scenario=<yaml-path> required");
        $finish(1);
    end
    cmodel_init(scenario_path);
    cm_ctx = cmodel_channel_model_create("channel_model_0");
end
```

Wire `cm_ctx` to the `u_channel_model` instance via `.ctx_i(cm_ctx)`.

- [ ] **Step 5: Remove `EXCLUDE_FROM_ALL` + add ChannelModel test cases**

In `c_model/tests/cosim/CMakeLists.txt`, delete the `set_target_properties(test_cmodel_dpi PROPERTIES EXCLUDE_FROM_ALL TRUE)` line added in Task 2.

The test session is process-global and terminal after `cmodel_finalize`. The TEST_F walks one lifecycle, ordered: UNINITIALIZED phase tests → init → INITIALIZED phase tests (incl create + handle validation) → finalize → FINALIZED phase tests. **Per-shell create cases (Tasks 5-9) are appended at the end of the INITIALIZED block, before finalize.** Post-finalize cases live in Task 10.

In `test_cmodel_dpi.cpp` `walk_session_state_machine` body, insert at the end of the INITIALIZED block (after the second `cmodel_init` reject, before any `cmodel_finalize`):

```cpp
// Case: channel_model_create after init succeeds.
void* cm_handle = cmodel_channel_model_create("cm_test");
ASSERT_NE(cm_handle, nullptr);
check_and_clear_error(CMODEL_DPI_OK);

// Case: garbage void* (non-registry) — verifies registry-membership SIGSEGV guard.
void* garbage = reinterpret_cast<void*>(0xDEADBEEFCAFEull);
cmodel_channel_model_tick(garbage);
check_and_clear_error(CMODEL_DPI_ERR_HERMETIC_VIOLATION);
```

- [ ] **Step 6: Build + verify cosim still passes**

```
cmake --build build
(cd cosim/verilator && make)
ctest --test-dir build -R test_cmodel_dpi --output-on-failure
ctest --test-dir build -R cosim --output-on-failure
```
Expected: both PASS. ChannelModel now ctx-keyed; other 4 shells unchanged (still singleton).

- [ ] **Step 7: Commit**

```
git add cosim/c/cmodel_dpi.h cosim/c/cmodel_dpi.cpp \
        cosim/sv/channel_model_wrap.sv cosim/sv/tb_top.sv \
        c_model/tests/cosim/test_cmodel_dpi.cpp
git commit -m "feat(cosim): migrate ChannelModel to chandle DPI ABI"
```

---

### Task 6: Master migration — incl per-instance dump path

**Files:**
- Modify: `cosim/c/cmodel_dpi.h`
- Modify: `cosim/c/cmodel_dpi.cpp`
- Modify: `cosim/sv/axi_master_wrap.sv`
- Modify: `cosim/sv/tb_top.sv`
- Modify: `c_model/tests/cosim/test_cmodel_dpi.cpp`

- [ ] **Step 1: Update DPI header for Master (declare create + change cycle op signatures to take void* ctx as first arg)**

In `cosim/c/cmodel_dpi.h`, replace the master block (lines 54-66) with the analogous form to Task 5 (add `void* ctx` as leading arg to set_inputs/tick/get_outputs, prepend a `void* cmodel_master_create(const char* name);` declaration).

- [ ] **Step 2: Add Master `*_create` + migrate cycle op handlers**

Add the master_create function in `cosim/c/cmodel_dpi.cpp` following the same pattern as Task 5 Step 2 (use `ShellType::Master`, allocate `MasterShellAdapter`, derive read_dump_path from name).

Key difference: Master's `init` needs scenario config plus the original YAML path. `Scenario` (in `c_model/include/axi/scenario_parser.hpp:55-59`) has no `path` field, so stash both as globals (set in `cmodel_init` in Task 3, accessed here):

```cpp
ni::cmodel::axi::Scenario g_scenario;          // populated in cmodel_init
std::string               g_scenario_yaml_path; // populated in cmodel_init
std::unique_ptr<ni::cmodel::axi::Scoreboard> g_scoreboard;  // pre-existing
```

In `cmodel_init` (modified in Task 3), after `auto scenario = load_scenario(...)`:
```cpp
g_scenario = std::move(scenario);
g_scenario_yaml_path = scenario_yaml_path;
g_scoreboard = std::make_unique<ni::cmodel::axi::Scoreboard>();  // global, single
```

Master create body:

```cpp
extern "C" void* cmodel_master_create(const char* name) {
    using namespace ni::cmodel::cosim;
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED,
                             "cmodel_master_create: not initialized");
        return nullptr;
    }
    DPI_BOUNDARY_BEGIN_R(cmodel_master_create, nullptr) {
        const std::string dump_path =
            "master_shell_read_dump_" + std::string(name) + ".txt";
        auto adapter = std::make_unique<MasterShellAdapter>();
        adapter->init(g_scenario_yaml_path, dump_path,
                      g_scenario.config.max_outstanding_write,
                      g_scenario.config.max_outstanding_read);
        adapter->configure_inject(g_scenario.config.inject);

        // Wire scoreboard callbacks. g_scoreboard.get() is stable across vector
        // growth because Scoreboard is heap-allocated (unique_ptr).
        auto* sb_raw = g_scoreboard.get();
        auto resp_str = [](ni::cmodel::axi::Resp r) -> const char* {
            switch (r) {
                case ni::cmodel::axi::Resp::OKAY:   return "OKAY";
                case ni::cmodel::axi::Resp::EXOKAY: return "EXOKAY";
                case ni::cmodel::axi::Resp::SLVERR: return "SLVERR";
                case ni::cmodel::axi::Resp::DECERR: return "DECERR";
            }
            return "?";
        };
        adapter->on_write_completed([sb_raw, resp_str](const ni::cmodel::axi::WriteResult& wr) {
            sb_raw->handle_write_completed(wr, wr.data, wr.strb_per_beat);
            std::fprintf(stderr, "[axi-w] id=0x%x addr=0x%llx len=%u size=%u resp=%s\n",
                         static_cast<unsigned>(wr.id),
                         static_cast<unsigned long long>(wr.addr),
                         static_cast<unsigned>(wr.len),
                         static_cast<unsigned>(wr.size), resp_str(wr.resp));
        });
        adapter->on_read_observed([sb_raw, resp_str](const ni::cmodel::axi::ReadResult& rr) {
            sb_raw->handle_read_observed(rr);
            const uint8_t first_byte = rr.data.empty() ? 0 : rr.data[0];
            std::fprintf(stderr,
                         "[axi-r] id=0x%x addr=0x%llx len=%u size=%u resp=%s data[0]=0x%02x\n",
                         static_cast<unsigned>(rr.id),
                         static_cast<unsigned long long>(rr.addr),
                         static_cast<unsigned>(rr.len),
                         static_cast<unsigned>(rr.size),
                         resp_str(rr.resp), static_cast<unsigned>(first_byte));
        });

        auto* h = new HandleBlock{
            static_cast<uint32_t>(ShellType::Master),
            ShellType::Master,
            HandleState::Live,
            std::string(name),
            std::unique_ptr<void, void (*)(void*)>(
                adapter.release(),
                [](void* p) { delete static_cast<MasterShellAdapter*>(p); })
        };
        g_handle_registry.insert(h);
        ++g_ever_created_master;
        return static_cast<void*>(h);
    }
    DPI_BOUNDARY_END_R(cmodel_master_create);
}
```

For the 3 cycle ops: follow Task 5 Step 2 pattern — add `void* ctx` first arg, replace `REQUIRE_ADAPTER(g_master_adapter, ...)` with `REQUIRE_HANDLE(ctx, ShellType::Master, fn_name)` and `auto* master = static_cast<MasterShellAdapter*>(_h->adapter.get());`.

- [ ] **Step 3: Update `axi_master_wrap.sv` + `tb_top.sv`**

`axi_master_wrap.sv`: add `input chandle ctx_i,` port. Update each DPI import to take leading `input chandle ctx`. Pass `ctx_i` in each DPI call.

`tb_top.sv`: add `import "DPI-C" context function chandle cmodel_master_create(input string name);` near other imports. Add `chandle master_ctx;` declaration. Append `master_ctx = cmodel_master_create("master_0");` inside the existing `initial` block (after `cm_ctx = ...`). Wire `.ctx_i(master_ctx)` to `u_master`.

- [ ] **Step 4: Add Master test cases**

Insert in TEST_F body, after channel_model_create:

```cpp
// Case: master_create + scoreboard callbacks wired.
void* master_handle = cmodel_master_create("master_test");
ASSERT_NE(master_handle, nullptr);
check_and_clear_error(CMODEL_DPI_OK);

// Case: type mismatch — channel_model handle passed to master_tick.
cmodel_master_tick(cm_handle);
check_and_clear_error(CMODEL_DPI_ERR_HERMETIC_VIOLATION);
```

- [ ] **Step 5: Build + regression**

```
cmake --build build
(cd cosim/verilator && make)
ctest --test-dir build --output-on-failure
```
Expected: PASS, including the existing cosim scenarios.

- [ ] **Step 6: Commit**

```
git add cosim/c/cmodel_dpi.h cosim/c/cmodel_dpi.cpp \
        cosim/sv/axi_master_wrap.sv cosim/sv/tb_top.sv \
        c_model/tests/cosim/test_cmodel_dpi.cpp
git commit -m "feat(cosim): migrate Master to chandle DPI ABI with per-instance dump path"
```

---

### Task 7: Slave migration

**Files:**
- Modify: `cosim/c/cmodel_dpi.h`
- Modify: `cosim/c/cmodel_dpi.cpp`
- Modify: `cosim/sv/axi_slave_wrap.sv`
- Modify: `cosim/sv/tb_top.sv`
- Modify: `c_model/tests/cosim/test_cmodel_dpi.cpp`

- [ ] **Step 1: Update DPI header for Slave**

In `cosim/c/cmodel_dpi.h`, replace the slave block (lines 76-87) — prepend `void* cmodel_slave_create(const char* name);`, add `void* ctx` as leading arg to set_inputs/tick/get_outputs.

- [ ] **Step 2: Add slave `*_create` + migrate cycle ops**

Mirror Task 5 Step 2 with:
- `ShellType::Slave`
- `auto adapter = std::make_unique<SlaveShellAdapter>(); adapter->init(g_scenario.config.memory_base, g_scenario.config.memory_size, g_scenario.config.write_latency, g_scenario.config.read_latency);`
- Cycle ops: `REQUIRE_HANDLE(ctx, ShellType::Slave, fn_name)`; cast `_h->adapter.get()` to `SlaveShellAdapter*`.

- [ ] **Step 3: Update `axi_slave_wrap.sv` + `tb_top.sv`**

Same pattern as Task 6 Step 3: add `input chandle ctx_i` port to slave_wrap, thread to DPI calls; add `chandle slave_ctx;` + create call in tb_top initial; wire `.ctx_i(slave_ctx)`.

- [ ] **Step 4: Build + regression + commit**

```
cmake --build build
(cd cosim/verilator && make)
ctest --test-dir build --output-on-failure
git add cosim/c/cmodel_dpi.h cosim/c/cmodel_dpi.cpp \
        cosim/sv/axi_slave_wrap.sv cosim/sv/tb_top.sv \
        c_model/tests/cosim/test_cmodel_dpi.cpp
git commit -m "feat(cosim): migrate Slave to chandle DPI ABI"
```

---

### Task 8: NMU migration

**Files:**
- Modify: `cosim/c/cmodel_dpi.h`
- Modify: `cosim/c/cmodel_dpi.cpp`
- Modify: `cosim/sv/nmu_wrap.sv`
- Modify: `cosim/sv/tb_top.sv`
- Modify: `c_model/tests/cosim/test_cmodel_dpi.cpp`

- [ ] **Step 1: Update DPI header for NMU**

In `cosim/c/cmodel_dpi.h`, replace the NMU block (lines 97-112) — prepend `void* cmodel_nmu_create(const char* name);`, add `void* ctx` as leading arg to all cycle ops.

- [ ] **Step 2: Add NMU `*_create` + migrate cycle ops**

Mirror Task 5 Step 2 with:
- `ShellType::Nmu`
- `auto adapter = std::make_unique<NmuShellAdapter>(); adapter->init();`
- Cycle ops: `REQUIRE_HANDLE(ctx, ShellType::Nmu, fn_name)`; cast `_h->adapter.get()` to `NmuShellAdapter*`.

- [ ] **Step 3: Update `nmu_wrap.sv` + `tb_top.sv`**

Same pattern as Task 6 Step 3.

- [ ] **Step 4: Add NMU multi-instance test case**

Insert in TEST_F body after master test:

```cpp
// Case: create 2 NMU adapters — distinct void*, both validate as live.
void* nmu_a = cmodel_nmu_create("nmu_a");
void* nmu_b = cmodel_nmu_create("nmu_b");
ASSERT_NE(nmu_a, nullptr);
ASSERT_NE(nmu_b, nullptr);
EXPECT_NE(nmu_a, nmu_b);
check_and_clear_error(CMODEL_DPI_OK);
```

- [ ] **Step 5: Build + regression + commit**

```
cmake --build build
(cd cosim/verilator && make)
ctest --test-dir build --output-on-failure
git add cosim/c/cmodel_dpi.h cosim/c/cmodel_dpi.cpp \
        cosim/sv/nmu_wrap.sv cosim/sv/tb_top.sv \
        c_model/tests/cosim/test_cmodel_dpi.cpp
git commit -m "feat(cosim): migrate NMU to chandle DPI ABI"
```

---

### Task 9: NSU migration

**Files:**
- Modify: `cosim/c/cmodel_dpi.h`
- Modify: `cosim/c/cmodel_dpi.cpp`
- Modify: `cosim/sv/nsu_wrap.sv`
- Modify: `cosim/sv/tb_top.sv`
- Modify: `c_model/tests/cosim/test_cmodel_dpi.cpp`

- [ ] **Step 1: Update DPI header for NSU**

In `cosim/c/cmodel_dpi.h`, replace the NSU block (lines 125-139) — prepend `void* cmodel_nsu_create(const char* name);`, add `void* ctx` as leading arg.

- [ ] **Step 2: Add NSU `*_create` + migrate cycle ops**

Mirror Task 5 Step 2 with:
- `ShellType::Nsu`
- `auto adapter = std::make_unique<NsuShellAdapter>(); adapter->init();`
- Cycle ops: `REQUIRE_HANDLE(ctx, ShellType::Nsu, fn_name)`; cast to `NsuShellAdapter*`.

- [ ] **Step 3: Update `nsu_wrap.sv` + `tb_top.sv`**

Same pattern as Task 6 Step 3.

- [ ] **Step 4: Build + regression + commit**

```
cmake --build build
(cd cosim/verilator && make)
ctest --test-dir build --output-on-failure
git add cosim/c/cmodel_dpi.h cosim/c/cmodel_dpi.cpp \
        cosim/sv/nsu_wrap.sv cosim/sv/tb_top.sv \
        c_model/tests/cosim/test_cmodel_dpi.cpp
git commit -m "feat(cosim): migrate NSU to chandle DPI ABI"
```

---

### Task 10: Remove singletons + lifecycle aggregation

**Files:**
- Modify: `cosim/c/cmodel_dpi.cpp`
- Modify: `c_model/tests/cosim/test_cmodel_dpi.cpp`

- [ ] **Step 1: Remove the 5 singleton declarations**

Delete `cosim/c/cmodel_dpi.cpp:48-52` (the 5 `unique_ptr<*ShellAdapter>` decls). Also delete the `g_scoreboard` declaration only if it's no longer used elsewhere — for now keep it since the single scoreboard is preserved per spec.

- [ ] **Step 2: Remove singleton-construction code from `cmodel_init`**

In `cmodel_init`, remove the previously-preserved construction block (the calls to `make_unique<ChannelModelShellAdapter>()` etc. and the singleton assignments). The only remaining work in `cmodel_init` is: state machine guard → clear latch → `load_scenario` → store `g_scenario` → flip to INITIALIZED.

The scoreboard wiring previously inside `cmodel_init` (lines 99-131 of the original code) now lives in `cmodel_master_create` (added in Task 6 Step 2). Verify it's there.

- [ ] **Step 3: Remove singleton resets from `cmodel_finalize`**

Remove the `g_channel_adapter.reset();` ... `g_nsu_adapter.reset();` block. The registry-walk delete already handles the chandle-allocated adapters.

- [ ] **Step 4: Rewrite `cmodel_done` for aggregation**

Replace `cosim/c/cmodel_dpi.cpp:165-168` with:

```cpp
extern "C" int cmodel_done(void) {
    using namespace ni::cmodel::cosim;
    if (g_session_state != SessionState::Initialized) return 0;
    if (g_ever_created_master == 0) return 0;
    for (HandleBlock* h : g_handle_registry) {
        if (h->type != ShellType::Master) continue;
        auto* m = static_cast<MasterShellAdapter*>(h->adapter.get());
        if (!m->done()) return 0;
    }
    return 1;
}
```

- [ ] **Step 5: Rewrite `cmodel_dump_scoreboard` to iterate**

Replace `cosim/c/cmodel_dpi.cpp:182-197` with:

```cpp
extern "C" void cmodel_dump_scoreboard(void) {
    using namespace ni::cmodel::cosim;
    DPI_BOUNDARY_BEGIN(cmodel_dump_scoreboard) {
        if (g_scoreboard) {
            std::fprintf(stderr, "[scoreboard] %zu reads checked, %zu mismatches\n",
                         g_scoreboard->reads_checked(),
                         g_scoreboard->mismatch_count());
            for (const auto& msg : g_scoreboard->mismatch_report()) {
                std::fprintf(stderr, "  %s\n", msg.c_str());
            }
        }
        for (HandleBlock* h : g_handle_registry) {
            if (h->type != ShellType::Master) continue;
            auto* m = static_cast<MasterShellAdapter*>(h->adapter.get());
            std::fprintf(stderr, "[dump] master=%s read-dump file: %s\n",
                         h->name.c_str(), m->read_dump_path().c_str());
        }
    }
    DPI_BOUNDARY_END(cmodel_dump_scoreboard);
}
```

- [ ] **Step 6: Add aggregation + finalize-block test cases**

Append to the INITIALIZED block of `walk_session_state_machine` (after per-shell create cases from Tasks 5-9, before any finalize):

```cpp
// Case: cmodel_done with master created but not driven → 0.
//   ever_created_master ≥ 1 (Task 6 created master_handle) but master.done()
//   is false until scenario completes. This exercises BOTH the non-vacuous
//   guard (≥1 master) AND the "any master not done → return 0" path.
EXPECT_EQ(cmodel_done(), 0);
```

Append the FINALIZED-phase block to the END of `walk_session_state_machine`:

```cpp
// Case: finalize from INITIALIZED → registry destroyed, state = FINALIZED.
cmodel_finalize();
check_and_clear_error(CMODEL_DPI_OK);

// Case: cycle op on stale ctx after finalize → registry-miss → HERMETIC_VIOLATION.
cmodel_channel_model_tick(cm_handle);
check_and_clear_error(CMODEL_DPI_ERR_HERMETIC_VIOLATION);

// Case: finalize twice → second is no-op.
cmodel_finalize();
check_and_clear_error(CMODEL_DPI_OK);

// Case: cmodel_init after finalize → REINIT_FORBIDDEN (terminal state).
const char* good_yaml = std::getenv("CMODEL_TEST_SCENARIO_YAML");
cmodel_init(good_yaml);
check_and_clear_error(CMODEL_DPI_ERR_REINIT_FORBIDDEN);
```

**Test scope note**: the spec test matrix lists "`cmodel_done`, 2 masters, 1 not done → 0; both done → 1". The "both done → 1" half cannot be unit-tested without driving a full scenario through `MasterShellAdapter::tick()` in-process, which requires reproducing the cosim harness. This half is verified by the existing cosim scenario regression (`Vtb_top` exits cleanly via `cmodel_done() == 1` once scenarios complete). Document as known limitation in the test file's TEST_F docstring.

- [ ] **Step 7: Build + full regression**

```
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: ALL tests pass, including the full cosim scenario suite. This is the largest single-task surface — confirm both unit + integration green.

- [ ] **Step 8: Commit**

```
git add cosim/c/cmodel_dpi.cpp c_model/tests/cosim/test_cmodel_dpi.cpp
git commit -m "refactor(cosim): remove singletons, add cmodel_done/dump aggregation"
```

---

### Task 11: main.cpp clean-exit `cmodel_finalize`

**Files:**
- Modify: `cosim/verilator/main.cpp`

- [ ] **Step 1: Declare DPI imports in main.cpp**

After the existing `extern "C"` block (or near the includes), add:

```cpp
extern "C" void cmodel_finalize(void);
```

- [ ] **Step 2: Call `cmodel_finalize()` before clean return**

Modify `cosim/verilator/main.cpp:54-55`. Replace:

```cpp
    top->final();
    return contextp->gotError() ? 1 : 0;
```

with:

```cpp
    cmodel_finalize();
    top->final();
    return contextp->gotError() ? 1 : 0;
```

Also add `cmodel_finalize();` before the timeout-path `top->final()` at `cosim/verilator/main.cpp:41-42` for symmetry.

- [ ] **Step 3: Build + regression + commit**

```
cmake --build build && (cd cosim/verilator && make)
ctest --test-dir build -R cosim --output-on-failure
git add cosim/verilator/main.cpp
git commit -m "fix(cosim): call cmodel_finalize on clean-exit and timeout paths"
```

---

### Task 12: Cross-file doc consistency

**Files:**
- Modify: `CLAUDE.md`
- Modify: `docs/architecture.md`
- Modify: `docs/development.md`

- [ ] **Step 1: Update `CLAUDE.md:11`**

Replace the bullet:

> - **Shell adapters**: `NmuShellAdapter` / `NsuShellAdapter` / `MasterShellAdapter` / `SlaveShellAdapter` / `ChannelModelShellAdapter` in `c_model/include/cosim/`. Hermetic singleton invariant — one global per adapter in `cosim/c/cmodel_dpi.cpp`; no cross-component pointers.

with:

> - **Shell adapters**: `NmuShellAdapter` / `NsuShellAdapter` / `MasterShellAdapter` / `SlaveShellAdapter` / `ChannelModelShellAdapter` in `c_model/include/cosim/`. Per-instance via chandle ABI — `void* cmodel_<shell>_create(name)` returns a `HandleBlock*` tracked in a process-wide registry; cycle ops validate via `REQUIRE_HANDLE`. No cross-component pointers.

- [ ] **Step 2: Update `docs/architecture.md:152` — paragraph starting "Hermetic singleton invariant"**

Replace the singleton-mandate paragraph with a chandle-pattern description matching the spec's "ABI surface" + "Handle internals" sections (terse summary; link to spec for detail).

- [ ] **Step 3: Update `docs/development.md:146` — `### Hermetic singleton invariant` section**

Rename section to `### Per-instance chandle ABI`. Rewrite body to describe the create / cycle op / finalize lifecycle and the typed control block, replacing the "one global per adapter" mandate.

- [ ] **Step 4: Verify cross-file consistency**

```
git diff CLAUDE.md docs/architecture.md docs/development.md
```
Manual read: no remaining "singleton" / "one global" / "5 singletons" claims in these 3 docs.

- [ ] **Step 5: Commit**

```
git add CLAUDE.md docs/architecture.md docs/development.md
git commit -m "docs: replace singleton invariant with chandle ABI description"
```

---

## Test layout convention

`test_cmodel_dpi.cpp::walk_session_state_machine` is a **single ordered TEST_F walking one lifecycle**. The session is process-global and terminal after `cmodel_finalize`, so a single TEST_F cannot revisit earlier phases. Cases must live in their lifecycle phase block:

```
walk_session_state_machine:
  // [UNINITIALIZED phase]
  - finalize-from-UNINITIALIZED no-op            (Task 4)
  - *_create before init → ERR_NOT_INITIALIZED   (Task 2)
  - init on bad yaml → ERR_GENERIC, retry OK     (Task 3)

  // [INITIALIZED phase] — cmodel_init(good_yaml)
  - init twice → ERR_REINIT_FORBIDDEN            (Task 3)
  - channel_model_create + garbage void* test    (Task 5)
  - master_create + cm-ctx-to-master-tick mismatch (Task 6)
  - slave_create                                  (Task 7)
  - 2x nmu_create + multi-instance independence  (Task 8)
  - nsu_create                                    (Task 9)
  - cmodel_done with master not driven → 0       (Task 10)

  // [FINALIZED phase] — cmodel_finalize()
  - stale ctx → ERR_HERMETIC_VIOLATION           (Task 10)
  - finalize twice → second is no-op             (Task 10)
  - init after finalize → ERR_REINIT_FORBIDDEN   (Task 10)
```

Tasks 5-9 each insert their cases at the **end of the INITIALIZED block** (just before Task 10's done-check). Task 10 also moves the bad-yaml-retry test ordering if needed so it precedes the successful init that begins the INITIALIZED block.

## Known plan gaps (Codex-flagged, accepted)

- **Strict TDD ordering** (Codex M13): Tasks 1, 3, 4 add C-side behavior before the test that exercises it because the test target itself is bootstrapped in Task 2 and excluded-from-default-build until Task 5. The Codex-recommended reordering (test-first per task) inflates plan length without changing the outcome — accept as-is.
- **Step granularity** (Codex M14/M15): Tasks 5-9 and Task 10 each cover multiple logically related edits in one task. Per `feedback_match_ceremony_to_feature_complexity`, this is intentional — splitting per-handler would make each commit smaller than the buildability invariant allows (mid-shell-migration tree wouldn't link).
- **Multi-master `cmodel_done` matrix** (Codex H8): "both done → 1" unit test omitted; covered by cosim regression. Documented in Task 10 Step 6.
- **"Similar to Task 5" wording** in Tasks 7-9 (Codex L17): instead of repeating ~80 lines per shell, the plan points back to Task 6 for the master template (which is shown in full) and explains the per-shell deltas (ShellType, init args, scoreboard wiring vs. none). Acceptable trade-off for plan length.

## Self-review checklist

- Spec sections covered:
  - Scope in/out → Tasks 1-12 cover all "in"; "out" items (mesh topology, scenario evolution) are not touched. ✓
  - ABI surface (5 create + 15 cycle ops + global lifecycle) → Tasks 5-9 + 10. ✓
  - Handle internals → Tasks 1-2. ✓
  - SV-side pattern → Tasks 5-9 (per shell). ✓
  - Testing matrix (10 cases) → Tasks 2-10 spread the cases across the test file. ✓
  - Cross-file consistency → Task 12. ✓
  - main.cpp finalize fix → Task 11. ✓
- No "TBD/TODO/similar to Task N" placeholders.
- File paths exact.
- Each task ends with a commit and a buildable state.
