"""Approved illustrative TP4/SP ownership, not a simulator workload."""

from pathlib import Path
from ai_mapping_preview import Drawing, xy_path
from tp_reduce_scatter_preview import RING, COORDS


def gather_schedule():
    state = {r: {r} for r in RING}
    steps = []
    for step in range(len(RING) - 1):
        transfers = [(r, RING[(i+1) % len(RING)], RING[(i-step) % len(RING)])
                     for i, r in enumerate(RING)]
        for source, destination, shard in transfers:
            assert shard in state[source]
        for source, destination, shard in transfers:
            state[destination].add(shard)
        steps.append((transfers, {r: frozenset(s) for r, s in state.items()}))
    return steps


def pp_routes():
    return {r: xy_path(COORDS[r], (COORDS[r][0]+2, COORDS[r][1])) for r in RING}


def collective(d, oy):
    d.text(30, oy, 'Group 內通訊：All-Gather 與 Reduce-Scatter 共用 ring 路徑', 25)
    d.text(30, oy+28, '每步沿藍色箭頭傳送一份 shard 大小的資料。兩者使用相同 links，接收後的處理不同。', 19)
    for step, (transfers, _) in enumerate(gather_schedule()):
        ox = step*590
        d.text(ox+35, oy+65, f'Step {step+1}', 21)
        points = {2:(ox+145,oy+130),3:(ox+425,oy+130),0:(ox+145,oy+340),1:(ox+425,oy+340)}
        for transfer in transfers:
            source, destination, shard = transfer[:3]
            x,y = points[source]
            tx,ty = points[destination]
            if y == ty:
                direction = 1 if tx>x else -1
                route = [(x+direction*86,y),(tx-direction*86,ty)]
                lx,ly = min(x,tx)+91,y-12
            else:
                direction = 1 if ty>y else -1
                route = [(x,y+direction*40),(tx,ty-direction*40)]
                lx,ly=x+10,(y+ty)/2
            d.line(route, True, f'data-collective="shared-ring" data-step="{step+1}" data-source="{source}" data-destination="{destination}"')
            d.text(lx,ly,'1 shard',14,'#1769aa')
        for rank,(x,y) in points.items():
            d.box(x-85,y-39,170,78,'#edf4fa')
            cx,cy=COORDS[rank]
            d.text(x-60,y+7,f'A{rank} ({cx},{cy})',21)


def render():
    d=Drawing(1800,1400,'Dense forward traffic: TP4 with sequence partition')
    d.text(30,42,'Dense forward traffic：TP4 與 sequence partition',30)
    columns = (45, 325, 650, 1270)
    rows = [
        ('運算階段', '輸入通訊', '本地運算', '輸出通訊'),
        ('Attention', 'All-Gather', 'Partitioned attention and output projection', 'Reduce-Scatter'),
        ('MLP', 'All-Gather', 'Partitioned MLP', 'Reduce-Scatter'),
    ]
    for i, row in enumerate(rows):
        d.box(30, 92+i*41, 1720, 41, '#edf4fa' if i == 0 else 'white')
        for x, value in zip(columns, row):
            d.text(x, 119+i*41, value, 18)
    d.text(30,247,'每個 layer 重複上述通訊。完成一個 PP stage 的 layers 後，再將 activation 傳給下一個 group。',19)
    collective(d,295)
    d.text(30,735,'All-Gather：交換各自持有的 shards，最後每個 node 都取得完整輸入。對應 attention 與 MLP 的輸入收集。',20)
    d.text(30,768,'Reduce-Scatter：合併 partial results，最後每個 node 保留一份結果 shard。對應 attention 與 MLP 的輸出合併。',20)
    d.text(30,801,'若封包大小、資料就緒時間與網路條件相同，兩者可產生相同 traffic trace，不能只依名稱區分 NoC 負載。',19)
    d.text(30,870,'跨 group 通訊：A 的四份 sequence shards 分別傳給 B 的對應 nodes',25)
    def point(x,y):
        return 90+x*180,950+(3-y)*95
    for y in range(4):
        for x in range(4):
            p=point(x,y)
            if x<3: d.line([p,point(x+1,y)])
            if y<3: d.line([p,point(x,y+1)])
    for rank,path in pp_routes().items():
        offset = -13 if rank%2==0 else 13
        route=[(point(x,y)[0],point(x,y)[1]+offset) for x,y in path]
        d.line(route,True,f'data-pp-rank="{rank}"')
        d.text(route[0][0]+28,route[0][1]-5,f'S{rank}: A{rank} → B{rank}',13,'#1769aa')
    for y in range(4):
        for x in range(4):
            px,py=point(x,y)
            d.box(px-5,py-5,10,10,'white')
            group = ('A' if x<2 else 'B') if y>=2 else ('D' if x<2 else 'C')
            rank=x%2+2*(y%2)
            d.text(px-25,py+39,f'{group}{rank} ({x},{y})',13)
    notes=[
        'Aᵢ 將 Si 傳給 Bᵢ，兩端保留相同的 shard ownership。',
        '每份 shard 經過兩個水平 links。',
        'S0、S1 共用下方的跨 group link。',
        'S2、S3 共用上方的跨 group link。',
    ]
    for i,note in enumerate(notes): d.text(760,950+i*40,note,20)
    d.text(760,1150,'圖例：偏移線為同一實體 link 上的不同傳送。',18)
    d.text(30,1310,'圖說：本圖為通訊映射示例，非量測結果。Reduction 在 endpoint 執行，步驟不代表固定 cycle 數。',18)
    d.text(30,1340,'範圍：sequence-partitioned forward，不直接套用單一 request 的單 token decode。未含 weight/KV 與 control traffic。',18)
    d.text(30,1370,'Sources: Korthikanti et al., 2023, Figures 5-6. Patarasuk and Yuan, 2007, Figure 3. Mesh mapping is project-specific.',16)
    return d.finish()


def write_figure(destination):
    target=Path(destination)/'dense_forward.svg'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(render(),encoding='utf-8')
    return target


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', nargs='?', type=Path,
                        default=Path(__file__).resolve().parents[2]/'docs/figures/ai_mapping_review')
    write_figure(parser.parse_args().destination)
