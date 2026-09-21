"""Reproducible review figures for approved memory placements.

Presentation only. Does not configure memory capacity, service or RTL ports.
"""

import argparse
from html import escape
from pathlib import Path


GROUPS = {"A": (0, 2), "B": (2, 2), "C": (2, 0), "D": (0, 0)}
# Stable address-owner order for the first placement comparison.
# Ascending x within the bottom edge, then the top edge.
MEMORIES = ((1, 0, "S"), (2, 0, "S"), (1, 3, "N"), (2, 3, "N"))
PLACEMENTS = {
    'dual_edge': MEMORIES,
    'single_edge': ((0, 0, 'S'), (1, 0, 'S'), (2, 0, 'S'), (3, 0, 'S')),
    'four_corners': ((0, 0, 'S'), (3, 0, 'S'), (0, 3, 'N'), (3, 3, 'N')),
}
TITLES = {'dual_edge': 'Dual-edge', 'single_edge': 'Single-edge', 'four_corners': 'Four-corner'}


def xy_path(source, destination):
    x, y = source
    dx, dy = destination
    path = [(x, y)]
    while x != dx:
        x += 1 if dx > x else -1
        path.append((x, y))
    while y != dy:
        y += 1 if dy > y else -1
        path.append((x, y))
    return path


def flows(placement='dual_edge'):
    result = []
    for m, (group, (gx, gy)) in enumerate(GROUPS.items()):
        for rank in range(4):
            destination = (gx + rank % 2, gy + rank // 2)
            result.append((m, group, rank, destination,
                           xy_path(PLACEMENTS[placement][m][:2], destination)))
    return result


class Drawing:
    def __init__(self, width, height, title):
        self.parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img">',
                      f'<title>{escape(title)}</title>',
                      '<desc>Approved logical placement, proposed illustration. Not a measured trace or a floorplan.</desc>',
                      '<defs><marker id="arrow" markerWidth="5" markerHeight="5" refX="4" refY="2.5" orient="auto"><path d="M0 0L5 2.5L0 5Z" fill="#1769aa"/></marker></defs>',
                      f'<rect width="{width}" height="{height}" fill="white"/>']

    def text(self, x, y, text, size=16, color="#243747"):
        self.parts.append(f'<text x="{x}" y="{y}" font-family="Arial, sans-serif" font-size="{size}" fill="{color}">{escape(text)}</text>')

    def box(self, x, y, w, h, fill="#f4f7fa", attrs=""):
        self.parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{fill}" stroke="#b2bec9" {attrs}/>')

    def line(self, points, active=False, attrs=""):
        self.parts.append('<polyline points="' + ' '.join(f'{x},{y}' for x, y in points)
                          + f'" fill="none" stroke="{"#1769aa" if active else "#bdc7cf"}" stroke-width="{2 if active else 1.5}" '
                          + ('marker-end="url(#arrow)" ' if active else '') + attrs + '/>')

    def finish(self):
        return '\n'.join(self.parts + ['</svg>']) + '\n'


def mesh(d, ox, oy, pitch=125, active_group=None, placement='dual_edge'):
    def point(x, y):
        return ox + x * pitch, oy + (3 - y) * pitch

    for group, (gx, gy) in GROUPS.items():
        px, py = point(gx, gy + 1)
        d.box(px - 20, py - 25, pitch + 117, pitch + 91,
              '#edf4fa' if (active_group is None or active_group == group
                            or isinstance(active_group, tuple) and group in active_group)
              else '#fafafa',
              f'data-group-background="{group}"' if isinstance(active_group, tuple) else '')
        d.text(px + 15, py - 8, group, 13)
    for y in range(4):
        for x in range(4):
            px, py = point(x, y)
            if x < 3:
                d.line([(px, py), point(x + 1, y)])
            if y < 3:
                d.line([(px, py), point(x, y + 1)])
    for y in range(4):
        for x in range(4):
            px, py = point(x, y)
            d.box(px - 9, py - 7, 18, 14, 'white', f'data-router="{x},{y}"')
            d.text(px - 5, py + 4, 'R', 10)
            d.line([(px + 9, py), (px + 40, py), (px + 40, py + 22)])
            d.box(px + 14, py + 22, 79, 39, 'white', f'data-storage="{x},{y}"')
            d.text(px + 18, py + 36, f'Compute ({x},{y})', 10)
            d.text(px + 18, py + 52, 'Local storage', 10)
    for m, (x, y, port) in enumerate(PLACEMENTS[placement]):
        px, py = point(x, y)
        my = py - 72 if port == 'N' else py + 96
        d.line([(px, py), (px, my)])
        d.box(px - 30, my - 14, 92, 28, '#fff6e6', f'data-memory="{m}" data-port="{port}"')
        d.text(px - 25, my + 5, f'M{m} / {"ABCD"[m]}', 14)
    d.text(ox - 26, oy + 3 * pitch + 145, '(0,0) bottom left   x right   y up', 13)
    return point


def architecture(placement='dual_edge'):
    title = TITLES[placement] + ' memory attachment'
    d = Drawing(1160, 860, title)
    d.text(32, 42, title, 28)
    d.text(32, 72, 'Approved placement / Logical connectivity / Not measured', 16)
    mesh(d, 85, 190, placement=placement)
    d.text(680, 145, 'One tile, expanded', 22)
    for y, label in ((175, 'Router: N / E / S / W / Local'),
                     (250, 'NI: NMU + NSU'), (325, 'Tile-local interconnect')):
        d.box(680, y, 395, 45)
        d.text(695, y + 28, label, 17)
    d.line([(870, 220), (870, 250)])
    d.line([(870, 295), (870, 325)])
    d.box(680, 420, 155, 50)
    d.text(700, 451, 'Compute', 18)
    d.box(875, 420, 200, 50)
    d.text(891, 451, 'Local storage', 18)
    d.line([(755, 420), (755, 395), (975, 395), (975, 420)])
    d.line([(870, 370), (870, 395)])
    for y, text in enumerate((
        'Local storage: capacity and organization [TBD]',
        'Gray lines show connections, not active traffic.',
        '16 compute tiles retain their Local ports.',
        'M0 / M1 / M2 / M3 use South.' if placement == 'single_edge' else 'M0 / M1 use South. M2 / M3 use North.',
        'Each memory endpoint serves one group.',
        'Memory service bandwidth and capacity [TBD]',
    )):
        d.text(680, 525 + y * 30, text, 15)
    d.text(32, 790, 'M0 owns A backing shards. M1 owns B. M2 owns C. M3 owns D.', 17)
    d.text(32, 820, 'Local access stays inside the tile. Box sizes do not represent silicon area.', 16)
    return d.finish()


def loading(placement='dual_edge'):
    title = 'Weight loading: complete payload routes'
    if placement != 'dual_edge':
        title = TITLES[placement] + ' weight loading: complete payload routes'
    d = Drawing(1320, 1620, title)
    d.text(30, 40, title, 28)
    d.text(30, 70, 'Four views of one placement / 16 distinct shard transfers / Not a time schedule', 17)
    for m, group in enumerate(GROUPS):
        ox, oy = 70 + (m % 2) * 650, 205 + (m // 2) * 710
        d.text(ox - 30, oy - 94, f'M{m} to group {group}: W{group}0, W{group}1, W{group}2, W{group}3', 20)
        point = mesh(d, ox, oy, 120, group, placement)
        for fm, fg, rank, destination, path in flows(placement):
            if fm != m:
                continue
            offset = (rank - 1.5) * 5
            sx, sy = point(*path[0])
            my = sy - 58 if PLACEMENTS[placement][m][2] == 'N' else sy + 82
            points = [(sx + offset, my)] + [(point(x, y)[0] + offset, point(x, y)[1] + offset) for x, y in path]
            dx, dy = point(*destination)
            points += [(dx + 40, dy + offset), (dx + 40, dy + 22)]
            coords = ' '.join(f'{x},{y}' for x, y in path)
            d.line(points, True, f'data-shard="W{group}{rank}" data-path="{coords}" data-memory="{m}"')
            d.text(dx + 47, dy + 15, f'W{group}{rank}', 12, '#1769aa')
        d.text(ox - 25, oy + 535, 'Blue: weight payload. Offset lines share the same physical links.', 13)
    d.text(30, 1540, 'All payload destinations are shown. Requests, completion messages and local reads are omitted.', 17)
    d.text(30, 1570, 'Each arrow is a distinct unicast shard. Sizes, packetization and overlap remain [TBD].', 17)
    return d.finish()


def write_figures(destination, placements=('dual_edge',)):
    directory = Path(destination)
    directory.mkdir(parents=True, exist_ok=True)
    for placement in placements:
        for kind, render in (("architecture", architecture), ("loading", loading)):
            (directory / f'{placement}_{kind}.svg').write_text(render(placement), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', nargs='?', type=Path,
                        default=Path(__file__).resolve().parents[2] / 'docs' / 'figures' / 'ai_mapping_review')
    parser.add_argument('--placements', nargs='+', choices=PLACEMENTS, default=['dual_edge'])
    args = parser.parse_args()
    write_figures(args.destination, args.placements)
