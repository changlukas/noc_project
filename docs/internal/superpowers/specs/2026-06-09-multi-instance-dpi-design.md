# Multi-instance DPI refactor — lift 5-singleton invariant

**Date:** 2026-06-09

Lift the singleton invariant in `cosim/c/cmodel_dpi.cpp` so the DPI layer can host N instances per shell. Mesh topology / scenario evolution deferred to a future `noc_config` session.

## Scope

**In:** chandle-based per-instance DPI ABI; typed handle + registry validation; single-shot session state machine; aggregation for `cmodel_done` / `cmodel_dump_scoreboard`; SV-side chandle-as-port + centralized lifecycle in `tb_top.sv`.

**Out:** mesh topology, scenario YAML evolution, routing tables, per-instance scenario sections, per-master scoreboards. N>1 master ABI-capable but scenario-semantics unspecified until `noc_config`.

**Cross-file consistency updates** (must land in same series):
- `CLAUDE.md:11` — drop "Hermetic singleton invariant — one global per adapter"
- `docs/architecture.md:152` — same paragraph
- `docs/development.md:146` — `### Hermetic singleton invariant` section

## Ground truth

- `cosim/c/cmodel_dpi.cpp:48-52` — 5 singletons being lifted
- `cosim/c/cmodel_dpi.cpp:61-142` — current `cmodel_init` (constructs adapters; to be split: parse-only here, construct in `*_create`)
- `cosim/c/dpi_boundary_macros.h:27-51` — `DPI_BOUNDARY_*` / `REQUIRE_ADAPTER` (void-return only; needs return-variant)
- `cosim/sv/tb_top.sv:349-353` — existing convention: centralized lifecycle, no per-wrap finalize
- `cosim/verilator/main.cpp` — missing `cmodel_finalize` on clean exit; bundled fix here

## ABI surface

Per shell ×5 (master / slave / nmu / nsu / channel_model):

```c
void* cmodel_<shell>_create(const char* name);          // SV initial; nullptr on error
void  cmodel_<shell>_set_inputs(void* ctx, /* sig */);  // gain leading ctx
void  cmodel_<shell>_tick(void* ctx);
void  cmodel_<shell>_get_outputs(void* ctx, /* sig */);
```

No per-shell close export — close is internal, driven by `cmodel_finalize`.

| Global function | Behavior after refactor |
|---|---|
| `cmodel_init(yaml)` | On entry from `UNINITIALIZED`, **clear the error latch** before parsing (enables retry after a bad-YAML failure). Parse scenario, store globally, flip to `INITIALIZED`. Failed parse keeps state `UNINITIALIZED`. From `INITIALIZED` / `FINALIZED` → `ERR_REINIT_FORBIDDEN`. No adapter construction. |
| `cmodel_finalize()` | If `INITIALIZED`: iterate registry, destroy each `HandleBlock`, flip to `FINALIZED`. If `UNINITIALIZED` / `FINALIZED`: no-op. Always idempotent. Called by `main.cpp` clean exit + SV fatal path. |
| `cmodel_done()` | `INITIALIZED ∧ ever_created_master ≥ 1 ∧ all_live_masters.done()` |
| `cmodel_scoreboard_clean()` | Unchanged (single global scoreboard) |
| `cmodel_dump_scoreboard()` | Iterate master registry, label each dump path by instance name |
| `cmodel_check_error()` | API signature unchanged. Behavior changes: first-error-wins applies — handlers skip error update if `g_dpi_error_code != OK`. In production, `cmodel_init` entered from `UNINITIALIZED` is the only latch-clearing event (test code also has a friend-access reset; see Testing). |

Per-shell `*_create(name)` reads its config from the scenario state set by `cmodel_init`:

| Shell | Reads from scenario state | Per-instance derivation from `name` |
|---|---|---|
| `master` | `max_outstanding_write`, `max_outstanding_read`, `inject` config | `read_dump_path = "master_shell_read_dump_" + name + ".txt"`; wires scoreboard callbacks |
| `slave` | `memory_base`, `memory_size`, `write_latency`, `read_latency` | — |
| `nmu` / `nsu` / `channel_model` | none — `init()` called argument-less | — |

`name` is copied into `HandleBlock::name`. No uniqueness check enforced; collisions only affect debug labels and master dump-file paths (caller's responsibility).

New macros + error code:

```c
#define DPI_BOUNDARY_BEGIN_R(fn_name, fail_value)  \
    auto _dpi_fail = (fail_value); try               // capture for END_R to return
#define DPI_BOUNDARY_END_R(fn_name)                                                \
    catch (const std::exception& e) { /* first-error-wins set */; return _dpi_fail; } \
    catch (...)                     { /* first-error-wins set */; return _dpi_fail; }

enum { /* existing */, CMODEL_DPI_ERR_REINIT_FORBIDDEN = 6 };
```

## Handle internals

```cpp
enum class ShellType { Master, Slave, Nmu, Nsu, ChannelModel };
enum class HandleState { Live };  // only Live; closed handles are removed from registry

struct HandleBlock {
    uint32_t    magic;
    ShellType   type;
    HandleState state;
    std::string name;
    std::unique_ptr<void, void(*)(void*)> adapter;  // type-erased; deleter knows real type
};

namespace ni::cmodel::cosim {
    extern std::unordered_set<HandleBlock*> g_handle_registry;
}
```

`validate_handle(ctx, expected_type, fn_name)` checks in order; fail → `ERR_HERMETIC_VIOLATION`, return nullptr:

1. `g_handle_registry.find(ctx)` — avoids garbage `void*` SIGSEGV
2. cast → `HandleBlock*`, check `magic`
3. check `type == expected`
4. check `state == Live`

`cmodel_finalize` deletes each `HandleBlock` and `erase`s from registry. Post-finalize handle access fails at step 1 (not a tombstone — block is gone; this is `ERR_HERMETIC_VIOLATION`, distinct from any "Closed" state).

Session state transition table:

| State | `cmodel_init` | `cmodel_finalize` | `*_create` / cycle ops |
|---|---|---|---|
| `UNINITIALIZED` | parse, clear error latch, → `INITIALIZED` (on failure stays `UNINITIALIZED`) | no-op | `ERR_NOT_INITIALIZED` |
| `INITIALIZED` | `ERR_REINIT_FORBIDDEN` | destroy registry, → `FINALIZED` | allowed |
| `FINALIZED` | `ERR_REINIT_FORBIDDEN` | no-op (idempotent) | rejected — `*_create` → `ERR_NOT_INITIALIZED`; cycle op on stale ctx → `ERR_HERMETIC_VIOLATION` (registry empty) |

Generation field absent by design: reject-second-init blocks address aliasing without it.

## SV-side pattern

Wraps gain `input chandle ctx_i`. No `initial` block for lifecycle in wraps.

```sv
module nmu_wrap (
    input  logic     clk_i,
    input  logic     rst_ni,
    input  chandle   ctx_i,                  // NEW
    axi4_intf.slave  axi_i,
    noc_intf.mosi    noc_mosi_o
);
    always_ff @(posedge clk_i) begin
        if (!rst_ni) /* reset */;
        else begin
            cmodel_nmu_set_inputs(ctx_i, axi_i.awvalid, /* ... */);
            cmodel_nmu_tick(ctx_i);
            cmodel_nmu_get_outputs(ctx_i, /* ... */);
        end
    end
endmodule
```

`tb_top.sv` owns all lifecycle in **one** `initial` block (single-block sequencing is the init-ordering guarantee — wrap `always_ff` fires after `initial` completes at time 0, so `ctx_i` is always populated before the first DPI cycle call):

```sv
chandle master_ctx, nmu_ctx, cm_ctx, nsu_ctx, slave_ctx;

initial begin
    cmodel_init(scenario_path);
    master_ctx = cmodel_master_create("master_0");
    nmu_ctx    = cmodel_nmu_create("nmu_0");
    cm_ctx     = cmodel_channel_model_create("channel_model_0");
    nsu_ctx    = cmodel_nsu_create("nsu_0");
    slave_ctx  = cmodel_slave_create("slave_0");
end
```

Future multi-instance: same single `initial` block extends with `for (int i = 0; i < NUM_NMU; i++) nmu_ctx[i] = cmodel_nmu_create(...)` inside the same block; `generate` blocks wire `.ctx_i(nmu_ctx[i])` to each wrap. Wrap module code unchanged.

## Testing

| Test | Scope | Status |
|---|---|---|
| `c_model/tests/cosim/test_*_shell_adapter.cpp` | adapter classes; bypass DPI | unchanged |
| `c_model/tests/cosim/test_cmodel_dpi.cpp` | extern "C" ABI: handle validation, session state machine, `cmodel_done` aggregation | new |
| Verilator cosim scenario regression | N=1 end-to-end | existing scenarios must pass |

`test_cmodel_dpi.cpp` is **one ordered TEST_F walking the session state machine** — separate `TEST`s would collide on the process-global session. Each negative-case assertion calls a local `check_and_clear_error(code)` helper that asserts the expected code, then resets `g_dpi_error_{code,msg}` via a friend / test-only accessor before the next assertion.

Cases covered (in the order they fit a single session lifecycle):

| Case | Expected |
|---|---|
| `*_create` before `cmodel_init` | nullptr + `ERR_NOT_INITIALIZED` |
| `cmodel_init` on bad YAML | error set; state stays `UNINITIALIZED`; retrying `cmodel_init` from `UNINITIALIZED` clears the prior latch on entry, then good YAML succeeds |
| `cmodel_init` twice (both succeed) | second → `ERR_REINIT_FORBIDDEN` |
| Create 2 NMU adapters | distinct `void*`; both validate as live |
| NMU ctx → `cmodel_nsu_set_inputs` | `ERR_HERMETIC_VIOLATION` (type mismatch) |
| Garbage `void*` (non-registry) | `ERR_HERMETIC_VIOLATION` (membership fail) |
| `cmodel_done`, 0 masters created | returns 0 (non-vacuous) |
| `cmodel_done`, 2 masters, 1 not done | 0; both done → 1 |
| `cmodel_finalize`, then cycle op on stale ctx | `ERR_HERMETIC_VIOLATION` (membership fail) |
| `cmodel_finalize` twice | second is no-op |
| `cmodel_init` after finalize | `ERR_REINIT_FORBIDDEN` |

End-to-end cosim stays N=1 until `noc_config` adds multi-traffic scenarios; multi-instance ctx-independence coverage lives in `test_cmodel_dpi.cpp`.
