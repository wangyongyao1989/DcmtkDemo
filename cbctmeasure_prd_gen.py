#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CBCT 三维测量与手术规划模块 - 需求规格说明书 PDF 生成脚本
基于 DcmtkDemo 现有架构 + CBCT 行业应用调研
"""

import os
import sys
import platform

from reportlab.lib.pagesizes import A4
from reportlab.lib.units import inch, mm
from reportlab.lib.colors import HexColor
from reportlab.lib.enums import TA_LEFT, TA_CENTER, TA_RIGHT, TA_JUSTIFY
from reportlab.lib import colors
from reportlab.lib.styles import ParagraphStyle
from reportlab.platypus import (
    SimpleDocTemplate, Paragraph, Spacer, Table, LongTable,
    PageBreak, KeepTogether, Image, Flowable
)
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont

# ── CJK 字体注册 ──────────────────────────────────────────
def register_cjk_font():
    system = platform.system()
    if system == "Darwin":
        font_paths = [
            "/System/Library/Fonts/PingFang.ttc",
            "/Library/Fonts/Arial Unicode.ttf",
            "/System/Library/Fonts/STHeiti Medium.ttc",
        ]
    elif system == "Windows":
        font_paths = ["C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simsun.ttc"]
    else:
        font_paths = [
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        ]
    for fp in font_paths:
        if os.path.exists(fp):
            pdfmetrics.registerFont(TTFont("CJKFont", fp, subfontIndex=0))
            return "CJKFont"
    return None

CJK_FONT = register_cjk_font()
assert CJK_FONT, "无法注册 CJK 字体"

# ── 页面几何常量 ───────────────────────────────────────────
PAGE_SIZE = A4
PAGE_W, PAGE_H = PAGE_SIZE
LM = 0.75 * inch
RM = 0.75 * inch
TM = 0.75 * inch
BM = 0.75 * inch
CONTENT_W = PAGE_W - LM - RM
CONTENT_H = PAGE_H - TM - BM

# ── 颜色定义 ──────────────────────────────────────────────
PRIMARY = HexColor('#1a365d')
ACCENT = HexColor('#2b6cb0')
LIGHT_BG = HexColor('#f7fafc')
GRID_COLOR = HexColor('#e2e8f0')
TEXT_DARK = HexColor('#2d3748')
TEXT_MID = HexColor('#4a5568')
TEXT_LIGHT = HexColor('#718096')
WARN_COLOR = HexColor('#c53030')
SUCCESS_COLOR = HexColor('#2f855a')

# ── 段落样式 ──────────────────────────────────────────────
def get_styles():
    return {
        'title': ParagraphStyle('Title', fontName=CJK_FONT, fontSize=26, leading=32,
            textColor=PRIMARY, spaceAfter=16, alignment=TA_CENTER, wordWrap='CJK'),
        'subtitle': ParagraphStyle('Subtitle', fontName=CJK_FONT, fontSize=14, leading=20,
            textColor=TEXT_MID, spaceAfter=24, alignment=TA_CENTER, wordWrap='CJK'),
        'doc_info': ParagraphStyle('DocInfo', fontName=CJK_FONT, fontSize=11, leading=16,
            textColor=TEXT_LIGHT, alignment=TA_CENTER, spaceAfter=4, wordWrap='CJK'),
        'h1': ParagraphStyle('H1', fontName=CJK_FONT, fontSize=18, leading=24,
            textColor=PRIMARY, spaceBefore=22, spaceAfter=10, wordWrap='CJK'),
        'h2': ParagraphStyle('H2', fontName=CJK_FONT, fontSize=14, leading=20,
            textColor=ACCENT, spaceBefore=16, spaceAfter=6, wordWrap='CJK'),
        'h3': ParagraphStyle('H3', fontName=CJK_FONT, fontSize=12, leading=16,
            textColor=TEXT_DARK, spaceBefore=12, spaceAfter=4, wordWrap='CJK'),
        'body': ParagraphStyle('Body', fontName=CJK_FONT, fontSize=10.5, leading=17,
            textColor=TEXT_DARK, spaceBefore=0, spaceAfter=8, wordWrap='CJK',
            firstLineIndent=0),
        'body_indent': ParagraphStyle('BodyIndent', fontName=CJK_FONT, fontSize=10.5, leading=17,
            textColor=TEXT_DARK, spaceBefore=0, spaceAfter=6, wordWrap='CJK',
            firstLineIndent=21),
        'bullet': ParagraphStyle('Bullet', fontName=CJK_FONT, fontSize=10.5, leading=16,
            textColor=TEXT_DARK, spaceBefore=0, spaceAfter=4, wordWrap='CJK',
            leftIndent=21, bulletIndent=10),
        'caption': ParagraphStyle('Caption', fontName=CJK_FONT, fontSize=9, leading=12,
            textColor=TEXT_LIGHT, alignment=TA_CENTER, spaceBefore=4, spaceAfter=10, wordWrap='CJK'),
        'table_header': ParagraphStyle('TH', fontName=CJK_FONT, fontSize=9.5, leading=13,
            textColor=colors.white, wordWrap='CJK', splitLongWords=1, alignment=TA_CENTER),
        'table_body': ParagraphStyle('TB', fontName=CJK_FONT, fontSize=9, leading=12,
            textColor=TEXT_DARK, wordWrap='CJK', splitLongWords=1),
        'note': ParagraphStyle('Note', fontName=CJK_FONT, fontSize=9.5, leading=14,
            textColor=TEXT_MID, spaceBefore=4, spaceAfter=8, wordWrap='CJK',
            leftIndent=15, rightIndent=10, borderColor=ACCENT, borderWidth=0,
            backColor=LIGHT_BG, borderPadding=8),
    }

S = get_styles()

# ── 特殊字符规范化 ────────────────────────────────────────
DASH_MAP = {
    '\u2010': '-', '\u2011': '-', '\u2012': '-', '\u2013': '-',
    '\u2014': '-', '\u2015': '-', '\u2212': '-', '\u00ad': '-',
}
def norm(t):
    for old, new in DASH_MAP.items():
        t = t.replace(old, new)
    return t

# ── 分隔线 Flowable ───────────────────────────────────────
class Divider(Flowable):
    def __init__(self, width, height=2, color=ACCENT, sb=4, sa=10):
        Flowable.__init__(self)
        self.width = width
        self.height = height
        self.color = color
        self.spaceBefore = sb
        self.spaceAfter = sa
    def draw(self):
        self.canv.setFillColor(self.color)
        self.canv.rect(0, 0, self.width, self.height, fill=1, stroke=0)

def title_div():
    return Divider(CONTENT_W, height=3, color=ACCENT, sa=16)
def h1_div():
    return Divider(CONTENT_W * 0.35, height=2, color=ACCENT, sa=8)
def subtle_div():
    return Divider(CONTENT_W, height=1, color=GRID_COLOR, sa=8)

# ── 表格构建辅助 ──────────────────────────────────────────
def make_table(data, col_widths=None, is_long=False):
    wrapped = []
    for i, row in enumerate(data):
        style = S['table_header'] if i == 0 else S['table_body']
        wrapped.append([Paragraph(str(c), style) for c in row])
    TC = LongTable if is_long else Table
    t = TC(wrapped, colWidths=col_widths, repeatRows=1 if is_long else 0)
    t.setStyle([
        ('BACKGROUND', (0,0), (-1,0), ACCENT),
        ('TEXTCOLOR', (0,0), (-1,0), colors.white),
        ('FONTNAME', (0,0), (-1,-1), CJK_FONT),
        ('FONTSIZE', (0,0), (-1,0), 9.5),
        ('FONTSIZE', (0,1), (-1,-1), 9),
        ('ALIGN', (0,0), (-1,-1), 'CENTER'),
        ('VALIGN', (0,0), (-1,-1), 'MIDDLE'),
        ('BOTTOMPADDING', (0,0), (-1,0), 8),
        ('TOPPADDING', (0,0), (-1,0), 8),
        ('BOTTOMPADDING', (0,1), (-1,-1), 6),
        ('TOPPADDING', (0,1), (-1,-1), 6),
        ('ROWBACKGROUNDS', (0,1), (-1,-1), [LIGHT_BG, colors.white]),
        ('GRID', (0,0), (-1,-1), 0.5, GRID_COLOR),
    ])
    return t

def P(text, style_key='body'):
    return Paragraph(norm(text), S[style_key])

def B(text):
    """带缩进段落"""
    return Paragraph(norm(text), S['body_indent'])

def BL(text):
    """无序列表项"""
    return Paragraph(norm(f"  - {text}"), S['bullet'])

# ══════════════════════════════════════════════════════════
#  文档内容构建
# ══════════════════════════════════════════════════════════
story = []

# ── 封面 ──────────────────────────────────────────────────
story.append(Spacer(1, 1.2 * inch))
story.append(P("CBCT 三维测量与手术规划模块", 'title'))
story.append(P("需求规格说明书", 'subtitle'))
story.append(title_div())
story.append(Spacer(1, 0.3 * inch))
story.append(P("Product Requirements Document (PRD)", 'doc_info'))
story.append(P("Module: :cbctmeasure", 'doc_info'))
story.append(Spacer(1, 0.6 * inch))

# 封面信息表
cover_info = [
    ["文档编号", "DCMTK-PRD-006"],
    ["版本", "V1.0 (Draft)"],
    ["日期", "2026-09-29"],
    ["项目", "DcmtkDemo - Android 医疗影像全流程解决方案"],
    ["编写人", "AI 辅助生成 (基于项目架构 + 行业调研)"],
    ["审核状态", "待审查"],
    ["保密级别", "内部技术文档"],
]
cover_tbl = Table(
    [[Paragraph(f"<b>{r[0]}</b>", S['table_body']),
      Paragraph(r[1], S['table_body'])] for r in cover_info],
    colWidths=[1.5*inch, 3.5*inch]
)
cover_tbl.setStyle([
    ('FONTNAME', (0,0), (-1,-1), CJK_FONT),
    ('FONTSIZE', (0,0), (-1,-1), 10),
    ('ALIGN', (0,0), (0,-1), 'RIGHT'),
    ('ALIGN', (1,0), (1,-1), 'LEFT'),
    ('VALIGN', (0,0), (-1,-1), 'MIDDLE'),
    ('TOPPADDING', (0,0), (-1,-1), 6),
    ('BOTTOMPADDING', (0,0), (-1,-1), 6),
    ('LEFTPADDING', (1,0), (1,-1), 12),
    ('LINEBELOW', (0,0), (-1,-1), 0.5, GRID_COLOR),
])
story.append(cover_tbl)
story.append(PageBreak())

# ── 修订历史 ──────────────────────────────────────────────
story.append(P("修订历史", 'h1'))
story.append(h1_div())
rev_data = [
    ["版本", "日期", "修订人", "修订内容"],
    ["V1.0", "2026-09-29", "AI 辅助", "初稿：基于 cbctdeal 模块现有能力 + CBCT 行业应用调研，提出新模块需求"],
]
story.append(make_table(rev_data, [0.8*inch, 1.2*inch, 1.5*inch, 3.0*inch]))
story.append(Spacer(1, 0.3 * inch))

# ── 目录提示 ──────────────────────────────────────────────
story.append(P("目录概要", 'h2'))
story.append(subtle_div())
toc_items = [
    "1. 项目背景与目标",
    "2. 现有系统能力分析",
    "3. CBCT 行业应用调研摘要",
    "4. 新模块定位与范围",
    "5. 功能需求详述",
    "   5.1 三维测量工具",
    "   5.2 ROI 分割与三维裁剪",
    "   5.3 口腔手术规划",
    "   5.4 标注与标记系统",
    "   5.5 报告导出与 DICOM SR",
    "   5.6 AI 辅助分析 (Phase 2)",
    "6. 非功能需求",
    "7. 架构设计",
    "8. 技术实现方案",
    "9. 里程碑与交付计划",
    "10. 风险评估",
    "11. 验收标准",
    "附录 A: 术语表",
    "附录 B: 接口清单",
]
for item in toc_items:
    story.append(P(item, 'body'))
story.append(PageBreak())

# ══════════════════════════════════════════════════════════
#  1. 项目背景与目标
# ══════════════════════════════════════════════════════════
story.append(P("1. 项目背景与目标", 'h1'))
story.append(h1_div())

story.append(P("1.1 背景", 'h2'))
story.append(B(
    "DcmtkDemo 项目已实现 Android 平台上的 CBCT DICOM 序列解析、VR 三维体绘制与 MPR 三平面切面浏览，"
    "覆盖了从 PACS 通信到三维可视化的全链路功能。然而，当前 :cbctdeal 模块仅支持「查看」级别的能力，"
    "缺乏临床场景所需的测量、规划、标注与报告能力，无法满足口腔种植、正畸评估等实际诊疗需求。"
))
story.append(B(
    "根据行业调研，CBCT 在口腔种植术前规划（92% 以上三级口腔医院常规使用）、正畸三维诊断（使用率 79.4%）、"
    "耳鼻喉颞骨/鼻窦成像、骨科脊柱测量等场景中，三维测量和手术规划是核心高频功能。"
    "移动端 CBCT 应用正向轻量化、远程诊断、基层普及方向发展，亟需在移动端补齐这一能力缺口。"
))

story.append(P("1.2 目标", 'h2'))
story.append(B("本需求定义一个新的 Gradle 模块 <b>:cbctmeasure</b>，在不修改 :cbctdeal 核心渲染管线的前提下，扩展以下能力："))
story.append(BL("<b>三维测量</b>：距离、角度、面积、体积的精确测量，支持空间任意两点/三点/ROI"))
story.append(BL("<b>ROI 分割与裁剪</b>：基于 HU 阈值的三维 ROI 提取、空间裁剪盒、感兴趣区域隔离显示"))
story.append(BL("<b>口腔手术规划</b>：种植体模拟植入（位置/角度/深度）、牙弓弧线测量、神经管标识"))
story.append(BL("<b>标注标记</b>：2D/3D 空间标注点、线段、文字标记，与测量数据关联持久化"))
story.append(BL("<b>报告导出</b>：测量数据汇总报告、截图导出、DICOM SR（Structured Report）结构化报告生成"))
story.append(BL("<b>AI 辅助分析（Phase 2）</b>：牙齿自动分割、解剖标志点自动识别、种植体位推荐"))

story.append(P("1.3 价值主张", 'h2'))
val_data = [
    ["维度", "现状（仅 :cbctdeal）", "目标（+ :cbctmeasure）"],
    ["临床可用性", "仅三维可视化查看", "测量 + 规划 + 报告，具备初步临床决策支持能力"],
    ["目标用户", "技术开发者/演示", "口腔医生/正畸医生/耳鼻喉医生的实际诊疗辅助"],
    ["商业价值", "技术 Demo", "移动医生站/口腔阅片 App 的核心差异化功能"],
    ["技术壁垒", "VTK 渲染集成", "渲染 + 测量 + 规划 + AI 的全栈能力闭环"],
]
story.append(make_table(val_data, [1.0*inch, 2.2*inch, 3.3*inch]))
story.append(Spacer(1, 0.2*inch))

# ══════════════════════════════════════════════════════════
#  2. 现有系统能力分析
# ══════════════════════════════════════════════════════════
story.append(P("2. 现有系统能力分析", 'h1'))
story.append(h1_div())

story.append(P("2.1 :cbctdeal 模块现有能力", 'h2'))
story.append(B(
    ":cbctdeal 模块基于 VTK 9.1.0 和 DCMTK 3.6.9 实现，核心 C++ 类包括 CbctSeriesParser（序列解析）、"
    "CbctVtkRenderer（渲染管线）、CbctJniHelper（JNI 桥接），Kotlin 层包含 CbctVtkView（渲染载体）、"
    "CbctParseEngine（协程编排）、CbctJni / CbctVtkJni（JNI 接口）。"
))

cap_data = [
    ["能力项", "现有实现", "复用可行性"],
    ["序列解析", "双 Pass 多线程解析，JPEG/JPEG-LS 解压，Z 轴排序", "直接复用，Volume 指针共享"],
    ["VR 体绘制", "vtkSmartVolumeMapper GPU RayCast + 窗宽窗位传递函数", "渲染管线可注入测量 Overlay Actor"],
    ["MPR 切面", "vtkImageReslice 三轴切面 + 灰阶 LUT", "切面坐标可作为测量基准平面"],
    ["手势交互", "单指旋转/平移、双指缩放/平移", "需扩展为测量工具模式切换"],
    ["窗宽窗位", "实时 HU 域调节", "ROI 阈值分割可直接复用"],
    ["Surface 对接", "EGL 渲染窗口 + SurfaceView 生命周期", "测量 Actor 复用同一 RenderWindow"],
    ["Volume 指针", "Native 堆零拷贝 vtkImageData（float HU）", "测量计算直接从 Volume 采样 HU 值"],
]
story.append(make_table(cap_data, [1.0*inch, 2.5*inch, 3.0*inch], is_long=True))

story.append(P("2.2 现有能力缺口", 'h2'))
story.append(BL("无任何测量工具（距离/角度/体积），无法进行量化评估"))
story.append(BL("无 ROI 分割能力，无法隔离特定解剖结构（如单颗牙齿、下颌神经管）"))
story.append(BL("无手术规划工具（种植体模拟、正畸分析），临床决策支持为零"))
story.append(BL("无标注/标记持久化机制，测量数据随会话结束丢失"))
story.append(BL("无报告导出能力，无法生成可归档的临床文档"))
story.append(BL("无 AI 辅助能力，全靠医生手工操作"))

story.append(P("2.3 复用策略", 'h2'))
story.append(B(
    "新模块 :cbctmeasure 作为 :cbctdeal 的<b>扩展层</b>，通过以下方式复用现有能力："
))
story.append(BL("<b>Volume 共享</b>：直接使用 CbctJni.loadSeries() 返回的 Volume 指针，从 vtkImageData 采样 HU 值进行测量计算"))
story.append(BL("<b>RenderWindow 注入</b>：在现有 CbctVtkRenderer 的 vtkRenderWindow 中追加测量/标注 Actor，无需独立渲染管线"))
story.append(BL("<b>手势框架扩展</b>：扩展现有 rotate/pan/zoom 手势处理，增加「测量模式」状态机切换"))
story.append(BL("<b>窗宽窗位复用</b>：ROI 阈值分割直接复用 setWindowLevel 的 HU 域参数"))

story.append(PageBreak())

# ══════════════════════════════════════════════════════════
#  3. CBCT 行业应用调研摘要
# ══════════════════════════════════════════════════════════
story.append(P("3. CBCT 行业应用调研摘要", 'h1'))
story.append(h1_div())

story.append(P("3.1 口腔/牙科应用", 'h2'))
story.append(B(
    "CBCT 在口腔领域的应用最为成熟。种植牙术前规划方面，92% 以上三级口腔医院常规使用 CBCT 评估牙槽骨高度、"
    "宽度和密度，显著提升种植成功率。正畸三维诊断使用率达 79.4%，用于牙弓弧度测量、牙齿排列分析。"
    "牙体牙髓病诊断中，CBCT 可清晰显示根管形态和根尖周病变。牙周病评估中可量化牙槽骨吸收程度。"
))

story.append(P("3.2 耳鼻喉(ENT)应用", 'h2'))
story.append(B(
    "CBCT 聚焦颞骨、鼻窦及颅底精细成像，用于慢性鼻窦炎、中耳炎、听神经瘤等术前评估。"
    "三甲医院使用率超 55%，但基层医院普及不足 20%，移动端 CBCT 应用有巨大下沉空间。"
))

story.append(P("3.3 骨科/脊柱应用", 'h2'))
story.append(B(
    "立位锥束 CT 支持髋/膝/颈椎/腰椎/足踝三维重建，实现全脊柱和全下肢扫描。"
    "集成自动脊柱分割、椎体配准、曲度分析、椎弓根测量等后处理模块，是移动端骨科阅片的核心需求。"
))

story.append(P("3.4 技术趋势", 'h2'))
trend_data = [
    ["趋势方向", "关键指标", "对本模块的启示"],
    ["低剂量成像", "0.06mGy 即可达标准剂量图像质量 (uAI-Enhance 3.0)", "预处理降噪可复用 :rawpixeldeal 的双边滤波"],
    ["AI 辅助诊断", "牙齿自动分割 F1=0.881，标志点识别准确率 92%", "Phase 2 引入 ONNX Runtime 推理"],
    ["三维测量", "距离/角度/体积/表面积，STL 格式输出", "核心测量功能需覆盖全部基础维度"],
    ["多源融合", "CBCT + 口内扫描 STL 配准", "Phase 3 可扩展 STL 导入与配准"],
    ["移动端趋势", "轻量化设计、无线传输、云存储/远程诊断", "本模块面向 Android 移动端，需控制内存与功耗"],
]
story.append(make_table(trend_data, [1.2*inch, 2.0*inch, 3.3*inch], is_long=True))

story.append(PageBreak())

# ══════════════════════════════════════════════════════════
#  4. 新模块定位与范围
# ══════════════════════════════════════════════════════════
story.append(P("4. 新模块定位与范围", 'h1'))
story.append(h1_div())

story.append(P("4.1 模块定位", 'h2'))
story.append(B(
    ":cbctmeasure 是 :cbctdeal 的<b>功能扩展模块</b>，而非独立渲染模块。"
    "它不创建新的 Volume 或 RenderWindow，而是在已有的 CbctVtkRenderer 渲染管线上叠加测量/标注 Actor，"
    "在已有的 Volume 指针上进行 HU 值采样计算。架构上遵循项目现有的三层解耦原则："
    "JNI 桥接层只做类型转换，业务逻辑层使用纯 C++ 类型。"
))

story.append(P("4.2 In Scope / Out of Scope", 'h2'))
scope_data = [
    ["分类", "功能项", "说明"],
    ["In Scope", "三维点距/角度/体积测量", "基于 Volume HU 采样的空间测量"],
    ["In Scope", "ROI 阈值分割与裁剪盒", "HU 阈值分割 + 空间 Box/Plane 裁剪"],
    ["In Scope", "种植体模拟植入", "位置/角度/深度/直径可调的虚拟种植体"],
    ["In Scope", "牙弓弧线测量", "弧线上等距采样点 + 弧长计算"],
    ["In Scope", "神经管/重要解剖标识", "路径标记与安全距离警告"],
    ["In Scope", "2D/3D 标注与标记", "文字/线段/箭头标注，与测量数据关联"],
    ["In Scope", "测量报告导出", "截图 + 数据表 + DICOM SR 结构化报告"],
    ["In Scope", "测量数据本地持久化", "JSON 格式，按 Study/Series UID 关联"],
    ["Phase 2", "AI 牙齿自动分割", "ONNX Runtime 推理，F1 >= 0.85"],
    ["Phase 2", "解剖标志点自动识别", " CNN 模型，准确率 >= 90%"],
    ["Phase 2", "种植体位 AI 推荐", "基于骨密度/神经管距离的智能推荐"],
    ["Out of Scope", "STL 导入与配准", "Phase 3 扩展"],
    ["Out of Scope", "云端远程协作", "独立云服务模块"],
    ["Out of Scope", "DICOM 网络传输报告", "由 :dcmtk 模块扩展"],
]
story.append(make_table(scope_data, [1.0*inch, 2.5*inch, 3.0*inch], is_long=True))

story.append(P("4.3 用户角色", 'h2'))
role_data = [
    ["角色", "使用场景", "核心诉求"],
    ["口腔种植医生", "术前规划种植体位置/角度/深度", "种植体模拟 + 骨密度评估 + 神经管避让"],
    ["正畸医生", "三维诊断牙颌畸形", "牙弓弧线测量 + 牙齿排列分析 + 标志点"],
    ["耳鼻喉医生", "颞骨/鼻窦术前评估", "MPR 测量 + ROI 裁剪 + 距离/角度"],
    ["骨科医生", "脊柱/四肢测量", "椎体测量 + 曲度分析 + 椎弓根参数"],
    ["影像科技师", "阅片标注与报告", "标注标记 + 截图导出 + 结构化报告"],
]
story.append(make_table(role_data, [1.2*inch, 2.3*inch, 3.0*inch]))

story.append(PageBreak())

# ══════════════════════════════════════════════════════════
#  5. 功能需求详述
# ══════════════════════════════════════════════════════════
story.append(P("5. 功能需求详述", 'h1'))
story.append(h1_div())

# ── 5.1 三维测量工具 ────────────────────────────────────
story.append(P("5.1 三维测量工具", 'h2'))
story.append(subtle_div())

story.append(P("5.1.1 功能描述", 'h3'))
story.append(B(
    "在 VR 三维视图和 MPR 切面上提供交互式测量工具。用户通过触摸手势在三维空间或切面上选取测量基准点，"
    "系统实时计算并显示测量结果。测量结果以 3D 文字标签的形式叠加在渲染场景中，同时记录到测量数据列表。"
))

story.append(P("5.1.2 测量类型", 'h3'))
measure_types = [
    ["编号", "测量类型", "输入", "输出", "精度要求"],
    ["M-01", "两点距离", "2 个空间点 P1, P2", "欧氏距离 (mm)", "<= 0.1mm"],
    ["M-02", "三点角度", "3 个空间点 A, B, C", "BAC 夹角 (度)", "<= 0.5 度"],
    ["M-03", "点到线距离", "点 P + 线段 AB", "P 到 AB 的垂直距离 (mm)", "<= 0.1mm"],
    ["M-04", "ROI 体积", "HU 阈值范围 + 空间裁剪盒", "体素体积 (cm3)", "<= 1%"],
    ["M-05", "ROI 面积", "MPR 切面封闭路径", "截面面积 (cm2)", "<= 1%"],
    ["M-06", "弧线长度", "弧线采样点序列", "弧长 (mm)", "<= 0.5mm"],
    ["M-07", "HU 值采样", "单点坐标", "HU 值 + 组织类型推断", "精确值"],
    ["M-08", "骨密度评估", "ROI 内体素 HU 统计", "均值/标准差/最大/最小 HU", "精确值"],
]
story.append(make_table(measure_types, [0.5*inch, 1.2*inch, 1.8*inch, 1.8*inch, 1.2*inch]))

story.append(P("5.1.3 交互流程", 'h3'))
story.append(BL("<b>进入测量模式</b>：从工具栏选择测量类型（距离/角度/体积/HU 采样），进入对应测量模式"))
story.append(BL("<b>选取基准点</b>：在 VR 或 MPR 视图上触摸点按，通过 vtkCellPicker / vtkPointPicker 将屏幕坐标映射为 Volume 空间坐标"))
story.append(BL("<b>实时反馈</b>：每选取一个点即显示标记 Sphere Actor；满足最小输入点数后实时计算并显示结果"))
story.append(BL("<b>结果确认/取消</b>：双击确认测量结果并添加到列表；长按取消当前测量"))
story.append(BL("<b>结果列表管理</b>：侧滑面板展示所有测量项，支持删除、重命名、颜色标记、跳转定位"))

story.append(P("5.1.4 技术要点", 'h3'))
story.append(B(
    "空间坐标拾取使用 VTK 的 vtkCellPicker，将触摸屏幕坐标映射为 Volume 世界坐标。"
    "由于 Android 触摸事件无鼠标悬停，采用「点按 -> 高亮预选 -> 再点按确认」两步交互。"
    "MPR 模式下可直接从 vtkImageReslice 的输出平面拾取像素坐标，再逆映射到 Volume 坐标系。"
    "距离/角度计算在 Native C++ 层完成（保证浮点精度），结果通过 JNI 回调到 Kotlin 层显示。"
))

story.append(PageBreak())

# ── 5.2 ROI 分割与三维裁剪 ──────────────────────────────
story.append(P("5.2 ROI 分割与三维裁剪", 'h2'))
story.append(subtle_div())

story.append(P("5.2.1 功能描述", 'h3'))
story.append(B(
    "提供基于 HU 阈值的三维 ROI 提取和空间裁剪能力。用户设定 HU 阈值范围和空间裁剪盒，"
    "系统从 Volume 中提取满足条件的体素子集，以独立 Actor 渲染显示，支持隔离/透明度调节/导出。"
))

story.append(P("5.2.2 ROI 定义方式", 'h3'))
roi_defs = [
    ["编号", "定义方式", "参数", "适用场景"],
    ["R-01", "HU 阈值范围", "[HU_min, HU_max]", "骨骼分割 (>200HU)、软组织 (0~100HU)"],
    ["R-02", "空间裁剪盒", "Box: xmin,ymin,zmin, xmax,ymax,zmax", "提取特定解剖区域"],
    ["R-03", "平面裁剪", "Plane: origin + normal", "冠状/矢状/任意平面切割"],
    ["R-04", "球面 ROI", "Center + Radius", "种植体周围骨密度评估"],
    ["R-05", "组合 ROI", "R-01 AND R-02 AND NOT R-03", "骨骼裁剪盒内减去裁剪平面以下区域"],
]
story.append(make_table(roi_defs, [0.5*inch, 1.5*inch, 2.2*inch, 2.3*inch]))

story.append(P("5.2.3 交互流程", 'h3'))
story.append(BL("<b>创建 ROI</b>：从工具栏选择 ROI 类型（阈值/裁剪盒/平面/球面），设定参数"))
story.append(BL("<b>实时预览</b>：参数调节时实时更新 ROI Actor 的颜色/透明度/可见性"))
story.append(BL("<b>隔离显示</b>：可隐藏原始 Volume，仅显示 ROI 子集"))
story.append(BL("<b>ROI 内测量</b>：ROI 确定后可在其范围内执行 M-04 体积测量 / M-08 骨密度评估"))

story.append(P("5.2.4 技术要点", 'h3'))
story.append(B(
    "HU 阈值分割复用现有 VR 的 Opacity Transfer Function 机制，将阈值范围外体素不透明度设为 0。"
    "空间裁剪盒使用 vtkBox/vtkPlane 作为 vtkSmartVolumeMapper 的 ClippingPlanes。"
    "ROI 体积计算通过对 Volume 内满足条件的体素计数乘以体素间距获得（Spacing[0] * Spacing[1] * Spacing[2]）。"
    "骨密度统计直接在 CbctSeriesParser 的 float HU 数组上遍历计算均值/标准差。"
))

story.append(PageBreak())

# ── 5.3 口腔手术规划 ────────────────────────────────────
story.append(P("5.3 口腔手术规划", 'h2'))
story.append(subtle_div())

story.append(P("5.3.1 功能描述", 'h3'))
story.append(B(
    "针对口腔种植和正畸场景提供专用规划工具。种植体模拟支持位置/角度/深度/直径参数化配置，"
    "实时显示种植体在骨骼中的三维位置，并标注与下颌神经管的安全距离。"
    "正畸评估支持牙弓弧线测量、牙齿排列角度分析。"
))

story.append(P("5.3.2 种植体规划功能", 'h3'))
implant_data = [
    ["编号", "功能", "参数", "约束/警告"],
    ["S-01", "种植体定位", "位置(x,y,z) + 角度(俯仰/偏转) + 深度(mm) + 直径(mm)", "骨高度 >= 10mm 为安全"],
    ["S-02", "种植体可视化", "圆柱体 Actor + 螺纹纹理", "红色=不安全, 黄色=临界, 绿色=安全"],
    ["S-03", "骨高度测量", "种植体轴线上皮质骨顶到底的距离", "< 10mm 需警告"],
    ["S-04", "骨宽度测量", "种植体植入处颊舌向骨宽度", "< 6mm 需警告"],
    ["S-05", "神经管距离", "种植体尖端到下颌神经管的最短距离", "< 2mm 红色警告"],
    ["S-06", "多种植体方案", "支持同时放置多颗种植体，列表管理", "最小间距 >= 3mm"],
]
story.append(make_table(implant_data, [0.5*inch, 1.3*inch, 2.2*inch, 2.5*inch]))

story.append(P("5.3.3 正畸评估功能", 'h3'))
story.append(BL("<b>S-07 牙弓弧线绘制</b>：在 MPR 冠状面上手动描记牙弓弧线，自动计算弧长"))
story.append(BL("<b>S-08 牙齿排列角度</b>：选取相邻牙齿中线，计算排列角度偏差"))
story.append(BL("<b>S-09 中线偏移测量</b>：上颌/下颌中线偏移距离"))
story.append(BL("<b>S-10 覆覆盖测量</b>：切牙覆覆盖距离（需正侧位切面配合）"))

story.append(P("5.3.4 神经管标识", 'h3'))
story.append(B(
    "下颌神经管（Inferior Alveolar Canal）是种植手术的关键安全解剖结构。"
    "用户在 MPR 矢状面上逐层标记神经管路径点，系统以管道 Actor（vtkTubeFilter）连接，"
    "计算种植体尖端到管道的最短距离，小于安全阈值（默认 2mm）时以红色高亮警告。"
))

story.append(P("5.3.5 技术要点", 'h3'))
story.append(B(
    "种植体可视化使用 vtkCylinderSource 生成圆柱几何 + vtkTransform 定位。"
    "安全距离计算使用 VTK 的 vtkImplicitPolyDataDistance 或点到线段/管道的距离公式。"
    "神经管路径使用 vtkTubeFilter 将折线转换为管道几何体。"
    "正畸弧线测量使用累积弦长法。"
))

story.append(PageBreak())

# ── 5.4 标注与标记系统 ──────────────────────────────────
story.append(P("5.4 标注与标记系统", 'h2'))
story.append(subtle_div())

story.append(P("5.4.1 功能描述", 'h3'))
story.append(B(
    "提供 2D/3D 空间的标注和标记能力。标注类型包括文字标签、线段、箭头、自由曲线和环形标记。"
    "所有标注与测量数据统一管理，支持颜色/可见性/名称设置，并随测量数据一同持久化。"
))

story.append(P("5.4.2 标注类型", 'h3'))
anno_data = [
    ["编号", "标注类型", "维度", "描述"],
    ["A-01", "文字标签", "3D", "绑定到空间点的文字注释"],
    ["A-02", "线段标注", "3D", "两点连线 + 长度标注"],
    ["A-03", "箭头标注", "3D", "线段 + 箭头终点"],
    ["A-04", "自由曲线", "3D", "手动描记的折线路径"],
    ["A-05", "环形标记", "3D", "圆形/椭圆标记区域"],
    ["A-06", "MPR 截面标注", "2D", "在切面图像上绘制的文字/线段"],
    ["A-07", "截图标注", "2D", "在截图上叠加的矩形/文字"],
]
story.append(make_table(anno_data, [0.5*inch, 1.3*inch, 0.6*inch, 4.1*inch]))

story.append(P("5.4.3 交互与持久化", 'h3'))
story.append(BL("<b>创建标注</b>：工具栏选择标注类型 -> 在视图上描记 -> 输入文字（可选）-> 确认"))
story.append(BL("<b>编辑标注</b>：长按选中 -> 拖动端点修改 -> 修改文字/颜色"))
story.append(BL("<b>持久化格式</b>：JSON 序列化，包含标注类型、坐标、文字、颜色、关联测量 ID"))
story.append(BL("<b>存储位置</b>：应用内部存储 /data/data/.../measurements/{StudyUID}_{SeriesUID}.json"))

story.append(PageBreak())

# ── 5.5 报告导出与 DICOM SR ────────────────────────────
story.append(P("5.5 报告导出与 DICOM SR", 'h2'))
story.append(subtle_div())

story.append(P("5.5.1 功能描述", 'h3'))
story.append(B(
    "将测量结果、标注数据和截图汇总为结构化报告，支持导出为 PDF（图文报告）和 DICOM SR（结构化报告）两种格式。"
    "报告内容包含患者信息（从 DICOM 元数据提取）、测量数据表、截图和医生签名栏。"
))

story.append(P("5.5.2 报告内容结构", 'h3'))
report_data = [
    ["章节", "内容", "数据来源"],
    ["患者信息", "姓名/性别/年龄/Study UID/Series UID", "CbctSeriesMeta"],
    ["扫描参数", "体素间距/层数/扫描日期/设备型号", "DICOM Tag (0028,0030)等"],
    ["测量数据表", "编号/类型/数值/单位/备注", "测量列表"],
    ["截图列表", "VR 截图 + MPR 截图（含标注）", "vtkWindowToImageFilter"],
    ["种植体方案", "位置/角度/深度/直径/安全评估", "S-01~S-06"],
    ["医生备注", "自由文本", "UI 输入"],
    ["生成时间", "时间戳 + 操作者", "系统"],
]
story.append(make_table(report_data, [1.2*inch, 2.5*inch, 2.8*inch]))

story.append(P("5.5.3 DICOM SR 结构化报告", 'h3'))
story.append(B(
    "DICOM SR (Structured Report) 是医学影像领域的标准化测量报告格式。"
    "本模块使用 DCMTK 的 DSRDocument 类生成 SR 文档，TID (Template ID) 采用基本测量报告模板。"
    "SR 文档以 DICOM 文件形式存储，可通过 :dcmtk 模块的 C-STORE 上传至 PACS，实现测量数据的临床归档。"
))

story.append(P("5.5.4 技术要点", 'h3'))
story.append(BL("<b>截图</b>：vtkWindowToImageFilter -> vtkPNGWriter 生成 PNG，分辨率随设备 DPI 自适应"))
story.append(BL("<b>PDF 报告</b>：Android 端使用 PdfDocument API 生成，内嵌截图 + 数据表 + 文字"))
story.append(BL("<b>DICOM SR</b>：DCMTK DSRDocument + DSRContentItem 构建树形测量内容"))
story.append(BL("<b>SR 上传</b>：复用 :dcmtk 模块的 ProgressScu C-STORE 流程"))

story.append(PageBreak())

# ── 5.6 AI 辅助分析 (Phase 2) ──────────────────────────
story.append(P("5.6 AI 辅助分析 (Phase 2)", 'h2'))
story.append(subtle_div())

story.append(P("5.6.1 功能描述", 'h3'))
story.append(B(
    "Phase 2 引入 AI 辅助能力，包括牙齿自动分割、解剖标志点自动识别和种植体位智能推荐。"
    "模型推理使用 ONNX Runtime Android 版本（onnxruntime-android），在设备端离线推理，"
    "不依赖网络，保证数据隐私和实时性。"
))

story.append(P("5.6.2 AI 功能清单", 'h3'))
ai_data = [
    ["编号", "AI 功能", "模型类型", "输入", "输出", "性能目标"],
    ["AI-01", "牙齿自动分割", "3D U-Net (ONNX)", "Volume HU 数据", "体素级分割标签", "F1 >= 0.85"],
    ["AI-02", "解剖标志点识别", "Point Regression CNN", "Volume HU 数据", "关键点 3D 坐标", "准确率 >= 90%"],
    ["AI-03", "种植体位推荐", "Rule + ML 混合", "骨密度 + 神经管路径", "推荐位置/角度/深度", "安全距离满足率 >= 95%"],
    ["AI-04", "金属伪影校正", "U-Net 2D (ONNX)", "MPR 切面图", "校正后图像", "PSNR 提升 >= 10dB"],
]
story.append(make_table(ai_data, [0.5*inch, 1.5*inch, 1.3*inch, 1.2*inch, 1.2*inch, 0.8*inch]))

story.append(P("5.6.3 技术要点", 'h3'))
story.append(B(
    "ONNX Runtime Android (onnxruntime-android) 支持 arm64-v8a，模型文件(.onnx)放置在 assets 中，"
    "首次使用时释放到内部存储。推理在子线程执行，通过 JNI 回调到 Kotlin 层。"
    "3D U-Net 模型输入需要将 Volume 降采样至固定尺寸（如 128x128x128），推理后上采样回原始尺寸。"
    "推理结果作为新的 vtkImageData 标签 Volume，使用 vtkColorTransferFunction 着色叠加显示。"
))

story.append(PageBreak())

# ══════════════════════════════════════════════════════════
#  6. 非功能需求
# ══════════════════════════════════════════════════════════
story.append(P("6. 非功能需求", 'h1'))
story.append(h1_div())

nfr_data = [
    ["类别", "需求项", "指标/约束", "依据"],
    ["性能", "测量计算延迟", "<= 50ms (从点按到结果显示)", "实时交互体感"],
    ["性能", "ROI 分割渲染帧率", ">= 30 FPS (参数调节时)", "流畅交互"],
    ["性能", "截图生成", "<= 500ms (VR + MPR 三平面)", "报告导出体感"],
    ["性能", "AI 推理延迟 (Phase 2)", "<= 5s (单次分割推理)", "临床可接受等待"],
    ["内存", "模块额外内存占用", "<= 100MB (测量 Actor + 标注数据)", "Android 内存限制"],
    ["内存", "AI 模型加载", "<= 80MB (ONNX 模型 + Runtime)", "不挤占渲染内存"],
    ["精度", "距离测量误差", "<= 0.1mm (相对于 DICOM Spacing)", "临床精度要求"],
    ["精度", "角度测量误差", "<= 0.5 度", "临床精度要求"],
    ["精度", "体积测量误差", "<= 1%", "临床可接受范围"],
    ["兼容", "ABI", "arm64-v8a (与 :cbctdeal 一致)", "架构一致性"],
    ["兼容", "minSdk", "24 (与 :cbctdeal 一致)", "设备覆盖率"],
    ["兼容", "OpenGL ES", "3.0+ (与现有渲染一致)", "GPU raycast 依赖"],
    ["存储", "测量数据文件大小", "<= 1MB / Study (JSON)", "本地存储约束"],
    ["存储", "报告文件大小", "<= 10MB (含截图)", "可分享性"],
    ["安全", "数据存储", "全部本地存储，无外部上传", "用户隐私约束"],
    ["安全", "AI 推理", "完全离线，不传输数据", "数据隐私"],
    ["可用性", "学习曲线", "5 步内完成一次种植体规划", "移动端简洁交互"],
]
story.append(make_table(nfr_data, [0.7*inch, 1.8*inch, 2.5*inch, 2.0*inch], is_long=True))

story.append(PageBreak())

# ══════════════════════════════════════════════════════════
#  7. 架构设计
# ══════════════════════════════════════════════════════════
story.append(P("7. 架构设计", 'h1'))
story.append(h1_div())

story.append(P("7.1 模块依赖关系", 'h2'))
story.append(B(
    ":cbctmeasure 依赖 :cbctdeal（共享 Volume 指针和 RenderWindow），可选依赖 :dcmtk（DICOM SR 上传）。"
    "不依赖 :rawpixeldeal（预处理由 :cbctdeal 内部完成）。"
))
dep_text = """
<b>依赖方向：</b><br/>
  :app --> :cbctmeasure --> :cbctdeal --> dcmtk (static lib) + vtk (static lib)<br/>
  :cbctmeasure --> :dcmtk (optional, for SR C-STORE)<br/><br/>
<b>禁止反向依赖：</b> :cbctdeal 不感知 :cbctmeasure 的存在，扩展层向下单向依赖。
"""
story.append(Paragraph(norm(dep_text), S['note']))

story.append(P("7.2 分层架构", 'h2'))
arch_data = [
    ["层", "目录", "职责", "技术"],
    ["UI 层", "app/.../measure/", "Fragment + ViewModel + 工具栏 + 列表面板", "Kotlin + Coroutines"],
    ["JNI 桥接层", "cbctmeasure/.../jni/", "类型转换 + Native 方法注册", "C++ + JNIEnv"],
    ["业务逻辑层", "cbctmeasure/.../cpp/core/", "测量计算 + ROI 分割 + 规划算法", "纯 C++ (VTK)"],
    ["渲染扩展层", "cbctmeasure/.../cpp/render/", "测量 Actor + 标注 Actor + Picker", "C++ (VTK Actor)"],
    ["数据层", "cbctmeasure/.../data/", "JSON 持久化 + 报告生成", "Kotlin + DCMTK SR"],
    ["AI 层 (P2)", "cbctmeasure/.../ai/", "ONNX 推理 + 后处理", "C++ (ONNX Runtime)"],
]
story.append(make_table(arch_data, [1.0*inch, 1.8*inch, 2.2*inch, 1.5*inch]))

story.append(P("7.3 关键类设计", 'h2'))
class_data = [
    ["类名", "语言", "继承/依赖", "核心方法"],
    ["MeasureJni", "Kotlin", "object", "startMeasure / pickPoint / getResult / clearMeasure"],
    ["RoiJni", "Kotlin", "object", "createRoi / setThreshold / setClipBox / setVisibility"],
    ["SurgeryPlanJni", "Kotlin", "object", "placeImplant / setNervePath / getSafetyReport"],
    ["AnnotationJni", "Kotlin", "object", "addLabel / addLine / addArrow / removeAnnotation"],
    ["ReportGenerator", "Kotlin", "class", "generatePdf / generateSr / generateScreenshot"],
    ["MeasurementManager", "C++", "依赖 vtkImageData", "addMeasurement / calculate / serialize"],
    ["RoiExtractor", "C++", "依赖 vtkSmartVolumeMapper", "extractByHu / extractByBox / extractByPlane"],
    ["ImplantPlacer", "C++", "依赖 vtkCylinderSource", "place / update / computeSafetyDistance"],
    ["AnnotationRenderer", "C++", "依赖 vtkRenderer", "addLabel / addLine / render / clear"],
    ["MeasurePicker", "C++", "依赖 vtkCellPicker", "pick / worldToIndex / indexToWorld"],
    ["AiInferenceEngine", "C++", "依赖 onnxruntime", "loadModel / infer / postProcess"],
]
story.append(make_table(class_data, [1.5*inch, 0.5*inch, 1.8*inch, 2.7*inch], is_long=True))

story.append(PageBreak())

# ══════════════════════════════════════════════════════════
#  8. 技术实现方案
# ══════════════════════════════════════════════════════════
story.append(P("8. 技术实现方案", 'h1'))
story.append(h1_div())

story.append(P("8.1 CMakeLists.txt 扩展", 'h2'))
story.append(B(
    "新模块 :cbctmeasure 的 CMakeLists.txt 需链接 VTK 静态库（复用 :cbctdeal 已入库的 libvtk*.a），"
    "包含 VTK 头文件路径。不引入新的第三方依赖（Phase 1），Phase 2 增加 ONNX Runtime。"
))
cmake_deps = """
<b>链接库（Phase 1）：</b><br/>
  libvtkCommonCore.a, libvtkRenderingCore.a, libvtkRenderingOpenGL2.a,<br/>
  libvtkFiltersGeneral.a, libvtkFiltersSources.a (vtkCylinderSource/vtkTubeFilter),<br/>
  libvtkImagingCore.a (vtkImageReslice for MPR picking),<br/>
  android, EGL, GLESv3 (复用 :cbctdeal 的系统库链接)<br/><br/>
<b>链接库（Phase 2 追加）：</b><br/>
  libonnxruntime.so (ONNX Runtime Android, arm64-v8a)
"""
story.append(Paragraph(norm(cmake_deps), S['note']))

story.append(P("8.2 VTK 渲染管线扩展", 'h2'))
story.append(B(
    "现有 CbctVtkRenderer 在 create() 时创建了 vtkRenderer 和 vtkRenderWindow。"
    "新模块的 AnnotationRenderer 和 MeasureActor 通过 JNI 获取已有的 renderer 指针，"
    "在其上 AddViewProp() 追加测量/标注 Actor，不创建新的渲染窗口。"
    "渲染线程由现有 CbctVtkRenderer 的 renderLoop 驱动，测量 Actor 修改后触发 Modified() 即可自动重绘。"
))

story.append(P("8.3 坐标拾取实现", 'h2'))
story.append(B(
    "Android 触摸坐标 -> vtkRenderWindow::SetEventPosition() -> vtkCellPicker::Pick() -> "
    "vtkCellPicker->GetPickPosition() 获取世界坐标。"
    "MPR 模式下需要将切面世界坐标逆映射回 Volume 原始坐标（考虑 vtkImageReslice 的变换矩阵）。"
    "为提升拾取精度，可设置 vtkCellPicker 的 Tolerance 和 PickFromList。"
))

story.append(P("8.4 数据持久化方案", 'h2'))
persist_data = [
    ["数据类型", "格式", "存储路径", "命名规则"],
    ["测量数据", "JSON", "filesDir/measurements/", "{StudyUID}_{SeriesUID}_measure.json"],
    ["标注数据", "JSON", "filesDir/annotations/", "{StudyUID}_{SeriesUID}_anno.json"],
    ["规划方案", "JSON", "filesDir/plans/", "{StudyUID}_{SeriesUID}_plan.json"],
    ["截图", "PNG", "filesDir/screenshots/", "{StudyUID}_{timestamp}.png"],
    ["PDF 报告", "PDF", "getExternalFilesDir()/reports/", "{StudyUID}_{timestamp}.pdf"],
    ["DICOM SR", "DCM", "getExternalFilesDir()/sr/", "{StudyUID}_{timestamp}.dcm"],
]
story.append(make_table(persist_data, [1.0*inch, 0.7*inch, 2.0*inch, 2.8*inch]))

story.append(P("8.5 手势状态机扩展", 'h2'))
story.append(B(
    "现有手势处理仅有「旋转/平移/缩放」三种。扩展为状态机模式："
))
state_text = """
<b>STATE_VIEW</b> (默认): rotate/pan/zoom -- 浏览模式<br/>
<b>STATE_MEASURE_DISTANCE</b>: 单指点按选点 -> 双击确认 -- 距离测量<br/>
<b>STATE_MEASURE_ANGLE</b>: 依次点按 3 个点 -> 双击确认 -- 角度测量<br/>
<b>STATE_MEASURE_VOLUME</b>: 拖拽定义裁剪盒 -> 双击确认 -- 体积测量<br/>
<b>STATE_ROI_EDIT</b>: 拖拽定义 ROI 参数 -> 双击确认 -- ROI 编辑<br/>
<b>STATE_IMPLANT_PLACE</b>: 点按种植位置 -> 拖拽调整角度 -> 松手确认 -- 种植体放置<br/>
<b>STATE_ANNOTATE</b>: 自由描记 -> 双击确认 -- 标注<br/>
<b>STATE_NERVE_TRACE</b>: 逐层点按神经管路径 -> 双击结束 -- 神经管标识<br/><br/>
<b>状态切换</b>：工具栏按钮 -> setState(mode) -> JNI 通知 Native 侧切换手势处理逻辑。<br/>
<b>取消</b>：长按 -> resetState() -> 清除当前操作的临时 Actor。
"""
story.append(Paragraph(norm(state_text), S['note']))

story.append(P("8.6 ONNX Runtime 集成方案 (Phase 2)", 'h2'))
story.append(BL("<b>依赖</b>：implementation 'com.microsoft.onnxruntime:onnxruntime-android:1.16+'"))
story.append(BL("<b>模型存储</b>：assets/models/ -> 首次释放到 filesDir/models/"))
story.append(BL("<b>推理线程</b>：Kotlin 协程 Dispatchers.Default -> JNI -> C++ Ortk::Session::Run()"))
story.append(BL("<b>输入预处理</b>：从 vtkImageData 提取 HU float 数组 -> 降采样 -> NCHW Tensor"))
story.append(BL("<b>输出后处理</b>：ArgMax -> 上采样 -> 新建标签 vtkImageData -> ColorTransferFunction 着色"))

story.append(PageBreak())

# ══════════════════════════════════════════════════════════
#  9. 里程碑与交付计划
# ══════════════════════════════════════════════════════════
story.append(P("9. 里程碑与交付计划", 'h1'))
story.append(h1_div())

story.append(P("9.1 分期交付策略", 'h2'))
story.append(B(
    "新模块按 <b>Phase 1 (核心测量 + 规划)</b> 和 <b>Phase 2 (AI 辅助)</b> 两期交付。"
    "Phase 1 聚焦测量/ROI/规划/标注/报告的基础能力闭环，Phase 2 叠加 AI 能力。"
))

milestone_data = [
    ["阶段", "里程碑", "交付内容", "预估工期"],
    ["Phase 1 - M1", "模块脚手架 + 手势状态机", "Gradle 模块创建 + CMakeLists + JNI 注册 + 状态机框架", "1 周"],
    ["Phase 1 - M2", "三维测量工具", "M-01~M-08 全部测量类型 + Picker + 结果列表 UI", "2 周"],
    ["Phase 1 - M3", "ROI 分割与裁剪", "R-01~R-05 + 阈值/裁剪盒/平面/球面 + 实时预览", "1.5 周"],
    ["Phase 1 - M4", "种植体规划", "S-01~S-06 + 圆柱可视化 + 安全距离 + 神经管标识", "2 周"],
    ["Phase 1 - M5", "标注系统", "A-01~A-07 + JSON 持久化 + 编辑/删除", "1 周"],
    ["Phase 1 - M6", "报告导出", "PDF 报告 + 截图 + DICOM SR 生成", "1.5 周"],
    ["Phase 1 - M7", "集成测试 + 优化", "性能优化 + 内存测试 + 真机验证 + 文档", "1 周"],
    ["Phase 2 - M8", "ONNX Runtime 集成", "依赖引入 + 模型加载 + 推理框架 + JNI 桥接", "1 周"],
    ["Phase 2 - M9", "牙齿自动分割", "AI-01 模型训练/转换 + 推理 + 标签着色", "2 周"],
    ["Phase 2 - M10", "标志点识别 + 种植推荐", "AI-02 + AI-03 + 后处理 + UI 集成", "2 周"],
    ["Phase 2 - M11", "AI 集成测试 + 优化", "推理性能优化 + 模型量化 + 真机验证", "1 周"],
]
story.append(make_table(milestone_data, [1.2*inch, 1.5*inch, 2.5*inch, 0.8*inch], is_long=True))

story.append(P("9.2 总工期估算", 'h2'))
story.append(B(
    "<b>Phase 1</b>：约 10 周（含 1 周集成测试）<br/>"
    "<b>Phase 2</b>：约 6 周（含 1 周集成测试）<br/>"
    "<b>总计</b>：约 16 周（4 个月），可并行缩短至 12 周"
))

story.append(PageBreak())

# ══════════════════════════════════════════════════════════
#  10. 风险评估
# ══════════════════════════════════════════════════════════
story.append(P("10. 风险评估", 'h1'))
story.append(h1_div())

risk_data = [
    ["编号", "风险", "概率", "影响", "缓解措施"],
    ["R-01", "VTK Picker 在 GLES3 上拾取精度不足", "中", "高", "实现自定义射线-体素交叉算法替代"],
    ["R-02", "测量 Actor 叠加后渲染帧率下降", "低", "中", "限制 Actor 数量 + LOD 简化几何"],
    ["R-03", "MPR 坐标逆映射误差", "中", "高", "使用 vtkImageReslice 的变换矩阵精确逆变换"],
    ["R-04", "DICOM SR 生成 DCMTK 编译缺失模块", "低", "高", "验证 dcmsr 模块是否已编译入库"],
    ["R-05", "种植体安全距离计算精度", "中", "高", "使用 vtkImplicitPolyDataDistance + 多采样验证"],
    ["R-06", "Android 内存限制导致 OOM", "中", "高", "限制 ROI 数量 + 及时释放 Actor + 大对象 Native 堆管理"],
    ["R-07", "ONNX 模型体积过大（Phase 2）", "中", "中", "模型量化 (INT8) + 动态加载"],
    ["R-08", "3D U-Net 推理延迟超 5s", "中", "中", "降采样输入 + NEON 优化 + 异步推理 + 进度条"],
    ["R-09", "手势状态机与现有手势冲突", "中", "中", "互斥状态 + 明确的进入/退出手势"],
    ["R-10", "JSON 持久化数据损坏", "低", "中", "原子写入 + 校验和 + 自动备份"],
]
story.append(make_table(risk_data, [0.4*inch, 2.0*inch, 0.5*inch, 0.5*inch, 2.6*inch], is_long=True))

story.append(PageBreak())

# ══════════════════════════════════════════════════════════
#  11. 验收标准
# ══════════════════════════════════════════════════════════
story.append(P("11. 验收标准", 'h1'))
story.append(h1_div())

story.append(P("11.1 功能验收", 'h2'))
accept_data = [
    ["编号", "验收项", "验收标准", "验证方法"],
    ["AC-01", "两点距离测量", "误差 <= 0.1mm", "已知距离 DICOM 序列比对"],
    ["AC-02", "三点角度测量", "误差 <= 0.5 度", "已知角度 DICOM 序列比对"],
    ["AC-03", "ROI 体积计算", "误差 <= 1%", "规则几何体验证"],
    ["AC-04", "HU 值采样", "与 DICOM Tag (0028,1050) 窗位一致", "直接比对 DICOM 元数据"],
    ["AC-05", "种植体放置", "位置/角度/深度可调", "3 颗种植体方案测试"],
    ["AC-06", "安全距离警告", "< 2mm 红色高亮", "神经管路径旁放置验证"],
    ["AC-07", "标注持久化", "退出后重进数据恢复", "杀进程后重新加载验证"],
    ["AC-08", "PDF 报告导出", "含截图 + 数据表 + 患者信息", "人工审查"],
    ["AC-09", "DICOM SR 导出", "DCMTK dsr2xml 可解析", "命令行验证"],
    ["AC-10", "手势切换", "9 种状态无缝切换", "真机操作验证"],
]
story.append(make_table(accept_data, [0.5*inch, 1.5*inch, 1.8*inch, 2.2*inch], is_long=True))

story.append(P("11.2 性能验收", 'h2'))
perf_data = [
    ["编号", "验收项", "验收标准", "验证方法"],
    ["PC-01", "测量计算延迟", "<= 50ms", "systrace 测量"],
    ["PC-02", "ROI 渲染帧率", ">= 30 FPS", "GFX info 测量"],
    ["PC-03", "截图生成时间", "<= 500ms", "计时器测量"],
    ["PC-04", "模块内存增量", "<= 100MB", "Android Profiler"],
    ["PC-05", "AI 推理延迟", "<= 5s", "计时器测量 (Phase 2)"],
]
story.append(make_table(perf_data, [0.5*inch, 1.8*inch, 1.5*inch, 2.2*inch]))

story.append(P("11.3 兼容性验收", 'h2'))
story.append(BL("arm64-v8a 真机（API 24+）可正常编译安装运行"))
story.append(BL("OpenGL ES 3.0+ 设备渲染正常（与现有 VR/MPR 一致）"))
story.append(BL("与 :cbctdeal 模块功能无冲突（同时使用 VR/MPR + 测量）"))
story.append(BL("与 :dcmtk 模块 DICOM SR C-STORE 上传正常"))

story.append(PageBreak())

# ══════════════════════════════════════════════════════════
#  附录
# ══════════════════════════════════════════════════════════
story.append(P("附录 A: 术语表", 'h1'))
story.append(h1_div())
glossary_data = [
    ["术语", "英文", "说明"],
    ["CBCT", "Cone Beam Computed Tomography", "锥形束计算机断层扫描"],
    ["VR", "Volume Rendering", "体绘制，三维体数据直接渲染"],
    ["MPR", "Multi-Planar Reconstruction", "多平面重建，任意角度切面"],
    ["HU", "Hounsfield Unit", "亨氏单位，CT 值标准化度量"],
    ["ROI", "Region of Interest", "感兴趣区域"],
    ["DICOM SR", "DICOM Structured Report", "DICOM 结构化报告"],
    ["HU 窗宽窗位", "Window Width / Window Center", "CT 图像灰阶映射范围"],
    ["DICOM Tag", "DICOM Data Element Tag", "DICOM 数据元素标签 (group,element)"],
    ["PACS", "Picture Archiving and Communication System", "医学影像存档与通信系统"],
    ["ONNX", "Open Neural Network Exchange", "开放神经网络交换格式"],
    ["种植体", "Dental Implant", "人工牙根，钛合金螺柱"],
    ["下颌神经管", "Inferior Alveolar Canal", "下颌骨内走行下牙槽神经的管道"],
]
story.append(make_table(glossary_data, [1.2*inch, 2.0*inch, 3.3*inch], is_long=True))

story.append(P("附录 B: JNI 接口清单", 'h1'))
story.append(h1_div())
story.append(B("以下为新模块 :cbctmeasure 需要新增的 JNI 接口清单（Phase 1）："))

jni_data = [
    ["类", "方法签名", "说明"],
    ["MeasureJni", "startMeasure(rendererPtr, type: Int): Int", "开始测量会话，返回 session id"],
    ["MeasureJni", "pickPoint(sessionId, rendererPtr, x, y): Boolean", "拾取屏幕坐标对应的空间点"],
    ["MeasureJni", "getResult(sessionId): String", "获取测量结果 JSON"],
    ["MeasureJni", "clearMeasure(sessionId)", "清除指定测量会话"],
    ["MeasureJni", "clearAllMeasure(rendererPtr)", "清除所有测量 Actor"],
    ["RoiJni", "createRoi(rendererPtr, type, params: String): Int", "创建 ROI，返回 id"],
    ["RoiJni", "setRoiThreshold(roiId, huMin, huMax)", "设置 HU 阈值范围"],
    ["RoiJni", "setRoiClipBox(roiId, xmin, ymin, zmin, xmax, ymax, zmax)", "设置裁剪盒"],
    ["RoiJni", "setRoiVisibility(roiId, visible: Boolean)", "设置 ROI 可见性"],
    ["RoiJni", "removeRoi(roiId)", "移除 ROI"],
    ["RoiJni", "computeRoiVolume(roiId, volumePtr): Double", "计算 ROI 体积 (cm3)"],
    ["RoiJni", "computeRoiStats(roiId, volumePtr): String", "计算 ROI 统计 (均值/标准差)"],
    ["SurgeryPlanJni", "placeImplant(rendererPtr, x, y, z, angle1, angle2, depth, dia): Int", "放置种植体"],
    ["SurgeryPlanJni", "updateImplant(implantId, params: String)", "更新种植体参数"],
    ["SurgeryPlanJni", "removeImplant(implantId)", "移除种植体"],
    ["SurgeryPlanJni", "setNervePath(rendererPtr, points: FloatArray): Int", "设置神经管路径"],
    ["SurgeryPlanJni", "computeSafetyDist(implantId, nerveId): Double", "计算安全距离"],
    ["AnnotationJni", "addLabel(rendererPtr, x, y, z, text): Int", "添加文字标注"],
    ["AnnotationJni", "addLine(rendererPtr, x1, y1, z1, x2, y2, z2): Int", "添加线段标注"],
    ["AnnotationJni", "removeAnnotation(annoId)", "移除标注"],
    ["AnnotationJni", "clearAllAnnotations(rendererPtr)", "清除所有标注"],
    ["AnnotationJni", "serializeAnnotations(rendererPtr): String", "序列化标注 JSON"],
    ["AnnotationJni", "deserializeAnnotations(rendererPtr, json: String)", "反序列化标注"],
    ["ReportJni", "captureScreenshot(rendererPtr, width, height): Bitmap", "截图"],
    ["ReportJni", "generateSr(volumePtr, measures, annotations: String): ByteArray", "生成 DICOM SR"],
]
story.append(make_table(jni_data, [1.2*inch, 2.8*inch, 2.5*inch], is_long=True))

story.append(Spacer(1, 0.3*inch))
story.append(subtle_div())
story.append(P(
    "本需求文档基于 DcmtkDemo 项目现有 :cbctdeal 模块能力 + CBCT 行业应用调研编写，"
    "供项目审查与评审使用。审查通过后进入详细设计与编码阶段。",
    'note'
))

# ══════════════════════════════════════════════════════════
#  构建 PDF
# ══════════════════════════════════════════════════════════
output_path = "/Users/wangyao/androidproject/DcmtkDemo/cbctmeasure_prd.pdf"
doc = SimpleDocTemplate(
    output_path,
    pagesize=A4,
    leftMargin=LM, rightMargin=RM,
    topMargin=TM, bottomMargin=BM,
    title="CBCT 三维测量与手术规划模块 - 需求规格说明书",
    author="DcmtkDemo Project",
    subject="CBCT Measurement & Surgical Planning Module PRD",
)
doc.build(story)
print(f"PDF generated: {output_path}")
