"""Discussion-only TP4 ring schedule, independent of simulation timing."""

from pathlib import Path
from ai_mapping_preview import Drawing

RING = (0, 1, 3, 2)
COORDS = {0: (0, 2), 1: (1, 2), 2: (0, 3), 3: (1, 3)}


def schedule():
    state = {r: {s: {r} for s in RING} for r in RING}
    steps = []
    for step in range(3):
        transfers = []
        for i, source in enumerate(RING):
            destination = RING[(i + 1) % 4]
            shard = RING[(i - step - 1) % 4]
            transfers.append((source, destination, shard, frozenset(state[source][shard])))
        updates = {}
        for source, destination, shard, contributors in transfers:
            state[destination][shard] |= contributors
            updates[destination] = (shard, frozenset(state[destination][shard]))
        steps.append((transfers, updates))
    return steps


def label(shard, contributors):
    return 'S' + str(shard) + '{' + ','.join(map(str, sorted(contributors))) + '}'


def render():
    d = Drawing(1800, 820, 'TP4 Reduce-Scatter: three communication steps')
    d.text(30, 42, 'TP4 Reduce-Scatter: three communication steps', 30)
    d.text(30, 72, 'DISCUSSION ONLY / Proposed schedule / Not measured / All-Gather and PP excluded', 18)
    # Small locator retains the full mesh coordinate convention.
    for y in range(4):
        for x in range(4):
            px, py = 35 + x * 26, 105 + (3-y)*22
            d.box(px, py, 22, 18, '#c9e0f2' if x < 2 and y >= 2 else '#fafafa')
            d.text(px + 5, py + 13, 'A' if x < 2 and y >= 2 else '', 11)
    d.text(165, 126, 'Active: group A in the upper-left 2x2 of the mesh', 18)
    d.text(165, 154, 'Initially every rank has its own contribution to S0, S1, S2 and S3.', 18)
    d.text(165, 182, 'S0{0,1} = shard 0 with contributions from ranks 0 and 1 already combined.', 18)
    d.text(30, 226, 'Blue arrows: payload sent during the step. Boxes: local state AFTER receiving and reducing.', 18)
    for step, (transfers, updates) in enumerate(schedule()):
        ox = step * 590
        d.text(ox + 40, 280, f'Step {step+1}', 25)
        points = {2: (ox+150, 365), 3: (ox+420, 365),
                  0: (ox+150, 595), 1: (ox+420, 595)}
        for source, destination, shard, contributors in transfers:
            x, y = points[source]
            tx, ty = points[destination]
            if y == ty:
                direction = 1 if tx > x else -1
                route = [(x+direction*82, y), (tx-direction*82, ty)]
                lx, ly = min(x,tx)+90, y-17
            else:
                direction = 1 if ty > y else -1
                route = [(x, y+direction*43), (tx, ty-direction*43)]
                lx, ly = x+12, (y+ty)/2
            attrs = f'data-step="{step+1}" data-source="{source}" data-destination="{destination}" data-shard="{shard}"'
            d.line(route, True, attrs)
            d.text(lx, ly, label(shard, contributors), 15, '#1769aa')
        for rank, (x,y) in points.items():
            shard, contributors = updates[rank]
            d.box(x-80, y-41, 160, 82, '#edf4fa', f'data-rank="{rank}"')
            cx,cy = COORDS[rank]
            d.text(x-70, y-17, f'rank {rank}  ({cx},{cy})', 17)
            d.text(x-70, y+7, 'Local storage update', 13)
            d.text(x-70, y+30, label(shard, contributors), 16)
        d.text(ox+40, 690, 'Final owner: one reduced shard per rank' if step == 2 else 'Receive + local reduction precedes forwarding.', 16)
    d.text(30, 740, 'Only changed storage entries are shown. Reduction occurs at endpoints, never inside routers.', 18)
    d.text(30, 770, 'Arrows use adjacent mesh links. Steps express dependencies, not cycles or synchronized handshakes.', 18)
    d.text(30, 800, 'Reference: Patarasuk and Yuan, 2007, Figure 3. Mesh placement is this project\'s proposed mapping.', 16)
    return d.finish()


if __name__ == '__main__':
    destination = Path(__file__).resolve().parents[2] / 'docs/figures/ai_mapping_review/tp_reduce_scatter.svg'
    destination.write_text(render(), encoding='utf-8')
