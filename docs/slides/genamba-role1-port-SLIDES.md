# gen_amba Role-1 Testbench Port — Slides 3-6

Source material: `docs/superpowers/specs/2026-06-08-genamba-role1-testbench-design.md`,
`...-findings.md`, `cosim/sv/genamba/ATTRIBUTION.md`. Body in English, speaker
notes in Traditional Chinese.

---

## Slide 3. Testbench Architecture

**Takeaway:** The gen_amba golden VIP drives the NMU/NSU bridge point-to-point — both observation points sit on plain AXI4.

> **Bridge correctness needs evidence from a VIP that shares no code or assumptions with the existing wb2axip path.**

**Visual:**

~~~
                 tb_genamba -- self-clocked SV top (ACLK = 10 ns)

 +--------------+  AXI4   +--------+    noc_intf     +--------+  AXI4   +---------+
 |              | ------> |        | --req flit--->  |        | ------> |         |
 | gen_amba BFM |  AW/W   |  NMU   |                 |  NSU   |  AW/W   | mem_axi |
 |  (vendored   |   AR    |(c_model|  mosi <-> miso  |(c_model|   AR    |(vendored|
 |   task lib   |         |  via   |  direct wiring  |  via   |         |  golden |
 |   + project  | <------ |  DPI)  |  no router      |  DPI)  | <------ |  slave) |
 |   wrapper)   |  B / R  |        | <--rsp flit---  |        |  B / R  | 16 KiB  |
 +--------------+         +--------+                 +--------+         +---------+
        |                    ID=8 / ADDR=64 / DATA=256 on both AXI boundaries
        |                             credit return tied 0
        v
  per-task data compare -- $fatal on mismatch

 tb-level monitors:  DPI error pump (cmodel_check_error per cycle)
                     1 us watchdog -- $fatal if no valid&&ready handshake progress
~~~

- **Topology — role-1 point-to-point**
  - NMU mosi mates NSU miso directly
  - No router; credit return stubbed at 0
- **Clock and lifecycle**
  - Self-clocked SV top, 10 ns period
  - DPI chandle creation before reset deassert
- **Failure detection**
  - Per-task data compare, `$fatal` on mismatch
  - DPI error pump + 1 µs handshake watchdog

**Speaker notes:**

- 這是 Phase 1 的 scope：單 master、單 slave、橋接中間無 fabric。XY routing 的 dst_id 在 NMU packetize 端有算，但下游沒有 router 消費。
- ChannelModel / AxiMaster / AxiSlave 的 chandle 有 create（讓 DPI state machine 的 assertion 過），但不 tick——實際 driver 只有 gen_amba BFM 跟 mem_axi。
- Watchdog 語意：任一 channel 的 valid && ready handshake 算 progress；1 µs 無 progress 就 `$fatal` dump 十個 channel 的 valid/ready 狀態。卡死的 handshake（VALID 高 READY 永不來）也抓得到。
- 一行就能跑：`make sim-genamba`。build 產物在 `obj_genamba/`，跟 tb_top 的 `obj_dir/` 分開。

---

## Slide 4. Component: Vendored gen_amba VIP

**Takeaway:** gen_amba ships a task library plus a memory model — not a ready-made BFM — vendored with two tracked patches.

> **A golden reference only counts as independent evidence if it stays as close to upstream as possible.**

**Visual:** File table of `cosim/sv/genamba/`:

| File | Role | Status |
|---|---|---|
| `axi_master_tasks.v` | channel-level AXI task primitives | Modified (2 patches) |
| `mem_test_tasks.v` | write-read-compare self-check | Modified (2 patches) |
| `mem_axi.v` | behavioural AXI slave memory | Pristine |

- **What upstream provides**
  - Task bodies only — no module shell, no ports
  - Reference tester wired for 32-bit bus
- **Patches, tracked in ATTRIBUTION.md**
  - `mem_test`: mask-offset width, watcher removal
  - `write_b`: B-latch read, `error_flag` escalation
- **What stays pristine**
  - `axi_master_read_r` — burst drains bypass it

**Speaker notes:**

- Upstream：github.com/adki/gen_amba_2021，commit 4ba7903，2-clause BSD。
- 為什麼有 patch：全部源自 Verilator `--timing` 的 procedural-vs-NBA 讀值語意（下一頁細講）。patch 原則是最小 diff + ATTRIBUTION.md 逐條記錄 root cause，方便 upstream PR 或日後 re-vendor。
- `error_flag` escalation：upstream 的 BID/BRESP/RID/RRESP/RLAST 檢查只 `$display` 不 fail——protocol error 會默默放行。patch 後每個 check 額外設 `error_flag`，由 project 端 trap 轉成 `$fatal` + 非零 exit。有做 fault injection 證明會 fire。
- `mem_axi` 完全沒動——它是 golden slave，動了就失去 reference 意義。

---

## Slide 5. Component: Project BFM Wrapper

**Takeaway:** A project module shell hosts the vendored task library; the adapter layer and R-shadow array make it safe under Verilator `--timing`.

> **Vendored tasks read bus signals procedurally after `@(posedge)` — under Verilator `--timing` that returns the next cycle's value, not the handshake cycle's.**

**Visual:** BFM internal diagram, three zones: (1) module ports — full AXI4 5-channel surface; (2) `` `include`` zone — vendored task bodies resolve wrapper-owned `dataW`/`dataR`; (3) project zone — adapter tasks + parallel shadow-capture `always` block.

- **Adapter task layer**
  - `bfm_post_aw / _w / _ar`, `bfm_drain_b`
  - Semantic names over positional vendored calls
- **Project-owned read drain**
  - `bfm_drain_r` never calls vendored `read_r`
  - Checks RID / RRESP / RLAST per beat
- **R-shadow capture**
  - Parallel `always` records every R handshake
  - NBA write counter vs blocking read counter

**Speaker notes:**

- 用 RTL 思維講 race：procedural code 從 `@(posedge ACLK)` 醒來時，Verilator 排在 NBA region 之後——讀到的是「下一拍視角」。NMU 正確做 AXI4 §A3.2.1 held-latch（handshake 後一拍 deassert），所以 vendored task 讀 BID/RID 會拿到 deassert 後的 0。Icarus/ModelSim 讀 pre-NBA 沒這個問題，這是純 simulator 語意差異，不是 bridge bug。
- 為什麼 read drain 要整個重寫而不是 patch：vendored 的 per-beat loop 每 iter 吃 2 cycle，但 bridge 推 R 是 1 beat/cycle——blen≥2 時 task 會在倒數第二 beat 餓死 hang。shadow array 把捕捉移到 parallel always block（每個 RVALID&&RREADY 把 data+metadata 寫進 array），drain task 只等 counter 差值到 blen 再整批取走。
- 每個 test wrapper 進場先做 barrier（`r_shadow_ridx = r_shadow_widx`）丟掉前一個 task 的殘留 entries。
- per-beat metadata check（RID==期望 id、RRESP==OKAY、RLAST 只在最後 beat）也做過 fault injection：故意 AR id=5 / drain 期望 id=6，checker fire + 非零 exit。
- 已知假設：bridge 目前跨 ID 也保 AR-issue 順序（單 noc_intf 無競爭下實證成立）。未來 bridge 若 cross-ID 亂序回 R，shadow 要改 per-ID FIFO——code comment 有記。

---

## Slide 6. Test Patterns A–G

**Takeaway:** Seven patterns, twelve scenarios — every one ported from gen_amba's own self-check or an IHI 0022 ordering invariant, all PASS.

> **Coverage must reach the bridge's queues and RoB beyond what wb2axip's structural limits allow.**

| Task | Pattern | Stress target | Result |
|---|---|---|---|
| A | vendored `mem_test` self-check | bring-up, golden loop | PASS |
| B | burst, blen 4 / 8 / 16 | multi-beat W and R bursts | PASS |
| C | outstanding N = 4 / 8, distinct IDs | AW / AR queue depth | PASS |
| D | outstanding burst, N=4 × blen 4 / 8 | queue + RoB combined | PASS |
| E | same-ID ×4 (ID = 7) | IHI 0022 §A5.3 same-ID order | PASS |
| F | mixed R+W concurrent fork | REQ / RSP plane concurrency | PASS |
| G | deep pressure N = 8 / 16 + watchdog | stall-vs-deadlock evidence | PASS |

- **Pattern provenance**
  - Task A is gen_amba's own checker
  - B–G compose vendored channel primitives
- **Result envelope**
  - One run, 16.3 µs sim, 0.007 s wall
  - G: 84 cycles (N=8), 156 (N=16)

**Speaker notes:**

- 12 scenarios = A×1 + B×3 + C×2 + D×2 + E×1 + F×1 + G×2，單次 run 全跑。
- 每個 task 用 disjoint address window（0x0000 / 0x0400 / 0x0800 / ...），per-address 記 expected data——aliasing 蓋不掉錯誤。
- Task E 是規格 invariant 測試：AXI4 同 ID 的 R 必須照 AR issue 順序回（IHI 0022 §A5.3）。NMU ROB Disabled mode 的 per-ID filter 撐住了。
- Task G 的 watchdog 有 calibration 流程：先跑 N=8 量 84 cycles，§3.7 公式算 N=16 預算 672，遠低於 2000 上限——實跑 156 cycles，credit-stub backpressure 下 stall 不 deadlock。
- 講 limitation 要誠實：這套沒測 credit flow control（stub 固定 0）、single VC、無 router、ROB Disabled mode only——這些是 Phase 2 的 input，findings doc §4 有完整清單。
