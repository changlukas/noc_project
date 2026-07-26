"""Retrieve the passages most similar to a query.

    python retrieve.py "why does WideAw ride the wide network" [-k 8] [--json]
"""

import argparse
import functools
import json

import numpy as np
from sentence_transformers import SentenceTransformer

MODEL = "intfloat/e5-small-v2"


@functools.cache
def encoder():
    return SentenceTransformer(MODEL)


def load(path):
    data = np.load(path, allow_pickle=True)
    return data["vectors"], json.loads(str(data["meta"]))


def search(query, vectors, meta, k):
    q = encoder().encode([f"query: {query}"], normalize_embeddings=True)[0]
    # ponytail: brute-force cosine over every chunk. Thousands of chunks scan in
    # milliseconds; reach for an index only if the corpus grows by two orders of magnitude.
    scores = vectors @ q
    order = np.argsort(-scores)[:k]
    return [dict(meta[i], score=float(scores[i])) for i in order]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("query")
    parser.add_argument("-k", type=int, default=8)
    parser.add_argument("--index", default="index.npz")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    vectors, meta = load(args.index)
    hits = search(args.query, vectors, meta, args.k)

    if args.json:
        print(json.dumps(hits, ensure_ascii=False, indent=2))
        return
    for rank, hit in enumerate(hits, 1):
        where = f"{hit['source']}#{hit['index']}"
        heading = f" | {hit['heading']}" if hit["heading"] else ""
        print(f"\n[{rank}] {hit['score']:.3f}  {where}{heading}\n{hit['text']}")


if __name__ == "__main__":
    main()
