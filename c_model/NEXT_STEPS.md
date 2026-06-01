# c_model — Next Session Handoff

**Status (2026-06-01)**：Stage 2 Phase C 完工（pure AXI subsystem + AXI4 exclusive access）；182/182 tests pass (sequential)；specgen 三 domain 純 symbolic，0 error / 0 warning；GitHub Action drift gate 在線。

---

## 完成清單

- **Layer A**（bootstrap）：`Flit` / `RegisterFile` / `ni_spec.hpp`
- **Stage 2 Phase A**（pure AXI subsystem 基底）：`c_model/include/axi/{types,memory_port,memory,axi_slave,axi_master,scenario_parser,scoreboard}.hpp` + `ATTRIBUTION.md`、`c_model/tests/axi/test_*.cpp` + 12 YAML fixtures、78 tests
- **Stage 2 Phase B**（pure AXI subsystem 全 AXI4 擴展）：
  - B-1 Sparse WSTRB（`strb_file` YAML field + Scoreboard 多 beat byte-merge）
  - B-2 Unaligned start address（first beat WSTRB lane mask）
  - B-3a Bus semantics refactor（AXI4 IHI 0022 lane-positioned，Memory/Scoreboard/master read accumulator 同步改）
  - B-3b Narrow transfer（AxiMaster narrow byte-lane W push + 2 fixtures）
  - B-4 WRAP + FIXED burst（`beat_addr` 共用 helper + WRAP-aware OOB + parser 接受/拒絕約束 + 3 fixtures）
  - B-5a 4KB cross + OperationContext + per-ID FIFO refactor（AxiSlave `map<id, deque<state>>`、AxiMaster `OperationContext` + `split_into_sub_bursts`、1 WriteResult per scenario_txn）
  - B-5b Runtime protocol validation（`protocol_rules.hpp` + `AXI_PROTOCOL_ASSERT` macro + ~22 helpers + 27 EXPECT_DEATH tests + 2 combined fixtures）
- **Stage 2 Phase C**（AXI4 exclusive access）：
  - LockType enum + ScenarioTransaction.lock + parser
  - 6 stateless protocol rules + 1 monitor match helper + compute_tag_range helper (shared)
  - AxiSlave per-ID exclusive_tags_ + 6-event state machine (E1 AR set, E2 normal overlap erase, E3 exclusive AW match+erase, E4 W suppress, E5 R ready, E6 B priority)
  - WriteResult.lock + Scoreboard commit gating for failed exclusive
  - 4 integration fixtures (exclusive_pair_success, exclusive_intervening_write, exclusive_no_prior_read, exclusive_wrap_pair_success)
  - ~32 new tests; sequential ctest ~181/181
- **Stage 1 AxiSlavePort 已 superseded**：原 `c_model/include/nmu/axi_slave_port.hpp` + tests 已刪；新設計改用 `c_model/include/axi/axi_slave.hpp`（normal AXI slave controller，不是 NMU forwarder）

---

## 已知限制（merge 後 follow-up）

- **`split_into_sub_bursts` `beats_to_4kb == 0` fallback**：unaligned tail-of-page INCR multi-beat 會產生 4KB-crossing sub-burst。Debug build `check_4kb_cross` runtime assert (at AxiSlave) 會 catch；source-level fix 是 follow-up。
- **Unaligned size=5 len=0 1-beat squeeze**：AXI4 spec 要求 2 beats，c_model 只送 1 beat + trailing 0 padding。已在 `unaligned_start.yaml` 註解標明。
- **Parallel ctest tempfile collision**：`ScenarioParser` 系列 tests 共用 `testing::TempDir() + "/scenario.yaml"`，`-j N` 會 flake。Sequential ctest 100% clean；fix 是改 per-test unique 名。
- **AxiMaster same-id concurrent operations 仍 disallowed**：sub-burst stacking 只發生在同一個 operation 內，跨 operations 還是 1-per-id。需要時可以擴展。
- **Phase C single-master only**：multi-master exclusive scenarios deferred. Monitor 不模擬不同 master 競爭。
- **Phase C cache/prot match**：`check_exclusive_write_matches_read_tag` 比對 cache + prot，但 ScenarioTransaction 未暴露 YAML 欄位，預設皆 0。若 RTL co-sim 需要 cache/prot 觀測，需擴充 YAML schema。
- **Phase C orphaned tag**：exclusive AR fired 但 paired AW 未送 → tag 永留直到同 ID AR overwrite 或 overlap write 清除。沒 stale-tag 計數器。

---

## Phase C audit follow-ups (deferred to future stage)

Verification audit (2026-06-01) found the following Important coverage/quality
gaps to address in a future stage:

- **A7.2.4 release-build enforcement**: exclusive constraints only checked via
  AXI_PROTOCOL_ASSERT (debug-only). Release build silently accepts illegal
  configs. Consider moving the 5 stateless checks into scenario_parser.hpp for
  unconditional rejection.
- **Cross-feature coverage gaps**: exclusive + narrow size (all exclusive tests
  are size=5); WRAP + sparse WSTRB; mixed-lock multi-outstanding; partial-OOB
  4KB-split DECERR.
- **Exclusive read byte verification**: ReadResult does not carry lock;
  scoreboard cannot validate exclusive read returned bytes. Consider adding
  ReadResult.lock + scoreboard read-side validation.
- **A4.1.3 unaligned full-width byte-lane drift**: c_model truncates user bytes
  when unaligned-prefix + lane-positioned span would exceed bus; AXI4 spec
  requires additional beat. Currently masked by fixture trailing-zero padding.
  RTL co-sim will surface this drift.
- **Positional aggregate-init brittleness**: WriteResult, ReadResult,
  WriteBurstState, ExclusiveTag all use positional `{...}` init across many
  callsites. Future field additions risk silent mis-binding. Consider migrating
  to designated initializers (C++20) or factory functions.
- **ExclusiveWRAP_TagRangeIsWrapWindow weak coverage**: cannot distinguish
  WRAP-branch from INCR-branch arithmetic when EXCLUSIVE_ALIGN forces
  wrap_lower==addr. Acceptable but worth a non-exclusive WRAP variant if test
  pool expands.

---

## 下一步：Stage 3 NoC integration（或 Phase D 視 roadmap）

- DPI bridge → 解鎖 handshake-level rules (`*_VALID_STABLE` 等)；SV testbench + Verilator integration
- NMU 重新設計 AXI slave forwarder
- 多 master exclusive 場景（Phase C deferred）

---

## Drift gates — every commit must pass

```
cd ../specgen
py -3 -m pytest -q                         # 159 tests
py -3 tools/codegen.py --check             # byte-identical .h / .sv
py -3 tools/gen_inventory.py --check       # FEATURE_INVENTORY drift
cd ../c_model && cmake --build build && ctest --test-dir build -j 1  # 182/182 sequential
```

GitHub Action 自動跑前 3 條 + `py -3 -m ni_spec ../spec/ni/doc`（要求 0 error / 0 warning）。

---

## Inputs the next session should consult

- `../../docs/noc_cmodel_rtl_plan.md` — NoC c_model + RTL integration plan
- `../../docs/superpowers/specs/2026-05-31-pure-axi-subsystem-phase-b-design.md` — Phase B design
- `../../docs/superpowers/specs/2026-05-31-pure-axi-subsystem-phase-c-design.md` — Phase C design
- `../specgen/generated/cpp/{ni_signals.h, ni_flit_constants.h}` — codegen 常數
- `include/axi/ATTRIBUTION.md` — cocotbext-axi MIT port mapping
- `include/axi/*.hpp` — Phase A/B/C class，仿照 style 擴展 Stage 3 (NMU/NSU)

---

## Process expectations (from saved memories)

- **OSS-first survey**：寫 source / test 前先看 OSS 對應實作
- **Subagent-driven**：每 stage 派 implementer subagent + spec reviewer + code quality reviewer
- **Karpathy 4-lens review** 每 task 完成後跑：overcomplication / surgical / surface assumptions / verifiable success
- **Concise doc style**：spec ~200-400 行內、不放展開的設計理由
- **不重新設計 protocol**：AXI standard behavior 不該由 c_model implementer 設計；runtime validation 是「監控合規」、不算重新設計
- **AXI4 lane-positioned bus semantics**：byte_lane = bus_addr & (DATA_BYTES-1)；narrow/unaligned 一律遵守
- **Don't bypass commit hooks**（no `--no-verify`）
