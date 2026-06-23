# Using constants

Two consumer surfaces exist: generated C++ headers (the common case) and the `ni_spec.constants` Python API (for scripts and custom tooling).

## In C++

Include the header and reference constants directly:

```cpp
#include <cstdio>
#include "ni_flit_constants.h"

int main() {
    std::printf("FLIT_WIDTH = %d\n", ni::FLIT_WIDTH);
    std::printf("dst_id at [%d:%d]\n",
                ni::header::DST_ID_MSB, ni::header::DST_ID_LSB);
    std::printf("AW payload width = %d\n", ni::payload::AW_WIDTH);
    return 0;
}
```

Compile with `-I specgen/generated/cpp`:

```bash
g++ -std=c++17 -I specgen/generated/cpp your_code.cpp -o your_code.exe
```

All values are `constexpr int` (or `constexpr bool` for `_ENABLED` flags), so the compiler folds them to immediates. The resulting binary has no Python dependency.

### Namespacing

| Namespace | Contents |
|-----------|----------|
| `ni::` | Top-level widths (`FLIT_WIDTH`, `HEADER_WIDTH`, `PAYLOAD_WIDTH`, etc.). |
| `ni::header::` | Per-header-field `<NAME>_LSB` / `_MSB` / `_WIDTH` / `_ENABLED`. |
| `ni::payload::` | Per-channel `<CH>_WIDTH` (`AW`, `AR`, `W`, `B`, `R`). |
| `ni::width::` | Every named width from the spec's `field_widths` table. |

### Worked sample

`examples/use_constants.cpp` packs an AR header using the generated bit positions:

```cpp
uint64_t hdr_lo = 0;
hdr_lo |= (uint64_t)(0x2)  << ni::header::AXI_CH_LSB;   // AR
hdr_lo |= (uint64_t)(0x05) << ni::header::SRC_ID_LSB;
hdr_lo |= (uint64_t)(0x12) << ni::header::DST_ID_LSB;
hdr_lo |= (uint64_t)(0x1)  << ni::header::LAST_LSB;
hdr_lo |= (uint64_t)(0x1)  << ni::header::ROB_REQ_LSB;
hdr_lo |= (uint64_t)(0x7)  << ni::header::ROB_IDX_LSB;
```

Build and run via [Quickstart § 3-4](quickstart.md#3-compile-the-sample).

## In Python

For scripts that consume the spec directly (test stimulus generation, custom elaborators, validation), use the firewall API instead of touching JSON shape:

```python
from ni_spec.loader import load_doc
from ni_spec import constants as C

packet = load_doc("generated/json/ni_packet.json")

C.flit_width_resolved(packet)                  # int -- total bits per flit
C.header_field_position(packet, "dst_id")      # (lsb, msb) -- or None if width=0
C.payload_field_position(packet, "AW", "addr")
C.header_field_enabled(packet, "axi_ch")
C.axi_channel_encoding(packet)                 # {"AW": 0, "W": 1, "AR": 2, ...}
```

### Full API surface

Each function takes the loaded spec as its first argument.

#### Packet (`generated/json/ni_packet.json`)

All width/position accessors are pure functions of `field_widths` + `width_param`
(pure-parameterization refactor). The JSON no longer stores resolved
`lsb`/`msb`/`width`/`derived` — the helpers below recompute them on each call.

| Function | Returns |
|----------|---------|
| `flit_width_resolved(spec)` | `int` |
| `header_width_resolved(spec)` | `int` |
| `payload_width_resolved(spec)` | `int` (max across channels) |
| `payload_channel_width(spec, channel)` | `int` (authored per-channel width) |
| `link_width_resolved(spec)` | `int` (= `flit_width_resolved + 1 + NUM_VC`) |
| `flit_data_width_resolved(spec)` | `int` (= header + payload − ECC) |
| `header_data_width_resolved(spec)` | `int` (= header − ECC) |
| `wstrb_width_resolved(spec)` | `int` (= `NOC_DATA_WIDTH / 8`) |
| `header_field_width(spec, name)` | `int`; resolves `width_param`. Raises `FieldNotFoundError`. |
| `header_field_position(spec, name)` | `(lsb, msb)`; `None` for width-0 placeholders. |
| `payload_field_width(spec, channel, name)` | `int`; handles `width_param='derived'`. |
| `payload_field_position(spec, channel, name)` | `(lsb, msb)`; `None` for width-0 placeholders. |
| `all_field_widths(spec)` | `{name: width}` |
| `header_field_enabled(spec, name)` | `bool` — `False` means padding. Raises `KeyError`. |
| `header_fields_padding(spec)` | `list[str]` of padding field names. |
| `axi_channel_encoding(spec)` | `{channel: int}` |
| `field_encoding(spec, name)` | `{value: int}` for any field with an `encoding` table. |
| `packet_eval_expr(spec, expr)` | `int`; safe AST evaluator over `field_widths`. |

#### Signals (`generated/json/ni_signals.json`)

| Function | Returns |
|----------|---------|
| `signals_pin_names(spec)` | `list[str]` of every RTL pin name. |
| `signals_reset_domains(spec)` | `set[str]` of legal reset signal names. |
| `signals_signal_by_pin(spec, pin_name)` | `dict` (full signal entry) or `None`. |

`noc_function_blocks.json` is kept as feature inventory + cross-domain consistency check, but no longer exposes accessor functions through `ni_spec.constants` — it does not drive codegen.

## Stability contract

`ni_spec.constants` is the public Python surface. Adding new accessors is non-breaking. Renaming or removing an existing accessor breaks every elaborator and downstream consumer — treat it as a versioned API.
