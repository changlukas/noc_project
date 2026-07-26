# FlatAttention: Dataflow and Fabric Collectives Co-Optimization for Large Attention-Based Model Inference on Tile-Based Accelerators

Source: https://arxiv.org/abs/2604.02110

# FlatAttention: Dataflow and Fabric Collectives Co-Optimization for Large Attention-Based Model Inference on Tile-Based Accelerators

Chi Zhang,
Luca Colagrande,
Renzo Andri,
and Luca Benini
This work has been submitted to the IEEE for possible publication.
Copyright may be transferred without notice, after which this version may no longer be accessible.Chi Zhang, Luca Colagrande, and Luca Benini are with the Integrated Systems Laboratory (IIS), ETH Zurich, 8092 Zurich, Switzerland (e-mail: chizhang@iis.ee.ethz.ch; colluca@iis.ee.ethz.ch; lbenini@iis.ee.ethz.ch).Luca Benini is also with the Department of Electrical, Electronic and Information Engineering (DEI), University of Bologna, 40126 Bologna, ItalyRenzo Andri is with the Computing Systems Laboratory, Huawei Technologies Switzerland AG, Zurich, Switzerland (e-mail: renzo.andri@huawei.com).

###### Abstract

Attention accounts for an increasingly dominant fraction of total computation during inference for mixture-of-experts (MoE) models, making efficient acceleration critical.
Emerging domain-specific accelerators for large model inference are shifting toward chip-scale and wafer-scale tile-based architectures.
Tiles contain large matrix and vector engines and are connected through on-chip interconnects, which support tile-to-tile traffic to reduce the tile-to-main-memory traffic bottleneck. Hence, dataflow management is crucial to achieve high utilization.
We propose FlatAttention, a dataflow for modern attention variants on tile-based accelerators.
FlatAttention minimizes expensive high-bandwidth memory (HBM) accesses by exploiting collective primitives integrated into the on-chip network fabric, achieving up to 92.3% utilization, 4.1 $\times$ speedup over FlashAttention-3, and 16 $\times$ lower HBM traffic. On a 32×32 tile configuration with peak performance comparable to NVIDIA GH200, FlatAttention generalizes across multiple attention variants, achieving an average of 86% utilization for compute-bound attentions and 78% HBM bandwidth utilization for memory-bound ones, resulting in an average 1.9 $\times$ speedup over attention implementations on GH200.
Finally, we evaluate end-to-end DeepSeek-v3 FP8 decoding with FlatAttention on a wafer-scale multi-die system, achieving a 1.9 $\times$ improvement in system throughput and a 1.4 $\times$ reduction in per-user token output latency, despite operating with 1.5 $\times$ lower peak system performance compared to the state-of-the-art solution.

## I Introduction

(a)

(a)

(b)

(b)

Figure 1: (a) FLOP breakdown for LLM models during prefill (seq length) and decode (kv length) stages. (b) Roofline plot of FlashAttention-3
prefill and FlashMLA decode performance on Nvidia GH200 GPU. Evaluated with FP16 precision, varying head dimension and sequence length for prefill, while varying speculative length and KV cache length for decoding [43].

Transformer-based Artificial Intelligence (AI) models, such as GPT-4, LLaMA, and DeepSeek-v3, have gained tremendous importance and are now a key, dominant workload for an increasing number of AI-serving systems. As these models continue to grow in size and complexity, they require substantial computational resources to achieve real-time performance. Consequently, efficient and scalable Large Language Model (LLM) inference has become a key challenge, driving innovations in both model architecture evolution and hardware acceleration solutions.

Recent models have moved from the classical decoder design, featuring stacked Multi-Head Attention (MHA) and Multilayer Perceptron (MLP) layers as in GPT and BERT [40], to more efficient attention variants such as Multi-Query Attention (MQA) [36] and Grouped-Query Attention (GQA) [1] in the LLaMA family, and further to advanced mechanisms like Multi-Head Latent Attention (MLA) combined with Mixture of Experts (MoE) in DeepSeek-v3 [24].
As MoE-based models significantly reduce the compute cost of the Feed-Forward Network (FFN) component, the attention mechanism accounts for an increasingly dominant fraction of total compute.
Reflecting this trend, Fig. 1(a) compares the floating-point operation breakdown of the attention mechanism and other computational kernels between the MHA +MLP model Qwen-chat-7B (Qw7B) and MLA +MoE models DeepSeek-v3-16B (DS16B) and DeepSeek-v3-671B (DS671B), during both the prefill and decode stages.
At long-context inference, the attention mechanism of Qw7B accounts for 19% of all floating-point operations, whereas in DS671B this proportion increases to 71% during decoding, with a similar trend observed in the prefill stage.

Such “attention bottleneck” has led to extensive research aimed at optimizing attention dataflows on the dominant AI hardware platform, namely Nvidia Graphics Processing Units (GPUs).
One of the most widely adopted solutions is FlashAttention [5] for MHA dataflow, which accelerates attention by efficiently fusing its microkernels.
Over two generations of improvements, FlashAttention-2 [6] introduces further algorithmic optimizations, while FlashAttention-3 [34] leverages the asynchronous execution capabilities of the latest Nvidia GPUs.
At the same time, recent application trends, driven by the increasing importance of “reasoning” models, have shifted focus toward inference efficiency. This has led to the development of attention variants such as MQA, GQA and MLA, aiming to reduce KV cache size and accelerate attention during inference on GPUs.
An optimized dataflow for MLA on GPUs is FlashMLA [18], developed by the DeepSeek team, which leverages a mechanism similar to FlashAttention.

Despite these advances, attention kernels still achieve only a fraction of the peak performance offered by the State-of-the-Art (SoA) Nvidia GH200 GPU. Fig. 1(b) shows the performance of FlashAttention-3 during the prefill stage and FlashMLA during decoding on the GH200 roofline model. Both implementations exhibit a large performance gap relative to the roofline, ranging from 26% to 64%.
Moreover, Nvidia’s GH200 GPU comes with significant cost and power requirements: it integrates an 814 mm2 die fabricated in TSMC’s 5nm process, coupled with six High Bandwidth Memory (HBM) stacks that account for more than half of the total cost, adding up to a total Thermal Design Power (TDP) of 700 W.

Given the suboptimal utilization and the high cost and power requirements of modern GPUs, industry and academia are aggressively developing accelerators for LLM inference, aiming to deliver competitive performance while boosting the efficiency of the system by minimizing energy-hungry HBM accesses, in hope to significantly reduce per-token cost, while maintaining flexibility and competitive token serving latency and throughput.
Fig. 2 illustrates an emerging scalable design pattern for inference accelerators targeting large transformer-based AI models [19, 32, 21, 39]. These systems adopt large, full-reticle, multi-die or even wafer-scale architectures [21], structured as meshes of compute tiles that integrate thousands of Processing Elements (PEs), as extremely dense, large matrix processing units alongside vector and scalar engines to accelerate all major LLM kernels.
Each tile is provided with a local, software-managed L1 scratchpad memory to buffer data and hide main memory latency.
Multiple HBMs are typically employed as main memory, positioned at die boundaries and connected to dedicated memory controllers.

Mapping LLM workloads onto these architectures presents a significant challenge.
While full, fine-grained control over inter-tile and tile-to-HBM transfers enables highly optimized data movement, this flexibility results in large design space: the dataflow must be carefully designed to maximize matrix engine utilization while minimizing costly off-chip HBM accesses.
Furthermore, co-designing a tile-based accelerator template that can efficiently map LLM workloads remains an open architectural problem which is tightly coupled with dataflow selection.

Among various parallel computing paradigms, collective communication—such as multicast and reduction—plays a crucial role in enabling efficient data exchange among processing tiles.
Intensively researched and widely used in distributed systems, these operations are now being adopted in tile-based many-PE architectures, especially with the introduction of hardware-supported collective primitives on the Network on Chip (NoC) [11, 32, 39].
Traditional software-based collective primitives rely on successive point-to-point inter-tile transfers, resulting in high communication latency.
In contrast, NoCs equipped with hardware-supported collective communication primitives establish direct, optimized communication paths, significantly reducing communication overhead.
Recent work has proposed dedicated hardware implementations of fabric-level collective primitives, demonstrating high performance with modest area and power overheads [4, 39, 8, 32, 20].
These advances motivate the co-design of dataflow with NoC-level collective primitives, enabling improved on-chip data reuse, reduced HBM traffic, and higher PE utilization.

Furthermore, modern LLMs continue to scale from tens to hundreds—and even thousands—of billions of parameters [24, 10].
As a result, LLM inference serving increasingly relies on interconnecting multiple accelerator chips and HBM stacks to meet the growing demands for memory capacity and compute throughput, as a single accelerator is fundamentally constrained by reticle size limits and the slowing of Moore’s Law.
Recent advances in wafer-scale technology, which enable high-density integration of multiple dies, have emerged as a promising system-integration paradigm for high-performance LLM serving [12, 41].
By placing chips in close physical proximity and interconnecting them with high-speed Die-to-Die (D2D) links, wafer-scale systems significantly improve communication efficiency while reducing system integration overhead.
This trend raises another key question: “how does accelerating the attention bottleneck on a single accelerator die translate to end-to-end modern LLM inference performance across multiple dies, and in wafer-scale systems?”

\phantomsubcaption

\phantomsubcaption

\phantomsubcaption

Figure 2: (a) Tile-based many-PE architecture template (b) Row-wise multicast implementation with fabric-supported hardware collectives (HW) compared against two software-based collective implementations (SW.Tree and SW.Seq) (c) A wafer-scale multi-die system consisting of multiple tile-based many-PE accelerators, with a 2D-mesh D2D interconnect topology.

In this work, we address these challenges in a bottom-up manner. We first propose FlatAttention, a dataflow that efficiently maps attention kernels and its variants on tile-based accelerators by leveraging collective communication primitives, such as multicast and reduction operations, in the NoC, to dramatically reduce off-chip memory accesses.
We demonstrate that co-designing the architecture to natively accelerate collective communication primitives enables mapping attention and its variants more efficiently than on SoA GPU solutions.
Furthermore, we take the full DeepSeek-v3-671B inference as an end-to-end study, mapping the complete decoding workload onto a wafer-scale multi-die system composed of tile-based many-PE accelerators. With the use of FlatAttention, we achieve substantial end-to-end decoding speedups over SoA solutions.
the contribuions of this work are:

•

Propose FlatAttention, an efficient workload allocation and scheduling strategy for modern attention variants like MHA, GQA and MLA on tile-based accelerators.
For evaluation, we developed a modeling and simulation framework to estimate the performance of a wide range of tile-based accelerator configurations, calibrated against cycle-accurate Register Transfer Level (RTL) simulations.
FlatAttention leverages collective primitives in the on-chip network fabric to achieve up to 92.3% utilization for the MHA layer on tile-based many-PE accelerators, delivering a 4.1 $\times$ speedup over the FlashAttention-3 dataflow on the same tile-based accelerator while reducing HBM traffic by 16 $\times$ .

•

Co-explore the accelerator architecture and FlatAttention parameters, identifying key trends and tradeoffs to guide the selection of optimal algorithm–architecture configurations.
We propose a general tiling strategy for FlatAttention dataflow and identify an optimal tile-based accelerator configuration that matches the peak FP16 performance and HBM bandwidth of NVIDIA’s state-of-the-art GH200 GPU.
By generalizing FlatAttention to advanced attention variants like GQA and MLA, FlatAttention on this tile-based accelerator configuration achieves an average utilization of 86% (up to 95.6%) for attention kernels in the compute-bound regime, and an average HBM bandwidth utilization of 78% (up to 92.1%) for memory-bound attention.
Overall, FlatAttention on the selected architecture delivers an average $1.9\times$ speedup over optimized FlashAttention-3 and FlashMLA implementations on GH200, across both prefill and decode phases and multiple attention variants.

•

Evaluate FlatAttention in an end-to-end deployment of DeepSeek-v3-671B decoder, a SoA open-source LLM, on a wafer-scale multi-die system composed of tile-based many-PE accelerators.
By examining various parallelism paradigms for multi-chip systems, we identify a highly optimized mapping of the DeepSeek-v3-671B decoder in terms of both overall throughput and per-user output token latency.
Our approach achieves a 1.9 $\times$ improvement in system throughput and a 1.4 $\times$ reduction in per-user token output latency, despite operating with 1.5 $\times$ lower peak system performance compared to a SoA solution deployed on 96 NVIDIA H800 GPUs.

In our preliminary work [42], we introduced the FlatAttention dataflow for the original MHA architecture and provided a preliminary evaluation focusing primarily on the prefill phase.
In this work, we propose a generalized FlatAttention dataflow supporting all major attention mechanisms employed in modern LLMs.
Additionally, we extend the evaluation to both prefill and decoding.
Furthermore, we provide new insights by assessing FlatAttention in a full end-to-end inference workload.
To this end, we extend our simulation framework to model wafer-scale multi-die systems comprising multiple tile-based accelerators, interconnected via dedicated D2D links.

## II Background

\phantomsubcaption

\phantomsubcaption

\phantomsubcaption

\phantomsubcaption

\phantomsubcaption

Figure 3: (a) SoA LLM model architecture overview and schematic for (b) MHA in prefill (c) MHA in auto-regressive decoding as well as (d) GQA in auto-regressive decoding.

### II-A SoA LLMs

Many of today’s SoA LLMs, such as GPT-4, LLaMA and DeepSeek-v3, employ decoder-only networks built from a stack of decoder blocks each comprising two main components: a MHA layer and a FFN. These modules are embedded within a residual topology with normalization layers to maintain stability at scale.

Fig. 3 illustrates this generic high-level architecture, as well as common variants for the MHA and FFN blocks, next to the SoA open source DeepSeek-v3 model architecture, a concrete implementation of this high-level architectural template.
DeepSeek-v3 employs MLA and MoE layers to implement the attention and FFN blocks, respectively.
Beyond attention and FFN innovations, DeepSeek-v3 employs other recent architectural improvements: Rotary Position Embeddings (RoPE) to capture relative positional information directly within the attention mechanism, Sigmoid-weighted Linear Unit (SiLU) activations, and RMSNorm for increased computational efficiency over LayerNorm.

### II-B Attention

In a standard MHA layer, an input sequence of $S$ tokens $X\in\mathbb{R}^{S\times d_{\text{model}}}$ is projected into $h$ query, key and value matrices:

| $Q_{i}=XW^{Q}_{i},\qquad K_{i}=XW^{K}_{i},\qquad V_{i}=XW^{V}_{i}$
| (1)

with $W^{Q}_{i},W^{K}_{i},W^{V}_{i}\in\mathbb{R}^{d_{\text{model}}\times d}$ and $d$ the per-head dimensionality, to calculate:

| $\begin{split}\text{MHA}(Q,K,V)&=\text{Concat}(Attn_{1},\dots,Attn_{h})W^{O}\\
Attn_{i}&=\text{Softmax}(\frac{Q_{i}K_{i}^{T}}{\sqrt{d}})V_{i}\end{split}$
| (2)

where $W^{O}\in\mathbb{R}^{d\times d_{\text{model}}}$ .
The computation of one attention head $Attn_{i}$ is illustrated in Fig. 3.

In autoregressive inference, an initial input sequence, or prompt, is fed to the network (prefill phase), after which tokens are generated sequentially and appended to the input sequence (decoding phase).
At decoding step $t$ , only the query, key and value vectors of the newly generated token are computed, while key and value vectors of previous tokens are retrieved from a Key-Value (KV) cache populated during prefill and prior decoding iterations.
Notably, the General Matrix Multiplication (GEMM) for both the attention score and output calculations reduce to matrix-vector multiplications (GEMV), as illustrated in Fig. 3, for all decoding steps.
However, the full $K_{i}$ and $V_{i}$ matrices are still used as part of this calculation; thus, at every decoding step, the KV cache grows linearly with the sequence length $S$ , increasingly stressing memory bandwidth and motivating more efficient attention variants.

In Multi-Query Attention (MQA) [36], adopted by LLaMA 2, key and value projections are shared by all heads, reducing the KV cache size by $h$ times:

| $W_{i}^{K}=W^{K},\qquad W_{i}^{V}=W^{V},\qquad\forall i\in[1,h]$
| (3)

Grouped-Query Attention (GQA) [1], adopted by LLaMA 3, generalizes MQA by sharing keys and values across groups of heads, providing a tunable trade-off between cache size, compute efficiency, and model accuracy:

| $W_{i}^{K}=W^{K}_{g(i)},\qquad W_{i}^{V}=W^{V}_{g(i)}$
| (4)

As $G$ heads in a group share the same $K_{g(i)}$ , $V_{g(i)}$ matrices, their respective queries can be concatenated to turn the attention score and output computations back into GEMMs, as shown if Fig. 3.

Instead of sharing key and value projections across heads, Multi-Head Latent Attention (MLA), introduced in DeepSeek-v2 [23], compresses each head’s key and value vectors by mapping them into a compact latent space, where the shared low-rank down-projections for the layers are:

| $c^{KV}=XW^{DKV},\qquad c^{Q}=XW^{DQ}$
| (5)

The attention projections are then decompressed from the latent space:

| $Q_{i}=c^{Q}W^{UQ}_{i},\qquad K_{i}=c^{KV}W^{UK}_{i},\qquad V_{i}=c^{KV}W^{UV}_{i}$
| (6)

with $W^{DKV}\in\mathbb{R}^{d_{\text{model}}\times d_{c}}$ a shared down-projection matrix for both keys and values, and $W^{UK}_{i},W^{UV}_{i}\in\mathbb{R}^{d_{c}\times d}$ unique up-projection matrices.
Only the compressed keys and values need to be cached, optimizing the KV cache size.

Apart from auto-regressive decoding, speculative decoding has emerged as an effective technique for reducing inference latency in large language models.
Speculative decoding leverages a lightweight draft model to generate multiple candidate tokens ahead of time, which are then verified in parallel by a larger target model.
Some models further support self-speculative decoding, where the target model itself produces draft tokens.
Similar to GQA, also speculative decoding can help restore GEMV operations into GEMM operations.

### II-C Feed-Forward Network (FFN)

In the FFN block, modern LLMs commonly adopt a gated MLP structure (Fig. 3), which consists of an up-projection that expands the hidden dimension, a gate projection that modulates activation through element-wise gating, and a down-projection that projects the activations back to the original hidden dimension.
This gated design improves expressiveness and training stability while maintaining computational efficiency.

To improve scaling efficiency, DeepSeek-v3 replaces the dense FFN with a Mixture of Experts (MoE) architecture.
In a MoE layer, a learned gating network routes each token to a small subset of experts—specialized FFNs—instead of activating all of them, as shown in Fig. 3. If only $k$ out of $E$ experts are selected per token, the computational cost becomes proportional to $k$ rather than $E$ , while $E$ and the total parameter count can be increased freely. This enables higher model capacity without a proportional increase in inference cost.

### II-D Tile-Based Many-PE Architecture

As illustrated in Fig. 2, the fundamental building block of the many-PE architecture is the “tile”.
Each tile comprises PEs, local memory (L1), a Direct Memory Access (DMA) engine, and local interconnects.
There are three main types of PEs: scalar cores, vector engines, and matrix engines.
Scalar cores mainly handle dataflow control tasks, whereas heavy computational tasks are offloaded to the vector and matrix engines based on the computation type.
The local L1 memory is implemented as a software-managed scratchpad memory for increased area efficiency; all PEs within a tile can directly access the local L1 memory via the local interconnect.
The DMA engine in each tile is responsible for bulk data movement in and out of the local L1 memory.

The tile-based many-PE system uses an on-chip 2D-mesh NoC to connect tiles.
Off-chip memory, such as HBM, is located at the boundary of the mesh NoC, interfaced through the respective memory controllers.
To improve the efficiency of data exchange among tiles, fabric-level acceleration of collective communication primitives has been proposed [14, 17, 27, 4].
Using a row-wise multicast as an illustrative example in Fig. 2, hardware-supported collective primitives perform fine-grained, flit-level data replication within NoC routers along the multicast path, enabling all destinations to receive data with low latency.
This approach is significantly more efficient than software-managed collectives, which require multiple stages of point-to-point transfers.

The tile-based accelerator designs can be replicated across multiple dies and integrated into a wafer-scale system.
Wafer-scale integration can be realized using either monolithic fabrication on a single wafer[20, 44] or chiplet-based integration[28, 41]. In the latter approach, compute, memory and IO dies are fabricated separately and integrated on a wafer-scale silicon interposer using Chip-on-Wafer-on-Substrate (CoWoS) packaging technology[3, 41]. The dies are electrically interconnected through through-silicon vias (TSVs) and redistribution layers (RDLs) in the interposer, enabling greater heterogeneity and design flexibility, as illustrated in Fig. 2.
In addition to compute dies hosting tile-based accelerators, the system integrates dedicated HBM memory dies and router dies to support wafer-scale D2D interconnect.
In this work, we adopt a conventional 2D mesh as the baseline interconnect topology.

## III Dataflow Implementation

### III-A Motivation

\phantomsubcaption

\phantomsubcaption

\phantomsubcaption

\phantomsubcaption

Figure 4: (a) Parametric definition of FlatAttention. (b) Detailed FlatAttention dataflow. (c) Naive FlatAttention schedule. (d) Optimized asynchronous FlatAttention schedule.

FlashAttention is the first widely adopted MHA dataflow implementation for GPUs that efficiently fuses matrix multiplication and softmax operations within each attention head, maximizing data reuse in the shared memory of each Streaming Multiprocessor (SM).
The FlashAttention-2 algorithm is illustrated in Algorithm 1.
In this approach, the MHA workload is partitioned across the batch and head dimensions over the SMs, and, when the batch size or number of heads is insufficient, additionally along the output sequence length dimension.

This dataflow and mapping strategy can also be applied to tile-based many-PE accelerators, since an SM in a GPU is analogous to a compute tile in such architectures. With this mapping, each compute tile operates on distinct data, as different heads do not share or reuse data, allowing them to access HBM independently. This eliminates the need for inter-tile communication, but also prevents any data reuse across tiles.
In prefill mode of MHA, with sequence length $S$ , head dimension $D$ , number of heads $H$ , batch size $B$ and block size $M\coloneqq B_{r}=B_{c}$ , the FlashAttention dataflow results in an HBM I/O complexity of:

| $\text{IO}=B\cdot H\cdot N_{\text{outer}}\cdot\Bigl(Q_{\text{block}}+O_{\text{block}}+N_{\text{inner}}\cdot(K_{\text{block}}+V_{\text{block}})\Bigr).$

With the number of outer and inner loop iterations
$N_{\text{outer}}=N_{\text{inner}}=\frac{S}{M}$ ,
and block sizes
$Q_{\text{block}}=O_{\text{block}}=K_{\text{block}}=V_{\text{block}}=D\cdot M$ ,
the expression simplifies to:

| $\text{IO}=2\cdot B\cdot H\cdot D\cdot S\cdot\left(1+\frac{S}{M}\right).$

While all other parameters are fixed by the computation, the block size parameter $M$ can be increased to reduce I/O complexity. Intuitively, larger blocks improve data reuse within a tile’s L1 memory.
However, $M$ is constrained by the capacity of the L1 memory on a single tile, which must be able to simultaneously store the tensors $Q_{i},K^{T}_{j},V_{j},$ and $O_{i}$ at any point in time.

To further reduce off-chip I/O accesses, we propose FlatAttention, a dataflow that fundamentally redefines how MHA is parallelized on tile-based architectures. FlatAttention treats multiple tiles as a unified compute unit, enabling them to collaboratively process an MHA block—defined above—of substantially larger size by leveraging the aggregate L1 memory of the tile group to collectively store the block.

Given that a single tile can host a block of size $M\coloneqq B_{r}=B_{c}$ , grouping ${\color[rgb]{1,0,0}\definecolor[named]{pgfstrokecolor}{rgb}{1,0,0}N}\times{\color[rgb]{1,0,0}\definecolor[named]{pgfstrokecolor}{rgb}{1,0,0}N}$ tiles allows the group to collectively host a larger block $({\color[rgb]{1,0,0}\definecolor[named]{pgfstrokecolor}{rgb}{1,0,0}N}B_{r},{\color[rgb]{1,0,0}\definecolor[named]{pgfstrokecolor}{rgb}{1,0,0}N}B_{c})$ . As a result, the I/O complexity of MHA becomes:

| $\text{IO}=2\cdot B\cdot H\cdot D\cdot S\cdot\left(1+\frac{S}{{\color[rgb]{1,0,0}\definecolor[named]{pgfstrokecolor}{rgb}{1,0,0}N}\cdot M}\right).$

For example, when $S=4096$ , $M=128$ , and $N=8$ , this yields a theoretical $6.6\times$ reduction in HBM accesses compared to the FlashAttention dataflow.
However, the reduction in HBM traffic achieved by our proposed FlatAttention dataflow comes at the cost of introducing on-chip inter-tile communication, which is required to collectively process a large MHA block within the tile group.
Efficiently handling on-chip inter-tile communication therefore becomes crucial to realizing the benefits of reduced off-chip communication.

### III-B Detailed FlatAttention Dataflow

The FlatAttention dataflow is depicted in Fig. 4.
We refer to a set of tiles collectively processing a block, as previously introduced, as a group, illustrated in Fig. 4.
We define the shape of the group as $G_{x}\times G_{y}$ . FlatAttention applies the same tiling and mapping scheme to groups as FlashAttention applies to tiles, but it introduces a secondary level of blocking within each group. This secondary blocking divides the $\{B_{c},B_{r}\}$ block dimensions into smaller slices based on the group shape $\{G_{x},G_{y}\}$ , resulting in $\{\frac{B_{c}}{G_{x}},\frac{B_{r}}{G_{y}}\}$ slice sizes for every tile.

Algorithm 2 and Fig. 4 outline the FlatAttention dataflow.
At a high level, the algorithm is conceptually similar to FlashAttention (Section III-A): different groups process distinct data, so no communication between groups is required. However, distributing the computation of an MHA block to tiles in a group introduces distinct data movement patterns within the group:

•

Loading and Multicasting:
Only diagonal tiles of the group load $Q$ slices from HBM (line 5), followed by multicasting $Q$ slices row-wise 6 to the other tiles in the group.
When entering the inner loop, the diagonal tiles load $K$ and $V$ slices from HBM 8 and multicast them column-wise 9.

•

Computing Attention ( $Q\cdot K^{T}$ ) and Rowmax:
Each tile computes a segment of the attention score matrix 10. During the computation of row-wise maxima for Softmax, tiles compute partial row maxima locally 11 updated with the tracking maxima 13, followed by a row-wise reduction within the group to calculate the global row maxima 15. The results are then multicast row-wise to ensure that each tile holds the global row maxima 16.

•

Softmax Denominator:
After computing the partial Softmax denominator locally with global row maxima 17 18, the same reduction 19 and multicast 20 procedure applies to computing the global denominator, which is then updated with the tracking maxima and denominator 22.

•

Output Matrix ( $O$ ):
Each tile updates local $O$ slices and tracking statistics in the inner loop, and computes partial results for $O$ slices on exit 23- 28. FlatAttention then performs a row-wise reduction of $O$ slices 29 followed by storing $O$ slices in HBM 30 only from diagonal tiles.

These communication requirements are a direct result of FlatAttention’s parallelization scheme, which enables minimizing costly global off-chip I/O by exploiting on-chip data reuse across tiles through local on-chip communication.
This trade-off of global for local requirements enables FlatAttention to achieve better scalability and performance compared to FlashAttention methods for tile-based many-PE architectures, as long as local on-chip communication is efficiently handled, as will be discussed in Section V-A.

### III-C Asynchronous FlatAttention

In the naïve version of FlatAttention (Algorithm 2), data movement and Softmax-related computations still account for a significant portion of the runtime, as illustrated in Fig. 4.
This reduces the overall utilization, as the system’s peak performance is primarily determined by the matrix engine, which has much higher computational power compared to the vector engine. To further improve utilization, we propose leveraging the asynchronous nature of DMA, vector and matrix engine invocations to overlap the runtime of data movement and Softmax operations with matrix multiplications.

The optimized dataflow schedules the computation of two heads concurrently on each group. While the matrix engine processes matrix multiplications for one head, the DMA and vector engine perform data movement and Softmax operations for the other111The same optimization can be applied with two output row blocks $O_{i}$ instead of two heads, reducing memory requirements as the $K^{T}_{j}$ and $V_{j}$ blocks are shared. To simplify the evaluation, where sufficient row blocks are not available, we adopt the presented implementation.. Fig. 4 demonstrates this optimization, showcasing how it can ensure that matrix engine remains nearly always active, provided that the runtime of the matrix multiplication overlaps completely with data movement and Softmax operations.
Notably, FlashAttention-3 employs a similar technique to improve upon FlashAttention-2, though the exact implementation varies due to architectural differences.

### III-D Generalization to Decode Stage and Attention Variants

Beyond the prefill-phase MHA dataflow, FlatAttention can be generalized to the decode phase, as well as to attention variants such as GQA and MLA. In the auto-regressive decode phase, the sequence length of $Q$ is no longer equal to that of $KV$ .
Instead, the sequence length of $Q$ becomes $1$ , while the sequence length of $KV$ corresponds to the KV-cache length, as illustrated in Fig. 3.
To apply the FlatAttention dataflow to MHA in the auto-regressive decode phase, the tile group can be configured to span a single row, processing a block size of $B_{r}=1$ for $Q$ .
Meanwhile, the block size $B_{c}$ can be increased to maximize reuse of the KV cache by leveraging the aggregated L1 memory capacity of the tile group.

For MHA in speculative decoding, the sequence length of $Q$ increases from $1$ to the speculative length $sp$ as shown in Fig. 3, with causal mask applied.
Depending on the sequence lengths of $Q$ and $KV$ , an appropriate tile-grouping and blocking scheme can be applied for FlatAttention, which will be discussed in Section V-B.

In the case of GQA, multiple query heads share a single KV cache. As a result, the grouped queries can be interpreted as forming an effective query sequence of increased length $1*G$ , while attending to the same KV data. The decode operation can thus be viewed as executing $\frac{H}{G}$ attention computations in parallel over a longer query sequence, as illustrated in Fig. 3.

To improve decoding efficiency, MLA is used together with a weight-absorption trick that converts it into MQA mode. The key observation is that the problematic up-projection matrices $W^{UK}_{i}$ and $W^{UV}_{i}$ always appear inside matrix products where the multiplications can be rearranged by associativity. For example, to absorb $W^{UK}_{i}$ into the query side, consider the attention score term $Q_{i}K_{i}^{\top}$ for head $i$ and rewrite it in terms of the up-projection matrices as follows:

| $Q_{i}K_{i}^{T}=(c^{Q}W^{UQ}_{i})(c^{KV}W^{UK}_{i})^{\top}=c^{Q}W^{UQ}_{i}W^{UK\top}_{i}c^{KV\top}$
| (7)

Where the $W^{UK}_{i}$ can be absorbed into $W^{UQ}_{i}$ as:

| $W^{UQK}_{i}=W^{UQ}_{i}W^{UK\top}_{i}$
| (8)

By further transformation, the MLA formulation can be viewed as MQA, where $c^{Q}$ is projected with $W^{UQK}_{i}$ to produce all query heads that share the same key–value representations derived from $c^{KV}$ .
In this case, the generalization of the FlatAttention dataflow is similar to MQA, where the queries share a grouped sequence length across all heads. The detailed formulations of the weight-absorption transformation and the corresponding FlatAttention generalization for MLA in the DeepSeek-v3 models are presented in Appendix A.

We observe that these modern attention variants can all be transformed into a unified multi-head attention (MHA) formulation.
They primarily differ in the shape of the attention score matrices and the number of attention heads across variants.
A key challenge for the performance of FlatAttention lies in mapping irregularly shaped attention score matrices and determining the appropriate scale of tile groups.
We elaborate on this in detail in Section V-B.

### III-E DeepSeek-v3 Decoder and GEMM Dataflow

\phantomsubcaption

\phantomsubcaption

\phantomsubcaption

\phantomsubcaption

\phantomsubcaption

Figure 5: (a) General SUMMA dataflow on tile-based accelerators for GEMM. DeepSeek-v3 workload distribution in (b) pipeline parallelism, (c) full expert parallelism, and (d) EP-PP hybrid parallelism. (e) Wafer-scale multi-chip system execution mode.

To evaluate the impact of the FlatAttention dataflow speedup on end-to-end LLM inference performance, we adopt the DeepSeek-v3-671B[24] decoder block as a case-study application.
DeepSeek-v3-671B enables Multi-Token Prediction (MTP) by default, which performs self-contained speculative decoding by predicting one next token per decoding iteration. This corresponds to a speculative length of 2, with an acceptance rate of 0.7 for the predicted token.
A DeepSeek-v3-671B layer is composed of a sequence of computational kernels, including normalization, projections, RoPE, the core MLA mechanism, among others.
The detailed kernel flow for a DeepSeek-v3-671B decoding layer is presented in Appendix B.
In our evaluation, we execute kernels sequentially, running one kernel at a time on the tile-based many-PE accelerator.

Among all kernels, GEMM appears in every projection kernel. We implement GEMM on a tile-based many-PE accelerator using the SUMMA[38] algorithm, as illustrated in Fig. 5.
In the SUMMA dataflow, each tile stationarily computes an output block $C_{i,j}$ . At every iteration over the $K$ dimension, a column of blocks $A_{i,k}$ is multicasted row-wise, while a row of blocks $B_{k,j}$ is multicasted column-wise across tiles.
If the column-wise multicasts were initiated from tiles in the same row, the read requests to the HBM would conflict on the same NoC links.
Therefore, we assign the task of fetching data from HBM and performing subsequent row- and column-wise multicasts to the tiles along the diagonal.

### III-F DeepSeek-v3 Decoder Mapping on Wafer-Scale Multi-Die System

As the parameter size reaches 671B, a single accelerator chip with HBMs typically provides limited memory capacity (e.g. 96-144 GiB in GH200), which is insufficient to store all parameters at the default FP8 precision.
Consequently, distributing the DeepSeek-v3 decoder workload across multiple chips is necessary, and various parallelism strategies must be considered.

Two key metrics are commonly used to evaluate decoder performance: overall throughput (tokens/s) and Time per Output Token (TPOT).
The TPOT metric captures per-user output token latency and is typically targeted to be within 50 ms. Many parallelism paradigms have been proposed to increase throughput while simultaneously reducing TPOT in LLM decoder systems.

In this study, we model and discuss Pipeline Parallelism (PP), Expert Parallelism (EP), and hybrid approaches combining the two.
In a typical PP organization, shown in Fig. 5e, each chip is assigned one or more decoder layers, and Chip-to-Chip (C2C) communication only transfers intermediate activations between layers.
In contrast, full EP organization is fundamentally different: each chip executes the complete decoder stack, except for the routed experts in the MoE, whose weights are distributed across chips.
During decoding, different users’ token batches are processed in parallel across all chips for the MLA and shared experts of the MoE, followed by C2C communication to dispatch tokens to the chips hosting the selected routed experts. After expert computation, C2C communication is used again to gather results.

Hybrid parallelism combines PP and EP. For example, an EP16–PP4 configuration applies expert parallelism within groups of 16 chips, while pipeline parallelism is used across 4 groups.

## IV Modeling and Analysis Methodology

(a)

(a)

(b)

(b)

(c)

(c)

Figure 6: GVSoC–RTL calibration for (a) RedMulE and NoC under (b) SW.Seq row-wise multicast and (c) HW row-wise reduction collective patterns.

We developed a modeling and simulation framework for tile-based many-PE accelerators on the GVSoC event-based simulator [2] for functional and performance simulation.
GVSoC is open source and released with models for the single-issue RISC-V core, the Spatz[31] vector engine supporting the RISC-V Vector (RVV) extension, the DMA engine, and tile-local L1 memory and interconnect.
To extend these capabilities, we developed and calibrated new models for the RedMulE[37] matrix engine and the FlooNoC fabric according to their open-source RTL implementations.
Colagrande et al. propose a lightweight extension to the FlooNoC RTL to support collective communication primitives [4].
We extend GVSoC’s NoC model with hardware support for the collective communication primitives described in Section II-D, modeled after the previous work.
We further extend Spatz with a custom RVV instruction to support exponential operations, along with a dedicated exponential unit integrated into the FPU, modeled and calibrated after the implementation described in [33].
Furthermore, we integrated the performance-accurate DRAMSys [15] simulator into GVSoC for HBM modeling.

We calibrate the GVSoC model of RedMulE using the default $12\times 4$ Compute Element (CE) array configuration across a range of matrix multiplication shapes.
As shown in Fig. 6(a), the average cycle deviation is 0.17%.
We further calibrate the GVSoC NoC model using the default $4\times 4$ mesh configuration under two collective communication patterns: SW.Seq row-wise multicast and HW row-wise reduction.
The results, shown in Fig. 6(b) and Fig. 6(c), exhibit average cycle deviations of 6% and 12%, respectively.

Using these building blocks, we constructed the SoftHier model and analysis framework on the GVSoC platform: a flexible, parameterizable tile-based many-PE accelerator simulator.
The framework is configurable using architecture configuration files, enabling the instantiation of specific accelerator designs, e.g. to explore different numbers of CEs in the RedMulE units.
In the SoftHier framework, we implemented the kernel dataflows in C and compiled them using the GNU RISC-V GCC compiler with -O3 optimization.

To evaluate wafer-scale system performance, we employ a naive parallel execution model, illustrated in Fig. 5, in which kernel execution on individual chips and chip-to-chip communication are fully separated by synchronization barriers.
Under this execution model, system performance can be estimated by combining kernel runtimes measured on individual accelerators modeled in SoftHier, with chip-to-chip communication latency obtained from a separate C2C model in the GVSoC simulator.
The C2C model abstracts each chip as a traffic generator while explicitly modeling the D2D links to provide a high-level representation of existing interconnect protocols (e.g., NVLink and CXL).
This abstraction supports credit-based flow control with configurable latency and bandwidth, and the modeled D2D routers handle packet-level congestion.

## V Experimental Results

(a)

(a)

(b)

(b)

Figure 7: Latency comparison of software-based and fabric-accelerated collective primitives: (a) row-wise multicast; (b) row-wise sum reduction on accelerator with 32 $\times$ 32 tiles.

TABLE I: System Specifications

| Chip
| 32×32 Tiles, 1024-bit NoC link width, run at 965MHz

| HBM
| HBM4 stack with 32 channels on south edge

| Tile
RedMulE Matrix.Eng: 32×16  CEs, 1024 FLOP/cyc@FP16

Spatz Vector.Eng: 4 Spatz, each with 32 FLOP/cyc@FP16

Local Memory: 384 KiB, 512 Byte/cyc

| Summary
| 988 TFLOPS@FP16 Peak Performance

| 2 TB/s  Peak HBM Bandwidth

### V-A FlashAttention vs. FlatAttention

We first characterize the performance of fabric collective primitives on a large-scale tile-based many-PE accelerator, as specified in Table I. The evaluated system consists of a $32\times 32$ mesh of tiles and delivers the same peak FP16/BF16 performance as the NVIDIA GH200 GPU.

We study two representative collective communication patterns used in the proposed FlatAttention dataflow, namely row-wise multicast and row-wise sum reduction. For each pattern, we compare the performance of fabric-supported hardware collectives (HW) against two software-based collective implementations (SW.Seq and SW.Tree).

As illustrated in Fig. 2, SW.Seq represents the most naive software implementation of row-wise multicast, in which the source tile issues sequential unicast transfers to all destination tiles. In contrast, SW.Tree employs a logarithmic tree-based multicast scheme, which significantly reduces the number of communication steps required. However, this approach requires synchronization between consecutive steps.

As shown by the results for row-wise multicast and row-wise sum reduction in Figs. 7(a) and 7(b), respectively, fabric-supported collectives achieve substantial performance improvements as the transfer size increases. On a $32\times 32$ mesh, hardware multicast delivers speedups of $5.1\times$ and $30.7\times$ over SW.Tree and SW.Seq, respectively, while hardware reductions deliver speedups of $10.9\times$ and $67.3\times$ over the software-based implementations.

We next compare different prefill-phase MHA implementations across multiple layer configurations. Specifically, we evaluate FlashAttention-2 (FA-2) and FlashAttention-3 (FA-3) on tile-based many-PE accelerators, where FA-3 incorporates a scheduling strategy similar to the optimization described in Section III-C. For FlatAttention, we configure a single group spanning the entire system, i.e., $G_{x}=G_{y}=32$ .

We evaluate several FlatAttention variants: FlatAttention with sequential software collectives (FlatSC), FlatAttention with tree-based software collectives (FlatTC), FlatAttention with hardware-supported NoC collectives (FlatHC), and the optimized FlatAttention dataflow (FlatAync) introduced in Section III-C, which also leverages NoC collective primitives. Our evaluation spans multiple MHA layers, varying the sequence length $S\in\{1024,2048,4096\}$ and head dimension $D\in\{64,128\}$ , while fixing the batch size to $B=2$ and the number of heads to $H=32$ .

Fig. 8 reports the runtime breakdown and average HBM bandwidth utilization. On the evaluated tile-based many-PE system, FlashAttention exhibits a strongly memory-bound execution profile, with average HBM bandwidth utilization reaching up to 80%. HBM accesses dominate the overall runtime, significantly constraining effective compute utilization. Although FA-3 adopts an optimized dataflow that overlaps matrix multiplication and Softmax computation, the already saturated HBM bandwidth leaves little headroom for additional performance gains. Moreover, the more sophisticated scheduling employed by FA-3 introduces non-negligible control overhead, further diminishing its potential benefits under bandwidth-bound conditions.

In contrast, FlatAttention-based implementations substantially reduce HBM access time relative to FA-3, primarily due to the lower I/O complexity of the FlatAttention dataflow, as described in Section III-B. However, FlatSC, which relies on the naive SW.Seq collective implementation, incurs significant on-chip communication overhead, ultimately resulting in worse performance than FlashAttention-based approaches.

Replacing SW.Seq with the tree-based software collective strategy in FlatTC mitigates part of this overhead, yet on-chip inter-tile communication still accounts for more than 65% of the total runtime across all evaluated prefill-phase MHA layers. In contrast, enabling efficient collective primitives in the NoC fabric (FlatHC) substantially accelerates inter-tile communication, allowing FlatAttention to outperform FlashAttention across most MHA configurations.

Furthermore, the FlatAsync implementation demonstrates additional performance gains by overlapping Softmax computation, data movement, and matrix multiplication. Overall, our proposed optimizations achieve up to a 4.1 $\times$ speedup and a 16 $\times$ reduction in HBM traffic over FA-3 (D128, S4096).

Figure 8: Runtime breakdown (bars) and average HBM BW utilization (star markers) for different MHA implementations and layer sizes. +Runtime not overlapped with matrix engine. ++Runtime not overlapped with either vector or matrix engine. *Implementations without double buffering.

### V-B Tile Group Scale Trade-offs for FlatAttention

Although FlatAsync delivers the best performance across all prefill-phase MHA layers in Fig. 8, it does not fully utilize the accelerator for shorter sequence lengths (e.g., $S=2048$ and $S=1024$ ), where the RedMulE execution cannot be completely overlapped with other pipeline stages. To identify the optimal FlatAttention configuration, we study the impact of different (square) group sizes, $G_{x},G_{y}\in\{4,8,16,32\}$ , on the tile-based many-PE accelerator configuration summarized in Table I. We evaluate multiple MHA layers with sequence lengths $S\in\{512,1024,2048,4096\}$ , while fixing the head dimension to $D=128$ , the number of heads to $H=32$ , and the batch size to $B=4$ .

Figure 9: Runtime breakdown for different (square) flattening scales and layer sizes. Percentage labels above the bars indicate the average utilization of the matrix engine when active. +Runtime not overlapped with matrix engine. ++Runtime not overlapped with either vector or matrix engine.

Fig. 9 reports the runtime breakdown and the per-tile MHA workload slice size, i.e., $\frac{B_{r}}{G_{y}}=\frac{B_{c}}{G_{x}}$ , across different group scales. For long sequences (e.g., $S=4096$ ), the per-tile slice size is bounded by L1 memory capacity and thus remains constant. In this regime, increasing the group size reduces the overall HBM I/O complexity, as discussed in Section III-B, leading to shorter HBM access times and improved overlap with matrix multiplication on RedMulE. As a result, the $16\times 16$ and $32\times 32$ group configurations achieve 92.7% and 92.3% utilization, respectively, for $S=4096$ .

In contrast, for shorter sequence lengths (e.g., $S=512$ ), increasing the group scale reduces the per-tile slice size due to the fixed total sequence length, introducing two sources of performance degradation:

•

Reduced RedMulE utilization: Smaller per-tile slices lead to lower compute efficiency. For example, under a $32\times 32$ grouping with $S=512$ , the workload slice assigned to every tile amounts to $\frac{B_{r}}{G_{y}}=\frac{B_{c}}{G_{x}}=16$ , and the RedMulE engines achieve only 20% utilization during their active periods.

•

Amplified synchronization overhead: Smaller slices shorten RedMulE execution time, causing fixed costs—such as synchronization and data movement, including HBM access latency (approximately 200 cycles)—to account for a larger fraction of total runtime. Consequently, RedMulE execution can no longer fully hide these overheads, resulting in degraded performance.

We refer to this phenomenon as over-flattening. For moderate sequence lengths, these effects coexist: larger group sizes reduce I/O complexity but simultaneously increase the risk of over-flattening, highlighting a trade-off between communication efficiency and compute utilization.

Figure 10: A general tiling and group-scaling strategy for FlatAttention.

(a)

(a)

(b)

(b)

Figure 11: (a) RedMulE utilization vs. tiling size (b) L1 area occupancy of FlatAsync dataflow vs. tiling size.

Figure 12: Benchmark FlatAttention on tile-based many-PE accelerator vs. optimized attention dataflow on Nvidia’s GH200 GPU[43], varying head dimension (hd) and sequence length (sq) for prefill, speculative (sp) and KV cache length (kv) for decoding.

Motivated by the previous analysis, we propose a general tiling and group-scaling strategy for the FlatAttention dataflow that applies across attention varaints (MHA, GQA, MLA). The key principle is to prioritize per-tile RedMulE utilization before aggressive flattening: we first determine an optimal per-tile tiling configuration, i.e., $\frac{B_{r}}{G_{y}}$ and $\frac{B_{c}}{G_{x}}$ , that maximizes compute efficiency on each tile, and then increase the group scale as much as possible based on the shape of the attention score matrix and the mesh topology of the tile-based many-PE accelerator. Fig. 10 illustrates this strategy across different attention variants.

To identify the optimal per-tile tiling, we evaluate RedMulE utilization and L1 memory occupancy under varying $\frac{B_{r}}{G_{y}}$ and $\frac{B_{c}}{G_{x}}$ configurations, as shown in Fig. 11(a) and Fig. 11(b), respectively. Under the constraints of achieving more than 95% RedMulE utilization while keeping L1 memory usage within the 384 KiB budget specified in Table I, we find that $\frac{B_{r}}{G_{y}}=\frac{B_{c}}{G_{x}}=128$ is the optimal choice for the evaluated tile-based many-PE configuration. This configuration delivers the highest RedMulE utilization, reaching up to 98%, without exceeding L1 capacity.

Following the proposed tiling and group-scaling strategy, we evaluate FlatAttention on the tile-based many-PE accelerator across multiple attention variants, including prefill-phase MHA, decode MHA, decode GQA, and decode MLA, under a wide range of input shapes.
We use the system configuration listed in Table I, placing two HBM4 stacks on the south edge with up to 4 TB/s of bandwidth, which matches the peak FP16 performance and off-chip bandwidth of the NVIDIA GH200 GPU.
We compare FlatAttention on the said tile-based many-PE accelerator against state-of-the-art attention implementations on GH200: FlashAttention for MHA and GQA, and FlashMLA for MLA.

Fig. 12 summarizes the comparison results.
Bars labeled C:x% denote compute-bound kernels (x% utilization), while M:y% denote memory-bound kernels (y% HBM bandwidth utilization).
Across all attention variants and input shapes, FlatAttention achieves an average utilization of 86% in compute-bound regimes and an average HBM bandwidth utilization of 78% in memory-bound regimes.
Overall, FlatAttention on the tile-based many-PE accelerator outperforms the optimized attention implementations on GH200 in most evaluated scenarios, delivering an average performance speedup of 1.9 $\times$ .

Beyond the FlatAttention performance analysis presented above for a given tile-based many-PE configuration, FlatAttention can also be leveraged as a feedback mechanism to guide tile-based many-PE accelerator design parameters. The detailed co-exploration of architectural parameters is presented in Appendix D.

### V-C DeepSeek-v3 Decoder Inference Performance Analyze on Wafer-Scale Multi-Die System

In this section, we shift our focus to end-to-end inference performance of DeepSeek-v3 on wafer-scale multi-die systems.
The system integrates 64 tile-based many-PE accelerators interconnected via a wafer-scale interposer in an $8\times 8$ mesh topology.
Each accelerator is connected through D2D links with 1 TB/s bandwidth and 256 ns latency.
All tile-based many-PE accelerators are configured with identical peak FP8 throughput (1976 TFLOPS without sparsity) and HBM bandwidth (4 TB/s), as DeepSeek-v3 inference operates entirely in FP8 precision.

In the RedMulE matrix engine, FP8 peak throughput matches that of FP16.
To achieve this FP8 peak performance, we increase the accelerator operating frequency to 1.9 GHz from the previous configuration in Table I.
Similar frequencies are achieved in the NVIDIA GH200 GPU.
Each accelerator is equipped with two HBM4 stacks, providing up to 4 TB/s of bandwidth and 128 GiB of HBM capacity, enabling full deployment of DeepSeek-v3-671B decoder inference with MTP enabled on the 64-accelerator wafer-scale system.

We first evaluate the impact of the FlatAttention dataflow on end-to-end decoding performance.
Specifically, we adopt the EP32–PP2 parallelization scheme, as described in Section III-F.
By varying the number of batched user token requests per accelerator ( $b$ ), we plot the system-level throughput versus TPOT, as shown in Fig. 13(a), comparing FlatAttention against FlashMLA.

(a)

(a)

(b)

(b)

(c)

(c)

(d)

(d)

Figure 13: (a) DeepSeek-v3-671B decoding performance on 64-accelerator system comparing using FlatAttention and FlashMLA dataflow, as well as (b) runtime breakdown for decoding with 256 batch size per chip. Effect of expert parallelism: (c) Performance on the wafer-scale system; (d) D2D communication overhead at batch size of 256.

At low batch sizes, TPOT is minimized while overall system throughput is low.
In this regime, FlatAttention and FlashMLA exhibit comparable performance.
As the batch size per accelerator increases, FlatAttention achieves up to $2.1\times$ higher system throughput than FlashMLA, while simultaneously delivering lower TPOT.

Figures 13(b) presents runtime breakdowns of one decoding layer at a batch size of 256 tokens per accelerator (within the TPOT constraint of 50 ms), using FlatAttention and FlashMLA, respectively.
In both cases, attention computation dominates decoding runtime, accounting for 42% of total runtime with FlatAttention and 71% with FlashMLA.
FlashMLA exhibits low accelerator utilization, whereas FlatAttention increases utilization to 83%, resulting in a $4.5\times$ speedup for the attention component and translating to an end-to-end decoding speedup of $2.1\times$ .

We further study the impact of expert parallelism on decoding performance, as shown in Fig. 13(c).
Under fully pipelined parallelism, increasing batch size in the low-batch regime does not improve overall throughput and instead degrades TPOT, primarily because not all experts are activated.
Once the batch size exceeds 16, at which point all experts become active, overall throughput increases, albeit with higher TPOT.

As expert parallelism scales from none to full, both system throughput and TPOT improve for low to medium batch sizes.
However, at high batch sizes, chip-to-chip communication overhead becomes a dominant contributor to decoding latency.
Increasing the degree of expert parallelism further amplifies this overhead, as illustrated in Fig. 13(d), particularly in mesh-based accelerator interconnection topologies where multi-hop communication is unavoidable.
Future work should explore more optimized yet physically realizable interconnect topologies for wafer-scale systems.

Finally, we compare the DeepSeek-v3-671B decoding performance of wafer-scale multi-die systems with SoA serving systems built from commercial GPUs and NPUs.
Table II summarizes the evaluated systems, including the number of chips, inter-chip interconnect topology, and decoding performance measured in per-chip throughput.
The compared wafer-scale systems show similar per-chip capabilities, particularly to the NVIDIA H800 used in DS-Prof[7].
We report results under a decoding operating point that satisfies a 50 ms TPOT constraint.

Compared to the best-performing SoA system (DS-Prof), our wafer-scale multi-die system with 1 TB/s D2D link bandwidth achieves a 2.9 $\times$ improvement in per-chip throughput and a 1.4 $\times$ reduction in TPOT.
Even when the D2D link bandwidth is reduced to a level comparable to the NVLink bandwidth used in the DS-Prof system, our wafer-scale multi-die system continues to outperform DS-Prof.
Despite the larger diameter of the 2D mesh wafer-scale interconnect and the lack of kernel–communication overlap, our wafer-scale multi-die system achieves a $1.6\times$ decoding throughput speedup.
This improvement is primarily driven by the increased utilization of attention kernels enabled by the FlatAttention dataflow proposed in this work.

In terms of overall system performance, DeepSeek-v3-671B decoding with FlatAttention on wafer-scale multi-die systems continues to outperform DS-Prof, achieving up to a 1.9 $\times$ improvement despite operating with 1.5 $\times$ lower peak system performance.

TABLE II: DeepSeek-v3-671B Decoding Performnace Comaprision with SoA GPU/NPU Solutions

| System
| No.Chips
| Interconnect Meth
| Per Chip
| TPOT (ms)

| HBM
| TFLOPS
| Batch
| KV.Len
| Token/s

| CM384[45]

| 384 Acsend 910C
| Multi-Plane: UBLink 382GB/s, RDMA 400Gbps
| 3.2 TB/s
| 1504@INT8
| 128
| 4096
| 1943
| 49.4

| DS-Prof[7]

| 96 Nvidia H800
| Multi-Plane: NV-Link 160GB/s, RDMA 400Gbps
| 3.6 TB/s
| 1979@FP8
| 128
| 4096
| 2325
| 50.2

| Ours1
| 64 Tile Accel.
| Wafer-Scale: 8 $\times$ 8 Mesh, D2DLink 1TB/s
| 4.0 TB/s
| 1979@FP8
| 256
| 4096
| 6940
| 35.8

| Ours2
| 64 Tile Accel.
| Wafer-Scale: 8 $\times$ 8 Mesh, D2DLink 160GB/s
| 4.0 TB/s
| 1979@FP8
| 128
| 4096
| 3773
| 33.1

## VI Related Work

TABLE III: Related Work Comparison


Works


Layer

Fusion


Atten-

tion


Multiple

Tiles


Archi-

tecture


Coll-

ectives


HW Mcast

/ Redu.

| [16, 29, 35, 22]
| ✓
| ✓
| ✗
| -
| ✗
| ✗

| FlashAttention-2[6]

| ✓
| ✓
| ✓
| gpu
| ✗
| ✗

| FlashFuser [13]

| ✓
| ✗
| ✓
| gpu
| ✓
| ✗

| Zen-Attention [9]

| ✓
| ✓
| ✓
| mesh
| ✓
| ✗

| COMET [30]

| ✓
| ✓
| ✓
| noc
| ✓
| ✗

| ClusterFusion [26]

| ✓
| ✓
| ✓
| gpu
| ✓
| ✗

| WaferLLM [12]

✗*
| ✓
| ✓
| mesh
| ✓
✓/✗

| FlatAttention [Ours]
| ✓
| ✓
| ✓
| mesh
| ✓
| ✓

*

Wafer-scale assumption: layer fusion is unnecessary for (small) models that fit entirely in on-chip memory.

A large body of works have developed optimized Attention dataflows, aiming to eliminate the costly off-chip transfers that arise when Softmax normalization is implemented as a separate stage from the surrounding GEMMs [16, 29, 35, 22].
The general solution is to fuse all of the elementary operations in the Attention kernel to keep intermediate tensors on-chip.

While previous works focus on single-tile (or single-SM) accelerators, FlashAttention-2 [6] introduced a state-of-the-art fused Attention dataflow for GPUs, achieving high efficiency through optimized tiling and work partitioning across SMs.
However, as inter-SM communication on classic pre-Hopper GPUs occurs through global memory, hindering layer fusion and making on-chip SM-to-SM communication prohibitively expensive, FlashAttention-2 adopts an embarassingly-parallel dataflow that explicitly avoids such communication.

Only in their most recent GPU architecture, Hopper, Nvidia has introduced efficient SM-to-SM communication within GPU Processing Clusters (GPCs) [25], enabling efficient on-chip collective communication operations between SMs in a GPC.
Huang et al. [13] have recently demonstrated that leveraging on-chip collective communication operations on Hopper GPUs can expand the opportunities for layer fusion beyond a single SM.
While this work is a first attempt at employing on-chip collectives in LLM workloads on GPUs, it exclusively targets GEMM chains and FFN layers.

In contrast, a few very recent works have started to explore multi-tile Attention dataflows leveraging on-chip collective communication operations, on GPUs and other tile-based architectures [9, 30, 26].
Zen-Attention [9], develops a tiling framework for fully-fused Attention layers on tile-based AMD NPUs, leveraging on-chip spatial reductions.
Unfortunately, due to the scarcity of details on their fusion scheme and dataflow implementation, we are unable to directly compare their implementation to our work.
Similarly, COMET [30] proposes a representation and modeling framework to evaluate the cost of on-chip collective communication in NoC-based accelerators for the exploration of compound operator dataflows, with a particular focus on Attention.
While the COMET representation appears to be generic enough to cover the FlatAttention dataflow at a high-level, the work proposes and analyzes collectives as a schedule-level cost rather than as an architecture-dataflow co-design knob.
On the other hand, ClusterFusion [26] proposes an optimized fully-fused Attention dataflow leveraging collective communications on Hopper GPUs.
While their dataflow shares some similarities with ours, published earlier [42], it is not as flexible: “flattening” only along one axis.
Furthermore, architectural differences impose different tradeoffs and constraints.
For example, though they briefly investigate the tradeoffs involved in selecting an optimal “flattening scale” (in their case determined by the thread block cluster size), as their work mentions, the scope of this exploration is bounded by the size of a single GPC.

Moreover, no prior work evaluates the use of hardware-accelerated collectives, with the exception of WaferLLM [12], which leverages hardware multicast support in Cerebras’ WSE-2 accelerator’s NoC.
However, as WaferLLM targets wafer-scale accelerators, it assumes the target models to fit entirely in on-chip memory, removing the need to develop a fused-layer dataflow.
As a consequence, their dataflow is not suited if they do not fit entirely within on-chip memory, and may fall back to a suboptimal Attention implementation with larger models, even on wafer-scale accelerators.
Table III summarizes comparative analysis.
Note, none of the previous works comprehensively explore the challenge of co-designing an optimized tile-based accelerator for LLMs.

## VII Conclusion

In this work, we propose FlatAttention, a dataflow co-designed with fabric-level collective communication primitives supported by the tile-based many-PE accelerator to efficiently accelerate modern attention variants in LLMs.
including MHA, GQA, and MLA.
FlatAttention achieves an average 86% utilization for compute-bound attention kernels and 78% HBM bandwidth utilization for memory-bound kernels, resulting in an average 1.9 $\times$ speedup over optimized attention implementations on GH200.
We further evaluate end-to-end DeepSeek-v3 FP8 decoding on a wafer-scale multi-die system composed of 64 tile-based accelerators.
FlatAttention improves both system throughput and per-user latency,
achieving up to a 1.9 $\times$ improvement in system throughput and a 1.4 $\times$ reduction in TPOT, despite operating with 1.5 $\times$ lower peak system performance compared to the SoA solution deployed on 96 NVIDIA H800 GPUs.

## Acknowledgments

This work was supported by the ETH Future Computing Laboratory (EFCL), financed by a donation from Huawei Technologies.


Chi Zhang
received his B.Sc. degree from Huazhong University of Science and Technology China in 2019 and his M.Sc. degree from KTH Royal Institute of Technology Sweden in 2022. He is currently pursuing a Ph.D. degree in the Digital Circuits and Systems group of Prof. Benini. His research interests include high-performance computing, memory systems, and near-memory computing.


Luca Colagrande
received his BSc degree from Politecnico di Milano in 2018 and his MSc degree from ETH Zurich in 2020.
He is currently pursuing a PhD in the Digital Circuits and Systems group of Prof. Benini.
His research focuses on the co-design of energy-efficient general-purpose manycore accelerators for machine learning and high-performance computing applications.


Renzo Andri
received the B.Sc., M.Sc. and Ph.D. degree in Electrical Engineering and Information Technology at ETH Zurich in 2013, 2015, and 2020, respectively. He is a principal researcher at the Computing Systems Laboratory, Huawei Technologies, Switzerland. His research interests are in energy-efficient machine learning hardware architecture. In 2019, he won the IEEE TCAD Donald O. Pederson Award.


Luca Benini
holds the chair of Digital Circuits and Systems at ETH Zurich and is Full Professor at the Università di Bologna. He has served as Chief Architect for the Platform2012 in STMicroelectronics, Grenoble. Dr. Benini’s research interests are in energy-efficient parallel computing systems, multi-core SoC design, smart sensing micro-systems and machine learning hardware. He is a Fellow of the ACM and a member of the Academia Europaea.
