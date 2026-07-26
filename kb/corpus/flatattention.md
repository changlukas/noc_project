# FlatAttention: Dataflow and Fabric Collectives Co-Optimization for Efficient Multi-Head Attention on Tile-Based Many-PE Accelerators

Source: https://arxiv.org/abs/2505.18824

# FlatAttention: Dataflow and Fabric Collectives Co-Optimization for Efficient Multi-Head Attention on Tile-Based Many-PE Accelerators

Chi Zhang1,
Luca Colagrande1,
Renzo Andri3,
Thomas Benz1,
Gamze Islamoglu1,
Alessandro Nadalini2,

Francesco Conti2,
Yawei Li1,
Luca Benini12

1Integrated Systems Laboratory (IIS), ETH Zurich, Zurich, Switzerland

2Department of Electrical, Electronic, and Information Engineering (DEI), University of Bologna, Bologna, Italy

3Computing Systems Lab, Huawei Zurich Research Center, Zurich, Switzerland

{chizhang, colluca, tbenz, gislamoglu, yawli, lbenini}@iis.ee.ethz.ch,

{alessandro.nadalini3, f.conti}@unibo.it, renzo.andri@huawei.com

###### Abstract

Multi-Head Attention (MHA) is a critical computational kernel in transformer-based AI models.
Emerging scalable tile-based accelerator architectures integrate increasing numbers of tightly-packed processing elements (PEs) with tensor units. MHA dataflow mapping is crucial for achieving high utilization of the available units.
We propose FlatAttention, a new dataflow for MHA on tile-based many-PE accelerators, minimizing costly main memory (HBM) accesses by leveraging collective
primitives integrated into the on-chip network fabric. FlatAttention achieves up to 89.3% utilization, and 4.1× performance speedup over FlashAttention-3 dataflow on tile-based accelerators whilst reducing HBM traffic by 16×.
Through algorithm-architecture co-exploration, we identify an optimal configuration for a large scaled-out tile-based accelerator featuring a 32×32 tile mesh with 1024 TFLOPS @ FP16 peak performance, comparable to the state-of-the-art Nvidia H100 GPU.
FlatAttention in this configuration achieves up to 1.3× higher utilization over FlashAttention-3 on the H100 GPU.
Meanwhile, this tile-based accelerator configuration requires 40% less HBM bandwidth compared to the H100 GPU, enabling a 1.8× reduction in die size, estimated on the same technology node.

###### Index Terms:

Multi-Head Attention, Tile-Base Architecture, Network on Chip, Collective Primitives.

## I Introduction

Transformer-based artificial intelligence (AI) models, such as GPT-4, LLaMA, and DeepSeek-V3 are dominating Large Language Models (LLMs).
Among the computational kernels in transformer-based models, Multi-Head Attention (MHA) exhibits quadratic complexity over sequence length[1], making it a critical factor in performance, especially for long sequences.
Previous LLM studies [2, 3] have shown that most operations in MHA are bottlenecked by memory accesses.

The “attention bottleneck” has driven extensive research focused on optimizing MHA dataflows on the dominant AI hardware platform, namely Nvidia GPUs.
One of the most widely adopted solutions is FlashAttention [4], which efficiently fuses MHA microkernels.
Over two generations of improvements, FlashAttention-2 [5] introduced algorithmic optimizations, whereas FlashAttention-3 [6] additionally leverages asynchronous execution for improved performance.
However, still no more than 75%111FlashAttention-3 baseline. Numbers use arXiv v1 (11 Jul 2024) [FA3-arXiv], the newest version when experiments were run; a later NeurIPS 2024 release reports $\sim$ 10 % higher throughput [FA3-NeurIPS]. utilization was achieved on the H100 GPUs [6].

Moreover, Nvidia’s state-of-the-art (SoA) H100 GPU comes with significant cost and power requirements, featuring an 814 mm2 die on TSMC’s 5nm process node, coupled with six High Bandwidth Memory (HBM) stacks accounting for over 50% of the total cost, and a Thermal Design Power (TDP) of 700 W.
Given the suboptimal utilization of the MHA layer and the high cost and power requirements, many competitors are working on hardware and software to improve system cost and energy efficiency with highly optimized accelerators designed for strong LLM performance.
The goal is to offer competitive performance while boosting the efficiency of the system by minimizing energy-hungry HBM accesses.

As recent application trends, linked to the “reasoning” use of LLMs, emphasize inference as a value-added workload, a scalable design pattern for inference accelerators targeting scaled-up transformer-based AI models is emerging [7, 8, 9, 10]: these accelerators are constructed as large, full-reticle, or even multi-die integrated systems [8] structured as meshes of compute tiles containing extremely dense, large matrix units, coupled with vector and scalar engines to accelerate all key kernels in LLMs, and local, explicitly managed memories for main memory data buffering and latency hiding.
Several HBMs are typically employed as main memory, positioned at die boundaries and supported by multiple memory controllers.

Such a tile-based many-Processing Element (PE) accelerator architectural template favors silicon efficiency and scalability, featuring a dense compute tile placement and a software-controlled partitioned memory hierarchy.
However, mapping LLM inference workloads onto these scalable tile-based accelerators presents a significant challenge.
The inter-tile and tile-to-HBM dataflow must be carefully designed to achieve high utilization of the tiles’ matrix engines and minimize energy-hungry off-chip access. In this work, we show that leveraging collective communication primitives in the Network on Chip (NoC), such as reduction operations and multicast, can be highly beneficial for both of these objectives.

Existing works investigating mapping and architecture co-exploration for LLM workloads either fail to fully consider key optimizations, such as operator fusion and overlap within the MHA layer, falling short of FlashAttention’s highly optimized SoA dataflow [11], or do not explore the use of inter-tile collective primitives in NoCs [12, 13], or both [14].
Although some AI accelerator vendors claim to achieve high LLM inference efficiency through optimized dataflows [7, 8], the details of their dataflow implementations—particularly for the MHA layer—and on-chip fabric architectures remain confidential. Additionally, accelerator architecture design must be co-explored alongside dataflow optimizations to determine optimal design parameters and features.

This work takes the MHA in the prefill stage of LLM inference as a case study, which aligns with FlashAttention[4] and could be generalized to training. We propose a new dataflow for scalable tile-based accelerators and present a co-design approach for algorithm-architecture co-exploration. The contributions of this paper are:

•

A modeling and simulation framework that enables estimating the performance of a large set of tile-based accelerators and the co-design of network primitives.

•

An efficient workload allocation and scheduling strategy called FlatAttention. FlatAttention leverages collective primitives on the on-chip network fabric to achieve up to 89.3% utilization for MHA layer on tile-based many-PE accelerators, and 4.1× performance speedup over FlashAttention-3 dataflow on the same tile-based accelerator, whilst reducing HBM traffic by 16×.

•

Co-exploration of accelerator architecture and FlatAttention parameters, highlighting key trends and trade-offs to guide the selection of optimal FlatAttention parameters.

•

An optimal tile-based accelerator configuration that matches the peak performance of the current SoA Nvidia H100 GPU.
FlatAttention in our configuration achieves up to 1.3× higher utilization over FlashAttention-3 on H100.
Meanwhile, this tile-based accelerator configuration requires 40% less HBM bandwidth than the H100 while achieving a 1.8× reduction in die size, estimated on the same technology node.

## II Reference Tile-Based Many-PE Architecture

Figure 1: Tile-Based Many-PE Architecture Template

This section introduces the architecture of the scalable tile-based many-PE design pattern, prominently featured in SoA commercial Machine Learning (ML) accelerators, e.g., Tesla’s Dojo system [15] and Tenstorrent’s Blackhole chip [9].

As illustrated in Fig. 1, the fundamental building block of the many-PE architecture is the “tile”. Each tile comprises PEs, local memory (L1), a Direct Memory Access (DMA) engine, and local interconnects. There are three main types of PEs: scalar cores, vector engines, and matrix engines. Scalar cores mainly handle dataflow control tasks, whereas heavy computational tasks are offloaded to the vector and matrix engines based on the computation type. All PEs within a tile can directly access the local L1 memory via the local interconnect. The DMA engine in each tile is responsible for data movement in and out of the local L1 memory.
The tile-based many-PE system uses an on-chip 2D-mesh NoC to connect tiles. Off-chip memory, such as HBM, is located at the boundary of the mesh NoC, interfaced through the respective memory controllers.

Collective communication operations [16], such as multicast and reduction, are involved in L1 data exchange among tiles.
Traditional software-based collective primitives rely on successive point-to-point inter-tile transfers, leading to high communication latency.
In contrast, NoCs with hardware-supported collective communication primitives establish direct, optimized communication paths, significantly reducing communication overhead [17].
For example, consider multicasting a message of size $\alpha$ to a chain of $N$ clusters, demonstrated in Fig. 1.
Given an L1-to-NoC router latency of $L_{d}$ , router-to-router latency of $L_{r}$ , and a router link bandwidth of $\beta$ , the communication latency for software-based collective primitives is $N\left(\frac{\alpha}{\beta}+2L_{d}+\frac{N+1}{2}L_{r}\right)$ .
In contrast, NoCs with hardware-supported collective communication primitives employ a path-based forwarding strategy. Each packet injected from the source node is duplicated and forwarded in-flight along the multicast-aware routing path, as illustrated in Fig. 1.
This optimization reduces the communication latency to
$\frac{\alpha}{\beta}+2L_{d}+NL_{r}$ .
For example, when $\alpha=16\>KB,\ \beta=128\>B/cycle,\ L_{d}=10\>cycles,\ L_{r}=4\>cycles,\ N=7$ , the multicast latency is reduced by $6.1\times$ .

## III FlatAttention Dataflow

(a)

(b)

Figure 2: (a) Parametric definition of FlatAttention. (b) Detailed FlatAttention dataflow, with each step corresponding to the line numbers in Algorithm 2. (c) FlatAttention dataflow optimization.

### III-A Motivation

As both FlashAttention-2 and FlashAttention-3 share the dataflow introduced in FlashAttention-2, we refer to it in the following.
Algorithm 1 outlines the FlashAttention-2 algorithm222To simplify the dataflow for tile-based many-PE accelerators, we assume the $K$ matrix is pre-transposed in HBM while still accounting for the pre-transposition time when comparing to FlashAttention on H100 for a fair comparison [6]. for each head, designed to operate directly on tile-based many-PE architectures. The MHA workload is partitioned over the batch, number of heads, and output sequence length dimensions, and these blocks are distributed to the tiles where they can be processed in parallel. With this mapping, every tile processes distinct data, which it can independently access in HBM. No communication between tiles is required, but at the same time no reuse of data across tiles is exploited.

With sequnce length $S$ , head dimension $D$ , number of heads $H$ , batch size $B$ and block size $M\coloneqq B_{r}=B_{c}$ , this dataflow results in an HBM I/O complexity of:

| $\text{IO}=2\cdot H\cdot B\cdot D\cdot S\cdot\left(1+\frac{S}{M}\right).$

While all other parameters are fixed by the computation, the block size parameter $M$ can be increased to lower the I/O complexity. Intuitively, larger blocks favor the reuse of data in the L1 memory of a tile.
However, the block size is constrained by the L1 memory of a single tile, which must be able to simultaneously host tensors $Q_{i},K^{T}_{j},V_{j},O_{i}$ , at any given time.

With the goal of further reducing off-chip I/O accesses, we propose the FlatAttention dataflow, which fundamentally redefines how MHA is parallelized on tile-based architectures. FlatAttention leverages multiple tiles as a unified entity to process an MHA block, as defined above, of a significantly larger size, given that the aggregate L1 memory of a group of tiles can now be used to collectively store the block.
When $N$ tiles are grouped together, the resulting I/O complexity becomes:

| $\text{IO}=2\cdot H\cdot B\cdot D\cdot S\cdot\left(1+\frac{S}{{\color[rgb]{%
1,0,0}\definecolor[named]{pgfstrokecolor}{rgb}{1,0,0}\sqrt{N}}\cdot M}\right).$

For example, when $S=4096$ , $M=128$ , and $N=64$ , this results in a $6.6\times$ theoretical reduction in HBM accesses.

### III-B Detailed FlatAttention Dataflow

The FlatAttention dataflow is depicted in Fig. 2b. We refer to a set of tiles collectively processing a block, as previously introduced, as a group, demonstrated in Fig. 2a. We define the shape of the group as $G_{x}\times G_{y}$ . FlatAttention applies the same tiling and mapping scheme to groups as FlashAttention applies to tiles but introduces a secondary level of blocking within each group. This secondary blocking divides the $\{B_{c},B_{r}\}$ block dimensions into smaller slices based on the group shape $\{G_{x},G_{y}\}$ , resulting in $\{\frac{B_{c}}{G_{x}},\frac{B_{r}}{G_{y}}\}$ slice sizes for every tile.

Algorithm 2 outlines the FlatAttention dataflow.
At a high level, the algorithm is conceptually similar to FlashAttention (Algorithm 1): different groups process distinct data, so no communication between groups is required. However, distributing the computation of an MHA block to tiles in a group introduces distinct data movement patterns within the group:

•

Loading and Multicasting:
Only tiles on the west edge of the group load $Q$ slices from HBM (line 5), followed by multicasting $Q$ slices row-wise 6 to the other tiles in the group.
When entering the inner loop, the tiles on the south edge load $K^{T}$ and $V$ slices from HBM 8 followed by multicasting them column-wise 9.

•

Computing Attention ( $Q\cdot K^{T}$ ) and Rowmax:
Each tile computes a segment of the attention score matrix 10. During the computation of row-wise maxima for Softmax, tiles compute partial row maxima locally 11 updated with the tracking maxima 13, followed by a row-wise reduction within the group to calculate the global row maxima 15. The results are then multicasted row-wise to ensure that each tile holds the global row maxima 16.

•

Softmax Denominator:
After computing the partial Softmax denominator locally with global row maxima 17 18, the same reduction 19 and multicast 20 procedure applies to computing the global denominator, which is then updated with the tracking maxima and denominator 22.

•

Output Matrix ( $O$ ):
Each tile updates local $O$ slices and tracking statistics in the inner loop, and computes partial results for $O$ slices on exit 23- 28. FlatAttention then performs a row-wise reduction of $O$ slices 29 followed by storing $O$ slices in HBM 30 only from west-edge tiles.

These communication requirements are a direct result of FlatAttention’s parallelization scheme, which enables minimizing costly global off-chip I/O by exploiting on-chip data reuse across tiles through local on-chip communication. This trade-off of global for local requirements enables FlatAttention to achieve better scalability and performance compared to FlashAttention methods for tile-based many-PE architectures, as long as local on-chip communication is efficiently handled, as will be discussed in Section V-A.

### III-C Asynchronous FlatAttention

In the naïve version of FlatAttention (Algorithm 2), data movement and SoftMax-related computations still account for a significant portion of the runtime, as illustrated in Fig. 2c. This reduces the overall utilization, as the system’s peak performance is primarily determined by the matrix engine, which has much higher computational power compared to the vector engine. To further improve utilization, we propose leveraging the asynchronous nature of DMA, vector and matrix engine invocations to overlap the runtime of data movement and SoftMax operations with matrix multiplications.

The optimized dataflow schedules the computation of two heads concurrently on each group. While the matrix engine processes matrix multiplications for one head, the DMA and vector engine perform data movement and SoftMax operations for the other333The same optimization can be applied with two output row blocks $O_{i}$ instead of two heads, reducing memory requirements as the $K^{T}_{j}$ and $V_{j}$ blocks are shared. To simplify the evaluation, where sufficient row blocks are not available in all configurations, we adopt the presented implementation.. Fig. 2c demonstrates this optimization, showcasing how it can ensure the matrix engine remains nearly fully active, provided that the matrix multiplications runtime overlaps completely with data movement and SoftMax operations. Notably, FlashAttention-3 employs the same technique to improve over FlashAttention-2.

## IV Modeling and Analysis Methodology

We developed a modeling and simulation framework for tile-based many-PE accelerators on the GVSoC event-based simulator [18] for functional and performance simulation. GVSoC is open source and released with models for the Snitch[19] single-issue RISC-V core, the Spatz[20] vector engine supporting the RISC-V Vector (RVV) extension, the iDMA[21] engine, and tile-local L1 memory and interconnect.
To extend these capabilities, we developed and calibrated new models for the RedMulE[22] matrix engine and the FlooNoC [23] fabric according to their open-source RTL implementations. The NoC model incorporates both software and hardware-supported collective communication primitives, as presented in Section II, for design space exploration. Specifically, we can simulate hardware support for row-wise and column-wise multicast, sum-reduction, and max-reduction operations. We also extended Spatz with a custom RVV instruction for exponential operations and a dedicated exponential unit within the FPU.
Furthermore, we integrated the DRAMSys [24] simulator into GVSoC for HBM modeling. i.e., we used the HBM2e specification with 64 GB/s bandwidth per channel.

Using these building blocks, we constructed the SoftHier model and analysis framework: a flexible, parameterizable tile-based many-PE accelerator simulator with functionality and performance models calibrated on cycle-accurate simulations of open-source RTL. SoftHier is configurable using architecture configuration files, enabling the instantiation of specific accelerator designs, e.g. to explore different numbers of Compute Elements (CEs) in the RedMulE units.

In the SoftHier framework, we implemented the FlashAttention and FlatAttention dataflows in C, incorporating APIs for matrix engine offload and DMA engine inter-tile communication, along with NoC collective primitives. The software was compiled using the GNU RISC-V GCC compiler with -O3 optimization. We assume a 1 GHz clock frequency, and preheat every tile’s instruction cache at the start of the simulation. For FlashAttention, we parallelize across the batch, number of heads and output sequence length dimensions to ensure that all tiles are utilized. For both implementations, we select the slice size per tile to maximize local L1 memory occupancy while maintaining a square configuration, i.e., $\frac{B_{r}}{G_{y}}=\frac{B_{c}}{G_{x}}$ .

## V Experimental Results

### V-A FlashAttention vs. FlatAttention

TABLE I: System Specifications

| System
| 32×32 Tiles, 1024-bit NoC link width

| HBM
| 16x2 Channels, equally divided over west and south edges

| Tile
RedMulE Matrix Engine: 32×16 CE array, 1 TFLOPS@FP16

Spatz Vector Engine: 16 FPU, 128 GFLOPS@FP16

Local Memory: 384 KB, 512 GB/s

| Summary
| 1024 TFLOPS Peak Performance , 2 TB/s  Peak HBM Bandwidth

Figure 3: Runtime breakdown (bars) and average HBM BW utilization (star markers) for different MHA implementations and layer sizes. +Runtime not overlapped with RedMulE. ++Runtime not overlapped with either Spatz or RedMulE. *Implementations without double buffering.

We first compare different MHA implementations and layer sizes on a given tile-based many-PE accelerator configuration, as specified in Table I.
We evaluate both FlashAttention-2 (FA-2) and FlashAttention-3 (FA-3) implementations on tile-based many-PE accelerators, where FA-3 introduces a similar scheduling mechanism as presented in Section III-C.
For FlatAttention, we set the group size to include all tiles in the system, i.e. $G_{x}=G_{y}=32$ .
We evaluate a naïve implementation without (Flat) and with (FlatColl) hardware support for efficient collective primitives on the NoC, as well as the optimized FlatAttention dataflow (FlatAsyn) described in Section III-C with NoC collective primitives. We evaluate multiple MHA layers, varying the sequence length $S\in\{1024,2048,4096\}$ and head dimension $D\in\{64,128\}$ , while fixing batch size $B=2$ and heads $H=32$ .

Fig. 3 presents the runtime breakdown and average HBM bandwidth utilization. We observe that FlashAttention exhibits a highly memory-bound behavior on the target tile-based many-PE system, with HBM bandwidth utilizations reaching up to 80% on average. HBM access is the dominant runtime component, limiting overall compute utilization. Even with FA-3’s optimized dataflow for overlapping matrix multiplication and Softmax operations, the saturated HBM bandwidth prevents further speedup. Additionally, FA-3 introduces an overhead for more complex scheduling.

Flat significantly reduces HBM access time compared to FA-3 due to the decreased I/O complexity.
However, software-based collective primitives used in Flat rely on successive point-to-point inter-tile transfers. For instance, a row-wise multicast in this configuration requires 31 sequential unicast transmissions, incurring substantial on-chip communication overhead and resulting in worse performance than FlashAttention.
Instead, with efficient collective primitives enabled on the NoC (FlatColl), on-chip inter-tile communication is significantly accelerated, leading to better performance than FlashAttention across most MHA layers. The FlatAsyn implementation shows that we can further improve performance by overlapping SoftMax, data movement, and matrix multiplication operations.
Overall, our optimizations result in up to 4.1× speedup and 16× HBM traffic reduction over FA-3 (D128, S4096).

### V-B Tile Group Scale Trade-offs for FlatAttention

Figure 4: Runtime breakdown for different (square) flattening scales and layer sizes. Percentage labels above the bars indicate the average utilization of the RedMulE units when active. +Runtime not overlapped with RedMulE. ++Runtime not overlapped with either Spatz or RedMulE.

Although FlatAsyn achieves the best performance across all MHA layers in Fig. 3, it does not fully optimize utilization for shorter sequence lengths such as 2048 and 1024, where RedMulE runtime cannot completely overlap with other operations. To determine the optimal performance configuration for FlatAttention, we analyze the impact of different (square) group sizes $G_{x},G_{y}\in\{4,8,16,32\}$ , on the specific tile-based many-PE accelerator configuration defined in Table I.

We evaluate multiple MHA layers, varying the sequence length $S\in\{512,1024,2048,4096\}$ , while fixing $D=128$ , $H=32$ and $B=4$ .

Fig. 4 presents the runtime breakdown and MHA workload slice size per tile, i.e. $\frac{B_{r}}{G_{y}}=\frac{B_{c}}{G_{x}}$ , across different group scales. For long sequence lengths such as 4096, the slice size per tile remains constant due to L1 memory capacity. In this case, increasing the group size reduces overall HBM I/O complexity, as demonstrated in Section III-B, leading to reduced HBM access runtimes and improved overlap with matrix multiplication on RedMulE. The 16×16 and 32×32 group scales achieve 88% and 87% utilization, respectively, for a sequence length of 4096.

However, for shorter sequence lengths such as 512, the situation is different. As the group scale increases, the slice size per tile decreases due to the fixed sequence length, introducing two performance overheads:

•

Reduced RedMulE utilization:
Smaller slices per tile lead to lower RedMulE utilizations. For example, in a 32×32 group with a sequence length of 512, every tile’s RedMulE achieves only 23% utilization when active.

•

Increased synchronization overhead:
Smaller slices per tile result in shorter RedMulE runtimes. As a result, constant overheads associated with synchronization and data movement, such as HBM access latency ( $\sim$ 200 cycles), constitute a larger fraction of the overall runtime.

We refer to this effect as over-flattening. For moderate sequence lengths, both effects occur simultaneously: larger group scales effectively reduce I/O complexity but also introduce the risk of over-flattening. For every sequence length, there exists an optimal group scale balancing the two effects.

### V-C Co-exploration of Architecture and Algorithm Parameters

Lastly, we demonstrate how the SoftHier framework can be used to identify an optimal tile-based many-PE architecture configuration. Our goal is to design a tile-based accelerator with comparable peak performance to Nvidia’s H100
(989 TFLOPS FP16/BF16 without sparsity)
while improving utilization and reducing overall HBM bandwidth requirements on MHA workloads. We evaluated a set of candidate architecture configurations with varying NoC fabric granularity and HBM channel connectivity. Table II presents the tile specifications as a function of fabric granularity, ensuring constant peak system performance (1024 TFLOPS) and on-chip memory capacity. For every accelerator candidate, we evaluate multiple MHA layers, searching for optimal performance across different dataflow implementations, including FlashAttention-3 and FlatAttention with varying square-shaped group sizes.

Based on the results shown in Fig. 5a, we can select a configuration (BestArch) that optimizes for performance over cost, featuring a 32×32 fabric granularity and 16×2 HBM channels. We then compare its performance directly against FlashAttention-3, based on the H100 performance numbers in Shah et al.[6], using the same MHA layers while also accounting for the $K$ matrix pre-transposition time in FlatAttention for fair comparison.
In Fig. 5b, the BestArch configuration with FlatAttention achieves up to 1.3× higher utilization while requiring 40% less HBM bandwidth compared to the H100 GPU.
Beyond MHA, common GEMM kernels utilizing the collective-based SUMMA dataflow [25] on BestArch also achieve up to 1.2× higher utilization over H100 [26] in Fig. 5c.
Using the Gate Equivalent (GE) reported for the individual components [19, 20, 21, 22, 23], we estimated the die size of BestArch in TSMC 5nm technology, the same used by the H100.
Considering 4 transistors per GE, a transistor density of 138.2 MTr/mm2, an SRAM bit-cell size of 0.021 µm2, and assuming 66% area utilization, BestArch features a die size of 457 mm2, enabling a 1.8× reduction to H100 GPU.

TABLE II: Fabric Granularity and Tile Specifications

| Fabric Granularity
| 32×32
| 16×16
| 8×8

| RedMulE CE Array
| 32×16
| 64×32
| 128×64

| Spatz FU Count
| 16
| 64
| 256

| Local Memory Size (KB)
| 386
| 1526
| 6144

| Local Memory Bandwidth (GB/s)
| 512
| 2048
| 8192

(a)

(b)

(c)

Figure 5: (a) Heatmap of utilization with best group size. (b) Comparison with FlashAttention-3 on H100 solutions, with absolute performance in TFLOPS labeled above the bar. (c) Comparison of GEMMs, including FFN layer in Meta’s LLaMA 70B [26], between BestArch and H100.

## VI Conclusion

We propose FlatAttention, an optimized dataflow for MHA on tile-based many-PE accelerators, co-designed with NoC collective primitives, achieving up to 89.3% utilization while reducing HBM traffic by 16× compared to FlashAttention-3 dataflow, on tile-based accelerators.
Through algorithm-architecture co-exploration, an optimal accelerator configuration matching the peak performance of Nvidia’s H100 GPU is selected, which requires 40% less HBM bandwidth than H100 and 1.8× reduction in die size, with FlatAttention achieving up to 1.3× higher utilization compared to FlashAttention-3 on H100, and its GEMM reaching up to 1.2× higher utilization over H100.
Future work includes end-to-end LLM inference on multi-chiplet systems with 3D-stacked memory.

## Acknowledgment

This work is supported by the ETH Future Computing Laboratory (EFCL) and Huawei ZRC.
