#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成博客配图（SVG -> PNG）。

    python3 make_figs.py            # 写 img/*.svg 并调 rsvg-convert 出 img/*.png

配色与字号统一在这里改，不在各图里重复。
"""
import os
import subprocess
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
IMG = os.path.join(HERE, "img")
RSVG = "/usr/local/bin/rsvg-convert"
SCALE = 2

INK = "#1f2933"
MUTED = "#5f6b7a"
BLUE, BLUE_BG = "#2f6fd0", "#eef4fd"
GREEN, GREEN_BG = "#2e8b57", "#eefaf1"
ORANGE, ORANGE_BG = "#c9761f", "#fff5e8"
PURPLE, PURPLE_BG = "#6b4bab", "#f3eefc"
RED, RED_BG = "#b3382c", "#fdefec"
GRAY, GRAY_BG = "#8a94a0", "#f5f6f8"
FONT = "PingFang SC, Hiragino Sans GB, Microsoft YaHei, sans-serif"
MONO = "Menlo, Consolas, monospace"


def tw(s, fs):
    """粗略估宽，用来在文本溢出方框时报警（而不是等肉眼发现）。"""
    w = 0.0
    for ch in s:
        if unicodedata.east_asian_width(ch) in ("W", "F"):
            w += fs * 1.02
        elif ch == " ":
            w += fs * 0.30
        else:
            w += fs * 0.56
    return w


class Fig:
    def __init__(self, w, h, title=None, subtitle=None):
        self.w, self.h = w, h
        self.el = []
        self.warn = []
        self.el.append(f'<rect width="{w}" height="{h}" fill="#ffffff"/>')
        if title:
            self.el.append(f'<text x="28" y="40" font-family="{FONT}" font-size="22" '
                           f'font-weight="600" fill="{INK}">{esc(title)}</text>')
        if subtitle:
            self.el.append(f'<text x="28" y="64" font-family="{FONT}" font-size="14" '
                           f'fill="{MUTED}">{esc(subtitle)}</text>')

    def box(self, x, y, w, h, label, sub=None, stroke=BLUE, fill=BLUE_BG, fs=15,
            subfs=12.5, mono=False, dash=False):
        d = ' stroke-dasharray="6 4"' if dash else ""
        self.el.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="9" '
                       f'fill="{fill}" stroke="{stroke}" stroke-width="1.4"{d}/>')
        fam = MONO if mono else FONT
        lines = [label] + (sub if isinstance(sub, list) else ([sub] if sub else []))
        total = fs + (len(lines) - 1) * (subfs + 5)
        top = y + (h - total) / 2
        for i, ln in enumerate(lines):
            size = fs if i == 0 else subfs
            cy = top + size + i * (subfs + 5)
            self.el.append(f'<text x="{x + w / 2}" y="{cy}" font-family="{fam}" '
                           f'font-size="{size}" fill="{INK}" text-anchor="middle">'
                           f'{esc(ln)}</text>')
            if tw(ln, size) > w - 12:
                self.warn.append(f"{label[:14]}: 文本超出方框（{int(tw(ln, size))}>{w - 12}）")

    def text(self, x, y, s, fs=13, color=MUTED, anchor="start", mono=False, bold=False):
        fam = MONO if mono else FONT
        wt = ' font-weight="600"' if bold else ""
        self.el.append(f'<text x="{x}" y="{y}" font-family="{fam}" font-size="{fs}" '
                       f'{wt} fill="{color}" text-anchor="{anchor}">{esc(s)}</text>')
        right = x if anchor == "end" else x + tw(s, fs) / (1 if anchor == "start" else 2)
        if right > self.w - 8:
            self.warn.append(f"text 超出画布（{int(right)}>{self.w - 8}）：{s[:14]}")

    def arrow(self, x1, y1, x2, y2, label=None, color=GRAY, fs=12, dash=False,
              label_dx=0, label_dy=-6):
        d = ' stroke-dasharray="5 4"' if dash else ""
        self.el.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" '
                       f'stroke-width="1.6"{d} marker-end="url(#ar)"/>')
        if label:
            mx, my = (x1 + x2) / 2 + label_dx, (y1 + y2) / 2 + label_dy
            self.el.append(f'<rect x="{mx - tw(label, fs) / 2 - 4}" y="{my - fs + 2}" '
                           f'width="{tw(label, fs) + 8}" height="{fs + 7}" fill="#ffffff" '
                           f'opacity="0.92"/>')
            self.text(mx, my, label, fs=fs, color=color, anchor="middle")

    def group(self, x, y, w, h, label, color=GRAY):
        self.el.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="12" fill="none" '
                       f'stroke="{color}" stroke-width="1.2" stroke-dasharray="7 5"/>')
        self.el.append(f'<rect x="{x + 10}" y="{y - 11}" width="{tw(label, 13) + 14}" '
                       f'height="22" fill="#ffffff"/>')
        self.text(x + 17, y + 5, label, fs=13, color=color, bold=True)

    def save(self, name):
        head = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
                f'viewBox="0 0 {self.w} {self.h}"><defs>'
                f'<marker id="ar" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" '
                f'markerHeight="7" orient="auto-start-reverse">'
                f'<path d="M0,0 L10,5 L0,10 z" fill="{GRAY}"/></marker></defs>')
        svg = head + "".join(self.el) + "</svg>"
        p = os.path.join(IMG, name + ".svg")
        with open(p, "w", encoding="utf-8") as f:
            f.write(svg)
        out = os.path.join(IMG, name + ".png")
        subprocess.run([RSVG, "-b", "white", "-w", str(self.w * SCALE),
                        "-h", str(self.h * SCALE), "-o", out, p], check=True)
        print(f"  {name}.png  {os.path.getsize(out) // 1024} KB"
              + ("  [WARN] " + "; ".join(self.warn) if self.warn else ""))


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def _pip(poly, p):
    """射线法判断点是否在多边形内（配图里体素角点的归属判定用）。"""
    x, y = p
    inside = False
    n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % n]
        if (y1 > y) != (y2 > y):
            xin = x1 + (y - y1) * (x2 - x1) / (y2 - y1)
            if x < xin:
                inside = not inside
    return inside


# ---------------------------------------------------------------- 图 1 三层架构
def fig_arch():
    f = Fig(1180, 640, "cbctmeasure 的三层结构与「可在主机编译」的边界",
            "core 不 include Android / VTK / DCMTK / ONNX Runtime 头，所以同一份源码能在 macOS 上直接跑单测")
    f.box(40, 96, 250, 62, ":app 宿主", ["CbctMeasureFragment", "CbctAiFragment（tab 4/5）"], stroke=INK, fill="#fff")
    f.box(40, 190, 250, 96, "Kotlin 编排层", ["view 状态机 / overlay 绘制", "store 归档 · report PDF", "ai 线程与 so 候选链"], stroke=BLUE, fill=BLUE_BG)
    f.box(40, 318, 250, 62, "JNI 桥（只做类型转换）", ["measure-native-lib.cpp", "ai-native-lib.cpp"], stroke=PURPLE, fill=PURPLE_BG)
    f.group(330, 88, 470, 400, "纯 C++ core（主机可编译）", GREEN)
    rows = [
        ("MeasurePicker", "射线-体素求交 / 6 种拾取模式"),
        ("MeasureMath", "距离 角度 点到线 弧长 多边形面积"),
        ("MeasurementManager", "条目 CRUD · JSON dump/restore"),
        ("RoiExtractor", "角点权重覆盖 · R-01~R-06 量化"),
        ("ImplantPlanner", "骨高/骨宽/神经距/间距 → 红黄绿"),
        ("AnnotationStore", "A-01~A-07 标注"),
        ("AiCore / AiEngine / AiPlanner", "特征 阈值 26CC 统计 推荐"),
    ]
    y = 118
    for name, desc in rows:
        f.box(352, y, 426, 38, name + "   " + desc, fs=13.5, stroke=GREEN, fill=GREEN_BG, mono=False)
        y += 44
    f.box(352, y + 4, 426, 40, "Json（自研极简解析/序列化）", fs=13.5, stroke=GREEN, fill=GREEN_BG)
    f.box(840, 118, 300, 62, "ai/OrtEngine.cpp", ["全工程唯一 include ORT 头", "唯一 dlopen libonnxruntime.so"], stroke=ORANGE, fill=ORANGE_BG)
    f.box(840, 208, 300, 62, "core/SrReport.cpp", ["唯一依赖 DCMTK dcmsr", "Comprehensive SR + 读回自校验"], stroke=ORANGE, fill=ORANGE_BG)
    f.box(840, 298, 300, 62, "MeasureOverlayView", ["2D Canvas 叠加层", "只投影不重算几何"], stroke=BLUE, fill=BLUE_BG)
    f.box(840, 388, 300, 62, ":cbctdeal 只读内省", ["projectPoints / renderSnapshot", "captureFrame + CbctVolume"], stroke=GRAY, fill=GRAY_BG)
    f.box(40, 520, 250, 74, "主机单测 src/host", ["build_and_run.sh：386 断言", "用假 AiBackend 顶替推理"], stroke=INK, fill="#fff")
    f.arrow(290, 127, 352, 140, "复用同一份体数据")
    f.arrow(290, 238, 352, 240)
    f.arrow(290, 349, 352, 330)
    f.arrow(818, 149, 840, 149, "backend 接口", color=ORANGE)
    f.arrow(818, 239, 840, 239, color=ORANGE)
    f.arrow(900, 388, 900, 362, color=GRAY)
    f.text(920, 380, "共用相机矩阵，像素级对齐", fs=11.5, color=MUTED)
    f.arrow(165, 490, 165, 520, "同一份源码", color=INK)
    f.arrow(565, 490, 565, 520, "", color=GRAY)
    f.el.append(f'<line x1="330" y1="500" x2="800" y2="500" stroke="{GREEN}" stroke-width="1.2" stroke-dasharray="4 4"/>')
    f.text(340, 545, "边界之上：真机与主机跑的是同一份 core + JNI 之下的同一套数值路径", fs=13.5, color=GREEN)
    f.text(340, 570, "→ 两端不一致时唯一可能的差异来源就是神经网络本身，这正是 AC-08 奇偶校验要证明的东西", fs=13.5, color=MUTED)
    f.save("fig01_arch")


# ------------------------------------------------------- 图 2 一次测量的端到端
def fig_dataflow():
    f = Fig(1180, 470, "一次「点击取点 → 出数 → 出报告」的端到端链路",
            "屏上每个数都是 Native 回执；Kotlin 不重算，所以屏幕、PDF、DICOM SR 三处必然同源")
    steps = [
        ("ACTION_DOWN", "触摸 + 当前相机", GRAY),
        ("pick(mode)", "射线 DDA 步进\n首个骨面 / HU 区间", BLUE),
        ("core 计算", "距离/角度/面积\nROI 角点权重", GREEN),
        ("records", "条目 + JSON dump", GREEN),
        ("overlayPoints", "Native 一次给全部图元", PURPLE),
        ("projectPoints", "共用 VTK 相机矩阵", BLUE),
        ("Canvas onDraw", "文字标签同帧避让", ORANGE),
    ]
    x = 34
    for i, (t, s, c) in enumerate(steps):
        bg = {GRAY: GRAY_BG, BLUE: BLUE_BG, GREEN: GREEN_BG, PURPLE: PURPLE_BG, ORANGE: ORANGE_BG}[c]
        f.box(x, 110, 148, 84, t, s.split("\n"), stroke=c, fill=bg, fs=14, subfs=11.5)
        if i:
            f.arrow(x - 12, 152, x, 152)
        x += 160
    f.box(34, 250, 250, 74, "结果列表（可折叠）", ["6) 结果 Results（N 项）", "点 ROI 行 → 现场量化"], stroke=BLUE, fill=BLUE_BG)
    f.box(318, 250, 250, 74, "三份 JSON 归档", ["measure / anno / plan", "原子写 + .bak，≈7.4KB/Study"], stroke=GREEN, fill=GREEN_BG)
    f.box(602, 250, 250, 74, "PDF 报告", ["ReportGenerator + PdfPager", "A4 分页 / 表格 / 页脚免责"], stroke=ORANGE, fill=ORANGE_BG)
    f.box(886, 250, 262, 74, "DICOM SR", ["Comprehensive SR 1.2.840…88.33", "导出后 verifySr 读回自校验"], stroke=PURPLE, fill=PURPLE_BG)
    f.arrow(150, 194, 150, 250)
    f.arrow(284, 287, 318, 287)
    f.arrow(568, 287, 602, 287)
    f.arrow(852, 287, 886, 287)
    f.text(34, 372, "关键点 1", fs=14, color=INK, bold=True)
    f.text(92, 372, "：拾取的深度靠「最近若干采样深度中位数 ± 窗口」锁层，否则断续骨面会让相邻采样在 1302/1430/1516/1869 mm 之间横跳。", fs=13.5)
    f.text(34, 398, "关键点 2", fs=14, color=INK, bold=True)
    f.text(92, 398, "：重投影只在事件流里做（cameraActive 时每帧 reprojectOnly），不在 onDraw 里拉 Native，避免把 projectPoints 变成常驻开销。", fs=13.5)
    f.text(34, 424, "关键点 3", fs=14, color=INK, bold=True)
    f.text(92, 424, "：SR 生成前必须给本模块自带的 DCMTK 副本再注入一次 dicom.dic（它不与 :cbctdeal 共享），否则拿到空 SOP。", fs=13.5)
    f.save("fig02_dataflow")


# --------------------------------------------------- 图 3 角点权重体素覆盖原理
def fig_voxel_weight():
    f = Fig(1180, 580, "ROI 体积为什么能做到 ≤1% 误差：体素角点权重",
            "把 ROI 边界当作穿过体素网格的曲面：每个体素按「8 个角点有几个落在 ROI 内」给 0~1 的连续权重（下图是切面示意，画 4 角点）")
    ox, oy, cell = 60, 118, 50
    cols, rows = 9, 7
    poly = [(ox + 1.1 * cell, oy - 14), (ox + 8.2 * cell, oy + 0.9 * cell),
            (ox + 7.1 * cell, oy + 7.1 * cell), (ox - 14, oy + 5.8 * cell)]

    # 逐格数角点：权重决定底色，所以图里的深浅就是代码里的 w
    grid = []
    for i in range(cols):
        for j in range(rows):
            x, y = ox + i * cell, oy + j * cell
            k = sum(1 for dx in (0, cell) for dy in (0, cell) if _pip(poly, (x + dx, y + dy)))
            grid.append((x, y, k / 4.0))
    for x, y, w in grid:
        if w <= 0:
            continue
        alpha = 0.10 + 0.55 * w
        f.el.append(f'<rect x="{x}" y="{y}" width="{cell}" height="{cell}" '
                    f'fill="{RED}" opacity="{alpha:.2f}"/>')
    f.el.append(f'<path d="M{" L".join(f"{px:.0f},{py:.0f}" for px, py in poly)} Z" '
                f'fill="none" stroke="{RED}" stroke-width="2.4"/>')
    for i in range(cols + 1):
        f.el.append(f'<line x1="{ox + i * cell}" y1="{oy}" x2="{ox + i * cell}" '
                    f'y2="{oy + rows * cell}" stroke="#c3ccd6" stroke-width="1"/>')
    for j in range(rows + 1):
        f.el.append(f'<line x1="{ox}" y1="{oy + j * cell}" x2="{ox + cols * cell}" '
                    f'y2="{oy + j * cell}" stroke="#c3ccd6" stroke-width="1"/>')
    f.text(ox, oy - 26, "ROI 边界（阈值面 / 空间盒 / 球面 / 平面多边形都走同一套判定）", fs=13, color=RED)

    hx, hy = ox + 4 * cell, oy + 2 * cell
    f.el.append(f'<rect x="{hx}" y="{hy}" width="{cell}" height="{cell}" fill="none" '
                f'stroke="{BLUE}" stroke-width="3"/>')
    for dx in (0, cell):
        for dy in (0, cell):
            inn = _pip(poly, (hx + dx, hy + dy))
            f.el.append(f'<circle cx="{hx + dx}" cy="{hy + dy}" r="6" '
                        f'fill="{"#ffffff" if not inn else BLUE}" stroke="{BLUE}" stroke-width="2"/>')
    f.el.append(f'<rect x="{hx + 8}" y="{hy + 14}" width="34" height="24" rx="5" '
                f'fill="#ffffff" opacity="0.9"/>')
    f.text(hx + cell / 2, hy + 32, "0.75", fs=15, color=BLUE, anchor="middle", bold=True)
    f.text(ox, oy + rows * cell + 34, "实心 = 角点在 ROI 内，空心 = 在外：蓝框体素 3/4 在内 → w = 0.75；3D 里同理，8 角点 → w ∈ {0, 1/8, …, 1}",
           fs=13.5, color=BLUE)
    f.text(ox, oy + rows * cell + 62, "整格判定（w 只能是 0 或 1）= 阶梯误差，且不随体素变细而收敛；角点权重与「逐角点定义」逐位一致。",
           fs=13.5, color=INK)
    f.text(ox, oy + rows * cell + 90, "R-06（AI 掩膜 ROI）走同一条 analyze，自动分割的体积与手工 ROI 同一口径。",
           fs=13.5, color=MUTED)

    bx = 760
    f.box(bx, 118, 380, 92, "coverageVoxels = Σ w(v)", ["volumeMm3 = Σ w(v) × 单个体素体积", "meanHu = Σ w·hu / Σ w（权重可非整数）"], stroke=GREEN, fill=GREEN_BG)
    f.box(bx, 232, 380, 92, "快速路径：只扫包围盒", ["spatialBoundsAt 给索引区间", "只有边界格走 8 角点判定"], stroke=BLUE, fill=BLUE_BG)
    f.box(bx, 346, 380, 92, "验收口径", ["AC：M-04 体积相对误差 ≤1%", "RoiStats 带 scanned/coverage 取证"], stroke=ORANGE, fill=ORANGE_BG)
    f.save("fig03_voxel_weight")


# --------------------------------------------------------- 图 4 AI 推理管线
def fig_ai_pipeline():
    f = Fig(1180, 620, "端上牙齿分割：从 265 MB 体数据到「每颗牙一条 ROI」",
            "整条链只有 3 处依赖外部运行时，其余全是纯 C++，可在主机侧逐断言")
    f.box(34, 96, 200, 78, "app 网格", ["192×192×128", "@0.6 mm 符号 int16"], stroke=GRAY, fill=GRAY_BG)
    f.box(268, 96, 210, 78, "抽稀 factor=2（块均值）", ["96×96×64 @1.2 mm", "一趟遍历，不开全分辨率临时缓冲"], stroke=BLUE, fill=BLUE_BG)
    f.box(512, 96, 210, 78, "6 通道特征 feat", ["归一化体 + 4 个盒均值", "+ 带通项（r=4 盒均值 − r=8）"], stroke=PURPLE, fill=PURPLE_BG)
    f.box(756, 96, 200, 78, "ONNX Runtime", ["feat[1,6,96,96,64]", "→ prob[1,2,96,96,64]"], stroke=ORANGE, fill=ORANGE_BG)
    f.box(990, 96, 156, 78, "阈值掩膜", ["prob[1] > 0.49", "严格大于，不是 argmax"], stroke=RED, fill=RED_BG)
    for x in (234, 478, 722, 956):
        f.arrow(x, 135, x + 34, 135)
    f.box(34, 216, 220, 78, "26 邻接洪水填充", ["邻居表/扫描序/LIFO 栈固定", "<8 体素的碎块丢弃"], stroke=BLUE, fill=BLUE_BG)
    f.box(288, 216, 220, 78, "降序重编号 → 实例", ["inst 图（int16）", "0101 实测 24 个实例"], stroke=GREEN, fill=GREEN_BG)
    f.box(542, 216, 220, 78, "逐实例几何与统计", ["质心 / PCA 长轴 / 包围盒", "轮廓 / 平均 HU / 体积"], stroke=PURPLE, fill=PURPLE_BG)
    f.box(796, 216, 200, 78, "R-06 掩膜 ROI", ["+ M-04 体积", "+ M-08 骨密度"], stroke=ORANGE, fill=ORANGE_BG)
    f.box(1030, 216, 116, 78, "AI-03 推荐", ["间隙检测", "0~100 打分"], stroke=RED, fill=RED_BG)
    f.arrow(1068, 174, 144, 216, color=GRAY)
    for x in (254, 508, 762, 996):
        f.arrow(x, 255, x + 34, 255)
    f.group(34, 336, 540, 150, "模型本体（随包 14,477 B）", INK)
    f.text(52, 372, "3 × Conv3d   6→8→8→2，kernel 3×3×3，pad 1，无下采样分支", fs=14, color=INK, mono=True)
    f.text(52, 398, "感受野 7³ · 参数量 3,474 · opset 17 · 纯 numpy 手写前向/反向训练后导出", fs=13.5)
    f.text(52, 424, "阈值 0.49 来自验证集扫描的 Dice 最大点，写在 assets/models/teeth_cnn.json", fs=13.5)
    f.text(52, 452, "→ 容量太小，代价是实例过合并：0101 前两名实例占全部预测牙齿体素的 78%", fs=13.5, color=RED)
    f.group(608, 336, 538, 150, "运行时集成（PRD 字面偏差，已记档）", INK)
    f.text(626, 372, "onnxruntime-android:1.17.0 的 AAR 里没有 prefab/ 元数据", fs=13.5, color=INK, mono=True)
    f.text(626, 398, "→ 头文件 vendor 4 个进 cpp/third_party/，Gradle 依赖只为把 .so 打进 APK", fs=13.5)
    f.text(626, 424, "→ OrtEngine 用 dlopen + dlsym(OrtGetApiBase) 拿 OrtApi 跳转表", fs=13.5)
    f.text(626, 452, "→ 候选路径由 Native 逐条试：真机命中的是 base.apk!/lib/arm64-v8a/…", fs=13.5, color=RED)
    f.box(34, 512, 1112, 66, "耗时分解（真机 0101，round6）：prep 659 ms + infer 331 ms + post 7 ms = 997 ms —— 预处理占大头，推理只 33%",
          stroke=GREEN, fill=GREEN_BG, fs=14.5)
    f.save("fig04_ai_pipeline")


# ----------------------------------------------------- 图 5 AC-08 奇偶校验闭环
def fig_parity():
    f = Fig(1180, 470, "AC-08：怎么证明「真机跑的模型」和「主机算的指标」是同一个东西",
            "肉眼看到掩膜不算证据；把中间张量落盘拉回主机逐元素比对才算")
    f.box(34, 96, 230, 84, "真机推理", ["runSegment(keepParity=true)", "保留 feat/prob 缓冲（+20 MB）"], stroke=BLUE, fill=BLUE_BG)
    f.box(300, 96, 250, 84, "dumpParity 落盘", ["ai_parity/device_*.parity.bin", "0101 = 20,643,904 B"], stroke=PURPLE, fill=PURPLE_BG)
    f.box(586, 96, 250, 84, "adb run-as 拉回主机", ["免 root 读应用私有目录", "两次独立导出 sha256 相同"], stroke=GREEN, fill=GREEN_BG)
    f.box(872, 96, 274, 84, "check_device_dump.py", ["与随包夹具 device_* 逐元素比对", "VERDICT: PASS / EXIT=0"], stroke=ORANGE, fill=ORANGE_BG)
    for x in (264, 550, 836):
        f.arrow(x, 138, x + 36, 138)
    f.box(34, 216, 500, 96, "dump 格式（64 字节头 = 8×int64 LE）", ["featCount probCount labelBytes modelDim0..2 redDim0..1", "随后 float32 feat · float32 prob · uint8 label · int16 inst"], stroke=INK, fill="#fff", fs=14)
    f.box(586, 216, 560, 96, "判定口径（默认容差）", ["feat ≤1e-6 · prob ≤1e-5 · label 不一致率 ≤0.1% · inst 必须精确", "实测：label 逐位一致、24 实例个体素不差、feat 2.98e-08、prob 1.19e-07"], stroke=INK, fill="#fff", fs=14)
    f.arrow(284, 312, 284, 348, "为什么不是 memcmp 级一致", color=RED, label_dy=-8)
    f.box(34, 352, 1112, 82, "真机 ORT 1.17 + arm64 kernel，主机 ORT 1.23 + x86：float32 的累加顺序不同 → 1e-8 量级差是必然",
          ["所以「feat 默认要求逐位相同」会把每台正常设备判成 FAIL；改任何一处求和顺序都必须重跑这条取证链"], stroke=RED, fill=RED_BG, fs=14.5)
    f.save("fig05_parity")


# ------------------------------------------------------------ 图 6 性能实测
def fig_perf():
    f = Fig(1180, 592, "真机实测（Honor AGM3-W09HN，arm64-v8a，Android 10）",
            "左：PC-01 各操作耗时对 ≤50 ms 门槛；右：PC-05 推理延迟与体积预算")
    base_x, base_y, max_w = 250, 120, 640
    f.text(34, 100, "PC-01 测量计算（门槛 50 ms）", fs=15, color=INK, bold=True)
    rows = [
        ("两点距离 / 角度 / HU 采样", 1, "0~1 ms"),
        ("面积 / 截面量化", 12, "12 ms"),
        ("体积盒（145 万体素）", 23, "21~23 ms"),
        ("R-06 掩膜量化（24/26 条）", 1, "0~1 ms"),
        ("R-06 巨型实例 #1（168 万遍历）", 178, "178 ms ← 超标", RED),
    ]
    scale = max_w / 200.0
    thr_x = base_x + 50 * scale
    f.el.append(f'<line x1="{thr_x}" y1="112" x2="{thr_x}" y2="{base_y + len(rows) * 46}" '
                f'stroke="{RED}" stroke-width="1.6" stroke-dasharray="6 4"/>')
    f.text(thr_x + 4, 108, "50 ms 门槛", fs=12.5, color=RED)
    y = base_y
    for r in rows:
        name, val, label = r[0], r[1], r[2]
        color = r[3] if len(r) > 3 else BLUE
        w = max(3.0, val * scale)
        f.el.append(f'<rect x="{base_x}" y="{y}" width="{w}" height="26" rx="4" fill="{color}" opacity="0.85"/>')
        f.text(base_x - 10, y + 18, name, fs=13, color=INK, anchor="end")
        f.text(base_x + w + 8, y + 18, label, fs=12.5, color=color if color == RED else MUTED)
        y += 46
    f.text(34, y + 16, "批量归档重算会出现 ~70 ms，那是装载耗时；大掩膜那 2 条是 PC-01 唯一的超标点（缺包围盒分块快速路径）。", fs=13)

    f.text(34, 400, "PC-05 AI 推理延迟（门槛 5 s；两次独立运行 1014 / 997 ms）", fs=15, color=INK, bold=True)
    x = 250
    segs = [("prep 659 ms", 659, PURPLE, PURPLE_BG), ("infer 331 ms", 331, BLUE, BLUE_BG), ("post", 7, GREEN, GREEN_BG)]
    psc = 640 / 1000.0
    for name, v, c, bg in segs:
        w = max(26.0, v * psc)
        f.el.append(f'<rect x="{x}" y="414" width="{w}" height="34" rx="4" fill="{bg}" stroke="{c}" stroke-width="1.4"/>')
        f.text(x + w / 2, 436, name, fs=12.5, color=c, anchor="middle")
        x += w
    f.text(x + 10, 436, "合计 997 ms", fs=12.5, color=INK)
    lx = x + 92
    f.el.append(f'<line x1="{lx}" y1="406" x2="{lx}" y2="456" stroke="{RED}" stroke-width="1.6" stroke-dasharray="6 4"/>')
    f.text(lx + 8, 436, "5,000 ms 门槛（余量 ~5×）", fs=12.5, color=RED)
    f.text(34, 488, "PC-05 体积：libonnxruntime.so 16,033,712 B + teeth_cnn.onnx 14,477 B = 16.05 MB（预算 80 MB），前提是 :app 收窄到 arm64-v8a", fs=13)
    f.text(34, 516, "PC-02 叠加层单帧 0.46~2.04 ms（prims=33~89）；PC-03 截图 319~934 ms，其中 frame 段是 :cbctdeal 的 GPU 读回，本模块只占 6~31 ms", fs=13)
    f.text(34, 544, "PC-04 测量层内存增量 0（Pss 波动 <±10 MB 噪声）；AI 装载+分割后 PSS 268.9 MB，清除结果后 257.5 MB（ORT 会话随 MeasureSession 存活）", fs=13)
    f.save("fig06_perf")


# --------------------------------------------------------- 图 7 页面结构
def fig_ui():
    f = Fig(1180, 560, "重构后的页面结构：为什么 AI 必须是 tab 而不是第二个 Fragment",
            "竖排 7 个区块，其中 4)/5) 是同一容器里的两个面板；会话与渲染窗口只有宿主持有")
    x, y, w = 40, 92, 520
    blocks = [
        ("1) 数据源 Series Source", "加载 neck_ct · 选择目录(SAF) · 解析 · 加载牙科 CBCT", GRAY),
        ("2) 视口 Viewport", "VR/MPR · 三平面 · 位置滑条 · WW/WC", GRAY),
        ("3) 三维画面 3D View（固定 300dp）", "SurfaceView + 2D 叠加层；下方 5 按钮快捷行", BLUE),
        ("tab 4) 测量工具与 ROI", "9 工具 × 子类型；HU 阈值/隔离/组合/神经管描记", GREEN),
        ("tab 5) AI 辅助分析 (Phase 2)", "装载模型/分割/逐牙测量/推荐/取证/清除", PURPLE),
        ("6) 结果 Results（可折叠）", "标题行 = 「N 项」读数；≤6 行自动展开", ORANGE),
        ("7) 归档与报告", "保存/恢复归档 · 导出 PDF + SR（两个 tab 共享）", RED),
    ]
    for t, s, c in blocks:
        bg = {GRAY: GRAY_BG, BLUE: BLUE_BG, GREEN: GREEN_BG, PURPLE: PURPLE_BG, ORANGE: ORANGE_BG, RED: RED_BG}[c]
        dash = t.startswith("tab")
        f.box(x, y, w, 54, t, s, stroke=c, fill=bg, fs=14, subfs=11.5, dash=dash)
        y += 62
    f.text(x, y + 6, "快捷行（常驻，与 tab 无关）：撤销取点 · 闭合路径 · 复位相机 · 丢弃草稿 · 清空全部", fs=13, color=INK)

    rx = 608
    f.group(rx, 84, 538, 196, "会话所有权", INK)
    f.box(rx + 20, 112, 240, 62, "MeasureSession（宿主唯一）", ["VolumeRef 零拷贝指向", ":cbctdeal 的 CbctVolume"], stroke=BLUE, fill=BLUE_BG)
    f.box(rx + 288, 112, 230, 62, "ORT 推理会话", ["随 MeasureSession 析构", "换例后必须重新装载"], stroke=ORANGE, fill=ORANGE_BG)
    f.box(rx + 20, 190, 240, 68, "CbctAiFragment", ["childFragmentManager.add", "只经 CbctAiHost 借句柄"], stroke=PURPLE, fill=PURPLE_BG)
    f.box(rx + 288, 190, 230, 68, "反例：抽屉切页 = replace()", ["做成第二个页面 → 离开即销毁会话", "回来要重解析 9.5 MB 序列"], stroke=RED, fill=RED_BG, fs=13.5, dash=True)
    f.arrow(rx + 140, 174, rx + 140, 190, color=PURPLE)
    f.box(rx, 306, 538, 76, "耦合面只有一个接口", ["aiSessionHandle() · aiVolumeInfo()", "aiInvalidate(overlay, results) · aiPickDentalCase()"], stroke=INK, fill="#fff", fs=14)
    f.box(rx, 400, 538, 118, "本轮布局改动的三条硬约束", ["① 读数排在按钮之后：7 行读数曾把按钮顶下去 128 px（y 572→700），用户点空", "② 默认 Button 的 48dp 最小高 + 6dp inset 是页面变高主因 → CompactButton 40dp", "③ selector 是 color 资源，夜间变体只能放 res/color-night/"], stroke=INK, fill="#fff", fs=14, subfs=12)
    f.save("fig07_ui")


if __name__ == "__main__":
    os.makedirs(IMG, exist_ok=True)
    for fn in (fig_arch, fig_dataflow, fig_voxel_weight, fig_ai_pipeline,
               fig_parity, fig_perf, fig_ui):
        fn()
    print("done")
