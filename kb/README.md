# Knowledge Base

Retrieval over the primary sources the NoC specifications cite. Modelled on the ChipNeMo
RAG setup: 512-character passages, `e5-small-v2` retriever, top-8 retrieval, hit rate
measured against a question set with known answer locations.

## Layout

| Path | Contents |
|---|---|
| `corpus/` | Source documents. `*.pdf` archival, `*.txt` and `*.md` are what gets indexed |
| `corpus/manifest.yaml` | One entry per source: identity, URL, status, what it is authoritative for |
| `ingest.py` | Chunk the corpus and build `index.npz` |
| `retrieve.py` | Query the index |
| `eval.py` | Hit rate against `questions.json` |
| `questions.json` | Evaluation questions with golden source and phrase |

## Use

The venv lives outside the repo at `~/noc_kb/.venv` (see `~/noc_kb/README.md` for the
interpreter version). Run from this directory:

```bash
source ~/noc_kb/.venv/bin/activate
python ingest.py                                    # after adding or changing corpus files
python retrieve.py "how are multicast write responses aggregated"
python eval.py
```

## Question set

Each entry names where the answer lives, so a miss distinguishes "wrong document retrieved"
from "right document, wrong passage".

```json
[
  {
    "id": "b-merge-semantics",
    "question": "How are the write responses of a multicast write aggregated?",
    "golden_source": "floonoc-collectives.md",
    "golden_phrase": "CollectB"
  }
]
```

## Measured baseline

2026-07-26, 2059 chunks from 7 documents, `e5-small-v2`, k=8: **9 of 14, 64.3%**.

Four of the five misses retrieved the right document but not the passage holding the answer,
which is the failure ChipNeMo reports for queries whose answers are not localized in one
passage. The fifth missed on `stream_join_dynamic`, a rare identifier that dense embeddings
represent poorly.

## Scope

Retrieval only. No model fine-tuning, no continued pretraining: the corpus is a handful of
papers, three orders of magnitude below the scale at which those pay off. Fine-tuning the
retriever is the one training step that would fit the available GPU, and is warranted only
if the measured hit rate falls short.
