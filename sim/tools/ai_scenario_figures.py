"""Render proposed AI lifecycle diagrams independently of measurement data."""

import html
import pathlib


# Positions are presentation coordinates, not hardware attachment parameters.
GROUPS = {"A": (0, 2), "B": (2, 2), "C": (2, 0), "D": (0, 0)}
SCENARIOS = {
    "startup_loading": {
        "title": "Initialization and weight loading",
        "roles": ("Weight shards A", "Weight shards B", "Weight shards C", "Weight shards D"),
        "storage": "Memory / DMA: logical role, attachment TBD",
        "events": (
            ("Host control", "Configured endpoints", "1  Optional initialization", "Only if platform CSR access uses the NoC", "control"),
            ("Runtime", "Residency metadata", "2  Check model residency", "Identify missing tensors before issuing copies", "local"),
            ("Memory / DMA", "Groups A, B, C, D", "3  Load missing weight shards", "Destination follows tensor ownership", "data"),
            ("Group endpoints", "Runtime", "4  Report loading completion", "Required copies complete before compute starts", "control"),
        ),
        "notes": ("Distinct shards: scatter. Replicated identical tensors: optional multicast.",
                  "Boot sequence is UNVERIFIED. Model-switch loading depends on residency."),
    },
    "dense_execution": {
        "title": "Prefill and decode on a layer pipeline",
        "roles": ("PP stage A / TP", "PP stage B / TP", "PP stage C / TP", "PP stage D / TP"),
        "storage": "Layer-owned weights and KV: tier / attachment TBD",
        "events": (
            ("Weight / KV storage", "Owning group", "1  Access operands", "Prefill creates KV. Decode reads history and appends KV", "data"),
            ("TP ranks in group", "TP ranks in group", "2  Exchange partial results", "Collective depends on tensor layout", "data"),
            ("Completed stage", "Next PP stage", "3  Transfer activation: A to B to C to D", "Transfer after compute. Redistribute if shard layouts differ", "data"),
            ("Stage D", "Output endpoint", "4  Deliver result", "Decode repeats the layer pipeline for subsequent steps", "data"),
        ),
        "notes": ("TP groups are local. KV belongs to its layer and request.",
                  "Memory traffic and collectives repeat within layers. Timings are TBD."),
    },

}


def render_scenario(name):
    case = SCENARIOS[name]
    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="880" viewBox="0 0 1280 880" role="img">',
        f'<title>{html.escape(case["title"])}</title>',
        '<desc>Proposed scenario. Left: group placement with bottom-left origin. Right: logical data movement and dependencies, not physical routes or measured timing.</desc>',
        '<rect width="1280" height="880" fill="white"/>',
        '<style>text{font-family:Arial,Helvetica,sans-serif;fill:#243747}.title{font-size:28px;font-weight:600}.label{font-size:20px;font-weight:600}.body{font-size:17px}.small{font-size:15px}.tile{fill:white;stroke:#bac8d4}.link{stroke:#cbd5df;stroke-width:3}</style>',
        '<defs><marker id="data" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto"><path d="M0 0 L8 4 L0 8Z" fill="#226a9b"/></marker><marker id="control" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto"><path d="M0 0 L8 4 L0 8Z" fill="#a36518"/></marker><marker id="local" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto"><path d="M0 0 L8 4 L0 8Z" fill="#687480"/></marker></defs>',
        f'<text class="title" x="30" y="44">{html.escape(case["title"])}</text>',
        '<text class="body" x="30" y="76">PROPOSED SCENARIO / Not measured / Arrows show logical roles, not router paths</text>',
        '<text class="label" x="30" y="126">Compute placement</text>',
        '<text class="label" x="580" y="126">Events and dependencies</text>',
    ]

    def text(x, y, content, cls="body", anchor="start"):
        parts.append(f'<text class="{cls}" x="{x}" y="{y}" text-anchor="{anchor}">{html.escape(content)}</text>')

    for (group, (gx, gy)), role in zip(GROUPS.items(), case["roles"]):
        x, y = 85 + gx * 105, 190 + (2 - gy) * 105
        parts.append(f'<g data-group="{group}" data-origin="{gx},{gy}">')
        parts.append(f'<rect x="{x}" y="{y}" width="195" height="195" rx="8" fill="#edf3f8" stroke="#99aebd"/>')
        text(x + 12, y + 27, "Group " + group, "label")
        text(x + 12, y + 51, role, "small")
        for dx in range(2):
            for dy in range(2):
                tx, ty = x + 15 + dx * 86, y + 75 + (1 - dy) * 53
                parts.append(f'<rect class="tile" x="{tx}" y="{ty}" width="78" height="43" rx="4" data-coordinate="{gx+dx},{gy+dy}"/>')
                text(tx + 39, ty + 27, f"({gx+dx},{gy+dy})", "small", "middle")
        parts.append('</g>')
    for coordinate in range(4):
        text((139, 225, 349, 435)[coordinate], 633, f"x={coordinate}", "small", "middle")
        text(72, (554, 501, 344, 291)[coordinate], f"y={coordinate}", "small", "end")
    text(85, 671, "(0,0) bottom left. x right, y up.", "body")
    # Stores are logical roles outside the coordinate map, not additional ports.
    parts.append('<rect x="30" y="697" width="500" height="58" rx="6" fill="#fff8eb" stroke="#b38942" stroke-dasharray="5 4"/>')
    text(45, 731, case["storage"], "small")
    for index, (src, dst, payload, condition, kind) in enumerate(case["events"]):
        y = 174 + index * 110
        color = {"data": "#226a9b", "control": "#a36518", "local": "#687480"}[kind]
        text(580, y, payload, "label")
        for x, label in ((580, src), (1000, dst)):
            parts.append(f'<rect x="{x}" y="{y+15}" width="235" height="36" rx="5" fill="#f7f9fb" stroke="#bcc8d1"/>')
            text(x + 117.5, y + 39, label, "body", "middle")
        dash = ' stroke-dasharray="6 4"' if kind != "data" else ""
        parts.append(f'<line data-kind="{kind}" data-source="{html.escape(src)}" data-destination="{html.escape(dst)}" x1="825" y1="{y+33}" x2="990" y2="{y+33}" stroke="{color}" stroke-width="2.5" marker-end="url(#{kind})"{dash}/>')
        text(580, y + 78, condition, "small")
    text(580, 755, "Blue: payload   Amber: control   Gray: local/runtime action", "small")
    for index, note in enumerate(case["notes"]):
        text(30, 799 + index * 27, note, "body")
    parts.append('</svg>')
    return "\n".join(parts) + "\n"


def write_scenario_figures(destination):
    directory = pathlib.Path(destination) / "ai_scenarios"
    directory.mkdir(parents=True, exist_ok=True)
    paths = []
    for name in SCENARIOS:
        path = directory / f"{name}.svg"
        path.write_text(render_scenario(name), encoding="utf-8", newline="\n")
        paths.append(path)
    return paths
