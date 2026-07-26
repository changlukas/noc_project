"""Measure retrieval hit rate against a question set with known answer locations.

A question hits when a passage from its golden source appears in the top-k and that
passage contains the golden phrase. Both conditions are needed: the right document
alone does not prove the answer was retrievable.

    python eval.py [--questions questions.json] [-k 8]
"""

import argparse
import json

from retrieve import load, search


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--questions", default="questions.json")
    parser.add_argument("--index", default="index.npz")
    parser.add_argument("-k", type=int, default=8)
    args = parser.parse_args()

    questions = json.loads(open(args.questions, encoding="utf-8").read())
    vectors, meta = load(args.index)

    hits, misses = 0, []
    for q in questions:
        results = search(q["question"], vectors, meta, args.k)
        phrase = q["golden_phrase"].lower()
        rank = next(
            (
                i
                for i, r in enumerate(results, 1)
                if r["source"] == q["golden_source"] and phrase in r["text"].lower()
            ),
            None,
        )
        if rank:
            hits += 1
            print(f"HIT  @{rank}  {q['id']}")
        else:
            source_only = any(r["source"] == q["golden_source"] for r in results)
            reason = "right source, wrong passage" if source_only else "source not retrieved"
            misses.append((q["id"], reason))
            print(f"MISS       {q['id']}  ({reason})")

    total = len(questions)
    print(f"\nhit rate {hits}/{total} = {hits / total:.1%} at k={args.k}")
    for qid, reason in misses:
        print(f"  miss: {qid} - {reason}")


if __name__ == "__main__":
    main()
