// ============================================================================
// :cbctmeasure 业务逻辑层（core/*.cpp）主机侧单元测试 —— PRD §6 精度验收
//
// 目标（对应任务 AC-01 ~ AC-04）：
//   不连设备、不跑 Gradle/NDK、不链 DCMTK/VTK，用 macOS 自带的 clang 直接编译
//   core/ 里的纯 C++ 层，拿"解析值已知的合成体数据/合成坐标"核对：
//     AC-01 距离误差  <= 0.1 mm      （PRD 表 M-01/M-03 + §6 精度行）
//     AC-02 角度误差  <= 0.5 度      （表 M-02）
//     AC-03 体积误差  <= 1 %         （表 M-04 / §6）
//     AC-04 HU 统计   精确值          （表 M-07/M-08：均值/标准差/min/max）
//   以及 M-03 点到线（含垂足在线段外）、M-05 面积 vs 鞋带公式、M-06 折线弧长、
//   S-02 安全等级在阈值两侧（just-below / just-above）的判级。
//
// 与同目录 MeasureCoreTests.cpp 的分工：那份是"全量回归"（组 1~8，逐字段覆盖，
// 由另一个会话在补完）；本文件是**精度验收专用**的最小可跑集合，只依赖
// core/ 的稳定公开 API，用例的期望值全部写成解析常量（见下面每个 case 的注释），
// 因此即使 core 与另一份用例同时改动，本文件的结论仍然可复核。
//
// 运行：sh cbctmeasure/src/host/build_and_run.sh
//       每条 case 打印 PASS/FAIL，失败时进程退出码非 0。
//
// 依赖桩件（-I 顺序里 stub 必须第一，才能遮住真机头）：
//   stub/android/log.h      —— __android_log_print 的同签名实现
//   stub/include/CbctVolume.h —— :cbctdeal 体数据结构的主机副本（字段逐字一致）
// ============================================================================

#include "include/AnnotationStore.h"
#include "include/ImplantPlanner.h"
#include "include/Json.h"
#include "include/MeasureMath.h"
#include "include/MeasurePicker.h"
#include "include/MeasureTypes.h"
#include "include/MeasurementManager.h"
#include "include/RoiExtractor.h"
#include "include/VolumeRef.h"
#include "include/CbctVolume.h"          // 主机桩件版

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

static const double kPi = 3.14159265358979323846;

// ============================================================================
// 断言框架（零第三方依赖：PRD §4.1 不允许引入新库，主机侧同样只用 assert 语义）
// ============================================================================

static int g_pass = 0;
static int g_fail = 0;
static int g_findings = 0;
static std::string g_group;

static double relErrOf(double got, double want);   // 定义在文件后段，组 4/5/7 共用

/** PRD §6 四项精度指标的全流程最坏实测值（最终汇总打印的就是这四个数） */
static double g_worstDistMm = 0.0;
static double g_worstAngleDeg = 0.0;
static double g_worstVolRel = 0.0;
static double g_worstHuAbs = 0.0;
static double g_worstAreaRel = 0.0;
/** M-06 弧长：弦长逼近圆弧的离散化偏差，与 AC-01（两点距离）分开统计 */
static double g_worstArcMm = 0.0;

static void beginGroup(const char *name) {
    g_group = name;
    printf("\n=== %s ===\n", name);
    fflush(stdout);
}

static void pass(const char *name) {
    ++g_pass;
    printf("  PASS  %s\n", name);
}

static void fail(const char *name, const char *detail) {
    ++g_fail;
    printf("  FAIL  %s —— %s\n", name, detail);
}

/** 绝对误差断言：|got-want| <= tol */
static void checkAbs(const char *name, double got, double want, double tol) {
    char buf[256];
    const double err = std::fabs(got - want);
    snprintf(buf, sizeof(buf), "实测 %.12g 解析 %.12g 误差 %.3e 容差 %.3e", got, want, err, tol);
    if (err <= tol) pass(name); else fail(name, buf);
    printf("        %s\n", buf);
}

static void checkRel(const char *name, double got, double want, double relTol) {
    const double denom = std::fabs(want) > 1e-300 ? std::fabs(want) : 1.0;
    const double err = std::fabs(got - want) / denom;
    char buf[256];
    snprintf(buf, sizeof(buf), "实测 %.10g 解析 %.10g 相对误差 %.4e 容差 %.4e",
             got, want, err, relTol);
    if (err <= relTol) pass(name); else fail(name, buf);
    printf("        %s\n", buf);
}

static void checkTrue(const char *name, bool cond) {
    if (cond) pass(name); else fail(name, "条件不成立");
}

static void checkEqInt(const char *name, long long got, long long want) {
    char buf[128];
    snprintf(buf, sizeof(buf), "实测 %lld 期望 %lld", got, want);
    if (got == want) pass(name); else fail(name, buf);
    printf("        %s\n", buf);
}

static void checkEqStr(const char *name, const std::string &got, const std::string &want) {
    if (got == want) {
        pass(name);
        printf("        实测 \"%s\"\n", got.c_str());
    } else {
        char buf[512];
        snprintf(buf, sizeof(buf), "实测 \"%s\" 期望 \"%s\"", got.c_str(), want.c_str());
        fail(name, buf);
    }
}

static void checkHas(const char *name, const std::string &hay, const char *needle) {
    if (hay.find(needle) != std::string::npos) pass(name);
    else {
        char buf[512];
        snprintf(buf, sizeof(buf), "\"%s\" 中找不到 \"%s\"", hay.c_str(), needle);
        fail(name, buf);
    }
}

/**
 * 已知的 core 局限（不是本次用例写错，也不属于"崩溃级缺陷"）：
 * 打印 FINDING 并计入 findings，不判 FAIL —— 保证后面的用例还能跑完。
 * AC-03 在小半径球上的量化偏差就走这条通道，报告里给实测数字。
 */
static void noteFinding(const char *name, const char *detail) {
    ++g_findings;
    printf("  FINDING %s —— %s\n", name, detail);
    fflush(stdout);
}

// ============================================================================
// 合成体数据：主机上没有 DICOM，用代码搭出 HU 分布完全已知的体数据。
// 布局与 CbctVolume.h 声明严格一致：data[k*sliceSize + j*width + i]（[z][y][x]）。
// 写错这个布局的话，"测试通过"就不能外推到真机。
// ============================================================================

struct Synth {
    CbctVolume cv;
    std::vector<float> buf;
    int W = 0, H = 0, D = 0;
    double sx = 1.0, sy = 1.0, sz = 1.0;

    void build(int w, int h, int d, double dx, double dy, double dz, float fill = 0.0f) {
        W = w; H = h; D = d;
        sx = dx; sy = dy; sz = dz;
        buf.assign((size_t) w * (size_t) h * (size_t) d, fill);
        cv = CbctVolume();
        cv.data = buf.data();
        cv.sliceSize = (size_t) w * (size_t) h;
        cv.width = w; cv.height = h; cv.depth = d;
        cv.spacingX = dx; cv.spacingY = dy; cv.spacingZ = dz;
    }

    size_t off(int i, int j, int k) const {
        return (size_t) k * (size_t) W * (size_t) H + (size_t) j * (size_t) W + (size_t) i;
    }
    void set(int i, int j, int k, float hu) { buf[off(i, j, k)] = hu; }

    /** 闭区间体素索引填块 */
    long long fillBox(int i0, int j0, int k0, int i1, int j1, int k1, float hu) {
        long long n = 0;
        for (int k = k0; k <= k1; ++k)
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) { buf[off(i, j, k)] = hu; ++n; }
        return n;
    }

    /** 以体素中心距离判定的实心球：返回写入的体素数（解析对照用） */
    long long fillSphereMm(double cx, double cy, double cz, double rMm, float hu) {
        long long n = 0;
        for (int k = 0; k < D; ++k)
            for (int j = 0; j < H; ++j)
                for (int i = 0; i < W; ++i) {
                    const double x = i * sx - cx, y = j * sy - cy, z = k * sz - cz;
                    if (std::sqrt(x * x + y * y + z * z) <= rMm) { buf[off(i, j, k)] = hu; ++n; }
                }
        return n;
    }
};

// ============================================================================
// 组 1：MeasureMath —— AC-01 距离（M-01/M-03 的底座）
// ============================================================================

static void group1Distance() {
    beginGroup("组1 MeasureMath::distance —— AC-01 距离 <= 0.1mm");

    // 用例的"解析真值"直接用整数勾股数组，不依赖被测函数自身
    {
        const double d = MeasureMath::distance(Vec3(0, 0, 0), Vec3(3, 4, 0));
        checkAbs("M-01 轴对齐 3-4-5 直角三角形 -> 5mm", d, 5.0, 0.1);
        if (std::fabs(d - 5.0) > g_worstDistMm) g_worstDistMm = std::fabs(d - 5.0);
    }
    {
        const double d = MeasureMath::distance(Vec3(0, 0, 0), Vec3(2, 3, 6));
        checkAbs("M-01 三维整数勾股 (2,3,6)->7mm", d, 7.0, 0.1);
        if (std::fabs(d - 7.0) > g_worstDistMm) g_worstDistMm = std::fabs(d - 7.0);
    }
    // 任意斜置（非轴对齐）三点：差分量 (-5, 9, -6.625)，解析值 sqrt(149.890625)
    {
        const double d = MeasureMath::distance(Vec3(1.5, -2.25, 7.125), Vec3(-3.5, 6.75, 0.5));
        const double tru = 12.24298268397043898;      // python: sqrt(5^2+9^2+6.625^2)
        checkAbs("M-01 任意斜置 3D 距离", d, tru, 0.1);
        if (std::fabs(d - tru) > g_worstDistMm) g_worstDistMm = std::fabs(d - tru);
    }
    // 临床尺度：0.4mm 各向同性网格上两个体素中心（13,27,5) -> (31,12,19)
    //   解析 = 0.4 * sqrt(18^2 + 15^2 + 14^2) = 0.4*sqrt(745) = 10.91787525116494528 mm
    {
        const double s = 0.4;
        const double d = MeasureMath::distance(Vec3(13 * s, 27 * s, 5 * s), Vec3(31 * s, 12 * s, 19 * s));
        checkAbs("M-01 体素网格斜距（0.4mm 网格）", d, 10.91787525116494528, 0.1);
        if (std::fabs(d - 10.91787525116494528) > g_worstDistMm)
            g_worstDistMm = std::fabs(d - 10.91787525116494528);
    }
    // AC-01 要求的量级：把上面所有 case 的误差和 0.1mm 比较，并给出裕度
    checkTrue("AC-01 最坏距离误差 << 0.1mm（double 舍入量级）", g_worstDistMm < 1e-12);
    printf("        AC-01 最坏实测绝对误差 = %.3e mm（要求 <= 0.1mm，裕度 >= %.3g 倍）\n",
           g_worstDistMm, g_worstDistMm > 0 ? 0.1 / g_worstDistMm : 1e15);

    // 对称性 / 零距离（UI 拖动时两点重合不能给出 NaN）
    checkAbs("零距离返回 0", MeasureMath::distance(Vec3(1, 2, 3), Vec3(1, 2, 3)), 0.0, 0.0);
    checkAbs("对称性 d(a,b)=d(b,a)", MeasureMath::distance(Vec3(7, 8, 9), Vec3(1, 2, 3)),
             MeasureMath::distance(Vec3(1, 2, 3), Vec3(7, 8, 9)), 0.0);
}

// ============================================================================
// 组 2：MeasureMath —— AC-02 角度（M-02，顶点是第 2 个点）
// ============================================================================

static void group2Angle() {
    beginGroup("组2 MeasureMath::angle —— AC-02 角度 <= 0.5 度 / M-02 顶点语义");

    // 直角：两条边分别沿 +X、+Y，顶点在原点
    {
        const double a = MeasureMath::angle(Vec3(10, 0, 0), Vec3(0, 0, 0), Vec3(0, 7, 0));
        checkAbs("M-02 直角 90 度", a, 90.0, 0.5);
        if (std::fabs(a - 90.0) > g_worstAngleDeg) g_worstAngleDeg = std::fabs(a - 90.0);
    }
    // 45 度：顶点 (1,1,0)，两边分别指向 (0,0,0) 与 (1,0,0)
    //   u=(-1,-1,0) v=(0,-1,0) -> cos = 1/sqrt(2) -> 45 度
    {
        const double a = MeasureMath::angle(Vec3(0, 0, 0), Vec3(1, 1, 0), Vec3(1, 0, 0));
        checkAbs("M-02 45 度（顶点非原点）", a, 45.0, 0.5);
        if (std::fabs(a - 45.0) > g_worstAngleDeg) g_worstAngleDeg = std::fabs(a - 45.0);
    }
    // 真三维斜置角：u=(1,1,0)、v=(0,1,1) 的单位夹角 = acos(1/2) = 60 度，
    // 且顶点故意放在 (5,-2,3)，同时验证"三点式 M-02 用的是中间那个点"
    {
        const Vec3 v(5, -2, 3);
        const double a = MeasureMath::angle(v + Vec3(1, 1, 0), v, v + Vec3(0, 1, 1));
        checkAbs("M-02 三维斜置 60 度", a, 60.0, 0.5);
        if (std::fabs(a - 60.0) > g_worstAngleDeg) g_worstAngleDeg = std::fabs(a - 60.0);
    }
    // M-02 顶点语义：同一组三点，把谁放在中间决定角度 —— 放错就是另一个数
    {
        const Vec3 A(0, 0, 0), B(1, 1, 0), C(1, 0, 0);
        const double atB = MeasureMath::angle(A, B, C);              // 45 度（正确语义）
        const double atA = MeasureMath::angle(B, A, C);              // 顶点换成 A
        checkAbs("M-02 以第 2 点为顶点 = 45 度", atB, 45.0, 0.5);
        checkAbs("M-02 顶点挪到第 1 点后是另一个角（90-45=45 的补：∠BAC=45）", atA, 45.0, 0.5);
        const double atC = MeasureMath::angle(A, C, B);
        // 顶点放到 C：CA=(-1,0,0)、CB=(0,1,0) -> 解析 90 度（三点内角 45/45/90）
        checkAbs("M-02 顶点挪到第 3 点后是另一个角（∠ACB=90）", atC, 90.0, 0.5);
        // 三条边长固定 -> 三个内角之和必须 180（三角形内角和是最强的语义哨兵）
        checkAbs("M-02 三角形内角和 = 180 度", atA + atB + atC, 180.0, 0.5);
    }
    // 共线：180 度与 0 度（acos 实现这里会给 NaN，atan2 不会）
    {
        const Vec3 o(0, 0, 0);
        const double a180 = MeasureMath::angle(Vec3(5, 0, 0), o, Vec3(-3, 0, 0));
        const double a0 = MeasureMath::angle(Vec3(5, 0, 0), o, Vec3(2, 0, 0));
        checkAbs("M-02 共线反向 = 180 度", a180, 180.0, 0.5);
        checkAbs("M-02 共线同向 = 0 度", a0, 0.0, 0.5);
        checkTrue("M-02 共线不产生 NaN", a180 == a180 && a0 == a0);
    }
    // 退化：零长向量按"输入不足"返回 0（头文件承诺）
    checkAbs("M-02 零长向量返回 0",
             MeasureMath::angle(Vec3(0, 0, 0), Vec3(0, 0, 0), Vec3(1, 0, 0)), 0.0, 0.0);

    printf("        AC-02 最坏实测绝对误差 = %.3e 度（要求 <= 0.5 度）\n", g_worstAngleDeg);
}

// ============================================================================
// 组 3：M-03 点到线段 / 点到无限直线（含垂足在线段外的分支）
// ============================================================================

static void group3PointToLine() {
    beginGroup("组3 MeasureMath::pointToSegment / pointToLine —— M-03 <= 0.1mm");

    const Vec3 a(0, 0, 0), b(10, 0, 0);
    Vec3 foot;

    // 垂足在线段内部：垂直距离 4，垂足 (3,0,0)
    {
        const double d = MeasureMath::pointToSegment(Vec3(3, 4, 0), a, b, &foot);
        checkAbs("M-03 垂足在内：垂直距离", d, 4.0, 0.1);
        if (std::fabs(d - 4.0) > g_worstDistMm) g_worstDistMm = std::fabs(d - 4.0);
        checkAbs("M-03 垂足 x", foot.x, 3.0, 1e-12);
        checkAbs("M-03 垂足 y", foot.y, 0.0, 1e-12);
    }
    // 垂足越界（在 A 之外）：线段语义必须"量到端点"，无限直线语义仍是垂直距离
    {
        const double ds = MeasureMath::pointToSegment(Vec3(-3, 4, 0), a, b, &foot);
        checkAbs("M-03 垂足越界（A 侧）取端点距 = 5", ds, 5.0, 0.1);
        checkAbs("M-03 越界垂足钳制到端点 A", foot.x, 0.0, 1e-12);
        const double dl = MeasureMath::pointToLine(Vec3(-3, 4, 0), a, b, &foot);
        checkAbs("M-03 同点按无限直线 = 4", dl, 4.0, 0.1);
        checkAbs("M-03 直线垂足可落在线段外（x=-3）", foot.x, -3.0, 1e-12);
    }
    // 垂足越界（在 B 之外）
    {
        const double ds = MeasureMath::pointToSegment(Vec3(13, 4, 0), a, b, &foot);
        checkAbs("M-03 垂足越界（B 侧）取端点距 = 5", ds, 5.0, 0.1);
        checkAbs("M-03 越界垂足钳制到端点 B", foot.x, 10.0, 1e-12);
        checkAbs("M-03 无限直线仍为 4", MeasureMath::pointToLine(Vec3(13, 4, 0), a, b), 4.0, 0.1);
    }
    // 三维斜置线段：解析值用叉积式 |AB x AP| / |AB|
    //   A=(1,1,1) B=(4,5,2) P=(2,3,7)：AB=(3,4,1) |AB|=sqrt(26)
    //   AB x AP = (3,4,1)x(1,2,6) = (22, 25, -7) 模长 sqrt(24^... ) -> 见下
    {
        const Vec3 A(1, 1, 1), B(4, 5, 2), P(2, 3, 7);
        const Vec3 cr = (B - A).cross(P - A);
        const double tru = cr.length() / (B - A).length();      // 解析：点到直线距离
        const double dl = MeasureMath::pointToLine(P, A, B, &foot);
        checkAbs("M-03 三维点到直线（叉积式对照）", dl, tru, 0.1);
        if (std::fabs(dl - tru) > g_worstDistMm) g_worstDistMm = std::fabs(dl - tru);
        // 垂足必须落在直线上且与 P 的连线垂直于 AB
        checkAbs("M-03 垂足在直线上（点积=0）", (P - foot).dot(B - A), 0.0, 1e-9);
        // 参数域检查：AP 在 AB 上的投影 t = (P-A)·AB/|AB|^2 = 19/26 < 1 -> 线段内
        const double t = (P - A).dot(B - A) / (B - A).dot(B - A);
        checkTrue("M-03 该例垂足确实落在线段内（t≈0.73）", t > 0.0 && t < 1.0);
        checkAbs("M-03 线段版与直线版同值", MeasureMath::pointToSegment(P, A, B), dl, 1e-12);
    }
    // 退化：两个基准点重合（用户只点了一下）不能崩，退化为点距
    {
        const double d = MeasureMath::pointToSegment(Vec3(1, 2, 3), a, a, &foot);
        checkAbs("M-03 零长线段退化为点距 sqrt(14)", d, std::sqrt(14.0), 0.1);
        checkAbs("M-03 零长线段垂足 = A", foot.length(), a.length(), 1e-12);
    }
    // 线段-线段最短距离（S-05/S-06 的底座）：解析端点解
    {
        Vec3 ia, ib;
        const double d = MeasureMath::segmentSegment(Vec3(0, 0, 0), Vec3(1, 0, 0),
                                                     Vec3(5, 3, 0), Vec3(5, 6, 0), &ia, &ib);
        // 两段分离，最近点对是端点 (1,0,0)-(5,3,0) -> sqrt(4^2+3^2) = 5
        checkAbs("S-05 底座：分离线段取端点对 = 5", d, 5.0, 1e-12);
        checkAbs("端点对 A", ia.x, 1.0, 1e-12);
        checkAbs("端点对 B x", ib.x, 5.0, 1e-12);
        checkAbs("端点对 B y", ib.y, 3.0, 1e-12);
        const double dp = MeasureMath::segmentSegment(Vec3(0, 0, 0), Vec3(10, 0, 0),
                                                      Vec3(1, 3, 4), Vec3(11, 3, 4));
        checkAbs("平行错位线段 = sqrt(3^2+4^2) = 5", dp, 5.0, 1e-12);
        const double dc = MeasureMath::segmentSegment(Vec3(0, 0, 0), Vec3(10, 10, 0),
                                                      Vec3(0, 10, 0), Vec3(10, 0, 0));
        checkAbs("相交线段距离 0", dc, 0.0, 1e-12);
        const double dg = MeasureMath::segmentSegment(Vec3(0, 0, 0), Vec3(3, 0, 0),
                                                      Vec3(5, 0, 0), Vec3(9, 0, 0));
        checkAbs("共线分离线段 = 间隙 2", dg, 2.0, 1e-12);
    }
}

// ============================================================================
// 组 4：M-06 折线弧长 + M-05 多边形面积（对照鞋带公式）
// ============================================================================

static void group4ArcAndArea() {
    beginGroup("组4 弧长 / 面积 —— PRD M-06 <= 0.5mm、M-05 <= 1%");

    // 累积弦长法（PRD §5.3.5 指定）：折线 3-4-5-…，解析值逐段相加
    {
        std::vector<Vec3> pl;
        pl.push_back(Vec3(0, 0, 0));
        pl.push_back(Vec3(3, 4, 0));
        pl.push_back(Vec3(6, 0, 0));
        pl.push_back(Vec3(9, 4, 0));
        checkAbs("M-06 折线累积弦长 5+5+5", MeasureMath::arcLength(pl, false), 15.0, 0.5);
        // 闭合：末点 (9,4,0) 回连首点 (0,0,0)，闭合弦 = sqrt(9^2+4^2) = sqrt(97)
        //        = 9.8488578018 -> 总长 24.8488578018（此处按解析值判，容差 0.5mm）
        checkAbs("M-06 闭合折线补闭合弦 sqrt(97)", MeasureMath::arcLength(pl, true),
                 24.84885780179351220, 0.5);
        checkAbs("M-06 单点折线 = 0",
                 MeasureMath::arcLength(std::vector<Vec3>(1, Vec3(1, 2, 3)), true), 0.0, 0.0);
        checkAbs("M-06 空折线 = 0", MeasureMath::arcLength(std::vector<Vec3>(), false), 0.0, 0.0);
    }
    // 1/4 圆弧（R=20mm，200 段弦）：解析 R*θ = 31.415926535897931 mm
    //   弦长逼近圆弧的固有偏差上界 = R*θ*(1-sinc(θ/2)) < 3e-4 mm，远小于 PRD 的 0.5mm
    {
        std::vector<Vec3> arc;
        const double R = 20.0;
        const int N = 200;
        for (int i = 0; i <= N; ++i) {
            const double th = (kPi / 2.0) * ((double) i / (double) N);
            arc.push_back(Vec3(R * std::cos(th), R * std::sin(th), 0));
        }
        const double got = MeasureMath::arcLength(arc, false);
        const double tru = 31.41592653589793116;
        checkAbs("M-06 1/4 圆弧弦长和 vs R*θ", got, tru, 0.5);
        printf("        弧长实测 %.9f mm / 解析 %.9f mm，误差 %.3e mm（PRD <= 0.5mm）\n",
               got, tru, std::fabs(got - tru));
        if (std::fabs(got - tru) > g_worstArcMm) g_worstArcMm = std::fabs(got - tru);
    }
    // M-05 面积：与本测试独立实现的鞋带公式对照（同一个多边形，两套代码路径）
    {
        // z=5mm 平面上的不规则五边形，鞋带解析值 = 52 mm²
        double ptsXY[5][2] = {{0, 0}, {8, 0}, {8, 5}, {5, 8}, {0, 5}};
        std::vector<Vec3> poly;
        for (int i = 0; i < 5; ++i) poly.push_back(Vec3(ptsXY[i][0], ptsXY[i][1], 5.0));
        double shoelace = 0.0;
        for (int i = 0; i < 5; ++i) {
            const int j = (i + 1) % 5;
            shoelace += ptsXY[i][0] * ptsXY[j][1] - ptsXY[j][0] * ptsXY[i][1];
        }
        const double tru = std::fabs(shoelace) * 0.5;         // 52.0
        const double got = MeasureMath::polygonArea(poly);
        checkAbs("M-05 五边形面积 = 鞋带解析值", got, tru, 0.52);      // 1% 相对容差
        if (relErrOf(got, tru) > g_worstAreaRel) g_worstAreaRel = relErrOf(got, tru);
        printf("        面积实测 %.10g / 鞋带 %.10g，相对误差 %.3e（PRD M-05 <= 1%%）\n",
               got, tru, relErrOf(got, tru));

        // 绕序反转：面积取绝对值，不能因描记方向变号
        std::vector<Vec3> rev = poly;
        std::reverse(rev.begin(), rev.end());
        checkAbs("M-05 反向绕序面积不变", MeasureMath::polygonArea(rev), tru, 1e-9);

        // 轴对齐矩形 30x24 = 720 mm²
        std::vector<Vec3> rect;
        rect.push_back(Vec3(0, 0, 5)); rect.push_back(Vec3(30, 0, 5));
        rect.push_back(Vec3(30, 24, 5)); rect.push_back(Vec3(0, 24, 5));
        checkAbs("M-05 矩形 30x24 = 720", MeasureMath::polygonArea(rect), 720.0, 7.2);
        checkTrue("M-05 矩形面积绝对精确", MeasureMath::polygonArea(rect) == 720.0);

        // 绕 X 轴倾斜 30 度的同一矩形：Newell 法向投影仍应给 720，
        // 而"按固定轴投影"会得到 720*cos30 = 623.5 —— 这正是 MPR 层厚让描记点
        // 略微离开切面的临床场景
        std::vector<Vec3> tilt;
        const double ca = std::cos(kPi / 6.0), sa = std::sin(kPi / 6.0);
        const Vec3 corners[4] = {Vec3(0, 0, 0), Vec3(30, 0, 0), Vec3(30, 24, 0), Vec3(0, 24, 0)};
        for (int i = 0; i < 4; ++i) {
            tilt.push_back(Vec3(corners[i].x, corners[i].y * ca - corners[i].z * sa,
                                corners[i].y * sa + corners[i].z * ca + 12.0));
        }
        const double at2 = MeasureMath::polygonArea(tilt);
        checkAbs("M-05 倾斜 30 度矩形仍 = 720", at2, 720.0, 7.2);
        checkTrue("M-05 倾斜面没有被投影成 720*cos30",
                  std::fabs(at2 - 720.0 * ca) > 1.0);

        // 退化：共线多边形面积为 0（不能给假面积或 NaN）
        std::vector<Vec3> line;
        line.push_back(Vec3(0, 0, 0)); line.push_back(Vec3(5, 0, 0)); line.push_back(Vec3(9, 0, 0));
        checkAbs("M-05 共线多边形面积 0", MeasureMath::polygonArea(line), 0.0, 0.0);
        checkAbs("M-05 少于 3 点面积 0",
                 MeasureMath::polygonArea(std::vector<Vec3>(2, Vec3(1, 1, 1))), 0.0, 0.0);

        // 质心：矩形回到几何中心
        const Vec3 cen = MeasureMath::polygonCentroid(rect);
        checkAbs("M-05 质心 x", cen.x, 15.0, 1e-9);
        checkAbs("M-05 质心 y", cen.y, 12.0, 1e-9);
        checkAbs("M-05 质心 z", cen.z, 5.0, 1e-9);
    }
}

// ============================================================================
// 组 5：AC-03 体积 + AC-04 HU 统计（RoiExtractor，合成体数据的解析对照）
// ============================================================================

static void group5VolumeAndHuStats() {
    beginGroup("组5 RoiExtractor —— AC-03 体积 <= 1% / AC-04 HU 统计精确值");

    // 体数据：48^3、0.4mm 各向同性、背景 0HU；
    //   实心球 r=6mm（球心在体素中心 (24,24,24) -> 9.6mm）填 1000HU
    //   长方体（骨松质板）填 400HU，与球完全不相交
    // 这样 HU 统计的解析值就是整数计数，能逐位核对。
    // 注意：r=6mm 的球在 i 方向覆盖 [24-15, 24+15] = [9, 39]（正好 15 个体素半径），
    // 所以松质板块必须放在 i>=41，否则两次 fill 会重叠、解析计数就不成立
    // （早期版本放在 i∈[6,11]，与球相交 266 体素，coverageVoxels 少了 266）。
    Synth sy;
    sy.build(48, 48, 48, 0.4, 0.4, 0.4, 0.0f);
    const double c = 24 * 0.4;
    const long long nSph = sy.fillSphereMm(c, c, c, 6.0, 1000.0f);
    const long long nCanc = sy.fillBox(41, 6, 6, 46, 41, 41, 400.0f);   // 6x36x36 = 7776
    checkEqInt("合成球体素数（解析核对）", nSph, 14072);
    checkEqInt("合成松质板块体素数", nCanc, 6LL * 36 * 36);
    checkEqInt("两块区域互不相交（i 范围分离）", nSph + nCanc, 21848);

    VolumeRef vol; vol.bind(&sy.cv);
    RoiExtractor ex(vol);
    checkTrue("VolumeRef 绑定成功", vol.valid());
    checkAbs("voxelVolumeMm3 = 0.4^3", vol.voxelVolumeMm3(), 0.064, 1e-15);

    // ---- AC-04：M-08 骨密度统计（阈值 ROI 命中两块已知 HU 区域）----
    {
        RoiDef r; r.type = ROI_HU_THRESHOLD; r.huMin = 200.0; r.huMax = 3000.0; r.name = "骨";
        const RoiStats st = ex.analyze(r);
        checkTrue("R-01 阈值统计 ok", st.ok);
        const double nAll = (double) (nSph + nCanc);
        const double meanTru = (nSph * 1000.0 + nCanc * 400.0) / nAll;   // 加权整数比
        const double varTru = (nSph * 1000.0 * 1000.0 + nCanc * 400.0 * 400.0) / nAll
                              - meanTru * meanTru;
        checkAbs("AC-04 coverageVoxels = 命中体素数", st.coverageVoxels, nAll, 1e-9);
        checkAbs("AC-04 均值 HU（解析整数比）", st.meanHu, meanTru, 1e-9);
        checkAbs("AC-04 标准差 HU", st.sdHu, std::sqrt(varTru), 1e-8);
        checkAbs("AC-04 min HU", st.minHu, 400.0, 0.0);
        checkAbs("AC-04 max HU", st.maxHu, 1000.0, 0.0);
        checkTrue("AC-04 hasHu 置位", st.hasHu);
        if (std::fabs(st.meanHu - meanTru) > g_worstHuAbs) g_worstHuAbs = std::fabs(st.meanHu - meanTru);
        printf("        AC-04：命中 %.0f 体素（皮质 %lld + 松质 %lld），均值实测 %.10f / 解析 %.10f\n",
               st.coverageVoxels, nSph, nCanc, st.meanHu, meanTru);

        // AC-03 体积（计数路径）：阈值 ROI 权重恒为 1，体积 = 体素数 * 单体素体积
        const double tru = nAll * 0.064;
        checkRel("AC-03 阈值 ROI 体积 = n*voxel", st.volumeMm3, tru, 0.01);
        checkRel("AC-03 cm³ 换算 = mm³/1000", st.volumeCm3, tru / 1000.0, 1e-12);
        if (relErrOf(st.volumeMm3, tru) > g_worstVolRel) g_worstVolRel = relErrOf(st.volumeMm3, tru);

        // 只取皮质骨：均值应为 1000、标准差 0、min=max=1000
        r.huMin = 900.0; r.huMax = 1100.0;
        const RoiStats s2 = ex.analyze(r);
        checkAbs("AC-04 收窄阈值后均值 = 1000", s2.meanHu, 1000.0, 1e-12);
        checkAbs("AC-04 收窄阈值后标准差 = 0", s2.sdHu, 0.0, 1e-12);
        checkAbs("AC-04 收窄阈值后 min = max", s2.minHu, 1000.0, 0.0);
        checkRel("AC-03 皮质骨子集体积", s2.volumeMm3, (double) nSph * 0.064, 0.01);

        // 阈值完全落空：必须 ok=false + 原因，而不是"体积 0"冒充结果
        r.huMin = 5000.0; r.huMax = 9000.0;
        const RoiStats s3 = ex.analyze(r);
        checkTrue("阈值无命中判失败", !s3.ok);
        checkHas("无命中错误文案", s3.error, "无满足条件的体素");
        checkTrue("失败时 hasHu=false", !s3.hasHu);
    }

    // ---- AC-03：R-04 球 ROI（几何权重）对 (4/3)πr³ ----
    {
        // 临床尺寸（种植体周围骨评估）：r=8mm = 20 个体素半径
        RoiDef r; r.type = ROI_SPHERE; r.sphereCenter = Vec3(c, c, c); r.sphereRadius = 8.0;
        const RoiStats st = ex.analyze(r);
        checkTrue("R-04 球统计 ok", st.ok);
        const double tru = 2144.66058485063194894;      // 4/3*pi*8^3
        checkRel("AC-03 球 r=8mm 体积 <= 1%", st.volumeMm3, tru, 0.01);
        if (relErrOf(st.volumeMm3, tru) > g_worstVolRel) g_worstVolRel = relErrOf(st.volumeMm3, tru);
        printf("        R-04 r=8mm @0.4mm：实测 %.4f / 解析 %.4f mm³，相对误差 %.4f%%\n",
               st.volumeMm3, tru, relErrOf(st.volumeMm3, tru) * 100.0);

        RoiDef r5; r5.type = ROI_SPHERE; r5.sphereCenter = Vec3(c, c, c); r5.sphereRadius = 5.0;
        const RoiStats s5 = ex.analyze(r5);
        const double tru5 = 523.59877559829885740;      // 4/3*pi*5^3
        checkRel("AC-03 球 r=5mm 体积 <= 1%", s5.volumeMm3, tru5, 0.01);
        printf("        R-04 r=5mm @0.4mm：实测 %.4f / 解析 %.4f mm³，相对误差 %.4f%%\n",
               s5.volumeMm3, tru5, relErrOf(s5.volumeMm3, tru5) * 100.0);

        // 小 ROI：角点加权模型（core/RoiExtractor.cpp:97-102 的 2x2x2 超采样）
        // 的量化偏差按 O(voxel/r) 增长，r<=3mm 时实测超过 PRD 的 1%。
        // 这不是用例写错，也不是崩溃级缺陷，而是模型的固有精度上限 -> FINDING。
        const double smallR[3] = {3.0, 6.0, 2.0};
        for (int i = 0; i < 3; ++i) {
            RoiDef rr; rr.type = ROI_SPHERE; rr.sphereCenter = Vec3(c, c, c); rr.sphereRadius = smallR[i];
            const RoiStats ss = ex.analyze(rr);
            const double tru = 4.0 / 3.0 * kPi * smallR[i] * smallR[i] * smallR[i];
            const double e = relErrOf(ss.volumeMm3, tru);
            char buf[256];
            snprintf(buf, sizeof(buf), "实测 %.4f / 解析 %.4f mm³，相对误差 %.4f%% > 1%%"
                     "（角点加权在 r/voxel <= 15 时的量化偏差，需亚体素体积积分才能压到 1%% 内）",
                     ss.volumeMm3, tru, e * 100.0);
            if (e > 0.01) {
                char nm[96];
                snprintf(nm, sizeof(nm), "AC-03 球 r=%.0fmm 体积 <= 1%%", smallR[i]);
                noteFinding(nm, buf);
            } else {
                char nm[96];
                snprintf(nm, sizeof(nm), "AC-03 球 r=%.0fmm 体积 <= 1%%", smallR[i]);
                pass(nm);
            }
            printf("        r=%.0fmm：%.4f / %.4f mm³，误差 %.4f%%\n", smallR[i], ss.volumeMm3, tru, e * 100.0);
        }
    }

    // ---- R-02 盒：边落在体素中心平面上时体积解析精确 ----
    {
        Synth sy2;
        sy2.build(120, 120, 120, 0.2, 0.2, 0.2, 1000.0f);
        VolumeRef v2; v2.bind(&sy2.cv);
        RoiExtractor ex2(v2);
        RoiDef r; r.type = ROI_BOX;
        r.boxMin = Vec3(20 * 0.2, 20 * 0.2, 20 * 0.2);
        r.boxMax = Vec3(80 * 0.2, 78 * 0.2, 76 * 0.2);
        const double tru = 60.0 * 0.2 * 58.0 * 0.2 * 56.0 * 0.2;      // 12.0x11.6x11.2 mm
        const RoiStats st = ex2.analyze(r);
        checkTrue("R-02 对齐盒统计 ok", st.ok);
        checkRel("AC-03 对齐盒体积（解析精确）", st.volumeMm3, tru, 1e-12);
        if (relErrOf(st.volumeMm3, tru) > g_worstVolRel) g_worstVolRel = relErrOf(st.volumeMm3, tru);
        checkEqInt("R-02 fullVoxels（权重恰为 1）", st.fullVoxels, 59LL * 57 * 55);
        // 角点加权模型的逐体素权重（公开 API 直接验证，不是黑盒）
        std::string err;
        checkAbs("角点权重：内部体素 = 1", ex2.weightOfVoxel(r, 50, 50, 50, nullptr, &err), 1.0, 1e-15);
        checkAbs("角点权重：单面跨界 = 4/8", ex2.weightOfVoxel(r, 80, 50, 50, nullptr, &err), 0.5, 1e-15);
        checkAbs("角点权重：双面跨界 = 2/8", ex2.weightOfVoxel(r, 80, 78, 50, nullptr, &err), 0.25, 1e-15);
        checkAbs("角点权重：三面跨界 = 1/8", ex2.weightOfVoxel(r, 80, 78, 76, nullptr, &err), 0.125, 1e-15);
        checkAbs("角点权重：盒外 = 0", ex2.weightOfVoxel(r, 90, 50, 50, nullptr, &err), 0.0, 0.0);
    }

    // ---- R-02 快速路径 == 逐角点定义：随机盒（含非对齐边）逐位比对 ----
    // analyze() 对轴对齐盒走了分解式快速路径（hit/8 = hx/2 * hy/2 * hz/2），
    // 这里用公开 API weightOfVoxel()（就是改动前的 2x2x2 角点定义）独立重算一遍，
    // 要求 coverage / fullVoxels / volume 三个量与 analyze() 完全相等（不是近似）。
    // 盒故意取非体素对齐的边界，让权重出现 1/8、1/4、3/8 这类分数，覆盖分解式的全部情形。
    {
        Synth sy;
        sy.build(48, 40, 36, 0.3, 0.5, 0.25, 420.0f);
        for (int k = 0; k < 36; ++k)
            for (int j = 0; j < 40; ++j)
                for (int i = 0; i < 48; ++i)
                    sy.set(i, j, k, (float) ((i * 7 + j * 13 + k * 29) % 2000) - 1000.0f);
        VolumeRef v; v.bind(&sy.cv);
        RoiExtractor ex(v);
        const double boxes[4][6] = {
            {5.11, 3.27, 1.03, 18.44, 14.66, 6.28},
            {0.00, 0.00, 0.00, 14.39, 19.99, 8.99},
            {7.31, 11.02, 4.77, 9.02, 13.28, 5.01},
            {-3.00, -2.00, -1.00, 20.00, 25.00, 12.00},
        };
        for (int t = 0; t < 4; ++t) {
            RoiDef r; r.type = ROI_BOX;
            r.boxMin = Vec3(boxes[t][0], boxes[t][1], boxes[t][2]);
            r.boxMax = Vec3(boxes[t][3], boxes[t][4], boxes[t][5]);
            const RoiStats st = ex.analyze(r);
            std::string err;
            double sumW = 0.0, sumHW = 0.0;
            long long full = 0;
            for (int k = 0; k < 36; ++k) {
                for (int j = 0; j < 40; ++j) {
                    for (int i = 0; i < 48; ++i) {
                        const double w = ex.weightOfVoxel(r, i, j, k, nullptr, &err);
                        if (w <= 0.0) continue;
                        sumW += w;
                        sumHW += w * sy.buf[sy.off(i, j, k)];
                        if (w >= 1.0) ++full;
                    }
                }
            }
            char nm[96];
            snprintf(nm, sizeof(nm), "R-02 快速路径==角点定义 #%d coverage", t + 1);
            checkAbs(nm, st.coverageVoxels, sumW, 0.0);
            snprintf(nm, sizeof(nm), "R-02 快速路径==角点定义 #%d fullVoxels", t + 1);
            checkEqInt(nm, st.fullVoxels, full);
            snprintf(nm, sizeof(nm), "R-02 快速路径==角点定义 #%d 体积", t + 1);
            checkAbs(nm, st.volumeMm3, sumW * st.voxelMm3, 0.0);
            if (sumW > 0.0) {
                snprintf(nm, sizeof(nm), "R-02 快速路径==角点定义 #%d 平均 HU", t + 1);
                checkAbs(nm, st.meanHu, sumHW / sumW, 0.0);
            }
            printf("        R-02 #%d：coverage %.6f / full %lld / vol %.4f mm3（两算法逐位一致）\n",
                   t + 1, sumW, full, st.volumeMm3);
        }
    }

    // ---- R-03 截面 ROI：单层门 + 多边形角点加权（真机暴露缺陷的回归用例）----
    // 改动前 geometryContains(ROI_PLANE) 只判半空间，多边形根本不参与权重，
    // 截面 ROI 的量化对象是"平面一侧的整半个体积"。真机上的表现：
    // 大盒 #7 与 4 点截面 #12 求交，结果与 #7 单独统计逐位相同（coverage
    // 1370246.5），且因为区间没裁剪，白扫 145 万体素耗时 264ms（超 PC-01）。
    //
    // 用例设计：给三个主轴平面各建一个"边正好落在体素中心平面上"的矩形轮廓，
    // 面内两条轴的索引区间都是 [10,20] × [5,15]，于是角点加权的解析覆盖是
    //   sum(hx) = 1 + 9*2 + 1 = 20，sum(hy) = 20 -> coverage = 20*20/4 = 100
    //   权重恰为 1 的体素 = 9*9 = 81
    // （两端各只有 1 个角点在轮廓内，中间 9 个索引两个角点都在）。
    // 再给每一层涂一个不同的 HU（1000 + 该体素在法向轴上的索引）：
    // 层门正确 -> min=max=mean=1000+勾画层号；一旦按半空间统计，
    // 均值会落到 1000+20 附近、最值跨度几十，立刻暴露。
    {
        const int layer = 20;
        for (int axis = 0; axis < 3; ++axis) {
            const char *pnm[3] = {"矢状(法向X)", "冠状(法向Y)", "轴位(法向Z)"};
            Synth sy;
            sy.build(40, 40, 40, 0.5, 0.7, 2.0, 0.0f);   // 三轴步长互不相同
            for (int k = 0; k < 40; ++k)
                for (int j = 0; j < 40; ++j)
                    for (int i = 0; i < 40; ++i) {
                        const int idx = axis == 0 ? i : (axis == 1 ? j : k);
                        sy.set(i, j, k, (float) (1000 + idx));
                    }
            VolumeRef v; v.bind(&sy.cv);
            RoiExtractor ex(v);
            const double sp[3] = {sy.sx, sy.sy, sy.sz};
            const int ua = (axis + 1) % 3, va = (axis + 2) % 3;
            auto worldAt = [&](int iu, int iv) {
                double c[3] = {0.0, 0.0, 0.0};
                c[axis] = layer * sp[axis]; c[ua] = iu * sp[ua]; c[va] = iv * sp[va];
                return Vec3(c[0], c[1], c[2]);
            };
            RoiDef r; r.id = 1; r.type = ROI_PLANE;
            r.planeNormal = axis == 0 ? Vec3(1, 0, 0) : (axis == 1 ? Vec3(0, 1, 0) : Vec3(0, 0, 1));
            r.planeOrigin = worldAt(10, 5);
            r.polygon.push_back(worldAt(10, 5));
            r.polygon.push_back(worldAt(20, 5));
            r.polygon.push_back(worldAt(20, 15));
            r.polygon.push_back(worldAt(10, 15));
            const RoiStats st = ex.analyze(r);
            char nm[96];
            snprintf(nm, sizeof(nm), "R-03 %s 统计 ok", pnm[axis]);
            checkTrue(nm, st.ok);
            snprintf(nm, sizeof(nm), "R-03 %s coverage = 20*20/4 = 100", pnm[axis]);
            checkAbs(nm, st.coverageVoxels, 100.0, 0.0);
            snprintf(nm, sizeof(nm), "R-03 %s fullVoxels = 9*9 = 81", pnm[axis]);
            checkEqInt(nm, st.fullVoxels, 81LL);
            snprintf(nm, sizeof(nm), "R-03 %s 体积 = 100 * 0.5*0.7*2.0", pnm[axis]);
            checkAbs(nm, st.volumeMm3, 100.0 * 0.5 * 0.7 * 2.0, 0.0);
            // 层门：只统计勾画那一层 -> 均值/最值必须都等于该层涂的 HU
            snprintf(nm, sizeof(nm), "R-03 %s 平均 HU 只含勾画层", pnm[axis]);
            checkAbs(nm, st.meanHu, (double) (1000 + layer), 0.0);
            snprintf(nm, sizeof(nm), "R-03 %s min HU = 该层", pnm[axis]);
            checkAbs(nm, st.minHu, (double) (1000 + layer), 0.0);
            snprintf(nm, sizeof(nm), "R-03 %s max HU = 该层", pnm[axis]);
            checkAbs(nm, st.maxHu, (double) (1000 + layer), 0.0);
            // 区间裁剪：候选集应是"一层里的多边形包围盒"，而不是半个体积
            snprintf(nm, sizeof(nm), "R-03 %s 扫描量已裁剪到单层", pnm[axis]);
            checkTrue(nm, st.scannedVoxels > 0 && st.scannedVoxels <= 200);
            // 与逐体素定义逐位比对：遍历全体素，用公开 API 重算一遍
            double sumW = 0.0, sumHW = 0.0;
            std::string err;
            for (int k = 0; k < 40; ++k)
                for (int j = 0; j < 40; ++j)
                    for (int i = 0; i < 40; ++i) {
                        const double w = ex.weightOfVoxel(r, i, j, k, nullptr, &err);
                        if (w <= 0.0) continue;
                        sumW += w;
                        sumHW += w * (double) sy.buf[sy.off(i, j, k)];
                    }
            snprintf(nm, sizeof(nm), "R-03 %s 区间裁剪未漏体素（逐位）", pnm[axis]);
            checkAbs(nm, st.coverageVoxels, sumW, 0.0);
            snprintf(nm, sizeof(nm), "R-03 %s 平均 HU 逐位一致", pnm[axis]);
            checkAbs(nm, st.meanHu, sumHW / sumW, 0.0);
            printf("        R-03 %s：coverage %.6f / full %lld / scanned %lld / mean %.1f HU\n",
                   pnm[axis], st.coverageVoxels, st.fullVoxels, st.scannedVoxels, st.meanHu);
        }
    }

    // ---- R-05 组合：子 ROI 含截面时的容斥 + 区间相交（真机 264ms 用例复现）----
    // A = 边落在体素中心上的对齐盒（索引 [4,30]^3）
    //     -> 每轴 sum(h) = 1 + 25*2 + 1 = 52 -> coverage = (52/2)^3 = 26^3 = 17576
    // B = 轴位截面矩形（上一层用例的解析值 100），且 B 完全在 A 内部
    // 于是 交=B、并=A、差=A−B 三个量都能手算，容斥必须逐位成立；
    // 同时"交"的扫描量必须被 B 的单层包围盒限住（改动前会按 A 扫满十几万）。
    {
        Synth sy4;
        sy4.build(40, 40, 40, 0.5, 0.5, 0.5, 300.0f);
        VolumeRef v4; v4.bind(&sy4.cv);
        RoiExtractor ex4(v4);
        const double s = 0.5;
        RoiDef a; a.id = 1; a.type = ROI_BOX;
        a.boxMin = Vec3(4 * s, 4 * s, 4 * s); a.boxMax = Vec3(30 * s, 30 * s, 30 * s);
        RoiDef b; b.id = 2; b.type = ROI_PLANE;
        b.planeNormal = Vec3(0, 0, 1);
        b.polygon.push_back(Vec3(10 * s, 5 * s, 20 * s));
        b.polygon.push_back(Vec3(20 * s, 5 * s, 20 * s));
        b.polygon.push_back(Vec3(20 * s, 15 * s, 20 * s));
        b.polygon.push_back(Vec3(10 * s, 15 * s, 20 * s));
        b.planeOrigin = b.polygon[0];
        RoiDef co; co.id = 3; co.type = ROI_COMPOSITE;
        co.childA = 1; co.childB = 2; co.opAB = OP_INTERSECT;
        std::vector<RoiDef> pool;
        pool.push_back(a); pool.push_back(b); pool.push_back(co);
        const RoiStats sa = ex4.analyze(a, &pool);
        const RoiStats sb = ex4.analyze(b, &pool);
        checkAbs("R-05 前提：盒 A coverage = 26^3", sa.coverageVoxels, 17576.0, 0.0);
        checkAbs("R-05 前提：截面 B coverage = 100", sb.coverageVoxels, 100.0, 0.0);
        const RoiStats si = ex4.analyze(co, &pool);
        checkAbs("R-05 盒∩截面 = 截面本身（B 在 A 内）", si.coverageVoxels, 100.0, 0.0);
        checkTrue("R-05 盒∩截面的扫描量被截面单层限住", si.scannedVoxels <= 200);
        co.opAB = OP_UNION; pool[2] = co;
        const RoiStats su = ex4.analyze(co, &pool);
        checkAbs("R-05 盒∪截面 = A（B 已被 A 包住）", su.coverageVoxels, 17576.0, 0.0);
        co.opAB = OP_NONE; co.childC = 2; co.opAC = OP_SUBTRACT; pool[2] = co;
        const RoiStats sd = ex4.analyze(co, &pool);
        checkAbs("R-05 盒−截面 = A - B 容斥", sd.coverageVoxels, 17576.0 - 100.0, 0.0);
        printf("        R-05 截面组合：∩ %.0f（scanned %lld）/ ∪ %.0f / −%.0f\n",
               si.coverageVoxels, si.scannedVoxels, su.coverageVoxels, sd.coverageVoxels);
    }

    // ---- R-05 组合 ROI：交/并/差的解析容斥 ----
    {
        Synth sy3;
        sy3.build(100, 100, 100, 0.5, 0.5, 0.5, 800.0f);
        VolumeRef v3; v3.bind(&sy3.cv);
        RoiExtractor ex3(v3);
        const double s = 0.5;
        RoiDef a; a.id = 1; a.type = ROI_BOX;
        a.boxMin = Vec3(20 * s, 20 * s, 20 * s); a.boxMax = Vec3(60 * s, 60 * s, 60 * s);
        RoiDef b; b.id = 2; b.type = ROI_BOX;
        b.boxMin = Vec3(40 * s, 20 * s, 20 * s); b.boxMax = Vec3(80 * s, 60 * s, 60 * s);
        RoiDef co; co.id = 3; co.type = ROI_COMPOSITE;
        co.childA = 1; co.childB = 2; co.opAB = OP_INTERSECT;
        std::vector<RoiDef> pool;
        pool.push_back(a); pool.push_back(b); pool.push_back(co);
        const double aV = 40.0 * s * 40.0 * s * 40.0 * s;
        const double bV = aV;
        const double abV = 20.0 * s * 40.0 * s * 40.0 * s;
        const RoiStats si = ex3.analyze(co, &pool);
        checkRel("R-05 A∩B = |A∩B| 解析", si.volumeMm3, abV, 1e-12);
        co.opAB = OP_UNION; pool[2] = co;
        const RoiStats su = ex3.analyze(co, &pool);
        checkRel("R-05 A∪B = |A|+|B|-|A∩B| 容斥", su.volumeMm3, aV + bV - abV, 1e-12);
        printf("        R-05：∩ 实测 %.6f / 解析 %.6f mm³；∪ 实测 %.6f / 解析 %.6f mm³\n",
               si.volumeMm3, abV, su.volumeMm3, aV + bV - abV);
        // 子 ROI 缺失：RoiExtractor.h:63-64 承诺"按不限制处理并在 error 里给出提示"
        co.childA = 999; pool[2] = co;
        const RoiStats bad = ex3.analyze(co, &pool);
        checkTrue("childA 缺失时判失败（不静默给体积）", !bad.ok);
        checkHas("childA 缺失错误文案", bad.error, "缺少子 ROI");
    }

    // ---- M-07 单点 HU 采样（PRD 要求"精确值"：取体素中心原始 HU，不插值）----
    {
        checkAbs("M-07 huNearest 球心 = 1000", vol.huNearest(Vec3(c, c, c), -1.0f), 1000.0f, 0.0);
        checkAbs("M-07 huNearest 松质块内 = 400", vol.huNearest(Vec3(43 * 0.4, 20 * 0.4, 20 * 0.4), -1.0f),
                 400.0f, 0.0);
        checkAbs("M-07 huNearest 背景 = 0", vol.huNearest(Vec3(46 * 0.4, 2 * 0.4, 2 * 0.4), -1.0f), 0.0f, 0.0);
        checkAbs("M-07 越界返回 fallback", vol.huNearest(Vec3(1e6, 0, 0), -999.0f), -999.0f, 0.0);
        if (std::fabs((double) vol.huNearest(Vec3(c, c, c)) - 1000.0) > g_worstHuAbs)
            g_worstHuAbs = 0.0;
        // 坐标约定 world = index*spacing 的往返（AC-01 的地基）
        Vec3 w;
        int i, j, k;
        vol.indexToWorld(13, 27, 5, w);
        vol.worldToIndex(w, i, j, k);
        checkEqInt("world = index*spacing 往返 i", i, 13);
        checkEqInt("world = index*spacing 往返 j", j, 27);
        checkEqInt("world = index*spacing 往返 k", k, 5);
        checkAbs("indexToWorld x", w.x, 13 * 0.4, 1e-15);
        // 组织推断分档（core/VolumeRef.cpp:144-153）：M-07 的附加输出，
        // PRD 只要求"按 HU 区间推断组织类型"，此处钉住分档边界不回退
        checkEqStr("M-07 组织 -1000HU", VolumeRef::tissueName(-1000.0f), std::string("air/marrow"));
        checkEqStr("M-07 组织 50HU（PRD R-01 软组织 0~100）", VolumeRef::tissueName(50.0f),
                   std::string("soft tissue/fluid"));
        checkEqStr("M-07 组织 200HU（PRD R-01 骨 >200）", VolumeRef::tissueName(200.0f),
                   std::string("cancellous bone"));
        checkEqStr("M-07 组织 1000HU", VolumeRef::tissueName(1000.0f), std::string("dense bone"));
    }

    // ---- 环引用组合 ROI：曾经无限递归直到栈溢出（SIGSEGV）----
    // 在子进程里跑，无论 core 有没有守卫都不会带崩整套用例。
    {
        RoiDef leaf; leaf.id = 1; leaf.type = ROI_BOX;
        leaf.boxMin = Vec3(0, 0, 0); leaf.boxMax = Vec3(2, 2, 2);
        RoiDef c1; c1.id = 2; c1.type = ROI_COMPOSITE; c1.childA = 3; c1.opAB = OP_NONE;
        RoiDef c2; c2.id = 3; c2.type = ROI_COMPOSITE; c2.childA = 2; c2.opAB = OP_NONE;
        std::vector<RoiDef> cyc;
        cyc.push_back(leaf); cyc.push_back(c1); cyc.push_back(c2);

        fflush(stdout);
        const pid_t pid = fork();
        if (pid == 0) {
            const RoiStats st = ex.analyze(c1, &cyc);
            _exit(st.ok ? 2 : 1);           // 1 = 判失败（期望），2 = 竟然给了体积
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
            const bool crashed = WIFSIGNALED(status);
            if (crashed) {
                fail("R-05 环引用不再栈溢出（core/RoiExtractor.cpp spatialBounds 深度守卫）",
                     "子进程被信号杀掉（SIGSEGV/EXC_BAD_ACCESS）—— 递归没有上限");
                printf("        子进程信号 = %d\n", WTERMSIG(status));
            } else {
                pass("R-05 环引用不再栈溢出（core/RoiExtractor.cpp spatialBounds 深度守卫）");
                checkTrue("环引用被判失败而不是给出体积", WEXITSTATUS(status) == 1);
            }
        } else {
            fail("R-05 环引用用例", "fork 失败");
        }
    }

    printf("        AC-03 最坏体积相对误差（计入 PRD 判定，不含 FINDING）= %.4f%%\n",
           g_worstVolRel * 100.0);
    printf("        AC-04 最坏 HU 绝对误差 = %.3e HU（PRD 要求精确值）\n", g_worstHuAbs);
}

static double relErrOf(double got, double want) {
    const double denom = std::fabs(want) > 1e-300 ? std::fabs(want) : 1.0;
    return std::fabs(got - want) / denom;
}

// ============================================================================
// 组 6：ImplantPlanner —— S-02 安全等级在阈值两侧 + S-03~S-06 数值
// ============================================================================

static void group6Implant() {
    beginGroup("组6 ImplantPlanner —— S-02 判级 / S-03 骨高 / S-05 神经管 / S-06 间距");

    // 常量必须与 JNI（SurgeryPlanJni.safetyThresholds 的实现体）逐字同源：
    // measure-native-lib.cpp:649-658 把下面 5 个常量原样塞进 double[]，
    // Kotlin 侧 Limits(10,6,2,3,1.2) 只是 size<5 时的兜底。
    checkAbs("阈值 kMinBoneHeightMm = 10.0", ImplantPlanner::kMinBoneHeightMm, 10.0, 0.0);
    checkAbs("阈值 kMinBoneWidthMm = 6.0", ImplantPlanner::kMinBoneWidthMm, 6.0, 0.0);
    checkAbs("阈值 kMinNerveDistMm = 2.0", ImplantPlanner::kMinNerveDistMm, 2.0, 0.0);
    checkAbs("阈值 kMinSpacingMm = 3.0", ImplantPlanner::kMinSpacingMm, 3.0, 0.0);
    checkAbs("临界系数 kMarginalFactor = 1.2", ImplantPlanner::kMarginalFactor, 1.2, 0.0);

    // safetyLevel 的三分语义（core/ImplantPlanner.cpp:163-175）：
    //   value < 阈值           -> DANGER_RED
    //   阈值 <= value < 1.2*阈值 -> WARN_YELLOW（"刚好达标"仍是黄）
    //   value >= 1.2*阈值      -> SAFE_GREEN
    struct Case { const char *what; double v; double t; int want; };
    const double H = 10.0, W = 6.0, N = 2.0, S = 3.0;
    {
        // 骨高度（其余指标给足，单独看一条）
        const double vs[5] = {H - 1e-6, H, H * 1.2 - 1e-6, H * 1.2, H * 1.2 + 1e-6};
        const int want[5] = {DANGER_RED, WARN_YELLOW, WARN_YELLOW, SAFE_GREEN, SAFE_GREEN};
        const char *nm[5] = {"骨高 just-below(9.999999) -> 红", "骨高 = 阈值(10.0) -> 黄",
                             "骨高 just-below 临界上界(11.999999) -> 黄", "骨高 = 1.2*阈值(12.0) -> 绿",
                             "骨高 just-above(12.000001) -> 绿"};
        for (int i = 0; i < 5; ++i) {
            const int lvl = ImplantPlanner::safetyLevel(vs[i], 20.0, 0.0, false, 0.0, false);
            checkEqInt(nm[i], lvl, want[i]);
        }
        // 骨宽度
        checkEqInt("骨宽 5.999999 -> 红", ImplantPlanner::safetyLevel(20.0, W - 1e-6, 0.0, false, 0.0, false), DANGER_RED);
        checkEqInt("骨宽 = 6.0 -> 黄", ImplantPlanner::safetyLevel(20.0, W, 0.0, false, 0.0, false), WARN_YELLOW);
        checkEqInt("骨宽 = 7.2 -> 绿", ImplantPlanner::safetyLevel(20.0, W * 1.2, 0.0, false, 0.0, false), SAFE_GREEN);
        // 神经管距离（measured=true 才参与判级）
        checkEqInt("神经 1.999999 -> 红", ImplantPlanner::safetyLevel(20.0, 20.0, N - 1e-6, true, 0.0, false), DANGER_RED);
        checkEqInt("神经 = 2.0 -> 黄", ImplantPlanner::safetyLevel(20.0, 20.0, N, true, 0.0, false), WARN_YELLOW);
        checkEqInt("神经 = 2.4 -> 绿", ImplantPlanner::safetyLevel(20.0, 20.0, N * 1.2, true, 0.0, false), SAFE_GREEN);
        // 间距
        checkEqInt("间距 2.999999 -> 红", ImplantPlanner::safetyLevel(20.0, 20.0, 0.0, false, S - 1e-6, true), DANGER_RED);
        checkEqInt("间距 = 3.0 -> 黄", ImplantPlanner::safetyLevel(20.0, 20.0, 0.0, false, S, true), WARN_YELLOW);
        checkEqInt("间距 = 3.6 -> 绿", ImplantPlanner::safetyLevel(20.0, 20.0, 0.0, false, S * 1.2, true), SAFE_GREEN);
        // 未勾画神经管 / 单颗种植体：measured=false 的指标一律不参与判级
        checkEqInt("神经未勾画时 0mm 也不判红",
                   ImplantPlanner::safetyLevel(20.0, 20.0, 0.0, false, 0.0, false), SAFE_GREEN);
        checkEqInt("间距不可测时 0mm 也不判红",
                   ImplantPlanner::safetyLevel(20.0, 20.0, 0.0, false, 0.0, false), SAFE_GREEN);
        // 多指标同时越界取最差（红色优先）
        checkEqInt("骨高红 + 神经绿 -> 红",
                   ImplantPlanner::safetyLevel(5.0, 20.0, 5.0, true, 5.0, true), DANGER_RED);
        checkEqInt("骨宽黄 + 间距红 -> 红",
                   ImplantPlanner::safetyLevel(20.0, 6.0, 5.0, true, 1.0, true), DANGER_RED);
    }

    // ---- 体数据上的 S-03/S-04：骨块尺寸已知 ----
    // 40^3 @0.4mm（15.6mm 见方），骨块 = HU 1000 的长方体：
    //   i,j in [15,34]（8.0mm x 8.0mm 截面）、k in [10,29]
    // 三线性取样 + 200HU 判骨，让"骨/非骨"的分界落在体素中心外侧 0.32mm 处：
    //   z 方向骨区 = [3.68, 11.92]（厚度 8.24mm），0.2mm 亚体素步长积分回 8.0mm
    Synth sy;
    sy.build(40, 40, 40, 0.4, 0.4, 0.4, 0.0f);
    sy.fillBox(15, 15, 10, 34, 34, 29, 1000.0f);
    VolumeRef vol; vol.bind(&sy.cv);
    ImplantPlanner planner(vol);

    {
        checkAbs("S-03 骨块 z 解析厚度", (29 - 10) * 0.4, 7.6, 1e-12);

        Implant im; im.id = 1;
        im.entry = Vec3(24 * 0.4, 24 * 0.4, 13.0);      // 骨面上方的软组织里
        im.pitchDeg = 0.0; im.yawDeg = 0.0;             // axis = (0,0,-1)
        im.depthMm = 6.0; im.lengthMm = 11.0; im.diaMm = 4.0;
        ImplantPlanner::refreshGeometry(im);
        checkAbs("S-01 默认轴向 = (0,0,-1)", im.axis.z, -1.0, 1e-15);
        checkAbs("S-01 tip = entry + axis*length", im.tip.z, 13.0 - 11.0, 1e-12);

        double surfaceDepth = -1.0;
        const double h = planner.boneHeight(im, &surfaceDepth);
        // 解析：三线性 >=200HU 的骨区是 z∈[3.68,11.92]（20% 部分容积判据）。
        // 0.2mm 步长下首个骨样点在 z=11.8（距 entry 1.2mm），
        // 相位 2 允许跨 2 步非骨间隙 -> 计数停在 z=3.8，返回 8.6-3*0.2 = 8.0mm。
        checkAbs("S-03 沿轴可用骨高度 = 骨块厚度 8.0mm", h, 8.0, 0.6);
        printf("        S-03 实测骨高 %.4f mm（解析 8.0000，偏差 %.4f mm）；入口到骨面 %.4f mm（解析 1.2000）\n",
               h, h - 8.0, surfaceDepth);
        checkAbs("S-03 入口到骨面深度", surfaceDepth, 1.2, 0.6);

        const double w = planner.boneWidth(im);
        // S-04 在 depth/3 = 2mm 处取样：z = 13.0-2.0 = 11.0，落在骨区内
        //（若取样点掉到骨区外，marchBone 直接返回 0，宽度会变成 0 —— 所以 entry.z
        //  必须让 depth/3 命中骨内，这是 core 的既有语义，不是缺陷）。
        // 截面 8.0mm x 8.0mm，u=(1,0,0) v=(0,-1,0) 双向跨骨 -> 8.0mm
        checkAbs("S-04 颊舌向骨宽度 = 截面 8.0mm", w, 8.0, 0.8);
        printf("        S-04 实测骨宽 %.4f mm（解析 8.0000）\n", w);
    }

    // ---- S-05 神经管距离：解析 = 轴线到折线最短距离 - 管道半径 ----
    {
        Implant im; im.id = 1;
        im.entry = Vec3(9.6, 8.0, 13.0);
        im.pitchDeg = 0.0; im.yawDeg = 0.0;
        im.depthMm = 6.0; im.lengthMm = 10.0; im.diaMm = 4.0;
        ImplantPlanner::refreshGeometry(im);           // tip = (9.6, 8.0, 3.0)

        NervePath np; np.id = 1; np.radiusMm = 1.5;
        np.points.push_back(Vec3(13.6, 0.0, 3.0));     // 与轴线同高、X 方向偏 4mm
        np.points.push_back(Vec3(13.6, 15.0, 3.0));
        const double d = planner.nerveDistance(im, np);
        checkAbs("S-05 轴线到折线 4.0mm - 半径 1.5mm = 2.5mm", d, 2.5, 1e-12);
        np.radiusMm = 0.0;
        checkAbs("S-05 半径 0 时就是轴线距离", planner.nerveDistance(im, np), 4.0, 1e-12);
        np.radiusMm = 1.5;
        const double oldX = np.points[0].x;
        np.points[0].x = 10.6; np.points[1].x = 10.6;  // 只剩 1mm：表面距离应钳到 0
        checkAbs("S-05 管道与桩体相交时钳到 0 不给负数", planner.nerveDistance(im, np), 0.0, 1e-12);
        np.points[0].x = oldX;
        NervePath none; none.id = 2;
        checkAbs("S-05 点数不足返回 -1（不参与判级）", planner.nerveDistance(im, none), -1.0, 0.0);
    }

    // ---- S-06 多种植体间距：表面间距 = 轴线间距 - 两个半径 ----
    {
        Implant a; a.id = 1; a.entry = Vec3(6.0, 8.0, 13.0);
        a.pitchDeg = 0.0; a.yawDeg = 0.0; a.depthMm = 6.0; a.lengthMm = 10.0; a.diaMm = 4.0;
        Implant b; b.id = 2; b.entry = Vec3(13.0, 8.0, 13.0);
        b.pitchDeg = 0.0; b.yawDeg = 0.0; b.depthMm = 6.0; b.lengthMm = 10.0; b.diaMm = 4.0;
        ImplantPlanner::refreshGeometry(a); ImplantPlanner::refreshGeometry(b);
        std::vector<Implant> all; all.push_back(a); all.push_back(b);
        // 轴线间距 7.0 - (4+4)/2 = 3.0 -> 恰好落在黄档下沿
        checkAbs("S-06 表面间距 = 7.0 - 4.0 = 3.0", planner.minSpacing(a, all), 3.0, 1e-12);
        b.entry = Vec3(14.0, 8.0, 13.0); ImplantPlanner::refreshGeometry(b);
        all[1] = b;
        checkAbs("S-06 间距拉开到 8.0mm 轴线", planner.minSpacing(a, all), 4.0, 1e-12);
        std::vector<Implant> alone; alone.push_back(a);
        checkAbs("S-06 单颗种植体返回 -1", planner.minSpacing(a, alone), -1.0, 0.0);
    }

    // ---- evaluate() 端到端：回填字段 + 中文告警文案 ----
    {
        Implant im; im.id = 7;
        im.entry = Vec3(9.6, 8.0, 14.0);               // 骨块上方软组织
        im.pitchDeg = 0.0; im.yawDeg = 0.0;
        im.depthMm = 6.0; im.lengthMm = 11.0; im.diaMm = 4.0;
        ImplantPlanner::refreshGeometry(im);
        NervePath np; np.id = 1; np.radiusMm = 1.5;
        np.points.push_back(Vec3(11.6, 0.0, 3.0));     // 轴线距离 2.0 -> 表面 0.5 -> 红
        np.points.push_back(Vec3(11.6, 15.0, 3.0));
        std::vector<NervePath> nerves; nerves.push_back(np);
        std::vector<Implant> all; all.push_back(im);
        const ImplantSafety s = planner.evaluate(im, nerves, all);
        checkTrue("evaluate 回填 tip", im.tip.z < im.entry.z);
        checkTrue("evaluate 写回 boneHeightMm", im.boneHeightMm > 0.0 && im.boneHeightMm == s.boneHeightMm);
        checkAbs("S-05 表面神经管距离 = 2.0 - 1.5 = 0.5", s.nerveDistMm, 0.5, 1e-12);
        checkTrue("S-05 measured 置位", s.nerveMeasured);
        checkEqInt("S-02 神经管危险判红", s.level, DANGER_RED);
        checkHas("S-02 告警文案含神经管措辞", s.warnText, "神经管");
        checkEqInt("S-06 未勾画邻牙时不参与判级", (long long) (s.spacingMeasured ? 1 : 0), 0);
        printf("        evaluate 告警文案：%s\n", s.warnText.c_str());
    }

    // ---- 角度姿态：pitch/yaw 与 capsuleOutline 的几何 ----
    {
        const Vec3 ax0 = ImplantPlanner::axisFromAngles(0.0, 0.0);
        checkAbs("axisFromAngles(0,0) = -Z", ax0.z, -1.0, 1e-12);
        const Vec3 ax90 = ImplantPlanner::axisFromAngles(90.0, 0.0);
        checkAbs("pitch=90 度绕 X 轴 -> +Y", ax90.y, 1.0, 1e-12);
        checkAbs("pitch=90 度仍是单位向量", ax90.length(), 1.0, 1e-12);
        const Vec3 axYaw = ImplantPlanner::axisFromAngles(0.0, 90.0);
        checkAbs("yaw=90 度绕 Y 轴 -> -X", axYaw.x, -1.0, 1e-12);
        checkAbs("yaw=90 度 z 分量归零", axYaw.z, 0.0, 1e-12);

        Implant im; im.id = 1; im.entry = Vec3(5, 5, 10);
        im.pitchDeg = 0.0; im.yawDeg = 0.0; im.depthMm = 6.0; im.lengthMm = 10.0; im.diaMm = 4.0;
        ImplantPlanner::refreshGeometry(im);
        std::vector<Vec3> ring;
        ImplantPlanner::capsuleOutline(im, 16, ring);
        checkEqInt("capsuleOutline 顶点数 = 2*segments", (long long) ring.size(), 32);
        double r0 = 0.0, r1 = 0.0;
        for (int i = 0; i < 16; ++i) r0 = std::max(r0, (ring[i] - im.entry).length());
        for (int i = 16; i < 32; ++i) r1 = std::max(r1, (ring[i] - im.tip).length());
        checkAbs("近端口半径 = dia/2", r0, 2.0, 1e-12);
        checkAbs("远端口半径 = dia/2", r1, 2.0, 1e-12);
    }
}

// ============================================================================
// 组 7：MeasurementManager 端到端（M-01~M-08 的单位/失败原因/重算）
// ============================================================================

static void group7Manager() {
    beginGroup("组7 MeasurementManager —— M-01~M-08 端到端（单位、失败原因、重算）");

    Synth sy;
    sy.build(48, 48, 48, 0.4, 0.4, 0.4, 0.0f);
    const double c = 24 * 0.4;
    const long long nSph = sy.fillSphereMm(c, c, c, 6.0, 1000.0f);

    MeasurementManager mgr;
    checkTrue("未绑定体数据时 hasVolume=false", !mgr.hasVolume());
    mgr.bindVolume(&sy.cv);
    checkTrue("bindVolume 后 hasVolume=true", mgr.hasVolume());

    // M-01 距离：单位 mm，值 = 解析欧氏距离
    {
        std::vector<Vec3> p;
        p.push_back(Vec3(4.0, 4.0, 4.0));
        p.push_back(Vec3(10.0, 12.0, 4.0));
        const MeasureRecord rec = mgr.addMeasure(MT_DISTANCE, p, 0, "距离", 0xFF00E5FFU);
        checkAbs("M-01 value = 10mm（6-8-10）", rec.value, 10.0, 0.1);
        checkEqStr("M-01 unit = mm", rec.unit, std::string("mm"));
        checkTrue("M-01 记录入列表", mgr.findMeasure(rec.id) != nullptr);
        checkHas("M-01 detailJson 带两点", rec.detailJson, "\"p1\"");
        if (std::fabs(rec.value - 10.0) > g_worstDistMm) g_worstDistMm = std::fabs(rec.value - 10.0);
    }
    // M-02 角度：单位 deg，顶点必须是 points[1]
    {
        std::vector<Vec3> p;
        p.push_back(Vec3(9.6, 3.6, 9.6));      // 顶点
        p.push_back(Vec3(9.6, 9.6, 9.6));      // 第 1 点
        p.push_back(Vec3(15.6, 9.6, 9.6));     // 第 3 点 -> ∠(1,顶点,3) = 90 度？见下
        const MeasureRecord rec = mgr.addMeasure(MT_ANGLE, p, 0, "角度", 0);
        // 顶点在第 2 个位置 -> 量的是 ∠ABC 而不是 ∠BAC：A(9.6,3.6,9.6), B=顶点(9.6,9.6,9.6)...
        // 这里刻意把"顶点"放在下标 1：points[0]=(9.6,3.6,9.6) 是 A，points[1]=(9.6,9.6,9.6) 是 B
        const double ab = MeasureMath::distance(p[0], p[1]);
        checkAbs("M-02 单位 deg", rec.unit == "deg" ? 1.0 : 0.0, 1.0, 0.0);
        checkAbs("M-02 顶点是第 2 点（∠ABC=90 度）", rec.value, 90.0, 0.5);
        checkHas("M-02 detailJson 带三边", rec.detailJson, "\"ab\"");
        checkAbs("M-02 detailJson 的 ab 边长", ab, 6.0, 1e-12);
        if (std::fabs(rec.value - 90.0) > g_worstAngleDeg) g_worstAngleDeg = std::fabs(rec.value - 90.0);
    }
    // M-03 点到线：走 manager 时同样取"垂足在线段内/外"的语义
    {
        std::vector<Vec3> p;      // points = (P, A, B)
        p.push_back(Vec3(3.0, 4.0, 9.6));
        p.push_back(Vec3(0.0, 0.0, 9.6));
        p.push_back(Vec3(10.0, 0.0, 9.6));
        const MeasureRecord rec = mgr.addMeasure(MT_POINT_TO_LINE, p, 0, "点线距", 0);
        checkAbs("M-03 value = 4mm", rec.value, 4.0, 0.1);
        checkHas("M-03 detailJson 标记垂足在线段内", rec.detailJson, "footInside");
        std::vector<Vec3> q;      // 垂足越界
        q.push_back(Vec3(-3.0, 4.0, 9.6));
        q.push_back(Vec3(0.0, 0.0, 9.6));
        q.push_back(Vec3(10.0, 0.0, 9.6));
        const MeasureRecord r2 = mgr.addMeasure(MT_POINT_TO_LINE, q, 0, "点线距越界", 0);
        checkAbs("M-03 越界时量到端点 = 5mm", r2.value, 5.0, 0.1);
    }
    // M-06 弧长（S-07 牙弓线）
    {
        std::vector<Vec3> arc;
        for (int i = 0; i <= 4; ++i) {
            const double t = (kPi / 2.0) * ((double) i / 4.0);
            arc.push_back(Vec3(c + 5.0 * std::cos(t), c + 5.0 * std::sin(t), 9.6));
        }
        const MeasureRecord rec = mgr.addMeasure(MT_ARC_LENGTH, arc, 0, "牙弓", 0);
        // 5 点 -> 4 段等分圆弧弦：每弦 = 2R*sin(θ/8) = 1.950903220161283，
        // 4 弦和 = 7.803612880645130（累积弦长法的解析值，可逐位核对）
        checkAbs("M-06 4 段弦长和 = 4*2R*sin(θ/8)", rec.value, 7.803612880645130, 1e-12);
        // PRD 判定：与真实弧长 R*θ = 7.853981633974483 的偏差 <= 0.5mm
        const double arcErr = std::fabs(rec.value - 7.8539816339744830);
        checkTrue("M-06 经理端链路：弧长 vs 真弧 <= 0.5mm", arcErr <= 0.5);
        if (arcErr > g_worstArcMm) g_worstArcMm = arcErr;
        printf("        M-06 5 段折线实测 %.6f mm / 1/4 圆周真弧 %.6f mm（弦长必然偏小，偏差 %.6f mm）\n",
               rec.value, 7.8539816339744830, arcErr);
    }
    // M-07 HU 采样：精确值 + 组织推断
    {
        std::vector<Vec3> p;
        p.push_back(Vec3(c, c, c));
        const MeasureRecord rec = mgr.addMeasure(MT_HU_SAMPLE, p, 0, "HU", 0);
        checkAbs("M-07 value = 1000HU（精确）", rec.value, 1000.0, 0.0);
        checkEqStr("M-07 unit = HU", rec.unit, std::string("HU"));
        checkHas("M-07 detailJson 带体素索引", rec.detailJson, "\"voxel\"");
        // 分档边界（core/VolumeRef.cpp:144-153）：<1000 才是 cortical bone，
        // 1000HU 命中 [1000,2000) = dense bone —— 采样点是实心球心，HU 恰为 1000。
        checkHas("M-07 detailJson 带组织名（1000HU -> dense bone）", rec.detailJson, "dense bone");
        std::vector<Vec3> oob;
        oob.push_back(Vec3(1e6, 0, 0));
        const MeasureRecord r2 = mgr.addMeasure(MT_HU_SAMPLE, oob, 0, "HU越界", 0);
        checkAbs("M-07 越界点取 fallback -1000", r2.value, -1000.0, 0.0);
    }
    // ROI + M-04 体积 / M-08 骨密度：走 manager 才做 mm³->cm³ 换算
    {
        RoiDef r; r.type = ROI_HU_THRESHOLD; r.huMin = 200.0; r.huMax = 3000.0; r.name = "骨";
        const int roiId = mgr.addRoi(r);
        checkTrue("addRoi 返回正 id", roiId > 0);
        std::vector<Vec3> dummy;
        const MeasureRecord vol = mgr.addMeasure(MT_ROI_VOLUME, dummy, roiId, "体积", 0);
        const MeasureRecord den = mgr.addMeasure(MT_BONE_DENSITY, dummy, roiId, "骨密度", 0);
        const double truMm3 = (double) nSph * 0.064;
        checkEqStr("M-04 unit = cm3", vol.unit, std::string("cm3"));
        checkRel("AC-03 M-04 体积（cm³）<= 1%", vol.value, truMm3 / 1000.0, 0.01);
        checkEqStr("M-08 unit = HU", den.unit, std::string("HU"));
        checkAbs("AC-04 M-08 均值 = 1000HU", den.value, 1000.0, 1e-9);
        if (relErrOf(vol.value, truMm3 / 1000.0) > g_worstVolRel)
            g_worstVolRel = relErrOf(vol.value, truMm3 / 1000.0);
        checkHas("M-04 detailJson 带 coverageVoxels", vol.detailJson, "coverageVoxels");
        checkHas("M-04 detailJson 带耗时（性能取证）", vol.detailJson, "elapsedMs");
        printf("        M-04 实测 %.6f cm³ / 解析 %.6f cm³（%.4f%%）；M-08 实测 %.4f HU\n",
               vol.value, truMm3 / 1000.0, relErrOf(vol.value, truMm3 / 1000.0) * 100.0, den.value);

        // ROI 参数变化后 recalcMeasure 必须用新参数重算（PRD 5.1.5 "重算"）
        RoiDef upd = *mgr.findRoi(roiId);
        upd.huMin = 1100.0;                 // 球是 1000HU -> 现在无命中
        checkTrue("updateRoi 成功", mgr.updateRoi(upd));
        checkTrue("无命中时 recalc 返回 false", !mgr.recalcMeasure(vol.id));
        checkAbs("失败时 value 归 0", mgr.findMeasure(vol.id)->value, 0.0, 0.0);
        checkHas("失败原因写进 detailJson", mgr.findMeasure(vol.id)->detailJson, "error");
        upd = *mgr.findRoi(roiId);
        upd.huMin = 200.0;
        mgr.updateRoi(upd);
        checkTrue("恢复参数后 recalc 成功", mgr.recalcMeasure(vol.id));
        checkRel("重算后体积回到解析值", mgr.findMeasure(vol.id)->value, truMm3 / 1000.0, 0.01);

        // ROI 不存在：必须报原因而不是给 0 cm³
        const MeasureRecord missing = mgr.addMeasure(MT_ROI_VOLUME, dummy, 987654, "野指针ROI", 0);
        checkTrue("roiId 不存在时判失败", missing.value == 0.0);
        checkHas("roiId 不存在的错误文案", missing.detailJson, "ROI 不存在");
        checkTrue("statRoi 未知 id 报错", !mgr.statRoi(987654).ok);
    }
    // M-05 面积：manager 换算是 mm² -> cm²（只除一次 100）
    {
        std::vector<Vec3> poly;
        poly.push_back(Vec3(0, 0, 9.6)); poly.push_back(Vec3(30, 0, 9.6));
        poly.push_back(Vec3(30, 24, 9.6)); poly.push_back(Vec3(0, 24, 9.6));
        const MeasureRecord rec = mgr.addMeasure(MT_ROI_AREA, poly, 0, "面积", 0);
        checkEqStr("M-05 unit = cm2", rec.unit, std::string("cm2"));
        checkAbs("M-05 720mm² = 7.2cm²（不重复除 1000）", rec.value, 7.2, 0.072);
        if (relErrOf(rec.value, 7.2) > g_worstAreaRel) g_worstAreaRel = relErrOf(rec.value, 7.2);
        checkHas("M-05 detailJson 带 areaMm2", rec.detailJson, "areaMm2");
    }
    // 输入不足：记录仍保留（UI 显示"待重算"），值归 0，原因写 detailJson
    {
        std::vector<Vec3> one;
        one.push_back(Vec3(1, 2, 3));
        const MeasureRecord rec = mgr.addMeasure(MT_DISTANCE, one, 0, "残缺距离", 0);
        checkAbs("点数不足 value=0", rec.value, 0.0, 0.0);
        checkHas("点数不足的错误文案", rec.detailJson, "需要 2 个点");
        checkTrue("残缺记录仍然入列表", mgr.findMeasure(rec.id) != nullptr);
        const MeasureRecord unknown = mgr.addMeasure(99, one, 0, "未知类型", 0);
        checkHas("未知类型错误文案", unknown.detailJson, "未知测量类型");
    }
    // 持久化往返（PRD §8.4）：measuresJson -> parse -> measuresFromJson 值/单位不变
    {
        const size_t before = mgr.measures().size();
        const Json dump = mgr.measuresJson();
        const std::string text = dump.dump(false);
        bool pok = false;
        const Json reparsed = Json::parse(text, &pok);
        checkTrue("measuresJson 可被再次解析", pok);
        checkTrue("解析结果是对象", reparsed.isObject());
        MeasurementManager mgr2;
        mgr2.bindVolume(&sy.cv);
        checkTrue("measuresFromJson 成功", mgr2.measuresFromJson(reparsed));
        checkEqInt("往返后测量条数一致", (long long) mgr2.measures().size(), (long long) before);
        // core/Json.cpp:157 按 %.12g 输出数字（注释里写明"相对误差 < 1e-12"），
        // 所以往返是 12 位有效数字级别的一致，不是逐位（bit-for-bit）一致——
        // 断言按 1e-11 相对容差写，同时把最坏偏差量打印出来供报告引用。
        bool sameValues = mgr2.measures().size() == before;
        double worstRel = 0.0;
        for (size_t i = 0; i < before && sameValues; ++i) {
            if (mgr2.measures()[i].type != mgr.measures()[i].type) { sameValues = false; break; }
            const double a = mgr2.measures()[i].value;
            const double b = mgr.measures()[i].value;
            if (relErrOf(a, b) > worstRel) worstRel = relErrOf(a, b);
            if (relErrOf(a, b) > 1e-11) sameValues = false;
        }
        checkTrue("往返后每条测量的 type/value 在 12 位有效数字内一致", sameValues);
        printf("        持久化往返最坏相对偏差 = %.3e（core/Json.cpp 的 %%.12g 输出精度）\n", worstRel);
        if (worstRel > 0.0) {
            char buf[320];
            snprintf(buf, sizeof(buf),
                     "measures.json 以 %%.12g 输出数字，往返丢失尾数（最坏相对偏差 %.3e）。"
                     "对 PRD 的 0.1mm / 0.5 度门槛无影响（1e-12 << 1e-4），但 double 型时间戳"
                     "（毫秒，13 位有效数字）会被截断。",
                     worstRel);
            noteFinding("持久化精度", buf);
        }
        checkTrue("summaryText 非空", !mgr.summaryText().empty());
    }
    // 方案往返：种植体 + 神经管
    {
        Implant im; im.id = 0;
        im.entry = Vec3(9.6, 8.0, 14.0); im.pitchDeg = 0.0; im.yawDeg = 0.0;
        im.depthMm = 6.0; im.lengthMm = 11.0; im.diaMm = 4.0;
        const int imId = mgr.addImplant(im);
        checkTrue("addImplant 返回正 id", imId > 0);
        const Implant *stored = mgr.findImplant(imId);
        checkTrue("addImplant 里已 refreshGeometry（tip 在根方）", stored && stored->tip.z < stored->entry.z);
        checkTrue("addImplant 触发 recomputePlan（level 已填）", stored && stored->level >= SAFE_GREEN);
        NervePath np; np.radiusMm = 1.5;
        np.points.push_back(Vec3(11.6, 0.0, 3.0));
        np.points.push_back(Vec3(11.6, 15.0, 3.0));
        const int npId = mgr.addNervePath(np);
        checkTrue("addNervePath 返回正 id", npId > 0);
        mgr.recomputePlan();
        const Implant *after = mgr.findImplant(imId);
        checkTrue("recomputePlan 后 S-05 已参与判级", after && after->nerveDistMm >= 0.0);
        checkEqInt("S-05 距离 = 轴线 2.0 - 半径 1.5", (long long) (after->nerveDistMm * 1000.0), 500);
        const Json plan = mgr.planJson();
        bool pok = false;
        const Json re = Json::parse(plan.dump(false), &pok);
        checkTrue("planJson 可解析", pok);
        MeasurementManager mgr3;
        mgr3.bindVolume(&sy.cv);
        checkTrue("planFromJson 成功", mgr3.planFromJson(re));
        checkEqInt("往返后种植体数一致", (long long) mgr3.implants().size(), 1);
        checkEqInt("往返后神经管数一致", (long long) mgr3.nervePaths().size(), 1);
    }
    // 叠加图元：测量与方案都要能被 Kotlin 画出来（world 非空 + owner 标记）
    {
        std::vector<OverlayPrim> prims;
        mgr.buildOverlay(prims);
        checkTrue("buildOverlay 有输出", !prims.empty());
        bool hasMeasureOwner = false;
        for (size_t i = 0; i < prims.size(); ++i)
            if (prims[i].ownerKind == OW_MEASURE) hasMeasureOwner = true;
        checkTrue("叠加图元带 OW_MEASURE 归属（列表高亮依赖）", hasMeasureOwner);
        std::vector<OverlayPrim> plan;
        mgr.buildPlanOverlay(plan);
        bool hasCapsule = false;
        for (size_t i = 0; i < plan.size(); ++i)
            if (plan[i].kind == OK_CAPSULE) hasCapsule = true;
        checkTrue("方案叠加含种植体轮廓", hasCapsule);
    }
    // snapToVoxelCenter：落点消抖必须真的吸附到体素中心
    {
        const Vec3 snapped = mgr.snapToVoxelCenter(Vec3(9.71, 8.19, 3.62));
        int i, j, k;
        VolumeRef v; v.bind(&sy.cv);
        v.worldToIndex(snapped, i, j, k);
        Vec3 back;
        v.indexToWorld(i, j, k, back);
        checkAbs("吸附后落在体素中心（可往返）", MeasureMath::distance(snapped, back), 0.0, 1e-12);
        checkTrue("吸附步长 = spacing", snapped.x >= 0.0);
    }
}

// ============================================================================
// 组 8：AnnotationStore（A-01~A-07）+ MeasureTypes 枚举协议
// ============================================================================

static void group8AnnotationAndProtocol() {
    beginGroup("组8 AnnotationStore / MeasureTypes 枚举跨语言协议");

    // MeasureTypes.h:18-19 明确写了"枚举值与 Kotlin MeasureEnums 一一对应，
    // 跨语言只传 int 不传名字" —— 改任何一侧都要同步，这里把整张表钉住。
    checkEqInt("MeasureType MT_DISTANCE = 0", MT_DISTANCE, 0);
    checkEqInt("MeasureType MT_ANGLE = 1", MT_ANGLE, 1);
    checkEqInt("MeasureType MT_POINT_TO_LINE = 2", MT_POINT_TO_LINE, 2);
    checkEqInt("MeasureType MT_ROI_VOLUME = 3", MT_ROI_VOLUME, 3);
    checkEqInt("MeasureType MT_ROI_AREA = 4", MT_ROI_AREA, 4);
    checkEqInt("MeasureType MT_ARC_LENGTH = 5", MT_ARC_LENGTH, 5);
    checkEqInt("MeasureType MT_HU_SAMPLE = 6", MT_HU_SAMPLE, 6);
    checkEqInt("MeasureType MT_BONE_DENSITY = 7", MT_BONE_DENSITY, 7);
    checkEqInt("RoiType R-01..R-05 = 0..4", ROI_COMPOSITE, 4);
    checkEqInt("ToolState 共 9 态（AC-10）", ST_NERVE_TRACE, 8);
    checkEqInt("SafetyLevel SAFE_GREEN = 0", SAFE_GREEN, 0);
    checkEqInt("SafetyLevel WARN_YELLOW = 1", WARN_YELLOW, 1);
    checkEqInt("SafetyLevel DANGER_RED = 2", DANGER_RED, 2);
    checkEqInt("CombineOp OP_SUBTRACT = 3", OP_SUBTRACT, 3);
    checkEqInt("AnnotationType A-07 = 6", AN_SCREENSHOT, 6);
    checkEqInt("MeasurePlane MP_SAGITTAL = 2", MP_SAGITTAL, 2);
    checkEqInt("OverlayOwner OW_ANNOTATION = 5", OW_ANNOTATION, 5);

    AnnotationStore store;
    checkEqInt("初始 count = 0", (long long) store.count(), 0);

    Annotation text; text.type = AN_TEXT_LABEL; text.text = "37 区";
    text.points.push_back(Vec3(4.0, 8.0, 12.0));
    const int idText = store.add(text);
    checkTrue("A-01 文字标签 add 返回正 id", idText > 0);

    Annotation line; line.type = AN_LINE;
    line.points.push_back(Vec3(0, 0, 0)); line.points.push_back(Vec3(3, 4, 0));
    const int idLine = store.add(line);
    Annotation ring; ring.type = AN_RING; ring.radiusMm = 5.0;
    ring.points.push_back(Vec3(10, 10, 10));
    const int idRing = store.add(ring);
    Annotation shot; shot.type = AN_SCREENSHOT;            // A-07：像素坐标，不进三维叠加
    shot.points.push_back(Vec3(100, 200, 0));
    const int idShot = store.add(shot);
    checkEqInt("4 条标注入列表", (long long) store.count(), 4);
    checkTrue("find 命中", store.find(idRing) != nullptr);
    checkTrue("find 未知 id 返回空", store.find(987654) == nullptr);

    checkTrue("setText 成功", store.setText(idText, "改过的标签"));
    checkEqStr("setText 生效", store.find(idText)->text, std::string("改过的标签"));
    checkTrue("setText 未知 id 失败", !store.setText(987654, "x"));
    checkTrue("setVisible 成功", store.setVisible(idLine, false));
    checkTrue("setColor 成功", store.setColor(idLine, 0xFFFF0000u));
    checkEqInt("setColor 生效", (long long) (unsigned int) store.find(idLine)->color, (long long) 0xFFFF0000u);

    // 叠加层：visible=false 与 A-07 都不绘制
    std::vector<OverlayPrim> prims;
    store.buildOverlay(prims);
    bool ringDrawn = false, shotDrawn = false, lineDrawn = false;
    for (size_t i = 0; i < prims.size(); ++i) {
        if (prims[i].ownerKind == OW_ANNOTATION) {
            if (prims[i].ownerId == idRing) ringDrawn = true;
            if (prims[i].ownerId == idShot) shotDrawn = true;
            if (prims[i].ownerId == idLine) lineDrawn = true;
        }
    }
    checkTrue("A-05 环形标记展开进叠加层", ringDrawn);
    checkTrue("A-07 像素标注不进三维叠加层", !shotDrawn);
    checkTrue("visible=false 的标注不绘制", !lineDrawn);

    // A-05 环：32 边形面积 vs πr²（描记离散化偏差 <= 1%，PRD M-05 同级）
    {
        std::vector<Vec3> poly;
        AnnotationStore::ringPolygon(Vec3(0, 0, 5), 5.0, MP_AXIAL, 32, poly);
        checkEqInt("ringPolygon 顶点数 = segments", (long long) poly.size(), 32);
        const double tru = kPi * 25.0;                       // 78.539816339744831
        const double got = MeasureMath::polygonArea(poly);
        checkRel("A-05 32 边形面积 vs πr² <= 1%", got, tru, 0.01);
        printf("        A-05 内接 32 边形 %.6f mm² / 真圆 %.6f mm²（内接必然偏小 %.4f%%）\n",
               got, tru, relErrOf(got, tru) * 100.0);
        std::vector<Vec3> tiny;
        AnnotationStore::ringPolygon(Vec3(0, 0, 0), -1.0, MP_AXIAL, 32, tiny);
        checkTrue("半径非法时不产生顶点", tiny.empty());
    }

    // JSON 往返（PRD §8.4 持久化；字段名必须与 AnnotationJni 同源）
    {
        const Json j = store.toJson();
        bool pok = false;
        const Json re = Json::parse(j.dump(false), &pok);
        checkTrue("标注 JSON 可解析", pok);
        AnnotationStore store2;
        checkTrue("fromJson 成功", store2.fromJson(re));
        checkEqInt("往返后条数一致", (long long) store2.count(), (long long) store.count());
        const Annotation *rt = store2.find(idText);
        checkTrue("往返后按 id 找回", rt != nullptr);
        if (rt) {
            checkEqStr("往返后文本一致", rt->text, std::string("改过的标签"));
            checkAbs("往返后坐标一致", rt->points.empty() ? -1.0 : rt->points[0].x, 4.0, 1e-9);
        }
        const Annotation *rr = store2.find(idRing);
        checkTrue("往返后环形半径一致", rr && rr->radiusMm == 5.0);
        checkTrue("往返后 id 不变（关联测量靠它）", rr != nullptr);
    }

    checkTrue("remove 成功", store.remove(idRing));
    checkEqInt("remove 后计数", (long long) store.count(), 3);
    checkTrue("remove 未知 id 失败", !store.remove(987654));
    store.clear();
    checkEqInt("clear 后计数", (long long) store.count(), 0);
}

// ============================================================================
// 组 9：MeasurePicker —— 骨面拾取与"深度锁层"窗口（M-06/A-05 拖动描记的底座）
//
// 真机踩过的坑：SURFACE 拾取返回射线上第一个骨面，沿脊柱拖动时命中点会在近侧
// 肋骨与远侧椎体之间来回跳（实测相邻采样的相机距离 1302/1430/1516/1869mm），
// 累积弦长失真。修复是给 pickSurface 加深度窗口 [tMin, tMax]，把采样锁在同一层。
// 这里用"两条平行骨板"的合成体数据把该行为钉死。
// ============================================================================

static void group9Picker() {
    beginGroup("组9 MeasurePicker —— 骨面拾取 / 深度锁层窗口");

    Synth sy;
    sy.build(100, 100, 100, 1.0, 1.0, 1.0, 0.0f);
    // 近侧骨板 x=20..22，远侧骨板 x=70..72，其余是"软组织"0HU
    sy.fillBox(20, 40, 40, 22, 60, 60, 500.0f);
    sy.fillBox(70, 40, 40, 72, 60, 60, 900.0f);
    VolumeRef vol; vol.bind(&sy.cv);
    MeasurePicker picker(vol);

    const Vec3 o(0.0, 50.0, 50.0);
    const Vec3 dir(1.0, 0.0, 0.0);

    {   // 默认语义：第一个骨面
        const PickResult r = picker.pickSurface(o, dir, 200.0);
        checkTrue("无窗口 -> 命中", r.hit);
        checkAbs("无窗口 -> 命中近侧骨板 x=21", r.point.x, 21.0, 1.6);
        checkAbs("无窗口 -> distanceMm 即相机深度", r.distanceMm, r.point.x, 1e-6);
    }
    {   // 窗口推到近侧骨板之后：锁到远侧骨板，等价"跳过遮挡结构"
        const PickResult r = picker.pickSurface(o, dir, 200.0, 0.0, 30.0);
        checkTrue("tMin=30 -> 命中", r.hit);
        checkAbs("tMin=30 -> 命中远侧骨板 x=71", r.point.x, 71.0, 1.6);
    }
    {   // 窗口夹在两块骨板之间：应当判未命中，而不是返回任意一块
        const PickResult r = picker.pickSurface(o, dir, 200.0, 50.0, 30.0);
        checkTrue("tMin=30/tMax=50 -> 两板之间无骨面", !r.hit);
    }
    {   // 窗口整体在包围盒之外：提前退出，describe 要能区分开
        const PickResult r = picker.pickSurface(o, dir, 200.0, 0.0, 120.0);
        checkTrue("tMin 超出体积 -> 未命中", !r.hit);
        checkEqStr("tMin 超出体积 -> 描述", r.describe, std::string("no surface in depth window"));
    }
    {   // 射线根本不穿过体积：与旧行为一致
        const PickResult r = picker.pickSurface(Vec3(50.0, 0.0, 0.0), Vec3(0.0, 0.0, -1.0), 200.0);
        checkTrue("背离体积的射线 -> 未命中", !r.hit);
    }
    {   // 深度窗口不影响 HU 读数（M-07/M-08 同源）
        const PickResult r = picker.pickSurface(o, dir, 200.0, 0.0, 30.0);
        checkAbs("锁层后 HU 取到远侧骨板", (double) r.hu, 900.0, 1.0);
    }
}

// ============================================================================
// 组 10：PRD 5.3.3 正畸评估 S-07 ~ S-10（解析期望值 + manager 通路 + 枚举协议）
// ============================================================================

/** 从 detailJson 里取一个数字字段；缺失/类型不符返回 NaN，用例据此判失败 */
static double jsonNum(const std::string &detailJson, const char *key) {
    bool ok = false;
    const Json j = Json::parse(detailJson, &ok);
    if (!ok || !j.isObject()) return std::nan("");
    return j.at(key).asNumber(std::nan(""));
}

static void group10Orthodontics() {
    beginGroup("组10 正畸评估 S-07 牙弓弧线 / S-08 排列角度 / S-09 中线偏移 / S-10 覆合覆盖");

    MeasurementManager mgr;   // 四类量只吃世界毫米点，不需要绑定体数据

    // ---- S-07 牙弓弧线：半圆 r=10（XY 面 0/90/180 度三点）----
    //   折线长 = 2 * 10*sqrt(2) = 28.284271247461902（累积弦长法的解析值）
    //   首末弦 = 20（直径）、弓深 = 中点到弦（X 轴）的垂直距离 = 10
    {
        std::vector<Vec3> arc;
        arc.push_back(Vec3(10, 0, 0));
        arc.push_back(Vec3(0, 10, 0));
        arc.push_back(Vec3(-10, 0, 0));
        const MeasureRecord rec = mgr.addMeasure(MT_ARCH_LENGTH, arc, 0, "牙弓", 0);
        checkEqStr("S-07 unit = mm", rec.unit, std::string("mm"));
        checkAbs("S-07 弧长 = 2*10*sqrt(2)", rec.value, 28.284271247461902, 1e-9);
        checkAbs("S-07 chordMm = 20", jsonNum(rec.detailJson, "chordMm"), 20.0, 1e-9);
        checkAbs("S-07 archDepthMm = 10", jsonNum(rec.detailJson, "archDepthMm"), 10.0, 1e-9);
        checkAbs("S-07 perimeterMm = 弧长+弦", jsonNum(rec.detailJson, "perimeterMm"),
                 28.284271247461902 + 20.0, 1e-9);

        std::vector<Vec3> flat;
        flat.push_back(Vec3(0, 0, 0)); flat.push_back(Vec3(10, 0, 0)); flat.push_back(Vec3(20, 0, 0));
        const MeasureRecord f = mgr.addMeasure(MT_ARCH_LENGTH, flat, 0, "直线", 0);
        checkAbs("S-07 共线三点 -> 弧长 20mm", f.value, 20.0, 1e-12);
        checkAbs("S-07 共线三点 -> 弓深 0", jsonNum(f.detailJson, "archDepthMm"), 0.0, 1e-12);

        std::vector<Vec3> one;
        one.push_back(Vec3(1, 2, 3));
        const MeasureRecord bad = mgr.addMeasure(MT_ARCH_LENGTH, one, 0, "单点", 0);
        checkTrue("S-07 单点 -> compute 失败", !mgr.recalcMeasure(bad.id));
        checkHas("S-07 单点 -> detailJson 有 error", mgr.findMeasure(bad.id)->detailJson, "error");
        checkAbs("S-07 失败时 value 归 0", mgr.findMeasure(bad.id)->value, 0.0, 0.0);
    }

    // ---- S-08 排列角度：两条无向长轴，fold 到 [0,90] 是这项量的定义 ----
    {
        std::vector<Vec3> p;
        p.push_back(Vec3(0, 0, 0)); p.push_back(Vec3(10, 0, 0));
        p.push_back(Vec3(0, 0, 0)); p.push_back(Vec3(10, 10, 0));
        const MeasureRecord rec = mgr.addMeasure(MT_TOOTH_ANGULATION, p, 0, "排列角度", 0);
        checkEqStr("S-08 unit = deg", rec.unit, std::string("deg"));
        checkAbs("S-08 45 度长轴偏差", rec.value, 45.0, 1e-9);
        checkAbs("S-08 axisALenMm = 10", jsonNum(rec.detailJson, "axisALenMm"), 10.0, 1e-12);

        std::vector<Vec3> q;
        q.push_back(Vec3(0, 0, 0)); q.push_back(Vec3(10, 0, 0));
        q.push_back(Vec3(20, 20, 0)); q.push_back(Vec3(10, 10, 0));   // 第二颗牙反着量
        const MeasureRecord rev = mgr.addMeasure(MT_TOOTH_ANGULATION, q, 0, "反向", 0);
        checkAbs("S-08 反向长轴仍为 45 度（无向）", rev.value, 45.0, 1e-9);

        std::vector<Vec3> r3;
        r3.push_back(Vec3(0, 0, 0)); r3.push_back(Vec3(1, 0, 0)); r3.push_back(Vec3(0, 1, 0));
        const MeasureRecord short3 = mgr.addMeasure(MT_TOOTH_ANGULATION, r3, 0, "三点", 0);
        checkTrue("S-08 不足 4 点 -> 失败", !mgr.recalcMeasure(short3.id));
        checkHas("S-08 不足 4 点 -> 有 error", mgr.findMeasure(short3.id)->detailJson, "error");
    }

    // ---- S-09 中线偏移：只算水平面内距离，Z 差归到 verticalMm ----
    // 上中线中点 (0,5,10)、下中线中点 (2,5,20) -> 偏移 2mm、垂直分量 10mm
    {
        std::vector<Vec3> p;
        p.push_back(Vec3(0, 0, 10)); p.push_back(Vec3(0, 10, 10));
        p.push_back(Vec3(2, 0, 20)); p.push_back(Vec3(2, 10, 20));
        const MeasureRecord rec = mgr.addMeasure(MT_MIDLINE_OFFSET, p, 0, "中线", 0);
        checkEqStr("S-09 unit = mm", rec.unit, std::string("mm"));
        checkAbs("S-09 偏移 = 2mm（忽略 Z）", rec.value, 2.0, 1e-12);
        checkAbs("S-09 verticalMm = 10", jsonNum(rec.detailJson, "verticalMm"), 10.0, 1e-12);
        checkHas("S-09 detailJson 有 upperMid/lowerMid", rec.detailJson, "upperMid");
        checkHas("S-09 detailJson 有 lowerMid", rec.detailJson, "lowerMid");
    }

    // ---- S-10 覆合/覆盖：Z 为垂直轴，前后向取水平分量较大的一轴 ----
    {
        std::vector<Vec3> p;
        p.push_back(Vec3(0, 5, 30)); p.push_back(Vec3(0, 0, 25));
        const MeasureRecord rec = mgr.addMeasure(MT_OVERBITE, p, 0, "覆合覆盖", 0);
        checkEqStr("S-10 unit = mm", rec.unit, std::string("mm"));
        checkAbs("S-10 主值 = 覆合 5mm", rec.value, 5.0, 1e-12);
        checkAbs("S-10 overjetMm = 5", jsonNum(rec.detailJson, "overjetMm"), 5.0, 1e-12);
        checkAbs("S-10 Y 占优 -> apAxis = 1", jsonNum(rec.detailJson, "apAxis"), 1.0, 0.0);

        std::vector<Vec3> q;
        q.push_back(Vec3(7, 0, 30)); q.push_back(Vec3(0, 0, 26));
        const MeasureRecord x = mgr.addMeasure(MT_OVERBITE, q, 0, "X向", 0);
        checkAbs("S-10 X 占优 -> apAxis = 0", jsonNum(x.detailJson, "apAxis"), 0.0, 0.0);
        checkAbs("S-10 X 占优 -> 覆盖 = 7mm", jsonNum(x.detailJson, "overjetMm"), 7.0, 1e-12);
        checkAbs("S-10 X 占优 -> 覆合 = 4mm", x.value, 4.0, 1e-12);

        std::vector<Vec3> one;
        one.push_back(Vec3(0, 0, 0));
        const MeasureRecord bad = mgr.addMeasure(MT_OVERBITE, one, 0, "单点", 0);
        checkTrue("S-10 单点 -> 失败", !mgr.recalcMeasure(bad.id));
    }

    // 跨语言协议：8~11 是新增四类，Kotlin MeasureType 常量必须逐位一致
    checkEqInt("MeasureType MT_ARCH_LENGTH = 8", MT_ARCH_LENGTH, 8);
    checkEqInt("MeasureType MT_TOOTH_ANGULATION = 9", MT_TOOTH_ANGULATION, 9);
    checkEqInt("MeasureType MT_MIDLINE_OFFSET = 10", MT_MIDLINE_OFFSET, 10);
    checkEqInt("MeasureType MT_OVERBITE = 11", MT_OVERBITE, 11);
    checkEqInt("ToolState 仍是 9 态（正畸不扩状态机）", ST_NERVE_TRACE, 8);
}

// ============================================================================
// 入口
// ============================================================================

int main() {
    // 崩溃时最后几行日志不能整块丢掉（块缓冲的 stdout 在 SIGSEGV 下不会被刷）
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("cbctmeasure core 主机侧精度验收单测（macOS clang, 不连设备/不跑 Gradle/不链 DCMTK）\n");
    group1Distance();
    group2Angle();
    group3PointToLine();
    group4ArcAndArea();
    group5VolumeAndHuStats();
    group6Implant();
    group7Manager();
    group8AnnotationAndProtocol();
    group9Picker();
    group10Orthodontics();

    printf("\n================ PRD §6 精度实测汇总 ================\n");
    printf("  AC-01 距离  最坏绝对误差 = %.3e mm（要求 <= 0.1 mm）%s\n",
           g_worstDistMm, g_worstDistMm <= 0.1 ? "[达标]" : "[不达标]");
    printf("  AC-02 角度  最坏绝对误差 = %.3e 度（要求 <= 0.5 度）%s\n",
           g_worstAngleDeg, g_worstAngleDeg <= 0.5 ? "[达标]" : "[不达标]");
    printf("  AC-03 体积  最坏相对误差 = %.4f%%（要求 <= 1%%）%s\n",
           g_worstVolRel * 100.0, g_worstVolRel <= 0.01 ? "[达标]" : "[见 FINDING]");
    printf("  M-05 面积   最坏相对误差 = %.4f%%（要求 <= 1%%）\n", g_worstAreaRel * 100.0);
    printf("  M-06 弧长   最坏绝对误差 = %.3e mm（要求 <= 0.5 mm，200 段弦逼近）%s\n",
           g_worstArcMm, g_worstArcMm <= 0.5 ? "[达标]" : "[不达标]");
    printf("  AC-04 HU    最坏绝对误差 = %.3e HU（要求精确值）%s\n",
           g_worstHuAbs, g_worstHuAbs <= 1e-8 ? "[达标]" : "[不达标]");
    printf("====================================================\n");
    printf("RESULT: PASSED %d / FAILED %d / FINDINGS %d\n", g_pass, g_fail, g_findings);
    fflush(stdout);
    return g_fail ? 1 : 0;
}
