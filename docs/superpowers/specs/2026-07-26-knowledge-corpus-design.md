# Knowledge Corpus Design

2026-07-26. Design for the project's retrieval-backed knowledge base.

## Goal

Every factual claim in `noc-target-spec.md` and its successors is traceable to a primary source
that an agent can retrieve. Success is measured, not asserted: a fixed question set with known
answer locations, and a hit rate against it.

## Method

Follows the ChipNeMo sequence: collect corpus, build the evaluation set with golden passages,
measure the baseline, then invest only where the measurement shows a gap. The paper fine-tuned a
retriever because it had measured an insufficient hit rate first, and its RAG served 1.8K documents
in 67K chunks. This corpus is two orders of magnitude smaller, so the form of retrieval is left to
Stage 4 rather than chosen up front.

One finding drives the design: retrieval succeeds when the answer sits inside a single passage and
fails when it must be assembled across several. Distilling a source into a skill is exactly the act
of localizing answers, so distillation serves both candidate retrieval forms.

## Stages

### Stage 1, corpus

Goal: every source cited by the spec or by `prior-art-and-claim-verification.md` is held locally
with a stable identifier.

| Source | Role | State |
|---|---|---|
| arXiv 2603.26438, collective FlooNoC | nearest prior art, positioning depends on it | to collect |
| arXiv 2502.19215, multicast AXI crossbar | AXI-level multicast and B aggregation precedent | to collect |
| arXiv 2505.18824 + 2604.02110, FlatAttention | workload definition, benchmark parameters | to collect |
| arXiv 2409.17606 + repo docs, FlooNoC | architecture baseline | to collect |
| AMBA AXI4, IHI 0022 | protocol authority for §6 and §8.1 | to collect |
| Boppana and Chalasani 1998 | wormhole multicast deadlock hazards | to collect |
| On-Chip Networks, 2nd ed. | fundamentals | held, skill |
| ChipNeMo 2311.00176 | method reference | held, skill |

Success: corpus manifest lists every entry with its local path or URL and the spec sections it
backs.

### Stage 2, evaluation set

Goal: 20 to 30 questions drawn from real spec-verification needs, each with a golden passage.
Questions come from what actually had to be checked, for example why `WideAw` rides `wide` rather
than `req`, what restriction the nearest prior art places on concurrent reductions, what AXI4
requires of a write response when a burst is answered by several targets.

Requires the user: golden passage selection is a judgement call, and ChipNeMo had designers write
their benchmark questions for this reason. Estimated one hour.

Success: question set with, per question, the answer and the source and location that holds it.

### Stage 3, baseline measurement

Goal: run the question set against retrieval as it stands today, which is the existing skills plus
reading and grepping source files, and record hit and miss per question.

Success: a hit rate and a list of which questions missed and why.

### Stage 4, targeted investment

Goal: close the measured gaps in cost order.

1. Sources that missed because their answers are scattered get distilled into a skill.
2. Sources that missed because nothing was retrievable at all get added to the corpus.
3. Embedding-based retrieval is built only if the hit rate remains short after the first two.

Success: re-measured hit rate on the same question set, with the delta attributed to the specific
change.

## Scope

Not included: fine-tuning any model, tokenizer work, or continued pretraining. The corpus is small
enough that these have no place. Not included: automatic corpus ingestion, since the source list is
short and hand-maintained.
