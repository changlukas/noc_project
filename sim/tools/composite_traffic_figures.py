"""Traffic figures using the report's approved mesh and drawing style."""

from pathlib import Path

from ai_mapping_preview import Drawing, mesh, xy_path


TILES = ((0, 2), (1, 2), (0, 3), (1, 3))


def payload(d, point, source, destination, offset=0, memory_source=False,
            memory_destination=False, attrs=""):
    sx, sy = point(*source)
    dx, dy = point(*destination)
    route = ([(sx + offset, sy + 82)] if memory_source else
             [(sx + 40, sy + 22), (sx + 40, sy + offset)])
    route += [(point(x, y)[0] + offset, point(x, y)[1] + offset)
              for x, y in xy_path(source, destination)]
    route += ([(dx + offset, dy + 82)] if memory_destination else
              [(dx + 40, dy + offset), (dx + 40, dy + 22)])
    d.line(route, True, attrs)


def shared_distribution():
    d = Drawing(1320, 1000, "Shared distribution: read and distribution routes")
    d.text(30, 42, "Shared distribution：同一份資料交付四個 tiles", 28)
    d.text(30, 77, "以 A 與 M0 示意。四組各需一份 2 MiB activation，組內每個 tile 都取得完整資料。", 20)
    for index, title in enumerate(("各 tile 分別 read", "A0 DMA read + multicast write")):
        ox = 70 + index * 650
        d.text(ox - 30, 125, title, 24)
        point = mesh(d, ox, 240, 120, "A")
        for rank, destination in enumerate(TILES if index == 0 else TILES[:1]):
            payload(d, point, (1, 0), destination, (rank - 1.5) * 5, memory_source=True,
                    attrs='stroke-dasharray="6 4"' if index else "")
        if index:
            sx, sy = point(*TILES[0])
            d.line([(sx + 40, sy + 22), (sx + 40, sy - 12), (sx, sy - 12), (sx, sy)], True)
            for source, destination in ((TILES[0], TILES[1]), (TILES[0], TILES[2]),
                                        (TILES[1], TILES[3])):
                d.line([point(*source), point(*destination)], True)
            for rank, tile in enumerate(TILES):
                px, py = point(*tile)
                d.line([(px, py), (px + 52, py), (px + 52, py + 22)], True)
                d.text(px + 16, py + 78, f"A{rank}", 16, "#1769aa")
            d.text(ox - 30, 815, "A0 的 DMA 讀取 M0，直接 multicast write。", 20)
        else:
            d.text(ox - 30, 815, "四個 tiles 各 read 一份，M0 以 unicast 回傳。", 20)
        if index == 1:
            d.text(ox - 30, 855, "Mask 包含 A0、A1、A2、A3，A0 也接收一份。", 19)
            d.text(ox - 30, 890, "不先將完整資料存入 A0 再搬一次。", 19)
    d.text(30, 950, "右圖虛線為 read response，實線為 multicast write。Read requests 與 write responses 未畫出。", 19)
    return d.finish()


def prefill_decode():
    d = Drawing(1320, 1100, "Prefill and decode: tile-issued KV traffic")
    d.text(30, 42, "Prefill／decode：KV 存取順序", 30)
    d.text(30, 78, "單一 request、單一 layer。Weights 已在 tile，KV 放在 M0。", 21)
    for index, title in enumerate(("1. Prefill KV 寫入 M0", "2. Decode 從 M0 讀回 KV")):
        ox = 70 + index * 650
        d.text(ox - 30, 128, title, 24)
        point = mesh(d, ox, 240, 120, "A")
        for rank, tile in enumerate(TILES):
            if index == 0:
                payload(d, point, tile, (1, 0), (rank - 1.5) * 5, memory_destination=True)
            else:
                payload(d, point, (1, 0), tile, (rank - 1.5) * 5, memory_source=True)
        d.text(ox - 30, 820, "四個 tiles 各寫 512 KiB，共 2 MiB。" if index == 0 else
               "四個 tiles 各讀 512 KiB，共 2 MiB。", 20)
    d.text(30, 883, "3. 各 tile 再寫入 1 KiB 新 KV，共 4 KiB，沿左圖的 write 路徑。", 21)
    d.text(30, 925, "各 tile 完成 prefill collective 後保存 KV。全部保存後開始 decode，依序讀回 KV、追加 KV、執行 collective。", 20)
    d.text(30, 965, "Collective 沿用 ring 圖。Prefill activation 4 MiB，decode activation 8 KiB。", 21)
    d.text(30, 1005, "全部 unicast，tiles 主動發起存取。藍線為 payload，read requests 與 write responses 未畫出。", 20)
    d.text(30, 1060, "存取依據：Efficient Memory Management for Large Language Model Serving with PagedAttention，§2.2。", 17)
    return d.finish()


def overlap_routes(control=False):
    title = "Control 與 KV 搬移" if control else "Activation 與 KV restore"
    d = Drawing(1320, 1010, title)
    d.parts.append('<defs><marker id="orange" markerWidth="5" markerHeight="5" refX="4" refY="2.5" orient="auto"><path d="M0 0L5 2.5L0 5Z" fill="#bb590e"/></marker></defs>')
    orange = 'style="stroke:#bb590e;marker-end:url(#orange)"'
    d.text(30, 42, title, 28)
    d.text(30, 77, "全部 unicast。箭頭沿 XY 路徑，偏移線仍使用同一實體 link。", 20)
    for index in range(2):
        ox = 70 + index * 650
        label = (("Control + KV offload", "Control + KV restore") if control else
                 ("兩股 DAT traffic", "共用 link 放大"))[index]
        d.text(ox - 30, 125, label, 24)
        if not control and index:
            for y, name in ((310, "C0 activation"), (460, "M0 KV read response")):
                d.text(ox - 25, y - 40, name, 20)
                d.line([(ox, y), (ox + 170, y), (ox + 170, 560)], True,
                       orange if y == 310 else "")
            d.box(ox + 115, 560, 165, 65, "#fff6e6")
            d.text(ox + 128, 585, "Router (1,0)", 19)
            d.text(ox + 128, 610, "WEST output", 18)
            d.line([(ox + 115, 592), (ox - 25, 592), (ox - 25, 690)], True)
            d.text(ox - 40, 725, "Router (0,0)", 19)
            d.text(ox - 30, 805, "C0 給 D0 的 activation 與", 20)
            d.text(ox - 30, 837, "M0 給 A0／A2 的 KV 共用此 output。", 20)
            continue
        point = mesh(d, ox, 240, 120, "A" if control else ("A", "C", "D"))
        for rank, tile in enumerate(TILES):
            offset = (rank - 1.5) * 5
            if control and index == 0:
                payload(d, point, tile, (1, 0), offset, memory_destination=True)
            else:
                payload(d, point, (1, 0), tile, offset, memory_source=True)
            if control:
                payload(d, point, (0, 0), tile, offset - 12, attrs=orange)
                payload(d, point, tile, (0, 0), offset + 12,
                        attrs=orange + ' stroke-dasharray="6 4"')
            else:
                payload(d, point, (2 + rank % 2, rank // 2),
                        (rank % 2, rank // 2), offset - 12, attrs=orange)
        if control:
            d.text(ox - 30, 805, "橘實線：Host node 0 發 command 給 A。", 19)
            d.text(ox - 30, 837, "橘虛線：A 回 acknowledgement 給 Host。", 19)
        else:
            d.text(ox - 30, 805, "橘色：C 各 tile 傳 1 MiB 給 D 對應 tile。", 19)
            d.text(ox - 30, 837, "藍色：M0 回傳 512 KiB 給各 A tile。", 19)
    d.text(30, 905, "Control 走 REQ，KV payload 走 DAT。Offload 與 acknowledgement 共用 tile 的 write 發送端。" if control else
           "共用方向：(1,0) 至 (0,0)。其餘 activation 與 KV 路徑也畫於左圖。", 20)
    d.text(30, 943, "藍色為 KV payload，每個 A tile 512 KiB。兩圖為不同案例。" if control else
           "兩股 flow 同時就緒。右圖僅放大共用 output，不代表新增連線。", 19)
    d.text(30, 979, "Read requests 與 write responses 未畫出。路徑依本次測試 mapping。", 18)
    return d.finish()


def concentrated_loading():
    d = Drawing(1320, 1580, "Four replicas read weights from M0")
    d.text(30, 42, "Replica weight loading：四組共用 M0", 28)
    d.text(30, 77, "同一案例分四個視圖。每組四個 tiles 各 read 2 MiB，M0 共回傳 32 MiB。", 20)
    for index, (group, origin) in enumerate((("A", (0, 2)), ("B", (2, 2)),
                                               ("C", (2, 0)), ("D", (0, 0)))):
        ox, oy = 70 + index % 2 * 650, 215 + index // 2 * 665
        d.text(ox - 30, oy - 95, f"M0 回傳 weights 給 {group}", 23)
        point = mesh(d, ox, oy, 120, group)
        for rank in range(4):
            tile = (origin[0] + rank % 2, origin[1] + rank // 2)
            payload(d, point, (1, 0), tile, (rank - 1.5) * 5, memory_source=True)
    d.text(30, 1515, "全部 unicast，tiles 主動 read。M1、M2、M3 未使用。", 20)
    d.text(30, 1550, "四組各取得相同 matrix 的四份 shards。分散接入案見 Weight loading 圖。", 19)
    return d.finish()


def pipeline_routes():
    d = Drawing(1320, 920, "Pipeline activation routes across four stages")
    d.text(30, 42, "Pipeline Parallelism：四個 stages 的 activation 路徑", 28)
    d.text(30, 77, "各 stage 的四個 tiles 分別傳給下一 stage 的對應 tile，全部 unicast。", 20)
    point = mesh(d, 95, 220, 145)
    for source, destination in (((0, 2), (2, 2)), ((2, 2), (2, 0)), ((2, 0), (0, 0))):
        for rank in range(4):
            delta = (rank % 2, rank // 2)
            payload(d, point, (source[0] + delta[0], source[1] + delta[1]),
                    (destination[0] + delta[0], destination[1] + delta[1]),
                    (rank - 1.5) * 7)
    for y, label in ((280, "1. A 傳給 B"), (390, "2. B 收到 shard 後傳給 C"),
                     (500, "3. C 收到 shard 後傳給 D")):
        d.text(780, y, label, 23)
    d.text(780, 610, "單批與四批測試共用此路徑。", 20)
    d.text(780, 645, "四批案允許不同批次跨 stage 重疊。", 20)
    d.text(30, 850, "箭頭表示 payload 方向，不代表所有 stage 同時傳送同一份 shard。", 20)
    d.text(30, 886, "Memory 接入端未使用。Read requests 與 write responses 未畫出。", 19)
    return d.finish()


def measured_control():
    d = Drawing(1320, 720, "Measured command, status and release")
    d.text(30, 42, "Host 控制：Command、Status、Release", 28)
    for x, title in ((120, "Host：node 0"), (800, "Tiles：nodes 1 至 15")):
        d.box(x - 55, 95, 340, 60, "#edf4fa")
        d.text(x - 35, 132, title, 22)
        d.line([(x + 110, 155), (x + 110, 560)], attrs='stroke-dasharray="5 5"')
    for y, reverse, label in ((230, False, "1. Command：unicast 或 multicast"),
                              (350, True, "2. Status：每個 tile 收到 command 後 unicast 回報"),
                              (510, False, "3. Release：Host 收齊 15 份 status 後分發")):
        d.line([(910 if reverse else 230, y), (230 if reverse else 910, y)], True)
        d.text(260, y - 20, label, 20)
    d.text(30, 600, "每筆訊息 8 bytes，走 REQ narrow write。AXI B responses 走 RSP，未畫出。", 20)
    d.text(30, 638, "Multicast 案以 1／2／4／8 個目的地分組，排除 Host。Status 一律 unicast。", 20)
    d.text(30, 676, "箭頭表示訊息方向與相依。各 tile 可獨立回報，不等待其他 tiles 收到 command。", 19)
    return d.finish()


def write_figures(destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    for name, render in (("shared_distribution", shared_distribution),
                         ("prefill_decode", prefill_decode),
                         ("control_kv_overlap", lambda: overlap_routes(True)),
                         ("activation_kv_overlap", overlap_routes),
                         ("pipeline_routes", pipeline_routes),
                         ("concentrated_loading", concentrated_loading),
                         ("host_initialization", measured_control)):
        (destination / f"{name}.svg").write_text(render(), encoding="utf-8")


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    write_figures(parser.parse_args().destination)
