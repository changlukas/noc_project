# Verification Terminology and OSS Precedents

本檔收 `test_environment.md` 用到的標準術語與 OSS 先例對映，與主文件分開，讓主文件保持乾淨。

## 標準術語

本文件使用下列常見 DV 術語：`co-simulation`、`lockstep / step-and-compare`、`differential testing`、`directed / synthetic microbenchmark validation`、`trace-based validation`、`calibration / correlation`、`golden / reference model checking`、`regression`。

**Verification vs Validation**：Verification 是符合 spec，Validation 是符合現實/RTL（perf 對得上）。本方法論兩者都做。

## OSS 先例（按軸對映）

本設計是 hybrid（cycle-accurate NoC perf model 又當 RTL reference），無單一 OSS 全等。

| 軸 | 先例類型 | 借用 | 差異 |
|---|---|---|---|
| 功能 reference + RTL co-sim 結構 | RISC-V 核心 ISS/RTL lockstep | reference 與 RTL 在明確同步點（transaction 完成 / 介面事件，非每內部 cycle）differential | ISS 是功能 model、無 timing |
| reference model qualification（無外部 golden）| spec-derived qualification | 靠 spec（`protocol_rules`）衍生 ABV + analytic oracle + frozen vectors | 本設計更靠 spec-derived，無 vendor golden |
| timing / perf fidelity | C++ NoC perf model（wormhole mesh）| synthetic traffic + latency-throughput curve + correlation | 不做 RTL-reference co-sim、不模 AXI |
| 介面 per-cycle 協定合法性（AXI handshake / credit / VC）| trace-to-RTL 介面驗證 | C model 產 trace，replay 過 RTL 檢查介面合法 | 方向：RTL 驗 C |

本設計概括：cycle-accurate NoC C++ reference model，靠 spec-derived oracle、ABV、frozen vectors 完成 reference-model qualification，再與 RTL 做 differential co-simulation。功能在 transaction 與介面同步點 cycle-exact 比對，bulk timing 用 synthetic-traffic latency-throughput correlation 校準。

OSS 對 cycle accuracy 的覆蓋：多數 NoC perf model（wormhole mesh、trace-driven）不追全系統 cycle-exact，只驗 latency-throughput fidelity。僅在介面協定逐 cycle contract（AXI handshake、credit/VC）才 cycle-exact，bulk 用 correlation。

## 驗證資源 OSS（功能正確性來源）

| 角色 | 工具類型 | 語言 / License | 可信度 |
|---|---|---|---|
| AXI master | SV random AXI master / file-driven master | SV，Python | 業界常用，多次 silicon 使用紀錄 |
| AXI protocol checker | SVA-based AXI4 FVIP（assertion-based，對照 IHI0022E）| SVA / ISC | assertion-based，標準規範對照 |
| AXI slave | SV random AXI slave | SV，Python | 同上 |
| memory / DDR | 功能：AXI RAM behavioral model；DDR 時序：cycle-accurate DDR timing model | SV / Python，C++ | trace→model 自驗 |
| flit driver/monitor | 客製 flit driver/monitor；標準 NoC traffic pattern（pattern 靈感）| SV，C++ | spec-derived |
| C/RTL 共用 stimulus | cocotb Python AXI extension（Python 一份驅兩 DUT）；SystemC TLM AXI bridge | Python / MIT，C++ / Apache | co-sim 廣用 |

**OSS reuse 注意事項**：
- traffic-job pattern generator：需客製實作（注入率、mem init 產生）。
- Hsiao SECDED generator：開源工具通常限 `k≤120` < whole-flit ~396-bit，需 fork 或自行實作。
- SVA-based FVIP 在 Verilator 下 SVA 支援有限，VCS 較完整。

## 使用範圍與限制

OSS 可提供 AXI master stimulus、AXI 協定合法性檢查（SVA）、AXI slave/memory/DDR model、NoC traffic pattern 參考。OSS 不提供客製 NMU 的 flit-encoding golden。客製 flit golden 可用 NMU+NSU loopback 的 AXI-in == AXI-out 比對（cross-ID reorder tolerated，同 ID 保序）降低依賴，或由 spec 推導 / C model（qualification 後）產生。

## 必讀

先讀 RISC-V 核心 ISS/RTL co-sim 文件（co-sim 結構，硬體 DV 最易上手），再讀 C++ NoC perf model（NoC timing 驗證），最後讀 trace-to-RTL 介面技術文件。

## Sources

AXI master/slave/memory SV/Python tools (IHI0022E-aligned)、SVA-AXI4 FVIP、cocotb AXI extension library、SystemC TLM AXI bridge、Hsiao SECDED generator、cycle-accurate DDR timing model、C++ NoC perf models (wormhole mesh family)、standard NoC traffic patterns (Dally & Towles et al.)。
