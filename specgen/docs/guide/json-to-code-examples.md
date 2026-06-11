# From JSON spec to generated C++/SV

以 generic placeholder 展示 `specgen` 三條 codegen 通路 (packet / signals / registers)
的「authored JSON → elaborated header」轉換 pattern。
讀者：要在 spec 新增 header field / signal pin / register field 的工程師。

JSON 是 canonical source — 直接 hand-edit `specgen/generated/json/ni_*.json`，
跑 `py -3 specgen/tools/codegen.py --target cpp/sv --domain packet/signals/registers`
重生 `.h` / `.sv`。drift gate 守住 JSON ↔ cpp/sv 一致。

```
specgen/generated/json/ni_*.json   (canonical source; hand-edit here)
        │  ni_spec.constants  helpers   (header_field_width / signal_pin_width / ...)
        │  tools/codegen.py
        ▼
generated/cpp/ni_*.h   +  generated/sv/ni_*_pkg.sv   (byte-identical 雙產物)
```

Drift gate: `tools/codegen.py --check` 比對 working tree 與 elaborate 結果，CI/pre-commit 強制 byte-equal。

---

## Pipeline overview

- **`ni_spec.constants`** 為 firewall：所有 consumer (codegen / invariants / tests) 透過它讀 JSON，
  不直接 reach into JSON shape；寬度與 bit-position 在此 lazy evaluate。
- **Elaborator** (`tools/codegen.py` + `tools/elaborate/*`) 呼叫 helper 將 symbolic 值
  evaluate 成整數，輸出 `.h` 與 `.sv` 兩份 byte-identical artifact。

---

## Domain A — Packet header field

**JSON excerpt** (`specgen/generated/json/ni_packet.json`, purely symbolic):

```json
{
  "flit": {
    "field_widths": { "<WIDTH_PARAM>": <int_default>, ... },
    "header_fields": [
      { "name": "<FIELD_NAME>", "width_param": "<WIDTH_PARAM>", "enabled": true }
    ]
  }
}
```

Elaborator 透過 `C.header_field_width(spec, "<FIELD_NAME>")` evaluate width，
`C.header_field_position(spec, "<FIELD_NAME>")` 依宣告順序累加得 `(lsb, msb)`。

**Elaborated `generated/cpp/ni_*_constants.h`**:

```cpp
namespace ni { namespace header {
constexpr int  <FIELD_NAME>_LSB     = /* cumulative lsb */;
constexpr int  <FIELD_NAME>_MSB     = /* lsb + width - 1 */;
constexpr int  <FIELD_NAME>_WIDTH   = /* eval(width_param) */;
constexpr bool <FIELD_NAME>_ENABLED = true;
}}  // namespace ni::header
```

**Elaborated `generated/sv/ni_*_pkg.sv`**:

```systemverilog
package ni_flit_pkg;
  localparam int unsigned <FIELD_NAME>_LSB     = /* resolved lsb */;
  localparam int unsigned <FIELD_NAME>_MSB     = /* resolved msb */;
  localparam int unsigned <FIELD_NAME>_WIDTH   = /* eval(width_param) */;
  localparam bit          <FIELD_NAME>_ENABLED = 1'b1;
endpackage
```

Width-0 placeholder (e.g. `enabled=false` 或 `<WIDTH_PARAM>=0`) 只 emit `_WIDTH=0` + `_ENABLED`，
不分配 `_LSB` / `_MSB`，避免 downstream 誤用空位。

目前 spec 中的實際 header fields 可在 `generated/json/ni_packet.json` 查到。

---

## Domain B — Signals AXI / NoC pin

Signals JSON 兩層巢狀 (`interfaces[] → channels[] → signals[]`)，加上 interface-local
`port_parameters[]`。Pin 寬度可 reference interface 本地參數或 packet domain symbol。

**JSON pin entry** (`generated/json/ni_signals.json`, pin 已無 `default`):

```json
{
  "name": "<INTERFACE>",
  "port_parameters": [
    { "name": "<WIDTH_PARAM>", "type": "int", "default": "<int_value>" }
  ],
  "channels": [
    { "name": "<CHANNEL>", "direction": "input",
      "signals": [
        { "pin_name": "<PIN_NAME>", "width_param": "<WIDTH_PARAM>",
          "width_expr": null,
          "reset_behavior": { "kind": "external_driven" } }
      ] }
  ]
}
```

Single symbol 寫 `width_param`，expression 寫 `width_expr`。Elaborator 透過
`C.signal_pin_width(signals_spec, packet_spec, "<INTERFACE>", "<PIN_NAME>")` resolve。
Namespace 合併順序：packet `field_widths` → packet derived totals (`FLIT_WIDTH` 等)
→ interface-local `port_parameters` (最近 scope 優先)。

**Elaborated `.h` / `.sv`** (reset constants for output pins):

```cpp
namespace ni { namespace signals {
constexpr int <PIN_NAME_UPPER>_RESET = 0;   // 僅 output / driven-by-NI pin
}}
```
```systemverilog
package ni_signals_pkg;
  localparam int unsigned <PIN_NAME_UPPER>_RESET = 0;
endpackage
```

Input pin (`reset_behavior.kind = "external_driven"`) 不出現在 `_RESET` 清單；
寬度仍由 `signal_pin_width` 提供，給 interface template 用於 port list。

目前 spec 中的實際 signal pins 與 interface port_parameters 可在 `generated/json/ni_signals.json` 查到。

---

## Domain C — Registers

Registers JSON 為 flat 列表，每筆是一個 register；register 內可選擇宣告 `fields[]`
(bit-slice 子欄位)。Width 與 reset 為 expression string，便於未來 reference 其他 symbol。

**JSON register entry** (`generated/json/ni_registers.json`):

```json
{
  "offset": "0x<HEX>", "name": "<REG_NAME>", "kind": "register",
  "access": "RW", "reset_expr": "0x0", "width_expr": "32",
  "fields": [
    { "name": "<REG_FIELD>", "bit_high": <hi>, "bit_low": <lo>,
      "reset": "0x0", "description": "<text>" }
  ]
}
```

`access`: `RO` / `RW` / `RW1C` / `WO` (見 `generated/cpp/ni_regs.h` 的 `AccessMode` enum)。無 sub-field 的 register 省略 `fields` 鍵。
Helper: `C.regs_offsets(spec)` / `C.regs_field_mask(spec, "<REG_NAME>", "<REG_FIELD>")` /
`C.regs_access_mode(spec, "<REG_NAME>")`。

**Elaborated `.h`**:

```cpp
namespace ni { namespace regs {
constexpr int    <REG_NAME>_OFFSET           = 0x<hex>;
constexpr int    <REG_NAME>_<REG_FIELD>_MASK = /* ((1<<(hi-lo+1))-1) << lo */;
enum class AccessMode { RO, RW, RW1C, WO };
constexpr AccessMode <REG_NAME>_ACCESS = AccessMode::RW;
constexpr uint32_t   <REG_NAME>_RESET  = 0x0;
}}
```

**Elaborated `.sv`**:

```systemverilog
package ni_regs_pkg;
  localparam int unsigned <REG_NAME>_OFFSET           = 0x<hex>;
  localparam int unsigned <REG_NAME>_<REG_FIELD>_MASK = /* resolved mask */;
endpackage
```

`.h` 另含 `static_assert(field_width_sum <= data_width, ...)`，compile time 抓 field 加總超過寬度。

目前 spec 中的實際 registers 可在 `generated/json/ni_registers.json` 查到。

---

## Cross-domain resolution

Signals 可以 reference packet domain symbol，常見於 NoC link：

```json
// signals JSON: NoC link pin 拿 packet domain 的 FLIT_WIDTH
{ "pin_name": "<PIN_NAME>", "width_param": "<PACKET_SYMBOL>" }
```

`<PACKET_SYMBOL>` 可為 `FLIT_WIDTH` / `HEADER_WIDTH` / `PAYLOAD_WIDTH` / `LINK_WIDTH`
或任何 packet `flit.field_widths` 內的 name。`signal_eval_expr` 的 namespace
同時看 packet `field_widths` + packet derived totals + interface `port_parameters`，
這條 cross-domain edge 不需在 JSON 複製數值，也不會 drift。

---

## Schema & validation footer

`generated/json/ni_*.schema.json` 是 JSON Schema Draft 2020-12 validation gate，
schema validation 由 `ni_spec.invariants` 以 jsonschema Draft 2020-12 執行（`load_doc` 本身不驗證）；不參與 codegen。

`ni_function_blocks.json` 為 spec-validation 元資料 (feature inventory + cross-domain
consistency checks)，不 emit C/SV，不在本文範圍。
