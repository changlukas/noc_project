"""Approved illustrative lifecycle routes. No simulation timing or workload changes."""

import argparse
from dataclasses import dataclass
from pathlib import Path
from ai_mapping_preview import Drawing, GROUPS, MEMORIES, xy_path, mesh as approved_mesh


@dataclass(frozen=True)
class Transfer:
    name: str
    source: tuple
    destination: tuple
    phase: str
    memory: int = -1

    @property
    def path(self):
        return xy_path(self.source, self.destination)


def rank_coord(group, rank):
    x, y = GROUPS[group]
    return x + rank % 2, y + rank // 2


def moe_transfers():
    result = []
    for rank in (0, 1):
        for expert in ('B', 'C'):
            src, dst = rank_coord('A', rank), rank_coord(expert, 0)
            name = f'T{rank}{expert}'
            result.extend((Transfer(name, src, dst, 'dispatch'),
                           Transfer(name, dst, src, 'return')))
    return result


def kv_transfers():
    result = []
    for m, group in enumerate(GROUPS):
        for rank in range(4):
            tile, memory = rank_coord(group, rank), MEMORIES[m][:2]
            name = f'K{group}{rank}'
            result.extend((Transfer(name, memory, tile, 'restore', m),
                           Transfer(name, tile, memory, 'offload', m)))
    return result


def handoff_transfers():
    return [Transfer(f'K{group}{rank}', rank_coord(group, rank), rank_coord(dst, rank), 'handoff')
            for group, dst in (('A', 'D'), ('B', 'C')) for rank in range(4)]


def canvas(width, height, title):
    d = Drawing(width, height, title)
    d.parts.append('<defs><marker id="orange" markerWidth="5" markerHeight="5" refX="4" refY="2.5" orient="auto"><path d="M0 0L5 2.5L0 5Z" fill="#b75a12"/></marker></defs>')
    d.text(30, 40, title, 28)
    return d


def route(d, point, transfer, offset):
    sx, sy = point(*transfer.source)
    dx, dy = point(*transfer.destination)
    points = [(sx+40, sy+22), (sx+40, sy+offset)]
    points += [(point(x,y)[0]+offset, point(x,y)[1]+offset) for x,y in transfer.path]
    if transfer.memory >= 0:
        x,y,port=MEMORIES[transfer.memory]
        px,py=point(x,y)
        points.append((px+offset, py-58 if port=='N' else py+82))
    else:
        points += [(dx+40,dy+offset),(dx+40,dy+22)]
    attrs = (f'data-transfer="{transfer.name}" data-phase="{transfer.phase}" '
             f'data-source="{transfer.source[0]},{transfer.source[1]}" '
             f'data-destination="{transfer.destination[0]},{transfer.destination[1]}" '
             f'data-path="'+' '.join(f'{x},{y}' for x,y in transfer.path)+'"')
    d.line(points, True, attrs)


def moe():
    d=canvas(1320,1020,'MoE：token dispatch 與 expert return')
    d.text(30,74,'A0、A1 為 token owners，B0、C0 為 expert owners。T 標籤識別送往同一 expert 的 token subset。',17)
    for index,phase in enumerate(('dispatch','return')):
        ox,oy=70+index*650,205
        d.text(ox-30,111,'Dispatch：送至 expert' if index==0 else 'Return：結果回到 token owner',22)
        point=approved_mesh(d,ox,oy,120)
        transfers=[t for t in moe_transfers() if t.phase==phase]
        for i,t in enumerate(transfers):
            route(d,point,t,(i-1.5)*5)
        targets={}
        for t in transfers:
            targets.setdefault(t.destination,[]).append(t.name)
        for coord,names in targets.items():
            px,py=point(*coord)
            d.text(px+15,py+77,' / '.join(names),12,'#1769aa')
        for i,t in enumerate(transfers):
            d.text(ox-30,775+i*28,f'{t.name}: ({t.source[0]},{t.source[1]}) → ({t.destination[0]},{t.destination[1]})',16)
    d.text(30,913,'Dispatch 的四筆傳送共用 (1,2)→(2,2)。Return 依 XY 另算路徑，C0 的結果先向西、再向北。',17)
    d.text(30,945,'偏移線共用實體 links。Token subsets 為示例，不限定 top-k。未畫 request/completion，非量測結果。',16)
    d.text(30,979,'依據：expert exchange §5.1，PMLR 162 (2022)。Mesh 配置為本專案選擇。',16)
    return d.finish()


def kv():
    d=canvas(1320,1620,'KV offload：各 tile 將指定 blocks 傳回 backing memory')
    d.text(30,70,'Restore 沿用已確認的 weight-loading 圖。本圖只補 tile→memory 的 offload 路徑。',17)
    for m,group in enumerate(GROUPS):
        ox,oy=70+(m%2)*650,205+(m//2)*710
        d.text(ox-30,oy-94,f'Group {group} to M{m}: K{group}0, K{group}1, K{group}2, K{group}3',20)
        point=approved_mesh(d,ox,oy,120,group)
        for t in kv_transfers():
            if t.memory!=m or t.phase!='offload': continue
            rank=int(t.name[-1])
            route(d,point,t,(rank-1.5)*5)
            px,py=point(*t.source)
            d.text(px+47,py+15,t.name,12,'#1769aa')
        d.text(ox-25,oy+535,f'K{group}0 至 K{group}3 分別來自 group {group} 的 ranks 0 至 3。',14)
    d.text(30,1540,'四個 panels 為同一配置的分開展示。只搬指定 blocks，free 不自動產生 offload。',17)
    d.text(30,1570,'依據：arXiv:2303.06865。偏移線共用 links，未畫 request/completion。容量與 timing [TBD]，非量測結果。',16)
    return d.finish()


def handoff():
    d=canvas(1320,990,'KV handoff：對應 layers 的 prefill→decode 傳送')
    d.text(30,74,'A/D、B/C 分別負責相同 layer sets，兩側 ranks 使用相同 KV shard layout。',18)
    for index,(group,dst) in enumerate((('A','D'),('B','C'))):
        ox,oy=70+index*650,205
        d.text(ox-30,111,f'Prefill {group} → Decode {dst}: matching ranks 0 至 3',21)
        point=approved_mesh(d,ox,oy,120,(group,dst))
        for t in handoff_transfers():
            if t.name[1]!=group: continue
            rank=int(t.name[-1])
            route(d,point,t,(rank-1.5)*5)
            for coord in (t.source,t.destination):
                px,py=point(*coord)
                d.text(px+47,py+15,t.name,12,'#1769aa')
        d.text(ox-30,785,f'{group}0→{dst}0、{group}1→{dst}1、{group}2→{dst}2、{group}3→{dst}3',18)
    d.text(30,848,'每個 column 的兩份 KV 共用 y=2→y=1 的 link。兩側各需對應 layers 的 weights 副本。',18)
    d.text(30,885,'藍底為本 panel 的傳送兩端，灰底 groups 未參與。偏移線共用 links，容量與 timing [TBD]。',16)
    d.text(30,923,'依據：disaggregated serving §4.2，OSDI 2024。Mesh 配對為本專案選擇，非量測結果。',16)
    return d.finish()


RENDERERS={'expert_routes':moe,'kv_copy_routes':kv,'handoff_routes':handoff}


def write_figures(destination):
    directory=Path(destination)
    directory.mkdir(parents=True,exist_ok=True)
    for name,render in RENDERERS.items():
        (directory/f'{name}.svg').write_text(render(),encoding='utf-8')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination',type=Path)
    write_figures(parser.parse_args().destination)
