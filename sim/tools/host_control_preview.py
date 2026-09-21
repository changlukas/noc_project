"""Host-controlled scenario proposals. Arrows are messages, not mesh routes."""

import argparse
from pathlib import Path

from ai_mapping_preview import Drawing


ACTORS = {'host': 170, 'tiles': 705, 'memory': 1170}


def base(title, height, memory=False):
    d = Drawing(1320, height, title)
    d.parts[2] = '<desc>Proposed host-managed message sequence. Physical attachment and runtime interfaces remain TBD.</desc>'
    d.text(30, 42, title, 28)
    d.text(30, 77, 'Proposed：向下閱讀事件順序，箭頭表示訊息方向，不表示實體 link 或固定時間。', 17)
    actors = ('host', 'tiles', 'memory') if memory else ('host', 'tiles')
    labels = {'host': ('External host CPU', 'NoC 接入 port [TBD]'),
              'tiles': ('Target tiles / runtime', '實際 node 集合 [TBD]'),
              'memory': ('Backing memory', '沿用 M0 至 M3')}
    for actor in actors:
        x = ACTORS[actor]
        d.box(x-130, 105, 260, 70, '#edf4fa' if actor == 'tiles' else '#f4f7fa',
              f'data-actor="{actor}"')
        d.text(x-115, 134, labels[actor][0], 19)
        d.text(x-115, 158, labels[actor][1], 15)
        d.line([(x, 175), (x, height-220)], attrs='stroke-dasharray="5 5"')
    return d


def message(d, y, source, destination, label, mode, key):
    x1, x2 = ACTORS[source], ACTORS[destination]
    d.line([(x1, y), (x2, y)], True,
           f'data-message="{key}" data-source="{source}" '
           f'data-destination="{destination}" data-mode="{mode}"')
    d.text(min(x1, x2)+12, y-12, label, 17)


def note(d, y, text):
    d.text(35, y, text, 17)


def initialization():
    from composite_traffic_figures import measured_control

    return measured_control()

def switching():
    d = base('Host CPU：request 收尾與切換', 1190, memory=True)
    message(d, 235, 'tiles', 'host', 'Request 運算完成狀態，回報方式 [TBD]', 'tbd', 'request-status')
    d.box(490, 270, 430, 58, '#fff6e6', 'data-condition="drain-before-reuse"')
    d.text(505, 294, 'Tile/runtime 等待必要 outstanding 完成', 17)
    d.text(505, 316, 'Result payload 的接收端仍為 [TBD]', 17)
    message(d, 380, 'tiles', 'host', '可重用狀態回報，介面 [TBD]', 'tbd', 'reusable')
    message(d, 450, 'host', 'tiles', 'Release／retain policy 與下一 request 描述', 'tbd', 'next-request')
    note(d, 515, '條件分支：已 resident 則跳過下方搬移，只讀取缺少的 tensors／KV')
    message(d, 575, 'tiles', 'memory', 'Read request：unicast', 'unicast', 'missing-read')
    message(d, 645, 'memory', 'tiles', 'Weight／KV payload：unicast', 'unicast', 'missing-data')
    d.text(750, 690, '沿用 loading／restore 圖的 ownership', 16)
    d.text(750, 718, '搬移發起端為邏輯角色，DMA 配置 [TBD]', 16)
    message(d, 790, 'tiles', 'host', '所需資料已就緒，回報方式 [TBD]', 'tbd', 'ready')
    message(d, 870, 'host', 'tiles', 'Start：同一命令可 multicast，否則 unicast', 'conditional', 'start')
    note(d, 1010, 'Host 控制 policy，weight／KV payload 不預設經過 Host CPU。Free 不自動觸發 offload。')
    note(d, 1045, '同 model 換 request 不預設重載。開始新工作前，需滿足資源重用與資料就緒條件。')
    note(d, 1080, '本圖為待確認的 host-managed 範例。實體接入、控制訊息編碼與完整 runtime 流程 [TBD]。')
    note(d, 1115, 'Unicast 標籤只指定傳送方式，箭頭不指定實體 port、路徑或 cycle 數。非量測結果。')
    return d.finish()


RENDERERS = {'host_initialization': initialization, 'host_request_switch': switching}


def write_figures(destination):
    directory = Path(destination)
    directory.mkdir(parents=True, exist_ok=True)
    for name, render in RENDERERS.items():
        (directory / f'{name}.svg').write_text(render(), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    write_figures(parser.parse_args().destination)
