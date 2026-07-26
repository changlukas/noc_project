# A Multicast-Capable AXI Crossbar for Many-core Machine Learning Accelerators

Source: https://arxiv.org/abs/2502.19215

# A Multicast-Capable AXI Crossbar for Many-core Machine Learning Accelerators

Luca Colagrande

Integrated
Systems Laboratory (IIS)

ETH Zurich

Zurich, Switzerland

colluca@iis.ee.ethz.ch
\orcidlink0000-0002-7986-1975


Luca Benini

Integrated
Systems Laboratory (IIS)

ETH Zurich

Zurich, Switzerland

lbenini@iis.ee.ethz.ch
\orcidlink0000-0001-8068-3806

###### Abstract

To keep up with the growing computational requirements of machine learning workloads, many-core accelerators integrate an ever-increasing number of processing elements, putting the efficiency of memory and interconnect subsystems to the test. In this work, we present the design of a multicast-capable AXI crossbar, with the goal of enhancing data movement efficiency in massively parallel machine learning accelerators. We propose a lightweight, yet flexible, multicast implementation, with a modest area and timing overhead (12 % and 6 % respectively) even on the largest physically-implementable 16-to-16 AXI crossbar. To demonstrate the flexibility and end-to-end benefits of our design, we integrate our extension into an open-source 288-core accelerator. We report tangible performance improvements on a key computational kernel for machine learning workloads, matrix multiplication, measuring a 29 % speedup on our reference system.

###### Index Terms:

AI accelerators, on-chip networks, multicast communication, AXI

## I Introduction

In recent years, a wide range of many-core general-purpose accelerators have emerged to keep up with the computational requirements of modern Machine Learning (ML) workloads [1].
Aiming for higher peak performance figures, these accelerators integrate an ever-increasing number of Processing Elements (PEs):
the number of CUDA cores in Nvidia’s leading Graphics Processing Units (GPUs) increased by more than $2\,\times$ in only two years, rising from 6912 in the A100 [2] to 16896 in the H100 [3].

To translate peak performance into actual performance, it is critical to keep all PEs busy for a significant fraction of the operating time.
This poses a significant challenge on the memory and interconnect subsystems, which must be able to sustain the bandwidth required to feed the PEs with data.
Pressure on main memory can be relieved by reusing data on-chip.
To this end, most accelerators present a Last-Level Cache (LLC); a notable example is Nvidia’s H100 GPU with its 50 MB L2 cache [3].

To further multiply on-chip bandwidth, most accelerators feature additional levels of memory, e.g. shared memory in GPU Streaming Multiprocessors (SMs), and on-chip networks to provide shorter and parallel communication paths between PEs.
Emblematically, Nvidia also recently introduced direct SM-to-SM communication within GPU Processing Clusters (GPCs) in their Hopper-architecture GPUs [3], where SMs in a GPC are interconnected together by a dedicated on-chip SM-to-SM network.

These parallel communication paths can be exploited by taking advantage of a computation’s data reuse patterns.
In the case of matrix multiplication $\textbf{C}=\textbf{A}\times\textbf{B}$ , a key kernel for ML workloads, blocks of rows of matrix A are loaded into distinct clusters, while blocks of columns of matrix B have to be broadcast to all clusters (as detailed in section III-B).
In this setting, multicast communication is extremely beneficial; as such, many recent commercial platforms [4, 5, 6, 7] integrate multicast-capable on-chip networks, but their implementations remain undisclosed and, to the best of our knowledge, there are no detailed performance analyses of these designs in the open literature.

Most works in the literature focus on the design of multicast-capable networks for cache-coherent shared-memory systems [8, 9, 10, 11], employing multicast in the coherency protocol implementation. For area and energy efficiency reasons, massively parallel ML accelerators do not typically implement cache-coherency, relying on software-managed Scratch-Pad Memories (SPMs) instead; GPUs being a prominent example with their SMs’ shared memories.
Other works either assume a mesh topology [10, 12, 13], implement destination encodings which are not scalable to the massive parallelism in ML accelerators [9, 14, 15], or both [16, 17, 18].

This work presents the design of a scalable multicast-capable crossbar (XBAR), suited for the implementation of on-chip networks for massively parallel ML accelerators.
It further differentiates from previous works in that it is fully AXI-compliant and open-source, and thus readily available for integration with standard IPs.
Finally, to the best of our knowledge, this is the first work to evaluate the benefits of multicast communication on a key ML kernel. To summarize our contributions, we:

1.

Design and implement a multicast-capable AXI XBAR, releasing it as open-source hardware.
111https://github.com/colluca/axi/tree/multicast

2.

Extend Occamy [19], an existing open-source many-core ML accelerator, with multicast capabilities.
222https://github.com/colluca/occamy/tree/multicast

3.

Evaluate the XBAR’s area and timing characteristics and the overhead to support multicast.

4.

Evaluate the benefits of multicast communication on a key computational kernel for ML workloads.

We elaborate on the first two contributions in section II.
The latter two are discussed in section III.

## II Implementation

### II-A Multicast-Capable AXI Crossbar

We develop our contributions on the open-source AXI XBAR design by Kurth et al. [20].
Its architecture is shown in figure 2(a).
Masters and slaves are connected through an array of demuxes and muxes.
The XBAR is associated with an address map: a set of address rules, each mapping an address interval to a slave of the XBAR.
When a master sends a write request, the address is compared with every rule in the address map (by the address decoder), and the request is routed to the slave associated with the matching rule.
The destination address is propagated, unmodified, in the output request.
As multicast only involves write transactions (AW, W and B channels), we ignore AXI’s AR and R channels in the following discussion [21].

To define a multicast transaction, a write request must carry multiple destination addresses.
Various multi-address encodings have been proposed in the networking field [22], to address multiple nodes in a network.
While our method presents some similarities with the “multiple region mask” encoding [22], we target the representation of multiple addresses in the global memory space of a system, rather than subcubes of a k-ary n-cube network.

We extend the AXI protocol, without compromising backward compatibility, by passing a mask in the aw_user signal.
If a bit in the mask is set to 1, the corresponding bit in the address is interpreted as a don’t care (X), encoding both logic 0 and 1.
By masking $n$ bits in the address we can represent $2^{n}$ addresses.
For direct correspondence between address and mask bits, we take the mask to be as wide as the address, although this is not mandatory.

Figure 1 presents two example address sets that can be represented with our encoding.
While not all possible address sets can be represented, our encoding is suited for massively parallel accelerators, as the encoding size scales logarithmically with the total size of the address space and is independent of the address set size.
Conversely, the “all destination” encoding [22], which can represent any address set, scales linearly with the address set size.

We extend the address decoder to support multi-address encodings.
The output is a mask (aw_select) indicating which slaves contain at least one of the destination addresses, together with the subset falling within each slave.

We require every multicast-targetable region, defined by a “multicast rule”, to 1) be a power-of-two in size and 2) be aligned to an integer multiple of its size.
Any rule satisfying these constraints can be converted from the interval-form encoding (IFE) to the mask-form encoding (MFE), using the following formulas:

{minted}

python
mfe.addr = ife.start_addr
mfe.mask = ife.end_addr - ife.start_addr - 1

We integrate logic to convert all multicast rules to mask form.
Calculating aw_select then boils down to:
{minted}python
masked_bits = req.mask | rule.mask
match_bits = (req.addr ^rule.addr)
aw_select[rule.idx] = &(masked_bits | match_bits)

The intersection between the request’s and a rule’s address sets can be found by resolving the masked bits as:
{minted}python
out.mask = req.mask & rule.mask
out.addr = ( req.mask req.addr) | (req.mask rule.addr)

Figure 1: Examples of contiguous (left) and strided (right) address sets representable with our encoding, as paths in the binary number tree. Mask bits selectively fork the path of the original address (blue).

The XBAR logic is implemented in the axi_demux and axi_mux submodules.
The prior demultiplexes AW and W channel transactions from a master to the addressed slaves, and multiplexes B channel transactions in the opposite direction.
As B responses from different slaves can arrive out-of-order, the demux blocks AW transactions with the same AXI ID as any outstanding transaction, unless directed to the same slave.
To evaluate this condition, it maintains a table of slaves occupied by outstanding transactions, indexed by AXI ID.

Upon a multicast, multiple B responses are expected from different slaves.
Processing multicast transactions out-of-order would require expensive buffering and deadlock-avoidance logic.
We thus disallow multicast transactions until all outstanding unicast transactions have completed and vice versa.
Multiple outstanding multicast transactions are allowed if directed to the same master ports, within a configurable maximum number.

(a)

(b)

(c)

(d)

(e)

Figure 2: (a) Block diagram of a 4-to-4 AXI XBAR, (b) AXI mux submodule (unicast datapath is highlighted in blue, multicast datapath in green, and the logic arbitrating the two in orange), (c) Occamy SoC and (d) AXI demux submodule (multicast stall logic is highlighted in orange, logic controlling AW channel forking in blue, and B channel joining in green); (e) Scenario creating the deadlock condition.

Figure 2(d) shows a high-level block diagram of the multicast logic in the axi_demux submodule.
The green region highlights the logic responsible for joining B responses from different master ports.
The stream_join_dynamic module ensures that a B handshake is propagated only after receiving a response from every slave.
All responses carry the same ID; we arbitrarily propagate the ID from the first addressed slave using a priority encoder.
On the other hand, the resp fields may differ and must be properly joined.
As the AXI specification does not cover this scenario, we choose to return a SLVERR response if any of the responses are either SLVERR or DECERR.
We further disallow exclusive multicast transactions, excluding EXOKAY responses, so the logic boils down to a simple OR-reduction.

Figure 2(b) shows a block diagram of the axi_mux submodule.
Highlighted in green is the logic required to handle multicast transactions.
Two additional 1-bit signals are generated in every demux and routed to every mux in the XBAR: aw.is_mcast and aw.commit.
The prior is used to select between unicast and multicast datapaths; multicast transactions are prioritized, as they have stricter ordering requirements.
The latter is required to prevent deadlocks.

Consider the scenario represented in figure 2(e).
Slave 0 receives the AW0 transaction before AW1. According to the AXI specification [21], it must thus receive all W0x transactions before any W1x transaction.
On the other hand, slave 1 expects W transactions in the opposite order.
As we cannot buffer all W transactions, we must stall a transaction until all destinations are ready to receive it.
This condition leads to a deadlock, as master 0 waits on w_ready from slave 1, and master 1 from slave 0.

To prevent this, we force a master to “acquire” all slaves at once, breaking Coffman’s “wait for” condition [23].
This is achieved by using a priority-encoder (lzc module), to ensure consistent master selections across muxes.
When all addressed muxes are ready, the demux asserts the aw.commit signal, “releasing” the muxes in the following cycle.

### II-B Multicast-Capable ML Accelerator

A block diagram of the Occamy system-on-chip (SoC) [19] is presented in figure 2(c).
Occamy integrates a configurable number of Snitch clusters [24], each equipped with a 128 KiB L1 memory and Direct Memory Access (DMA) engine.
Clusters are interconnected through two networks: a narrow 64-bit network for synchronization and control packets issued by the cores’ Load-Store Units (LSUs), and a wide 512-bit network shared by the instruction cache and DMA subsystems.
Both networks are implemented by a two-level hierarchy of XBARs.
At the top level, a configurable-size LLC is connected to the wide network.

Clusters are mapped to consecutive address intervals of size 0x40000 starting from address 0x01000000, satisfying the constraints imposed for the definition of multicast targets.

We integrate our extension in every XBAR of the two networks.
We further extend the Snitch cluster’s LSU and DMA engine, to respectively issue multicast interrupts on the narrow network, accelerating synchronization, and data transfers on the wide network, enhancing data movement efficiency, as we will see in section III-B.

## III Results

### III-A Area and Timing Analysis

We synthesize the design using Synopsys’ Fusion Compiler 2021.06 under
worst-case conditions at 0.72 V and 125 °C in GLOBALFOUNDRIES’ 12LP+ technology, with a 1 ns clock constraint.

Figure 3(a) shows the area of an N-to-N XBAR, with and without multicast support.
On 8-to-8 and 16-to-16 XBARs, our extensions introduce overheads of 13.1 kGE and 45.4 kGE (9 % and 12 % of the baseline XBAR), respectively.
As the area scales quadratically with N, 16-to-16 is typically at the upper limit for XBARs that can be implemented at the physical level, and interconnect scale-up is obtained by going multi-stage in a hierarchy of XBARs [20].

All configurations meet the target 1 GHz operating frequency, with the exception of the 16-to-16 XBAR which incurs a very modest 6 % frequency degradation.

(a)

(b)

(c)

(d)

Figure 3: (a) Area of the original and multicast-capable XBARs (numbers on top of the bars report the area increase); (b) Speedup on the microbenchmark with our extensions (numbers on top of the bars report the equivalent parallel fraction according to Amdahl’s law for the 32 KiB data points); (c) Performance of the matmul kernel; (d) Parallelization and scheduling of the matmul kernel.

### III-B Performance Evaluation

We conduct the performance evaluation through cycle-accurate RTL simulations of the Occamy SoC using QuestaSim 2023.4, with a 1 GHz clock frequency. We assume an Occamy system with 32 clusters, organized into 8 groups of 4 clusters each, and a 4 MiB LLC.

We first evaluate our extension on a microbenchmark, which consists in one cluster sending the same data to all other clusters using its DMA engine.
We compare the runtime of the multicast DMA transfer using our extensions to a multiple-unicast approach, where unicast DMA transfers are issued to every destination cluster.
For transfers to more than one group (i.e. 8, 16 and 32 clusters), we also compare to a hierarchical software-based multicast approach, where the source cluster sends the data to one cluster in every other group, which in turn forwards the data to the other clusters in its group.
The distribution within groups can thus proceed in parallel.

The colored bars in figure 3(b) show the speedup of the multicast transfer over the multiple-unicast baseline.
The speedup increases with the number of clusters, and approaches the ideal parallel speedup, with the equivalent parallel fraction per Amdahl’s law reaching 97 % on 32 clusters.
This is due to constant sequential overheads, such as the round-trip latency, being amortized over multiple transfers.
Similarly, we observe a small increase in speedup with growing transfer sizes, ranging from 13.5 $\times$ to 16.2 $\times$ on a 32-cluster transfer.
The white overlays represent the speedup of the hierarchical software-based multicast approach over the baseline.
As we can see, hardware-supported multicast still gives significant speedups over the software-based approach, with a geometric mean speedup of 5.6 $\times$ on the 32-cluster transfers.

Finally, we evaluate how multicast support translates to tangible performance improvements on a key computational kernel for ML applications, i.e. matrix multiplication (matmul).
We execute the largest square double-precision matrix multiplication tile which fits in Occamy’s LLC: $256\times 256$ matrices, accounting for double buffering.
As illustrated in figure 3(d), every cluster computes a distinct $8\times 256$ row block of the product matrix C, calculating an $8\times 16$ tile of the row block at a time.
The corresponding tile of A need only be loaded once into L1, and can be reused in every successive (steady-state) iteration.
The cluster DMAs are used to move data between LLC and cluster L1 memories in a double-buffered fashion.

Figure 3(c) displays the attained performance in a roofline plot of the Occamy architecture.
The baseline kernel features a low steady-state Operational Intensity (OI) of 1.9 FLOPS/byte, as all clusters have to load the B matrix tile from the LLC.
This places the kernel in the memory-bound region, achieving 114.4 GFLOPS, or 92 % of the maximum theoretical performance with this specific OI.

By exploiting multicast, we can load the B matrix tile once and broadcast it to all clusters in parallel. The total number of bytes read from the LLC is reduced, resulting in 3.7 $\times$ and 16.5 $\times$ higher OIs respectively with software-based and hardware-supported multicast.
These respectively translate to 2.6 $\times$ and 3.4 $\times$ performance improvements, reaching 391.4 GFLOPS with hardware-supported multicast. This result shows how ML applications can benefit from multicast support, making it a viable solution to enhance the on-chip bandwidth utilization of many-core ML accelerators.

## IV Conclusion

In this work, we presented the design of a multicast-capable AXI crossbar, leveraging a scalable, yet flexible, multi-address encoding scheme.
We analyzed the area and timing characteristics of the design, showing how multicast support can be achieved with a modest area and timing overhead (12 % and 6 % respectively) even on the largest physically-implementable 16-to-16 AXI crossbar.
We integrated our design into an open-source 288-core ML accelerator, demonstrating its flexibility on an actual system.
Finally, we evaluated the performance impact on a key computational kernel for machine learning workloads, matrix multiplication, measuring a 29 % improvement with our solution, proving that multicast can provide a low-cost solution to enhance the on-chip bandwidth utilization of massively parallel ML accelerators.
