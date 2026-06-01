# NMU Block Diagram (our spec, post-A4.5)

Layout follows NoC IP datasheet convention (AXI side on the left, NoC side on the right) but with our actual sub-block decomposition.

> **Width conversion is external (bolt-on).** Per ToO §Data Width Conversion, an external Width Bridge between the local AXI master and the NMU AXI port (`NOC_DATA_WIDTH`) handles `AXI_DATA_WIDTH ↔ NOC_DATA_WIDTH`. The NMU itself does not convert width.

```mermaid
flowchart LR
  subgraph AXI["AXI Slave I/F<br>(local AXI master ↔ NMU)"]
    AW(["AW"])
    W(["W"])
    AR(["AR"])
    B(["B"])
    R(["R"])
  end

  subgraph NMU["NMU (Network Master Unit)"]
    direction TB
    AT["AddrTrans<br>addr → dst_id"]
    QG["QoSGen<br>4 modes"]
    FP["FlitPack<br>AW/W/AR"]
    EG["ECC Gen<br>route_par + flit_ecc SECDED"]
    IB["Injection<br>Buffer"]
    VA["VC Arbiter<br>Hybrid R/W × QoS"]
    EC["ECC Check<br>+ correct"]
    ROB["RoB<br>Read Re-Ordering"]
    FU["FlitUnpack<br>B/R"]
    TT["Outstanding-tx<br>Timeout Tracker<br>(TXN_TIMEOUT)"]
  end

  subgraph CSR_BLK["CSR file (aclk domain)"]
    CSR["ERR_STATUS / IRQ_ENABLE<br>QoS / Probes / Runtime Ctrl"]
    IRQ((("irq_o")))
  end

  NoC[("NoC fabric")]

  AW --> AT
  AR --> AT
  AT --> QG --> FP --> EG --> VA
  W --> IB --> VA
  VA --> NoC
  NoC --> EC --> ROB --> FU
  FU --> B
  FU --> R
  ROB -.tracker.-> TT
  TT -.SLVERR.-> FU
  EC -.err log.-> CSR
  TT -.err log.-> CSR
  CSR --> IRQ
```

## Sub-block legend (one-line each)

| Block | Function |
|-------|----------|
| **AddrTrans** | AXI address → NoC `dst_id` + `local_addr`. XYRouting / SourceRouting / IDRouting |
| **QoSGen** | Generate flit `qos`. 4 modes: Bypass / Fixed / Limiter / Regulator |
| **FlitPack** | Pack AXI request fields into flit payload |
| **ECC Gen** | Compute `route_par` (1-bit even parity over `{dst_id, last}`, 9 bits) + `flit_ecc` (SECDED, 10-bit syndrome over 396-bit) |
| **Injection Buffer** | Per-VC FIFO buffering ready-to-inject flits |
| **VC Arbiter** | Hybrid R/W × QoS weighted RR. Fixed policy |
| **ECC Check** | Validate `flit_ecc` at endpoint. SECDED 1-bit correct / 2-bit detect (forward + log) |
| **RoB** | Per-AXI-ID order release. NoRoB / SimpleRoB / NormalRoB selectable |
| **FlitUnpack** | Reconstruct AXI B/R from response flit |
| **Outstanding-tx Timeout Tracker** | Per-entry timeout counter. On timeout: `bresp/rresp = SLVERR` + `ERR_STATUS[1]` + IRQ |

## Sub-block mapping (reference architecture → this spec)

| Reference NMU block | In our spec? | Note |
|---------------------|--------------|------|
| Address Map | ✓ (AddrTrans) | renamed |
| Packetizing | ✓ (FlitPack) | renamed |
| QoS Order Control | ✓ (QoSGen) | renamed |
| Read Re-Tagging Buffer | ✗ | AXI ID conversion not modelled at this layer |
| VC Mapping | ✓ (VC Arbiter) | richer (Hybrid R/W × QoS policy) |
| Write Buffer | ✓ (Injection Buffer) | renamed |
| Read Re-Ordering | ✓ (RoB) | renamed |
| De-Packetizing | ✓ (FlitUnpack) | renamed |
| **(not shown)** | **ECC Gen / Check** | added — two-layer integrity scheme (route_par + flit_ecc) |
| **(not shown)** | **Outstanding-tx Timeout Tracker** | added — sole AXI-rresp-generating mechanism on fabric error path |
| **(not shown)** | **CSR file + irq_o** | added — software-visible runtime control surface |
