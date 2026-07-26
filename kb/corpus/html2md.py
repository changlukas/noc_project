"""LaTeXML/ar5iv HTML -> markdown-ish plain text. stdlib only."""
import re
import sys
from html.parser import HTMLParser

SKIP_TAGS = {"script", "style", "head", "nav", "footer"}
# class substrings whose whole subtree is dropped
SKIP_CLASSES = (
    "ltx_page_navbar", "ltx_page_footer", "ltx_bibliography", "ltx_pagination",
    "ar5iv-footer", "package-alerts", "extra-services",
    "modal", "header-button", "ltx_page_logo",
)
HEADING = {"h1": 1, "h2": 2, "h3": 3, "h4": 4, "h5": 5, "h6": 6}
BLOCK = {"p", "div", "li", "tr", "br", "figcaption", "blockquote", "table"} | set(HEADING)


class Extract(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.out = []
        self.skip_depth = 0          # >0 while inside a dropped subtree
        self.skip_tag = None
        self.heading = 0
        self.hbuf = []

    def _cls(self, attrs):
        return dict(attrs).get("class", "")

    def handle_starttag(self, tag, attrs):
        if self.skip_depth:
            if tag == self.skip_tag:
                self.skip_depth += 1
            return
        if tag in SKIP_TAGS or any(c in self._cls(attrs) for c in SKIP_CLASSES):
            self.skip_depth, self.skip_tag = 1, tag
            return
        if tag == "math":
            alt = dict(attrs).get("alttext")
            if alt:
                (self.hbuf if self.heading else self.out).append(" $" + alt.strip() + "$ ")
            self.skip_depth, self.skip_tag = 1, "math"
            return
        if tag == "img":
            return
        if tag in HEADING:
            self.heading = HEADING[tag]
            self.hbuf = []
        elif self.heading:
            return                     # no block breaks inside a heading
        elif tag in BLOCK:
            self.out.append("\n\n")
        elif tag in ("td", "th"):
            self.out.append(" | ")

    def handle_endtag(self, tag):
        if self.skip_depth:
            if tag == self.skip_tag:
                self.skip_depth -= 1
                if not self.skip_depth:
                    self.skip_tag = None
            return
        if tag in HEADING and self.heading:
            title = " ".join("".join(self.hbuf).split())
            if title:
                self.out.append("\n\n" + "#" * self.heading + " " + title + "\n\n")
            self.heading = 0
        elif self.heading:
            return
        elif tag in BLOCK:
            self.out.append("\n\n")

    def handle_data(self, data):
        if self.skip_depth:
            return
        (self.hbuf if self.heading else self.out).append(data)


def clean(text):
    text = text.replace(" ", " ")
    text = re.sub(r"Report issue for preceding element", "", text)
    # collapse spaces/tabs but keep newlines
    text = re.sub(r"[ \t]+", " ", text)
    text = re.sub(r" *\n *", "\n", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    # heading lines: strip trailing blank-line artefacts inside heading text
    lines = [ln.rstrip() for ln in text.split("\n")]
    text = "\n".join(lines).strip()
    # drop leftover table-cell pipes on otherwise empty lines
    text = re.sub(r"\n\|(\s*\|)*\n", "\n", text)
    return text + "\n"


def main(src, dst, title="", url=""):
    with open(src, "r", encoding="utf-8", errors="replace") as f:
        html = f.read()
    # LaTeXML puts a heading's number in a sibling span; joining happens naturally.
    p = Extract()
    p.feed(html)
    text = clean("".join(p.out))
    # drop the arXiv page banner that precedes the first real heading
    m = re.search(r"(?m)^#{1,6} ", text)
    if m:
        text = text[m.start():]
    if title:
        text = "# {}\n\nSource: {}\n\n{}".format(title, url, text)
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(dst, len(text.split()))


if __name__ == "__main__":
    main(*sys.argv[1:])
