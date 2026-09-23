# NMU co-simulation boundary audit

2026-09-23, branch feat/nmu-cosim-84. Integration in progress.

- One Router at (0,0), NMU LOCAL (header port 0), NSU WEST (header port 1).
- Existing route_compute supports this through peripheral routing. Coordinate bounds remain 2x2; only one Router is instantiated, with no mesh links.
- One memory and one config SAM range both select (0,0), port 1. Both retain their existing base/size values from the standalone map's first destination.
- No DAT merge and no C++ NMU instance.
- RTL NMU receive FIFO depth: 8. Router LOCAL output credit seed: 8.
- C++ NSU receive FIFO depth: 8. Router WEST output credit seed: 8.
- Router receive FIFO depth: 8. RTL NMU sender seed: 8.
- C++ NSU sender seed currently 4 because its existing wrapper assumes a merge input. User choice pending: configurable initialization to direct Router depth 8, preserving legacy default 4; or retain the conservative 4-credit window.
- C++ NMU ingress remains unbounded, deferred. NSU REQ ingress is also unbounded in the existing model; this environment does not validate future NSU RTL backpressure.

## Validation so far

- 45 generator/topology tests PASS.
- Existing mesh_2x2, mesh_2x2_periph and mesh_4x4 topology packages are byte-identical after extracting their shared SAM emitter.
- Full co-simulation SV hierarchy passes Verilator lint/elaboration with upstream width, memory scheduling and initialized-register warnings. No C++ model build was performed by this lint.
- VCS functional acceptance, independent comparison counters, corruption detection and advanced backpressure/reset integration remain incomplete.
- GCC 9.3 compiles and runs an optional/filesystem C++17 probe on be16; the default GCC 4.8.5 is insufficient. VCS ABI/link validation remains pending.

## Approved depth-32 profile

User approved 32 on 2026-09-23. sim/cosim/nmu/profile.yml controls the generated co-simulation constants.yml. Existing SV/C++ emitters generate NOC_ROUTER_VC_DEPTH and NOC_NI_DAT_RX_VC_DEPTH consistently at 32 in the staged build only.

Consumers checked: RTL nmu.sv request credit parameter and response-buffer DAT FIFO depth; C++ RouterWrap input VC depth and LOCAL output seed, Router non-LOCAL output seed; C++ NSU depacketize receive capacity; new NsuWrap::set_dat_credit_depth through cmodel_dpi.h/.cpp and tb_nmu_cosim.sv. The TB passes the Router depth before the first model tick. Legacy merge initialization remains 4 and production global constants remain 8.

Storage implication for this test profile: each active DAT receive VC has 32 entries instead of 8; it is a capacity setting, not a claim of PPA improvement. No extra pipeline stage or arbiter was introduced.

## VCS compatibility and build identity

The first single control round trip passes on VCS M-2017.03-SP1 with the depth-32 profile. Initial integration fixes are limited to TB declaration order and rendering the existing scoreboard diagnostic queue through $sformatf before $warning. The scoreboard's data comparison is unchanged.

Workstation/NFS timestamps can be ahead of local time. Timestamp-based make reused an older TB, detected by the missing new COVERAGE line. That regression batch was stopped and is not acceptance evidence. VCS build directories now include a SHA256 of SV sources/filelist/flags; C++ and YAML build directories have independent content identities. No acceptance result from the interrupted timestamp-based batch is counted.
