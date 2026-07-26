"""Extract a PDF into the markdown form the corpus indexes.

Page headers and footers repeat on most pages and would otherwise pollute every chunk,
so any line appearing on more than a third of pages is dropped. Section numbers in the
Arm style (A3.2.1 Handshake process) become markdown headings.

    python extract_pdf.py corpus/amba-axi4.pdf corpus/amba-axi4.md "AMBA AXI and ACE Protocol Specification" "ARM IHI 0022D"
"""

import re
import sys
from collections import Counter

from pypdf import PdfReader

SECTION = re.compile(r"^((?:Chapter\s+)?[A-Z]\d+(?:\.\d+)*)\s+(\S.*)$")

# Letter-spaced typography survives extraction as spaced capitals. Fixed by name rather
# than by pattern: runs like "SC SC I" are genuine ACE cache-state table cells.
LETTER_SPACED = {"VA L I D": "VALID"}


def main():
    src, dst, title, source = sys.argv[1:5]
    pages = [p.extract_text() or "" for p in PdfReader(src).pages]

    counts = Counter(line.strip() for page in pages for line in page.splitlines() if line.strip())
    boilerplate = {line for line, n in counts.items() if n > len(pages) / 3}

    out = [f"# {title}", "", f"Source: {source}", ""]
    for page in pages:
        for line in page.splitlines():
            line = line.strip()
            if not line or line in boilerplate or line.isdigit():
                continue
            match = SECTION.match(line)
            if match:
                depth = min(match.group(1).count(".") + 2, 6)
                out.append("")
                out.append(f"{'#' * depth} {match.group(1)} {match.group(2)}")
                out.append("")
            else:
                out.append(line)

    text = re.sub(r"\n{3,}", "\n\n", "\n".join(out))
    for spaced, fixed in LETTER_SPACED.items():
        text = text.replace(spaced, fixed)
    with open(dst, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"{len(pages)} pages, {len(boilerplate)} boilerplate lines dropped, "
          f"{len(text.split())} words -> {dst}")


if __name__ == "__main__":
    main()
