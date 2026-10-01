#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把真机回归留下的原始截图裁选、缩放，整理成博客用的 img/shots/*.png。

    python3 prep_shots.py /tmp/dcmtk_verify

原始取证目录（round5 / round6）是 adb exec-out screencap 的产物，1920x1200，
一张 130~480 KB；直接发博客太大，也读不清关键区域，所以统一：
  - 整屏图缩到 1280 宽（文字仍然可读，体积降到 ~1/3）；
  - 需要看细节的地方用已经裁好的区域图（叠加层标签、面积多边形）；
  - PDF 报告页用 sips 渲染后可能带透明底，这里合成到白底再缩。
只做「搬运 + 缩放」，不画任何假数据：每张图都能在回归报告里找到对应步骤。
"""
import os
import sys
from PIL import Image, ImageDraw

SRC = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "/tmp/dcmtk_verify")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "img", "shots")
os.makedirs(OUT, exist_ok=True)

# (输出名, 源相对路径, 目标宽 or None=原尺寸, 说明)
ITEMS = [
    ("shot_01_page_top", "round6/01_01_measure_page_after_nav.png", 1280,
     "重构后的测量页顶部：1) 数据源 / 2) 视口 / 3) 三维画面"),
    ("shot_02_ai_tab", "round6/02_02_ai_tab_opened.png", 1280,
     "tab 5) AI 辅助分析：读数排在按钮之后"),
    ("shot_03_ai_after_segment", "round6/03_03_after_segment.png", 1280,
     "牙齿自动分割完成后的回执：运行时/阈值/dlopen 命中路径/inst/耗时"),
    ("shot_04_results_expanded", "round6/04_04_results_expanded.png", 1280,
     "6) 结果列表展开：AI 逐牙体积 + 骨密度与手工测量同表"),
    ("shot_05_overlay_labels", "crop_labels_round6.png", 1280,
     "叠加层文字标签特写（同帧避让）"),
    ("shot_06_hint_after_discard", "round6/06_06_hint_after_discard.png", 1280,
     "丢弃草稿后的提示：叠加层横幅 + 画面下方 TextView"),
    ("shot_07_dental_case_dialog", "round5/13_r5_02_dental_dialog.png", 1280,
     "选择加载牙科 CBCT：训练例/测试例与各自 Dice 都写在对话框里"),
    ("shot_08_ai_recommend_dialog", "round5/18_r5_07_recommend_dialog.png", 1280,
     "AI-03 种植位点推荐候选列表"),
    ("shot_09_archive_restored", "round5/22_r5_11_archive_restored.png", 1280,
     "恢复归档后的页面状态"),
    ("shot_10_area_roi_overlay", "round5/16b_area_polygon_crop.png", None,
     "面积多边形与截面 ROI 的叠加层特写"),
]

FLATTEN = [("shot_11_report_pdf_page1", "report_page1.png", 900)]


def main():
    for name, rel, width, desc in ITEMS:
        src = os.path.join(SRC, rel)
        im = Image.open(src).convert("RGB")
        if width and im.width > width:
            im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
        im.save(os.path.join(OUT, name + ".png"), optimize=True)
        print(f"{name}.png  {im.size}  {os.path.getsize(os.path.join(OUT, name + '.png')) // 1024} KB   {desc}")

    for name, rel, width in FLATTEN:
        src = os.path.join(SRC, rel)
        im = Image.open(src)
        if im.mode in ("RGBA", "LA") or "transparency" in im.info:
            bg = Image.new("RGB", im.size, "white")
            bg.paste(im.convert("RGBA"), mask=im.convert("RGBA").split()[-1])
            im = bg
        else:
            im = im.convert("RGB")
        if im.width > width:
            im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
        ImageDraw.Draw(im)
        im.save(os.path.join(OUT, name + ".png"), optimize=True)
        print(f"{name}.png  {im.size}  {os.path.getsize(os.path.join(OUT, name + '.png')) // 1024} KB   真机导出的 PDF 报告首页")


if __name__ == "__main__":
    main()
