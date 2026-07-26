"""Chunk the corpus and build the embedding index.

Chunk size and retrieval depth follow ChipNeMo (512-character passages, top-8),
which is the reference this knowledge base is modelled on.

    python ingest.py [--corpus corpus] [--out index.npz]
"""

import argparse
import json
import re
from pathlib import Path

import numpy as np
from sentence_transformers import SentenceTransformer

MODEL = "intfloat/e5-small-v2"
CHUNK_CHARS = 512
OVERLAP_CHARS = 64
HEADING = re.compile(r"^#{1,6}\s+(.*)")


def split_paragraphs(text):
    """Yield (paragraph, heading) with heading being the nearest preceding one."""
    heading = ""
    for block in re.split(r"\n\s*\n", text):
        block = block.strip()
        if not block:
            continue
        match = HEADING.match(block)
        if match:
            heading = match.group(1).strip()
            continue
        yield block, heading


def chunk(text):
    """Pack paragraphs up to CHUNK_CHARS, splitting any paragraph that alone exceeds it."""
    chunks = []
    buf, buf_heading = "", ""
    for para, heading in split_paragraphs(text):
        if heading != buf_heading and buf:
            chunks.append((buf, buf_heading))
            buf = ""
        buf_heading = heading
        while len(para) > CHUNK_CHARS:
            cut = para.rfind(" ", 0, CHUNK_CHARS)
            cut = cut if cut > CHUNK_CHARS // 2 else CHUNK_CHARS
            if buf:
                chunks.append((buf, buf_heading))
                buf = ""
            chunks.append((para[:cut], buf_heading))
            para = para[max(0, cut - OVERLAP_CHARS):].lstrip()
        if len(buf) + len(para) + 1 > CHUNK_CHARS:
            chunks.append((buf, buf_heading))
            buf = para
        else:
            buf = f"{buf}\n{para}" if buf else para
    if buf:
        chunks.append((buf, buf_heading))
    return chunks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", default="corpus")
    parser.add_argument("--out", default="index.npz")
    args = parser.parse_args()

    corpus = Path(args.corpus)
    files = sorted(list(corpus.glob("*.txt")) + list(corpus.glob("*.md")))
    if not files:
        raise SystemExit(f"no .txt or .md files under {corpus.resolve()}")

    records = []
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for i, (body, heading) in enumerate(chunk(text)):
            records.append({"source": path.name, "index": i, "heading": heading, "text": body})
        print(f"{path.name}: {sum(r['source'] == path.name for r in records)} chunks")

    model = SentenceTransformer(MODEL)
    vectors = model.encode(
        [f"passage: {r['text']}" for r in records],
        batch_size=64,
        normalize_embeddings=True,
        show_progress_bar=True,
    )

    np.savez_compressed(
        args.out,
        vectors=vectors.astype(np.float32),
        meta=np.array(json.dumps(records), dtype=object),
    )
    print(f"\n{len(records)} chunks from {len(files)} files -> {args.out}")


if __name__ == "__main__":
    main()
