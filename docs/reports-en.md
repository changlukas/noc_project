# Report translations (EN)

English, IC/AXI-idiomatic renderings of the report slides in `docs/image/report*.jpg`.
Condensed and reworded, not literal. No em dashes, no semicolons.

---

## Report 1. Reorder Handling at the NI Endpoint
*(original title: Current Reorder design)*

**Ordering strategy**
- AXI4 mandates per-ID ordering. Tracking outstanding transactions in every router makes complexity scale with network diameter, so it is not used.
- Reordering is handled at the NI endpoint instead. The router stays simple and does not guarantee in-order delivery.

**With outstanding requests on the same ID, split by destination**
- Same ID, same destination: bypass the RoB, with no fabric reordering. The flow is pinned end to end to a fixed VC.
- Same ID, different destination: use the RoB, with fabric reordering allowed. The router re-allocates the VC at each hop.

**RoB sizing**
- An 8 KB SRAM serves as the reorder buffer, sized to reorder two responses at the AXI maximum burst length of 4 KB.

**Packet-format change**
- Add a 1-bit `vc_fixed` field. When the router sees `vc_fixed = 1` it skips the VA stage, keeping the flow pinned to one VC.
- Flit header: `rob_idx | rob_req | tail | vc_fixed | vc_id | dst_id | src_id | axi_ch`

---

## Report 2. AXI ID Compression at the NSU
*(original title: AXI ID Compression)*

**On the wire**
- The NMU carries the full 8-bit AXI ID inbound. The NSU emits a compressed 2-bit AXI ID to the downstream slave.

**Why compress the AXI ID**
- The downstream AXI slave may support only a narrower AXI ID width.
- It reduces the AXI ID width carried across the interface.
- Without outstanding transactions, multiple AXI IDs are redundant (the FlooNoC approach). With one outstanding transaction end to end there is only ever a single transfer in flight, so extra IDs are not needed to allow reordering and no reorder buffer is required.

**Cost when the master keeps many outstanding, 2-bit ID example**
1. The master issues up to 32 outstanding transactions, each on a distinct AXI ID.
2. The NSU compresses the 32 distinct IDs into 4 distinct IDs.
3. The AXI slave can then reorder responses only among those 4 IDs.

**Compression modes (from AMD PG313, non-DDR NSU)**
- The request from the NMU carries `{Source ID, AXI ID}` for writes and `{Source ID, AXI ID, Tag}` for reads.
- The 2-bit downstream ID is either any two selected bits of `{Source ID, AXI ID}`, with the bit selection configurable and defaulting to the low two bits of the request AXI ID, or a fixed 2-bit value from a config register that forces reads and writes to stay in order.
- DDR-controller NSUs do not compress the AXI ID.

---

## Report 3. Topology YAML to Generator to Testbench

**Topology YAML with a packed address map**
```yaml
topology:
  name: mesh_4x4_vc1
  x_dim: 4
  y_dim: 4
  num_vc: 1
address_map:
  tiles:
    - { x: 0, y: 0, size: 0x100000000 }
    - { x: 1, y: 0, size: 0x100000000 }
    - { x: 2, y: 0, size: 0x100000000 }
    - { x: 3, y: 0, size: 0x100000000 }
    - { x: 0, y: 1, size: 0x100000000 }
    # ... one entry per node, 16 total
    - { x: 3, y: 3, size: 0x100000000 }
```

**Packed base addresses, contiguous and gap-free**

| node | (x, y) | Base |
|---|---|---|
| 0 | (0, 0) | 0x0 |
| 3 | (3, 0) | 0x3_0000_0000 |
| 4 | (0, 1) | 0x4_0000_0000 |
| 15 | (3, 3) | 0xF_0000_0000 |

**Flow** (`gen_test_patterns.py`)
1. Read the topology YAML with its `address_map` tiles.
2. Pack the tiles into per-node bases (the SAM).
3. Pick each node's destination from the traffic pattern.
4. Destination address = `base(dst) + local offset`.

---

## Report 4. Stimulus File Format

The directed driver reads one transaction per fixed-order block. `write.txt` is an AW plus its W beats. `read.txt` is an AR with no W beats.

**AW block (`write.txt`), 12 field lines then one line per W beat**

| Row | Value | AXI field |
|---|---|---|
| 1 | 0 | AXI id |
| 2 | 0x1100001000 | address |
| 3 | 0 | len (beats minus 1, so 1 beat) |
| 4 | 5 | size (2^5 = 32 B) |
| 5 | 1 | burst (INCR) |
| 6 | 0 | lock |
| 7 | 0 | cache |
| 8 | 0 | prot |
| 9 | 0 | qos |
| 10 | 0 | region |
| 11 | 0 | atop (write only) |
| 12 | 0 | user |
| 13 | 0x1f1e...00 0xffffffff 0 | W beat (w_data, w_strb, w_user) |

`read.txt` uses rows 1 to 10 plus user, giving 11 lines with no atop and no W beat. `w_last` is not in the file. The driver derives it from the burst length.
