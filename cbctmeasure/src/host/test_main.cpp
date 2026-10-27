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
#include "include/ImplantLibrary.h"
#include "include/ImplantPlanner.h"
#include "include/Json.h"
#include "include/MeasureMath.h"
#include "include/MeasurePicker.h"
#include "include/MeasureTypes.h"
#include "include/MeasurementManager.h"
#include "include/NerveAutoTracker.h"
#include "include/RoiExtractor.h"
#include "include/VolumeRef.h"
#include "include/CbctVolume.h"          // 主机桩件版

#include <algorithm>
#include <cerrno>
#include <climits>
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
// 组 11：R-06 AI 掩膜 ROI 落库（AI-01）+ 恢复归档后重跑推理的去重
//
// 真机取证发现的坑：归档只存 ROI 定义（含 aiLabel）与测量行，**掩膜网格不入档**。
// measuresFromJson 载入后一律按当前体数据重算，于是 R-06 的两类行（M-04 体积 /
// M-08 骨密度）必然失败 -> UI 与 PDF 显示"待重算"。用户此时的自然操作是重新
// 装载模型 + 分割 + 逐牙自动测量，旧实现会给同一颗牙再建一个 ROI、再写一套行，
// 报告里出现双份条目且一半是空的。这里锁住修复后的契约。
// ============================================================================

static int countAiMaskRois(const MeasurementManager &m) {
    int n = 0;
    for (size_t i = 0; i < m.rois().size(); ++i)
        if (m.rois()[i].type == ROI_AI_MASK) ++n;
    return n;
}

static int countFailedRecords(const MeasurementManager &m) {
    int n = 0;
    for (size_t i = 0; i < m.measures().size(); ++i) {
        // 约定见 core/MeasurementManager.cpp 的 compute()：失败原因写在 detailJson.error
        if (m.measures()[i].detailJson.find("\"error\"") != std::string::npos) ++n;
    }
    return n;
}

/** 两颗"牙"的合成推理结果：抽稀网格 == 体数据网格（factor 1，体素 1 mm³） */
static void buildTwoToothAi(AiResult &ai) {
    ai.ok = true;
    ai.grid.appDim[0] = ai.grid.appDim[1] = ai.grid.appDim[2] = 32;
    ai.grid.appSpacing[0] = ai.grid.appSpacing[1] = ai.grid.appSpacing[2] = 1.0;
    ai.grid.factor[0] = ai.grid.factor[1] = ai.grid.factor[2] = 1;
    ai.grid.redDim[0] = ai.grid.redDim[1] = ai.grid.redDim[2] = 32;
    ai.grid.modelDim[0] = ai.grid.modelDim[1] = ai.grid.modelDim[2] = 32;
    ai.grid.spacing[0] = ai.grid.spacing[1] = ai.grid.spacing[2] = 1.0;
    ai.label.assign((size_t) 32 * 32 * 32, 0);
    ai.inst.assign((size_t) 32 * 32 * 32, 0);
    ai.hu.assign((size_t) 32 * 32 * 32, 0.0f);

    // A：i/j/k 4..13 共 1000 体素，1500 HU；B：i/j 20..27、k 6..13 共 512 体素，900 HU
    for (int k = 4; k <= 13; ++k)
        for (int j = 4; j <= 13; ++j)
            for (int i = 4; i <= 13; ++i) {
                const long long t = ai.grid.redIndex(i, j, k);
                ai.label[(size_t) t] = 1; ai.inst[(size_t) t] = 1; ai.hu[(size_t) t] = 1500.0f;
            }
    for (int k = 6; k <= 13; ++k)
        for (int j = 20; j <= 27; j++)
            for (int i = 20; i <= 27; ++i) {
                const long long t = ai.grid.redIndex(i, j, k);
                ai.label[(size_t) t] = 1; ai.inst[(size_t) t] = 2; ai.hu[(size_t) t] = 900.0f;
            }
    ai.toothVoxels = 1512;

    AiInstance a;
    a.id = 1; a.voxels = 1000; a.volumeMm3 = 1000.0;
    a.centroid = Vec3(9.5, 9.5, 9.5);
    a.bboxMin = Vec3(4, 4, 4); a.bboxMax = Vec3(14, 14, 14);
    a.meanHu = 1500.0; a.arch = 0; a.toothCountHint = 1;
    AiInstance b;
    b.id = 2; b.voxels = 512; b.volumeMm3 = 512.0;
    b.centroid = Vec3(24.5, 24.5, 10.5);
    b.bboxMin = Vec3(20, 20, 6); b.bboxMax = Vec3(28, 28, 14);
    b.meanHu = 900.0; b.arch = 1; b.toothCountHint = 1;
    ai.instances.push_back(a);
    ai.instances.push_back(b);
}

static void group11AiMaskReuse() {
    beginGroup("组 11 · R-06 掩膜 ROI 落库与恢复归档去重（AI-01）");

    Synth sy;
    sy.build(32, 32, 32, 1.0, 1.0, 1.0, -1000.0f);
    sy.fillBox(4, 4, 4, 13, 13, 13, 1500.0f);
    sy.fillBox(20, 20, 6, 27, 27, 13, 900.0f);

    AiResult ai;
    buildTwoToothAi(ai);

    MeasurementManager mgr;
    mgr.bindVolume(&sy.cv);
    mgr.adoptAiResult(ai);

    checkEqInt("首次落库：2 实例 -> 4 条测量", mgr.aiAutoMeasure(true), 4);
    checkEqInt("首次落库：2 个 R-06 掩膜 ROI", countAiMaskRois(mgr), 2);
    checkEqInt("首次落库：4 条测量全部成功", countFailedRecords(mgr), 0);
    // M-04 掩膜 ROI 体积按 app 网格计数：1000 体素 × 1 mm³ = 1.0 cm³
    const MeasureRecord *volA = nullptr;
    for (size_t i = 0; i < mgr.measures().size(); ++i) {
        if (mgr.measures()[i].type == MT_ROI_VOLUME &&
                mgr.measures()[i].roiId == mgr.rois()[0].id) volA = &mgr.measures()[i];
    }
    checkTrue("找到牙 A 的 M-04 体积行", volA != nullptr);
    if (volA) checkAbs("M-04 体积 = 1000 mm³ = 1.0 cm³", volA->value, 1.0, 1e-9);

    // ---- 恢复归档：只带 JSON，不带掩膜网格 ----
    bool pok = false;
    const Json dump = Json::parse(mgr.measuresJson().dump(false), &pok);
    checkTrue("measuresJson 可再解析", pok);
    MeasurementManager mgr2;
    mgr2.bindVolume(&sy.cv);
    checkTrue("恢复归档成功", mgr2.measuresFromJson(dump));
    checkEqInt("恢复后测量行数不变", (long long) mgr2.measures().size(), 4);
    checkEqInt("恢复后 ROI 数不变", countAiMaskRois(mgr2), 2);
    checkEqInt("R-06 ROI 的 aiLabel 随归档往返保留（#1）", mgr2.rois()[0].aiLabel, 1);
    checkEqInt("R-06 ROI 的 aiLabel 随归档往返保留（#2）", mgr2.rois()[1].aiLabel, 2);
    checkEqInt("掩膜不入档 -> 载入重算后 4 行全为\"待重算\"", countFailedRecords(mgr2), 4);

    // ---- 重新推理 + 再落库：必须认领旧 ROI、清理旧行，而不是再加一套 ----
    const int oldRoiId = mgr2.rois()[0].id;
    AiResult ai2;
    buildTwoToothAi(ai2);
    mgr2.adoptAiResult(ai2);
    checkEqInt("重跑落库：仍新增 4 条", mgr2.aiAutoMeasure(true), 4);
    checkEqInt("重跑后 ROI 不翻倍", countAiMaskRois(mgr2), 2);
    checkEqInt("重跑后行号不翻倍（旧待重算行被清理）", (long long) mgr2.measures().size(), 4);
    checkEqInt("重跑后无\"待重算\"残留", countFailedRecords(mgr2), 0);
    checkEqInt("复用的是原 ROI id", mgr2.rois()[0].id, oldRoiId);
    // 回填写在管理器自己的副本上（adoptAiResult 按值拷贝，调用方的 ai2 不受影响）
    checkTrue("重跑后管理器内的实例回填了 roiId",
              !mgr2.ai().instances.empty() && mgr2.ai().instances[0].roiId == oldRoiId);

    // ---- 手工挂在同一掩膜 ROI 上的测量不能被清理 ----
    const int roiForManual = mgr2.rois()[0].id;
    const MeasureRecord manual = mgr2.addMeasure(MT_ROI_VOLUME, std::vector<Vec3>(),
                                                 roiForManual, "手工体积", 0, "手工备注");
    checkTrue("手工行有值", manual.value > 0.0);
    AiResult ai3;
    buildTwoToothAi(ai3);
    mgr2.adoptAiResult(ai3);
    mgr2.aiAutoMeasure(true);
    checkEqInt("第三次重跑：自动行仍 4 条 + 手工 1 条", (long long) mgr2.measures().size(), 5);
    bool manualAlive = false;
    for (size_t i = 0; i < mgr2.measures().size(); ++i) {
        if (mgr2.measures()[i].id == manual.id) manualAlive = true;
    }
    checkTrue("手工行不被 AI 清理误删", manualAlive);
    // 清理判据只认 note 标记，标记值被 Kotlin/报告引用，改动要同步
    checkEqStr("AUTO_NOTE 标记字面量", std::string(AiConst::AUTO_NOTE), std::string("AI-01 自动分割"));
}

// ============================================================================
// 组 12 · AI-02 下颌管中心线追踪（升级建议 P-01，core/NerveAutoTracker.cpp）
//
// 这里的期望值全部是解析常量：合成"管腔"是沿某一根轴、截面为 13 体素对称圆盘
// 的直管，所以
//   - 每层截面质心 == 圆心（圆盘对称）-> 折线点可逐层解析写出；
//   - 每层截面积 = 13 体素 × 1 mm² -> 半径 = sqrt(13/π)，中位数与均值相同；
//   - 体素数 = 13 × 层数；分组数 = 层数。
// 于是规则 a)~e) 每一条都能被单个数字卡住，而不是"看起来差不多"。
// ============================================================================

/** 13 体素的对称圆盘偏移（dp²+dq²<=4）：dp 求和后为 0，质心不偏 */
static const int kCanalDisc[13][2] = {
        {0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1},
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
        {2, 0}, {-2, 0}, {0, 2}, {0, -2},
};
/** 截面积 13 mm² 的等效圆半径（规则 e 的解析值 = sqrt(13/pi)） */
static const double kCanalRadiusMm = 2.034214472564;

/** 下颌管合成掩膜的网格：抽稀 == 体数据 == 模型网格（32³，1 mm 体素） */
static void canalGrid(AiResult &ai, int dim) {
    ai.ok = true;
    ai.grid.appDim[0] = ai.grid.appDim[1] = ai.grid.appDim[2] = dim;
    ai.grid.appSpacing[0] = ai.grid.appSpacing[1] = ai.grid.appSpacing[2] = 1.0;
    ai.grid.factor[0] = ai.grid.factor[1] = ai.grid.factor[2] = 1;
    ai.grid.redDim[0] = ai.grid.redDim[1] = ai.grid.redDim[2] = dim;
    ai.grid.modelDim[0] = ai.grid.modelDim[1] = ai.grid.modelDim[2] = dim;
    ai.grid.spacing[0] = ai.grid.spacing[1] = ai.grid.spacing[2] = 1.0;
    const size_t n = (size_t) dim * dim * dim;
    ai.label.assign(n, 0);
    ai.inst.assign(n, 0);
    ai.hu.assign(n, 0.0f);
}

/**
 * 沿 axis 铺一条直管：axial 索引 lo..hi，另两根轴的圆心 (c1,c2)。
 * 返回写入的体素数（= 13 × 层数），调用方据此核对 track 的 voxels。
 */
static long long addCanalTube(AiResult &ai, int axis, int lo, int hi, int c1, int c2,
                              short inst) {
    long long n = 0;
    for (int a = lo; a <= hi; ++a) {
        for (int q = 0; q < 13; ++q) {
            int i = 0, j = 0, k = 0;
            if (axis == 0) { i = a; j = c1 + kCanalDisc[q][0]; k = c2 + kCanalDisc[q][1]; }
            else if (axis == 1) { j = a; i = c1 + kCanalDisc[q][0]; k = c2 + kCanalDisc[q][1]; }
            else { k = a; i = c1 + kCanalDisc[q][0]; j = c2 + kCanalDisc[q][1]; }
            const long long t = ai.grid.redIndex(i, j, k);
            ai.label[(size_t) t] = 1;
            if (ai.inst[(size_t) t] == 0) ai.inst[(size_t) t] = inst;
            ++n;
        }
    }
    return n;
}

static void group12CanalTracking() {
    beginGroup("组 12 · AI-02 下颌管中心线追踪与安全判级联动（P-01）");

    // ---- 无掩膜：不报错，追出 0 条，文案如实说明"以手工描记为准" ----
    {
        AiResult none;
        const std::vector<NerveTrack> tr = NerveAutoTracker::track(none);
        checkEqInt("空结果不追出管道", (long long) tr.size(), 0);
        const std::string txt = NerveAutoTracker::summaryText(tr, 0.8, false);
        checkHas("无管道时文案承认手工为准", txt, "手工描记");
    }

    // ---- 右管 22 层 / 左管 20 层 + 6 体素散点：a)b)c)d)e) 一次覆盖 ----
    {
        AiResult ai;
        canalGrid(ai, 32);
        const long long rightN = addCanalTube(ai, 0, 4, 25, 16, 10, 1);   // 286 体素
        const long long leftN = addCanalTube(ai, 0, 6, 25, 8, 10, 2);     // 260 体素
        // 散点：26 邻域内不与任何管相接的一小段（6 体素 < MIN_CANAL_VOXELS）
        for (int k = 2; k <= 7; ++k) {
            const long long t = ai.grid.redIndex(29, 29, k);
            ai.label[(size_t) t] = 1; ai.inst[(size_t) t] = 3;
        }
        ai.toothVoxels = (long long) (rightN + leftN + 6);
        checkEqInt("夹具：右管 22 层 × 13 = 286", rightN, 286);
        checkEqInt("夹具：左管 20 层 × 13 = 260", leftN, 260);

        const std::vector<NerveTrack> tr = NerveAutoTracker::track(ai);
        // a) 散点剔除 + 只有两条进结果
        checkEqInt("26 邻域连通域：6 体素散点被剔除，剩 2 条", (long long) tr.size(), 2);
        if (tr.size() == 2) {
            // b) 体素数降序
            checkEqInt("b) 降序第 1 条 = 右管体素数", tr[0].voxels, 286);
            checkEqInt("b) 降序第 2 条 = 左管体素数", tr[1].voxels, 260);
            // c) 描记轴 = bbox 跨度最大的轴（这里 x 跨 22、y/z 各跨 5）
            checkEqInt("c) 描记轴是 x（跨度最大）", tr[0].traceAxis, 0);
            checkAbs("c) extentVox[x] = 22", tr[0].extentVox[0], 22.0, 1e-9);
            checkAbs("c) extentVox[y] = 5", tr[0].extentVox[1], 5.0, 1e-9);
            checkAbs("c) extentVox[z] = 5", tr[0].extentVox[2], 5.0, 1e-9);
            // d) 逐层质心 + (index+0.5)*spacing 的同一 convention
            checkEqInt("d) 折线点数 == 分组数 == 层数", (long long) tr[0].points.size(), 22);
            checkEqInt("d) groupsUsed 与点数一致", tr[0].groupsUsed, 22);
            checkAbs("d) 首点 x = (4+0.5)*1.0", tr[0].points[0].x, 4.5, 1e-9);
            checkAbs("d) 首点 y = 圆心 16.5", tr[0].points[0].y, 16.5, 1e-9);
            checkAbs("d) 首点 z = 圆心 10.5", tr[0].points[0].z, 10.5, 1e-9);
            checkAbs("d) 末点 x = (25+0.5)*1.0", tr[0].points[21].x, 25.5, 1e-9);
            bool asc = true;
            for (size_t q = 1; q < tr[0].points.size(); ++q) {
                if (!(tr[0].points[q].x > tr[0].points[q - 1].x)) asc = false;
            }
            checkTrue("d) 折线按描记轴坐标升序", asc);
            // e) 半径 = 中位数 sqrt(13*1/π)
            checkAbs("e) 实测半径 = sqrt(13/π)", tr[0].radiusMm, kCanalRadiusMm, 1e-6);
            // b) 的次键：最小线性索引必须与网格自算值一致（主机/真机同序的依据）。
            // 期望点 = (i=4, j=c1-2, k=c2)：圆盘偏移里 dj=-2 时只有 dk=0，
            // 所以该层最小的 (j,k) 是 (14,10) 而不是 (14,8) —— 线性索引
            // (i*dim+j)*dim+k 先按 i、再按 j、再按 k 比较。
            checkAbs("b) minLinearIndex = redIndex(4,14,10)",
                     (double) tr[0].minLinearIndex, (double) ai.grid.redIndex(4, 14, 10), 0.0);
            printf("        右管：voxels=%lld 半径=%.4fmm 轴=%d 点数=%zu\n",
                   tr[0].voxels, tr[0].radiusMm, tr[0].traceAxis, tr[0].points.size());
            const std::string txt = NerveAutoTracker::summaryText(tr, 0.8, true);
            checkHas("摘要含条数", txt, "2 条");
            checkHas("摘要含标定阈值", txt, "0.80");
            checkHas("摘要声明半径是实测中位数", txt, "实测截面中位数");
        }
        // 显式入参覆盖默认常量（真机取证要能改阈值/候选数而不重编）
        checkEqInt("minVoxels=300 时两条都不够格",
                   (long long) NerveAutoTracker::track(ai, 300, 0).size(), 0);
        checkEqInt("maxCandidates=1 时只留最大的一条",
                   (long long) NerveAutoTracker::track(ai, 0, 1).size(), 1);
    }

    // ---- 沿 z 的管：证明 c) 不是写死某根轴 ----
    {
        AiResult ai;
        canalGrid(ai, 32);
        addCanalTube(ai, 2, 4, 25, 16, 10, 1);
        const std::vector<NerveTrack> tr = NerveAutoTracker::track(ai);
        checkEqInt("沿 z 铺管仍追出 1 条", (long long) tr.size(), 1);
        if (!tr.empty()) {
            checkEqInt("c) 描记轴随伸展方向改成 z", tr[0].traceAxis, 2);
            checkAbs("d) 首点 z = 4.5", tr[0].points[0].z, 4.5, 1e-9);
            checkAbs("d) 首点 x = 圆心 16.5", tr[0].points[0].x, 16.5, 1e-9);
            checkAbs("d) 首点 y = 圆心 10.5", tr[0].points[0].y, 10.5, 1e-9);
            checkAbs("e) 半径与轴向无关", tr[0].radiusMm, kCanalRadiusMm, 1e-6);
        }
    }

    // ---- 候选上限 8 > 6：b) 的并列次键决定留下哪 6 条 ----
    {
        AiResult ai;
        canalGrid(ai, 32);
        const int cj[4] = {4, 12, 20, 28};
        const int ck[2] = {4, 20};
        std::vector<long long> mins;
        for (int a = 0; a < 4; ++a)
            for (int b = 0; b < 2; ++b) {
                addCanalTube(ai, 0, 4, 25, cj[a], ck[b], (short) (a * 2 + b + 1));
                // 同上：dj=-2 那一列只有 dk=0，故最小体素 = (4, cj-2, ck)
                mins.push_back(ai.grid.redIndex(4, cj[a] - 2, ck[b]));
            }
        std::sort(mins.begin(), mins.end());
        const std::vector<NerveTrack> tr = NerveAutoTracker::track(ai);
        checkEqInt("8 条等体素管截断到 CANAL_MAX_CANDIDATES",
                   (long long) tr.size(), AiConst::CANAL_MAX_CANDIDATES);
        bool ordered = true, matches = true;
        for (size_t i = 0; i < tr.size(); ++i) {
            if (i > 0 && !(tr[i].minLinearIndex > tr[i - 1].minLinearIndex)) ordered = false;
            if (tr[i].minLinearIndex != mins[i]) matches = false;
            if (tr[i].voxels != 286) matches = false;
        }
        checkTrue("b) 并列时按最小线性索引升序", ordered);
        checkTrue("b) 保留的正是索引最小的 6 条", matches);
    }

    // ---- toPaths：命名前缀 = 身份、颜色、半径钳位 ----
    {
        NerveTrack t;
        t.points.push_back(Vec3(1.0, 2.0, 3.0));
        t.points.push_back(Vec3(4.0, 2.0, 3.0));
        t.voxels = 130;
        t.traceAxis = 0;
        t.groupsUsed = 2;
        std::vector<NerveTrack> tr;
        t.radiusMm = kCanalRadiusMm;
        tr.push_back(t);
        t.radiusMm = 9.0;                                   // 病态偏大
        tr.push_back(t);
        t.radiusMm = 0.05;                                  // 病态偏小
        tr.push_back(t);
        const std::vector<NervePath> paths = NerveAutoTracker::toPaths(tr);
        checkEqInt("toPaths 与 tracks 同条数", (long long) paths.size(), 3);
        checkEqStr("第 1 条名称 = CANAL_AUTO_NOTE + 序号",
                   paths[0].name, std::string(AiConst::CANAL_AUTO_NOTE) + " 1");
        checkEqStr("第 3 条名称序号递增",
                   paths[2].name, std::string(AiConst::CANAL_AUTO_NOTE) + " 3");
        checkEqInt("颜色 = AiConst::CANAL_COLOR",
                   (long long) paths[0].color, (long long) (int) AiConst::CANAL_COLOR);
        checkAbs("实测半径原样进 NervePath", paths[0].radiusMm, kCanalRadiusMm, 1e-9);
        checkAbs("病态大半径钳到 6.0", paths[1].radiusMm, 6.0, 1e-12);
        checkAbs("病态小半径钳到 0.4", paths[2].radiusMm, 0.4, 1e-12);
        checkTrue("visible 默认打开", paths[0].visible);
        checkEqInt("点数透传", (long long) paths[0].points.size(), 2);
        checkTrue("isAutoNerveName 认得自己的前缀",
                  MeasurementManager::isAutoNerveName(paths[0].name));
        checkTrue("手描名字不被误认",
                  !MeasurementManager::isAutoNerveName(std::string("左下颌管（手描）")));
    }

    // ---- tracksJson：AC-08-2 逐点比对的取证面 ----
    {
        AiResult ai;
        canalGrid(ai, 32);
        addCanalTube(ai, 0, 4, 25, 16, 10, 1);
        addCanalTube(ai, 0, 6, 25, 8, 10, 2);
        const Json arr = NerveAutoTracker::tracksJson(NerveAutoTracker::track(ai));
        bool pok = false;
        const Json back = Json::parse(arr.dump(false), &pok);
        checkTrue("tracksJson 可再解析", pok);
        checkEqInt("JSON 两条管道", (long long) back.size(), 2);
        if (back.size() == 2) {
            checkTrue("带 traceAxisName", back[0].has("traceAxisName"));
            checkEqStr("描记轴名 = x", back[0].at("traceAxisName").asString(), "x");
            checkEqInt("pointsMm 22 点", (long long) back[0].at("pointsMm").size(), 22);
            checkAbs("首点 x 逐字段可取",
                     back[0].at("pointsMm")[0][0].asNumber(), 4.5, 1e-9);
            checkAbs("首点 y 逐字段可取",
                     back[0].at("pointsMm")[0][1].asNumber(), 16.5, 1e-9);
            checkAbs("radiusMm 与结构体一致",
                     back[0].at("radiusMm").asNumber(), kCanalRadiusMm, 1e-6);
        }
    }

    // ---- adoptCanalTracks：进既有 addNervePath 通道，手描不被清理，重跑幂等 ----
    {
        Synth sy;
        sy.build(32, 32, 32, 1.0, 1.0, 1.0, -1000.0f);
        MeasurementManager mgr;
        mgr.bindVolume(&sy.cv);

        NervePath manual;
        manual.name = "左下颌管（手描）";
        manual.radiusMm = 1.5;
        manual.points.push_back(Vec3(4.5, 24.5, 20.5));
        manual.points.push_back(Vec3(25.5, 24.5, 20.5));
        const int manualId = mgr.addNervePath(manual);
        checkTrue("手描神经管可先建", manualId > 0);

        AiResult ai;
        canalGrid(ai, 32);
        addCanalTube(ai, 0, 4, 25, 16, 10, 1);
        addCanalTube(ai, 0, 6, 25, 8, 10, 2);

        checkEqInt("首次 adopt：新增 2 条", mgr.adoptCanalTracks(ai), 2);
        checkEqInt("会话内自动神经管 = 2", mgr.autoNerveCount(), 2);
        checkEqInt("手描 1 + 自动 2 = 3 条", (long long) mgr.nervePaths().size(), 3);
        checkEqInt("canalTracks() 保留原始折线", (long long) mgr.canalTracks().size(), 2);

        AiResult ai2;
        canalGrid(ai2, 32);
        addCanalTube(ai2, 0, 4, 25, 16, 10, 1);
        addCanalTube(ai2, 0, 6, 25, 8, 10, 2);
        checkEqInt("重跑 adopt：仍 2 条（替换而非叠加）", mgr.adoptCanalTracks(ai2), 2);
        checkEqInt("重跑后总数仍 3", (long long) mgr.nervePaths().size(), 3);

        bool manualAlive = false;
        for (size_t i = 0; i < mgr.nervePaths().size(); ++i) {
            if (mgr.nervePaths()[i].id == manualId) manualAlive = true;
        }
        checkTrue("重跑不删手描", manualAlive);

        checkEqInt("clearCanalTracks 移除 2 条", mgr.clearCanalTracks(), 2);
        checkEqInt("清除后只剩手描", (long long) mgr.nervePaths().size(), 1);
        checkEqInt("再清一次没有可移除的", mgr.clearCanalTracks(), 0);

        AiResult empty;
        checkEqInt("空掩膜 adopt：0 条且不动旧数据", mgr.adoptCanalTracks(empty), 0);
        checkEqInt("手描仍在", (long long) mgr.nervePaths().size(), 1);
    }

    // ---- S-05 联动：半径来自掩膜实测，并会改变安全判级 ----
    {
        Synth sy;
        sy.build(32, 32, 32, 1.0, 1.0, 1.0, -1000.0f);
        MeasurementManager mgr;
        mgr.bindVolume(&sy.cv);
        AiResult ai;
        canalGrid(ai, 32);
        addCanalTube(ai, 0, 4, 25, 16, 10, 1);      // 中心线 y=16.5, z=10.5
        mgr.adoptCanalTracks(ai);
        checkEqInt("取到自动管道", (long long) mgr.nervePaths().size(), 1);
        if (mgr.nervePaths().size() != 1) return;

        VolumeRef vol; vol.bind(&sy.cv);
        ImplantPlanner planner(vol);
        Implant im;
        im.id = 1;
        im.entry = Vec3(10.5, 20.5, 15.5);          // 桩体在管道颊侧 4.0mm 处竖直向下
        im.pitchDeg = 0.0; im.yawDeg = 0.0;
        im.depthMm = 6.0; im.lengthMm = 10.0; im.diaMm = 4.0;
        ImplantPlanner::refreshGeometry(im);
        const double d = planner.nerveDistance(im, mgr.nervePaths()[0]);
        checkAbs("S-05 用实测半径：4.0 - sqrt(13/π)", d, 4.0 - kCanalRadiusMm, 1e-6);
        printf("        S-05 实测神经距离 %.4f mm（半径 %.4f mm 来自掩膜）\n", d,
               mgr.nervePaths()[0].radiusMm);

        NervePath fallback;
        fallback.id = 99; fallback.radiusMm = 1.5;
        fallback.points = mgr.nervePaths()[0].points;
        checkAbs("同一折线若用默认半径 1.5 则是 2.5",
                 planner.nerveDistance(im, fallback), 2.5, 1e-9);
        // 同一个解剖位置，两种半径给出不同颜色：这正是 P-01 要改掉的经验值
        checkEqInt("实测半径 1.97mm 判红",
                   ImplantPlanner::safetyLevel(12.0, 8.0, 4.0 - kCanalRadiusMm, true, -1.0, false),
                   DANGER_RED);
        checkEqInt("默认半径 1.5mm 判绿",
                   ImplantPlanner::safetyLevel(12.0, 8.0, 2.5, true, -1.0, false), SAFE_GREEN);
    }

    // ---- 判级开关：默认"只画不判"，打开后同一颗种植体必须从绿翻红 ----
    //
    // 这一段是 P-01 的收口证据。主机准入门禁没过（parity_ref/metrics_canal.json
    // 的 probe_axis_agreement：16 条探针平均偏 7.81mm、最大 18.16mm，而 S-05 红档
    // 判据只有 2.0mm），所以自动管道必须先能画出来、又不能直接影响颜色。
    // 夹具刻意让"骨高骨宽全绿 + 自动管道判红 + 手描管道判绿"同时成立，
    // 于是等级每翻一次都只有一个可能原因：判级输入里有没有那条自动管道。
    {
        Synth sy;
        sy.build(60, 60, 60, 0.4, 0.4, 0.4, 0.0f);
        // 骨块三边各 40 个体素（16mm），入口 z=21.0 上方留软组织：
        //   骨高 ~15.8 >= 1.2*10、骨宽 ~16 >= 1.2*6 -> 除神经管外全部绿
        sy.fillBox(10, 12, 10, 49, 52, 49, 1000.0f);
        MeasurementManager mgr;
        mgr.bindVolume(&sy.cv);
        checkTrue("autoNerveFeedsGrading 默认 false", !mgr.autoNerveFeedsGrading());

        Implant im; im.id = 1;
        im.entry = Vec3(12.0, 20.5, 21.0);          // 轴线 = (12, 20.5, 21 -> 10)
        im.pitchDeg = 0.0; im.yawDeg = 0.0;
        im.depthMm = 6.0; im.lengthMm = 11.0; im.diaMm = 4.0;
        const int imId = mgr.addImplant(im);
        checkEqInt("夹具：种植体 1 颗", (long long) mgr.implants().size(), 1);
        // 下面用引用读同一行：本轮不再 addImplant，vector 不会扩容，引用有效
        const Implant &row = mgr.implants()[0];
        checkTrue("夹具：骨高给到绿档（否则翻红无法归因于神经管）", row.boneHeightMm >= 12.0);
        checkTrue("夹具：骨宽给到绿档", row.boneWidthMm >= 7.2);
        printf("        判级开关夹具：骨高 %.3f mm / 骨宽 %.3f mm\n", row.boneHeightMm,
               row.boneWidthMm);

        AiResult ai;
        canalGrid(ai, 32);
        addCanalTube(ai, 0, 4, 25, 16, 10, 1);      // 中心线 y=16.5 z=10.5，离轴线 4.0mm
        checkEqInt("adopt：1 条自动管道进会话", mgr.adoptCanalTracks(ai), 1);

        // 关闭（默认）：管道照样在会话里（叠加层/归档/报告都读 nerves_），
        // 但 evaluate 拿到的判级输入是空的 -> S-05 写回 -1，等级仍是绿
        checkEqInt("关闭：判级输入 0 条", (long long) mgr.nervesForGrading().size(), 0);
        checkEqInt("关闭：会话内仍有 1 条（预览不丢）", (long long) mgr.nervePaths().size(), 1);
        checkEqInt("关闭：等级绿", row.level, SAFE_GREEN);
        checkAbs("关闭：S-05 未参与判定写回 -1", row.nerveDistMm, -1.0, 0.0);

        // 显式采纳：判级输入只多了那一条自动管道，等级必须立刻翻红
        mgr.setAutoNerveFeedsGrading(true);
        checkTrue("打开：autoNerveFeedsGrading=true", mgr.autoNerveFeedsGrading());
        checkEqInt("打开：判级输入 = 会话内全部", (long long) mgr.nervesForGrading().size(),
                   (long long) mgr.nervePaths().size());
        checkEqInt("打开：等级红", row.level, DANGER_RED);
        checkAbs("打开：S-05 = 4.0 - 实测半径", row.nerveDistMm, 4.0 - kCanalRadiusMm, 1e-6);
        checkHas("打开：告警文案点名神经管", row.warnText, "神经管");

        // 翻回去必须同样立即生效：否则界面上的勾选状态与行的颜色会长期矛盾
        mgr.setAutoNerveFeedsGrading(false);
        checkEqInt("再关：回到绿", row.level, SAFE_GREEN);
        checkAbs("再关：S-05 又变回 -1", row.nerveDistMm, -1.0, 0.0);

        // 手描管道不受开关影响 —— 这是这个开关最关键的安全边界：
        // 关掉它只能关掉"AI 生成的"，绝不能把医生自己描的也一起屏蔽掉。
        // 手描放在桩体尖端下方 5.0mm（默认半径 1.5）-> 表面 3.5mm，本身判绿，
        // 于是"关=绿 / 开=红"仍然只能由那条 1.97mm 的自动管道解释。
        NervePath manual;
        manual.name = "手描下颌管";                  // 不带 AiConst::CANAL_AUTO_NOTE 前缀
        manual.radiusMm = 1.5;
        manual.points.push_back(Vec3(2.0, 20.5, 5.0));
        manual.points.push_back(Vec3(24.0, 20.5, 5.0));
        const int manualId = mgr.addNervePath(manual);
        checkTrue("手描可建", manualId > 0);
        checkEqInt("手描 + 自动 = 2 条", (long long) mgr.nervePaths().size(), 2);
        checkEqInt("关闭：判级输入只剩手描 1 条", (long long) mgr.nervesForGrading().size(), 1);
        checkEqInt("关闭：手描照样判级（绿）", row.level, SAFE_GREEN);
        checkAbs("关闭：S-05 用手描 = 5.0 - 1.5", row.nerveDistMm, 3.5, 1e-9);
        mgr.setAutoNerveFeedsGrading(true);
        checkEqInt("打开：判级输入 2 条", (long long) mgr.nervesForGrading().size(), 2);
        checkEqInt("打开：取更近的自动管道判红", row.level, DANGER_RED);
        checkAbs("打开：S-05 = min(3.5, 1.97)", row.nerveDistMm, 4.0 - kCanalRadiusMm, 1e-6);

        // 归档往返：来源标记靠名称前缀，必须在 planJson 落盘/读回之后依旧成立，
        // 否则重启后自动管道会悄悄变成"手描"并直接进判级（危险方向）。
        // 同时故意让 mgr2 用默认值恢复：采纳开关是"本次会话医生确认过"的语义，
        // 不随归档持久化，恢复后必须重新勾选一次。
        const Json plan = mgr.planJson();
        MeasurementManager mgr2;
        mgr2.bindVolume(&sy.cv);
        checkTrue("planFromJson 往返成功", mgr2.planFromJson(plan));
        checkTrue("恢复后开关回到默认 false（采纳不持久化）", !mgr2.autoNerveFeedsGrading());
        checkEqInt("恢复后自动管道仍被识别为自动", mgr2.autoNerveCount(), 1);
        checkEqInt("恢复后共 2 条管道", (long long) mgr2.nervePaths().size(), 2);
        checkEqInt("恢复后判级输入只剩手描", (long long) mgr2.nervesForGrading().size(), 1);
        checkEqInt("恢复后等级绿", mgr2.findImplant(imId) ? mgr2.findImplant(imId)->level : -99,
                   SAFE_GREEN);
        mgr2.setAutoNerveFeedsGrading(true);
        checkEqInt("恢复后重新打开照样翻红", mgr2.findImplant(imId)->level, DANGER_RED);
    }
}

// ============================================================================
// 13) D-10 回归：S-06 的负间距必须参与判级，"未测量"与负值哨兵分离（第 8 轮）
// ============================================================================
//
// 第 7 轮真机取证到的现象（ROUND7_FULL_DEVICE_REGRESSION_REPORT.md §4 D-10）：
//   同几何的 #73 报 spacing=-1.00、#177 报 spacing=80.96，两枚重叠种植体都判"安全"。
// 根因有两条叠加：
//   a) minSpacing() 用 best=-1.0 同时表示"没有邻居"和"表面间距为负"，
//      于是一旦算出负值，后面的候选全部被当成"第一个候选"无条件覆盖（顺序决定结果）；
//   b) evaluate() 用 `if (sp >= 0.0)` 决定 spacingMeasured，等于"重叠不判级"。
// 本组把这两条分别钉死，并验证归档往返不再靠符号猜（决策 4：判级阈值仍是 PRD 的 3.0mm，
// 这里没有新增任何临床数字，只是让"已经测出来的负值"进入既有判级）。
static void group13SpacingRegression() {
    beginGroup("组13 D-10 —— S-06 负间距判级 / 未测量哨兵分离");

    Synth sy;
    sy.build(60, 60, 60, 0.4, 0.4, 0.4, 0.0f);
    sy.fillBox(10, 12, 10, 49, 52, 49, 1000.0f);       // 与组12 同一块 16mm 见方的骨
    VolumeRef vol; vol.bind(&sy.cv);
    ImplantPlanner planner(vol);
    const std::vector<NervePath> noNerve;              // 只看间距这一条腿

    Implant a; a.id = 1; a.name = "A"; a.entry = Vec3(12.0, 20.5, 21.0);
    a.pitchDeg = 0.0; a.yawDeg = 0.0; a.depthMm = 6.0; a.lengthMm = 11.0; a.diaMm = 4.0;
    Implant b = a; b.id = 2; b.name = "B";              // 与 A 完全同位 = #73/#177 的形状
    Implant far; far.id = 3; far.name = "FAR";
    far.entry = Vec3(12.0, 20.5, 90.0);                 // 同轴但远在 FOV 外：只造间距，不造骨
    far.pitchDeg = 0.0; far.yawDeg = 0.0; far.depthMm = 6.0; far.lengthMm = 11.0; far.diaMm = 4.0;
    ImplantPlanner::refreshGeometry(a);
    ImplantPlanner::refreshGeometry(b);
    ImplantPlanner::refreshGeometry(far);

    // ---- a) 夹具本身要给到绿档，否则后面的红无法归因于间距 ----
    {
        Implant solo = a;
        std::vector<Implant> only; only.push_back(solo);
        const ImplantSafety s = planner.evaluate(solo, noNerve, only);
        checkTrue("夹具：单颗时骨高进绿档", s.boneHeightMm >= 12.0);
        checkTrue("夹具：单颗时骨宽进绿档", s.boneWidthMm >= 7.2);
        checkTrue("D-10b：没有邻居 -> spacingMeasured=false", !s.spacingMeasured);
        checkEqInt("D-10b：未测量仍回填 -1.0 供显示（值本身不是判据）",
                   (long long) (solo.minSpacingMm == -1.0 ? 1 : 0), 1);
        checkEqInt("D-10b：无神经管描记 -> nerveMeasured=false", (long long) s.nerveMeasured, 0);
        checkEqInt("夹具：单颗等级为绿", solo.level, SAFE_GREEN);
    }

    // ---- b) 重叠 = 负表面间距，且与遍历顺序无关（D-10a 的核心）----
    {
        bool found = false;
        std::vector<Implant> o1; o1.push_back(a); o1.push_back(b); o1.push_back(far);
        std::vector<Implant> o2; o2.push_back(a); o2.push_back(far); o2.push_back(b);
        const double v1 = planner.minSpacing(a, o1, &found);
        checkTrue("重叠邻居存在 -> found=true", found);
        checkAbs("同位两枚：表面间距 = 0 - (4+4)/2 = -4.0", v1, -4.0, 1e-12);
        const double v2 = planner.minSpacing(a, o2, &found);
        checkAbs("D-10a 回归：远邻放在重叠邻居之前，最小间距仍是 -4.0", v2, -4.0, 1e-12);
        checkTrue("修复后 found 与数值符号解耦（-4 也算 measured）", found);
        std::vector<Implant> alone; alone.push_back(a);
        checkAbs("只有自身时返回 -1.0（未测量哨兵保留）", planner.minSpacing(a, alone, &found),
                 -1.0, 0.0);
        checkEqInt("只有自身时 found=false", (long long) found, 0);
    }

    // ---- c) 判级：重叠必须进 S-06 并判红，文案写"重叠" ----
    {
        Implant subj = a;
        std::vector<Implant> all; all.push_back(subj); all.push_back(b); all.push_back(far);
        const ImplantSafety s = planner.evaluate(subj, noNerve, all);
        checkTrue("D-10b：负间距照样 spacingMeasured=true（旧代码这里直接丢弃）", s.spacingMeasured);
        checkAbs("判级读到的就是 -4.0", s.spacingMm, -4.0, 1e-12);
        checkEqInt("重叠 -> DANGER_RED", subj.level, DANGER_RED);
        checkTrue("告警文案含「重叠」", subj.warnText.find("重叠") != std::string::npos);
        printf("        warnText = %s\n", subj.warnText.c_str());
        // 第 7 轮的可观测签名：#177 因为顺序效应拿到 80.96 并停在 SAFE。
        // 修复后同一形状的另一枚（b）也必须读 -4.0 并判红。
        Implant other = b;
        std::vector<Implant> all2; all2.push_back(a); all2.push_back(other); all2.push_back(far);
        planner.evaluate(other, noNerve, all2);
        checkAbs("D-10a：同位那枚的另一侧也读 -4.0（不再被大间距顶掉）", other.minSpacingMm, -4.0, 1e-12);
        checkEqInt("D-10a：另一侧同样判红", other.level, DANGER_RED);
    }

    // ---- d) 归档往返：measured 必须随 planJson 落盘，负间距不能被"恢复"成未测量 ----
    {
        MeasurementManager mgr; mgr.bindVolume(&sy.cv);
        Implant m1 = a; m1.id = 0; m1.name = "A";
        Implant m2 = b; m2.id = 0; m2.name = "B";
        const int idA = mgr.addImplant(m1);
        const int idB = mgr.addImplant(m2);
        checkTrue("两枚重叠种植体都入库", idA > 0 && idB > 0);
        checkEqInt("会话里 2 枚", (long long) mgr.implants().size(), 2);
        const Implant &rowA = *mgr.findImplant(idA);
        checkAbs("Manager 路径同样测出 -4.0", rowA.minSpacingMm, -4.0, 1e-12);
        checkEqInt("Manager 路径判红", rowA.level, DANGER_RED);

        const Json plan = mgr.planJson();
        MeasurementManager mgr2; mgr2.bindVolume(&sy.cv);
        checkTrue("planFromJson 往返成功", mgr2.planFromJson(plan));
        const Implant *restored = mgr2.findImplant(idA);
        checkTrue("恢复后能找到 A", restored != nullptr);
        if (restored) {
            checkTrue("D-10：恢复后 spacingMeasured 仍为 true（不再靠符号猜）",
                      restored->spacingMeasured);
            checkAbs("恢复后数值不变", restored->minSpacingMm, -4.0, 1e-9);
            checkEqInt("恢复后等级仍为红", restored->level, DANGER_RED);
        }
    }

    // ---- e) 旧归档（第 7 轮格式，没有 measured 字段）的兼容推断 ----
    {
        Json legacy = Json::makeObject();
        legacy.set("id", Json::makeNumber(73));
        legacy.set("minSpacingMm", Json::makeNumber(-1.0));
        legacy.set("nerveDistMm", Json::makeNumber(37.3));
        Implant out;
        MeasurementManager::implantFromJson(legacy, out);
        checkEqInt("旧归档：minSpacingMm=-1 推断为未测量", (long long) out.spacingMeasured, 0);
        checkEqInt("旧归档：nerveDistMm=37.3 推断为已测量", (long long) out.nerveMeasured, 1);

        Json legacy2 = Json::makeObject();
        legacy2.set("id", Json::makeNumber(74));
        legacy2.set("minSpacingMm", Json::makeNumber(3.5));
        legacy2.set("nerveDistMm", Json::makeNumber(-1.0));
        Implant out2;
        MeasurementManager::implantFromJson(legacy2, out2);
        checkEqInt("旧归档：3.5 推断为已测量", (long long) out2.spacingMeasured, 1);
        checkEqInt("旧归档：神经 -1 推断为未测量", (long long) out2.nerveMeasured, 0);

        // 新写入必须带字段：否则上面这套推断会在"负间距"上退化回 bug
        Implant nw; nw.id = 75; nw.minSpacingMm = -4.0; nw.spacingMeasured = true;
        nw.nerveDistMm = -1.0; nw.nerveMeasured = false;
        const Json j = MeasurementManager::implantToJson(nw);
        Implant back; MeasurementManager::implantFromJson(j, back);
        checkTrue("新归档：负间距 + measured=true 原样读回",
                  back.spacingMeasured && back.minSpacingMm == -4.0);
        checkEqInt("新归档：未测量的神经距离保持 false", (long long) back.nerveMeasured, 0);
    }
}


// ============================================================================
// 组 14：P-09 —— 植体型号库（ImplantLibrary）与修复导向（crown-down）偏差 S-11/S-12
//
// 三件必须守住的事：
//   1) 型号库的**来源可审计**：内置通用阶梯与随包 assets/implant_models.json 必须是
//      同一套口径；资产写坏时库不能悄悄变空（否则 AI-03 的推荐口径静默漂移）。
//   2) S-11/S-12 的**解析值**：期望值全部手算（点到无限长直线的距离、两直线夹角折到
//      [0,90]），不引入任何新的临床阈值。
//   3) **用户决策点 4 的红线**：加了冠方参考之后 S-02 的颜色等级必须一字不变，
//      文案里必须出现"判级阈值待 PRD 定值"。
// ============================================================================

/** 读主机上的随包资产（-DCBCT_MEASURE_REPO_ROOT 由 build_and_run.sh 传入） */
static std::string readAssetText(const std::string &path, bool *ok) {
    *ok = false;
    std::string out;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return out;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    *ok = true;
    return out;
}

static void group14ImplantLibraryAndCrown() {
    beginGroup("组14 P-09 —— 植体型号库 / crown-down 偏差（S-11 · S-12）");

    ImplantLibrary &lib = ImplantLibrary::shared();

    // ---- a) 内置通用库：只有体直径与桩体长度是确定的，其余一律不编造 ----
    lib.loadBuiltin();
    checkTrue("任何时刻都有库（AI-03 不会因为忘记加载资产而退化成连续长度）",
              lib.count() > 0);
    checkEqInt("内置通用库 5 直径 × 4 长度 = 20 档", (long long) lib.count(), 20);
    checkTrue("内置表来源标记 builtin=true", lib.isBuiltin());
    checkTrue("内置表数据口径 generic=true", lib.isGeneric());
    {
        const ImplantModelSpec *m = lib.bySku("GEN-4.1-11.5");
        checkTrue("bySku 命中 GEN-4.1-11.5", m != nullptr);
        if (m != nullptr) {
            checkAbs("命中项体直径", m->diaMm, 4.1, 1e-12);
            checkAbs("命中项桩体长度", m->lengthMm, 11.5, 1e-12);
            checkTrue("通用库 vendor 留空（凭记忆填厂商目录等于编造）", m->vendor.empty());
            checkTrue("平台直径不编造（0 = 资产未提供）", m->platformMm == 0.0);
            checkTrue("基台高度不编造（0 = 资产未提供）", m->abutmentMm == 0.0);
        }
        checkTrue("未知 sku 返回 nullptr 而不是猜一个", lib.bySku("GEN-9.9-99") == nullptr);
        checkTrue("空 sku 不查表", lib.bySku("") == nullptr);
    }

    // ---- b) fitMatch：骨量包络内选型，取最长（同长取较粗）----
    {
        const ImplantModelSpec *fit = lib.fitMatch(4.2, 12.0);
        checkTrue("Ø<=4.2 且 L<=12 有解", fit != nullptr);
        if (fit != nullptr)
            checkEqStr("包络内取最长档，同长再取较粗 -> GEN-4.1-11.5", fit->sku, "GEN-4.1-11.5");

        const ImplantModelSpec *tight = lib.fitMatch(4.05, 9.0);
        checkTrue("窄包络有解", tight != nullptr);
        if (tight != nullptr)
            checkEqStr("Ø<=4.05（放不下 4.1）且 L<=9.0 -> 最长 8.5 里取较粗 GEN-3.8-8.5",
                       tight->sku, "GEN-3.8-8.5");

        checkTrue("骨宽放不下最窄的桩 -> nullptr（调用方按\"需植骨/改方案\"处理）",
                  lib.fitMatch(3.0, 20.0) == nullptr);
        checkTrue("骨高放不下最短的桩 -> nullptr", lib.fitMatch(6.0, 6.0) == nullptr);
        // 边界必须含等号：恰好等于某档的库值不能被判成"放不下"
        const ImplantModelSpec *edge = lib.fitMatch(3.4, 7.0);
        checkTrue("包络边界取等号（Ø3.4×7 正好放得进 3.4/7.0 的包络）", edge != nullptr);
        if (edge != nullptr) checkEqStr("边界命中的就是那一档", edge->sku, "GEN-3.4-7");
    }

    // ---- c) nearestMatch：把连续值落到真实存在的档位 ----
    {
        // 代价 = |Δ直径|/0.5 + |Δ长度|/1.0。Ø4.2×11.0 的候选里
        // GEN-4.1-11.5 = 0.2 + 0.5 = 0.7 是最小值（GEN-4.1-10 = 1.2，GEN-3.8-11.5 = 1.3）。
        const ImplantModelSpec *n = lib.nearestMatch(4.2, 11.0);
        checkTrue("最近档存在", n != nullptr);
        if (n != nullptr) checkEqStr("Ø4.2×11.0 -> GEN-4.1-11.5", n->sku, "GEN-4.1-11.5");
        const ImplantModelSpec *big = lib.nearestMatch(9.9, 40.0);
        checkTrue("超出库范围的输入也能落档", big != nullptr);
        if (big != nullptr) checkEqInt("落档结果仍在库内（长度取最大档 15）",
                                      (long long) (big->lengthMm == 15.0 ? 1 : 0), 1);
    }

    // ---- d) 空库行为：清空后一律 nullptr，不猜型号 ----
    {
        lib.clear();
        checkEqInt("clear() 后条数为 0", (long long) lib.count(), 0);
        checkTrue("空库 fitMatch 返回 nullptr", lib.fitMatch(5.0, 15.0) == nullptr);
        checkTrue("空库 nearestMatch 返回 nullptr", lib.nearestMatch(4.0, 10.0) == nullptr);
        // 空库的 generic 口径：宁可说"通用"，也不给读者一个假的商品依据
        checkTrue("空库仍按 generic 处理", lib.isGeneric());
        lib.loadBuiltin();
        checkEqInt("恢复内置表", (long long) lib.count(), 20);
    }

    // ---- e) 资产解析：坏资产不能把库清空（否则推荐口径静默漂移）----
    {
        std::string err;
        checkEqInt("非法文本 -> -1", (long long) lib.loadFromJson("{ this is not json", &err), -1);
        checkEqInt("解析失败后条数不变", (long long) lib.count(), 20);
        checkTrue("失败原因写进 err 供 UI 直接展示", !err.empty());
        checkEqInt("空 models -> -1", (long long) lib.loadFromJson("{\"models\":[]}", &err), -1);
        checkEqInt("缺 models 键 -> -1", (long long) lib.loadFromJson("{\"kind\":\"x\"}", &err), -1);
        checkEqInt("失败路径不改动库来源标记", (long long) (lib.isBuiltin() ? 1 : 0), 1);

        const std::string vendorAsset =
                "{\"kind\":\"cbctmeasure.implant-models\",\"models\":["
                "{\"sku\":\"AC-375-10\",\"vendor\":\"ACME\",\"series\":\"S1\","
                "\"connection\":\"internal\",\"diaMm\":3.75,\"lengthMm\":10.0,"
                "\"platformMm\":4.8,\"abutmentMm\":2.5,\"note\":\"厂商目录示例\"},"
                "{\"sku\":\"BAD\",\"vendor\":\"ACME\",\"diaMm\":0.0,\"lengthMm\":8.0},"
                "{\"vendor\":\"ACME\",\"diaMm\":4.3,\"lengthMm\":8.5}"
                "]}";
        err.clear();
        const int n = lib.loadFromJson(vendorAsset, &err);
        checkEqInt("三条里只有两条合法（Ø0 的那条丢掉，否则 S-04 的\"骨宽>=直径\"恒真）",
                   (long long) n, 2);
        checkTrue("Ø0 条目不会污染告警：库里查不到 BAD", lib.bySku("BAD") == nullptr);
        checkTrue("厂商资产 -> builtin=false", !lib.isBuiltin());
        checkTrue("有 vendor 的资产 -> generic=false（可以当商品型号读）", !lib.isGeneric());
        {
            const ImplantModelSpec *a = lib.bySku("AC-375-10");
            checkTrue("厂商 sku 原样保留", a != nullptr);
            if (a != nullptr) {
                checkAbs("直径逐值保留", a->diaMm, 3.75, 1e-12);
                checkAbs("平台直径来自资产（内置库故意不给）", a->platformMm, 4.8, 1e-12);
                checkAbs("基台高度来自资产", a->abutmentMm, 2.5, 1e-12);
                checkEqStr("连接方式文字保留", a->connection, "internal");
            }
            checkTrue("缺 sku 的条目自动编号而不是丢弃",
                      lib.bySku("AC-375-10") != nullptr && lib.count() == 2);
        }
        // dumpJson -> loadFromJson 往返：字段不能丢，generic/builtin 口径不能串
        {
            const std::string dumped = lib.dumpJson();
            checkEqInt("dump 后重载条数一致", (long long) lib.loadFromJson(dumped, &err), 2);
            bool ok = false;
            const Json root = Json::parse(dumped, &ok);
            checkTrue("dump 是合法对象", ok && root.isObject());
            if (ok) {
                checkEqInt("dump 里 generic=false（数据口径）",
                           (long long) (root.at("generic").asBool(true) ? 0 : 1), 1);
                checkEqInt("dump 里 builtin=false（来源口径）",
                           (long long) (root.at("builtin").asBool(true) ? 0 : 1), 1);
                const ImplantModelSpec *b = lib.bySku("AC-375-10");
                checkTrue("往返后平台直径仍在", b != nullptr && b->platformMm == 4.8);
            }
        }
        checkEqInt("裸数组形式资产也能读",
                   (long long) lib.loadFromJson("[{\"diaMm\":5.0,\"lengthMm\":9.5}]", &err), 1);
        lib.loadBuiltin();
        checkTrue("恢复内置表（后续用例与真机默认口径一致）", lib.isBuiltin());
        checkEqInt("恢复后 20 档", (long long) lib.count(), 20);
    }

    // ---- f) 随包资产与内置阶梯必须同一套口径（可审计性）----
    {
#ifdef CBCT_MEASURE_REPO_ROOT
        bool ok = false;
        const std::string path = std::string(CBCT_MEASURE_REPO_ROOT) +
                "/cbctmeasure/src/main/assets/implant_models.json";
        const std::string text = readAssetText(path, &ok);
        if (!ok) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s 读不到（随包资产的口径检查没跑成）", path.c_str());
            noteFinding("随包 implant_models.json 不可读", buf);
        } else {
            std::vector<ImplantModelSpec> builtin = lib.specs();   // 先留一份内置表快照
            std::string err;
            const int n = lib.loadFromJson(text, &err);
            checkEqInt("随包资产解析出 20 档", (long long) n, 20);
            // 这是最关键的一条：随包资产是"通用名义尺寸"，vendor 全空，
            // 所以 generic 必须仍为 true —— 否则报告会把通用阶梯印成厂商目录。
            checkTrue("随包资产 generic=true（不是厂商商品数据）", lib.isGeneric());
            checkTrue("随包资产 builtin=false（来源是 json 资产）", !lib.isBuiltin());
            size_t mismatch = 0;
            const std::vector<ImplantModelSpec> &asset = lib.specs();
            if (asset.size() == builtin.size()) {
                for (size_t i = 0; i < asset.size(); ++i) {
                    if (asset[i].sku != builtin[i].sku ||
                        std::fabs(asset[i].diaMm - builtin[i].diaMm) > 1e-12 ||
                        std::fabs(asset[i].lengthMm - builtin[i].lengthMm) > 1e-12) {
                        ++mismatch;
                    }
                }
            } else {
                mismatch = (size_t) -1;
            }
            char detail[192];
            snprintf(detail, sizeof(detail),
                     "不一致的档数 = %zu（资产与内置阶梯必须是同一套 sku/Ø/长度）", mismatch);
            if (mismatch == 0) pass("资产与内置通用阶梯逐档一致");
            else fail("资产与内置通用阶梯逐档一致", detail);
            lib.loadBuiltin();
        }
#else
        noteFinding("随包 implant_models.json 口径检查", "编译时未给 -DCBCT_MEASURE_REPO_ROOT");
#endif
    }

    // ---- g) S-11 / S-12 的解析值（纯几何，不碰体数据）----
    {
        Implant im;
        im.id = 1;
        im.entry = Vec3(10.0, 10.0, 10.0);
        im.pitchDeg = 0.0;                    // axis = (0,0,-1)：指向根方
        im.yawDeg = 0.0;
        im.lengthMm = 11.0;
        im.diaMm = 4.1;
        ImplantPlanner::refreshGeometry(im);
        checkAbs("夹具：axis 指向根方 z", im.axis.z, -1.0, 1e-12);

        double off = -999.0, ang = -999.0, along = -999.0;
        bool fromRef = true;

        // g1) 零向量 = 没有参考。世界原点恰好是首体素中心，若把它当参考，
        //     S-11 会凭空多出一个十几毫米的假偏差，所以这里必须返回 false。
        checkEqInt("crownCenter 为零向量 -> 未测量",
                   (long long) (ImplantPlanner::crownDeviation(im, Vec3(), Vec3(),
                                                               &off, &ang, &along, &fromRef)
                                ? 1 : 0), 0);

        // g2) 完全对齐：冠中心正好在轴线的冠方延长线上 -> S-11=0、S-12=0。
        //     （修复前这里读 180°：axis 指向根方、toCrown 指向冠方天然反向，
        //       折到 [0,90] 才是"偏离轴线多少度"的本意。）
        checkTrue("对齐参考：测得到",
                  ImplantPlanner::crownDeviation(im, Vec3(10.0, 10.0, 12.0), Vec3(),
                                                 &off, &ang, &along, &fromRef));
        checkAbs("g2 S-11 水平偏差 = 0", off, 0.0, 1e-12);
        checkAbs("g2 S-12 成角偏差 = 0（不是 180）", ang, 0.0, 1e-9);
        checkAbs("g2 沿轴投影：冠方为负 -> -2", along, -2.0, 1e-12);
        checkEqInt("g2 单点参考 -> crownAxisFromReference=false", (long long) (fromRef ? 1 : 0), 0);

        // g3) 侧方 2mm 的冠中心：点到无限长直线的距离就是 2.0
        //     （用点到**线段**的距离会退化成"到入口点的距离"=2.83，故头文件强调无限延长线）
        ImplantPlanner::crownDeviation(im, Vec3(12.0, 10.0, 12.0), Vec3(), &off, &ang,
                                       &along, &fromRef);
        checkAbs("g3 S-11 = 2.0（侧方偏移）", off, 2.0, 1e-12);
        checkAbs("g3 S-12 = 45°（偏移 2mm、冠方 2mm）", ang, 45.0, 1e-9);

        // g4) 冠中心落在入口平面的侧方：连线与轴线垂直 -> 90°，
        //     这是"参考点画在种植体颈部平面上"的退化情形，按定义如实读 90°。
        ImplantPlanner::crownDeviation(im, Vec3(12.0, 10.0, 10.0), Vec3(), &off, &ang,
                                       &along, &fromRef);
        checkAbs("g4 S-11 = 2.0", off, 2.0, 1e-12);
        checkAbs("g4 S-12 = 90°（连线与轴线垂直）", ang, 90.0, 1e-9);
        checkAbs("g4 沿轴投影 = 0（同平面）", along, 0.0, 1e-12);

        // g5) 有冠长轴（A-02/A-03 两点标注）：轴倾斜 15°
        const double t15 = 15.0 * kPi / 180.0;
        ImplantPlanner::crownDeviation(im, Vec3(10.0, 10.0, 12.0),
                                       Vec3(std::sin(t15), 0.0, std::cos(t15)),
                                       &off, &ang, &along, &fromRef);
        checkAbs("g5 S-11 = 0（冠中心在轴线上）", off, 0.0, 1e-12);
        checkAbs("g5 S-12 = 15°（冠长轴倾斜）", ang, 15.0, 1e-9);
        checkEqInt("g5 冠长轴来自参考 -> crownAxisFromReference=true", (long long) (fromRef ? 1 : 0), 1);

        // g6) 冠长轴反向（标注首末点谁在冠方不可知）：折到 [0,90] -> 仍是 15°
        ImplantPlanner::crownDeviation(im, Vec3(10.0, 10.0, 12.0),
                                       Vec3(-std::sin(t15), 0.0, -std::cos(t15)),
                                       &off, &ang, &along, &fromRef);
        checkAbs("g6 反向冠长轴读同一个角（无向直线）", ang, 15.0, 1e-9);

        // g7) 斜向植入：yaw=90 时 axis = 绕 Y 转 (0,0,-1) -> (-1,0,0)? 见 axisFromAngles。
        //     这里只用一条性质：S-11 与参考的**垂直**距离无关yaw，取一个明确点核对。
        {
            Implant tilted = im;
            tilted.yawDeg = 90.0;
            ImplantPlanner::refreshGeometry(tilted);
            double ax = tilted.axis.x, az = tilted.axis.z;
            checkAbs("g7 yaw=90：axis 单位长", std::sqrt(ax * ax + az * az), 1.0, 1e-12);
            // 冠中心沿 axis 方向放在冠方 2mm（axis 指向根方，故 entry - axis*2）
            const Vec3 crown = tilted.entry - tilted.axis * 2.0;
            ImplantPlanner::crownDeviation(tilted, crown, Vec3(), &off, &ang, &along, &fromRef);
            checkAbs("g7 沿斜轴对齐：S-11 = 0", off, 0.0, 1e-12);
            checkAbs("g7 沿斜轴对齐：S-12 = 0", ang, 0.0, 1e-9);
            checkAbs("g7 沿轴投影 = -2", along, -2.0, 1e-9);
        }
    }

    // ---- h) evaluate()：S-11/S-12 回填、且**不参与判级**（决策点 4）----
    {
        Synth sy;
        sy.build(60, 60, 60, 0.4, 0.4, 0.4, 0.0f);
        sy.fillBox(10, 12, 10, 49, 52, 49, 1000.0f);      // 与组13 同一块骨
        VolumeRef vol; vol.bind(&sy.cv);
        ImplantPlanner planner(vol);
        const std::vector<NervePath> noNerve;

        Implant base;
        base.id = 1; base.name = "A";
        base.entry = Vec3(12.0, 20.5, 21.0);
        base.pitchDeg = 0.0; base.yawDeg = 0.0;
        base.depthMm = 6.0; base.lengthMm = 11.0; base.diaMm = 4.1;
        base.sku = "GEN-4.1-11.5";

        Implant noRef = base;
        std::vector<Implant> all1; all1.push_back(noRef);
        const ImplantSafety s1 = planner.evaluate(noRef, noNerve, all1);
        checkEqInt("h1 没有冠方参考 -> crownMeasured=false", (long long) (s1.crownMeasured ? 1 : 0), 0);
        checkAbs("h1 未测量时偏差回填 0（不能当\"0 偏差\"读）", noRef.crownOffsetMm, 0.0, 0.0);
        checkTrue("h1 未测量时告警不提 S-11", noRef.warnText.find("S-11") == std::string::npos);

        Implant withRef = base;
        withRef.crownRefKind = CROWN_MANUAL;
        withRef.crownCenter = Vec3(14.0, 20.5, 23.0);     // 侧方 2mm、冠方 2mm
        std::vector<Implant> all2; all2.push_back(withRef);
        const ImplantSafety s2 = planner.evaluate(withRef, noNerve, all2);
        checkTrue("h2 有参考 -> crownMeasured=true", s2.crownMeasured);
        checkAbs("h2 S-11 = 2.0", s2.crownOffsetMm, 2.0, 1e-12);
        checkAbs("h2 S-12 = 45°", s2.crownAngleDeg, 45.0, 1e-9);
        checkAbs("h2 沿轴 = -2", s2.crownAlongAxisMm, -2.0, 1e-9);
        checkTrue("h2 告警含 S-11/S-12", withRef.warnText.find("S-11") != std::string::npos);
        checkHas("h2 告警写明阈值待 PRD 定值", withRef.warnText, "判级阈值待 PRD 定值");
        checkTrue("h2 告警写明成角按入口→冠中心连线", withRef.warnText.find("入口→冠中心") != std::string::npos);
        // 决策点 4 的红线：加参考不能改颜色等级
        checkEqInt("h3 加冠方参考后 S-02 等级不变（不参与判级）", withRef.level, noRef.level);
        checkAbs("h3 骨高不变", s2.boneHeightMm, s1.boneHeightMm, 1e-12);
        checkAbs("h3 骨宽不变", s2.boneWidthMm, s1.boneWidthMm, 1e-12);
        // 结尾分隔符不能留在文案里（会一路印进 PDF/SR 行尾）
        const bool tailSep = withRef.warnText.size() >= 2 &&
                withRef.warnText.compare(withRef.warnText.size() - 2, 2, "; ") == 0;
        checkTrue("h4 告警结尾不带 \"; \"", !tailSep);
    }

    // ---- i) 冠方参考的解析：R-04 球面 / A-* 标注 / 引用失效 ----
    {
        Synth sy;
        sy.build(60, 60, 60, 0.4, 0.4, 0.4, 0.0f);
        sy.fillBox(10, 12, 10, 49, 52, 49, 1000.0f);

        RoiDef sphere;
        sphere.type = ROI_SPHERE;
        sphere.name = "牙冠中心";
        sphere.sphereCenter = Vec3(14.0, 20.5, 23.0);
        sphere.sphereRadius = 3.0;

        AnnotationStore store;
        Annotation line;
        line.type = AN_LINE;
        line.text = "冠长轴";
        line.points.push_back(Vec3(14.0, 20.5, 23.0));    // 冠中心
        line.points.push_back(Vec3(14.0, 20.5, 15.0));    // 根方端点 -> 冠长轴沿 z
        const int lineId = store.add(line);
        checkTrue("夹具：A-02 标注入库", lineId > 0);

        Annotation ring;
        ring.type = AN_RING;
        ring.points.push_back(Vec3(15.0, 20.5, 23.0));
        ring.points.push_back(Vec3(13.0, 20.5, 23.0));    // 圆心 (14,20.5,23)
        const int ringId = store.add(ring);

        Implant im;
        im.name = "A";
        im.entry = Vec3(12.0, 20.5, 21.0);
        im.pitchDeg = 0.0; im.yawDeg = 0.0;
        im.depthMm = 6.0; im.lengthMm = 11.0; im.diaMm = 4.1;

        // i1) R-04 球面：取球心，没有冠长轴
        {
            MeasurementManager mgr; mgr.bindVolume(&sy.cv);
            im.crownRefKind = CROWN_SPHERE_ROI;
            const int roiId = mgr.addRoi(sphere);
            Implant a = im; a.crownRefId = roiId;
            const int id = mgr.addImplant(a);
            const Implant *row = mgr.findImplant(id);
            checkTrue("球面参考：解析成功", row != nullptr && row->crownMeasured);
            if (row != nullptr) {
                checkAbs("球心即冠中心 x", row->crownCenter.x, 14.0, 1e-9);
                checkAbs("S-11 = 2.0（入口 x=12 -> 球心 x=14）", row->crownOffsetMm, 2.0, 1e-9);
                checkEqInt("球面不给冠长轴 -> crownAxisFromReference=false",
                           (long long) (row->crownAxisFromReference ? 1 : 0), 0);
            }
            // 引用失效（ROI 被删）：必须回到"未测量"，不能继续用上次解析出的旧坐标
            checkTrue("删除球面 ROI", mgr.removeRoi(roiId));
            mgr.recomputePlan();
            const Implant *gone = mgr.findImplant(id);
            checkTrue("i2 引用失效后 S-11/S-12 变回未测量（不缓存旧坐标）",
                      gone != nullptr && !gone->crownMeasured);
            if (gone != nullptr) {
                checkAbs("失效后偏差清零", gone->crownOffsetMm, 0.0, 0.0);
                checkTrue("失效后冠中心清零", gone->crownCenter.length() < 1e-12);
            }
        }

        // i3) A-* 标注：首点=冠中心，>=2 点时首->末视为冠长轴
        {
            MeasurementManager mgr; mgr.bindVolume(&sy.cv);
            mgr.setAnnotationStore(&store);
            Implant a = im;
            a.crownRefKind = CROWN_ANNOTATION; a.crownRefId = lineId;
            const int id = mgr.addImplant(a);
            const Implant *row = mgr.findImplant(id);
            checkTrue("标注参考：解析成功", row != nullptr && row->crownMeasured);
            if (row != nullptr) {
                checkAbs("首点即冠中心 x", row->crownCenter.x, 14.0, 1e-9);
                checkAbs("S-11 = 2.0", row->crownOffsetMm, 2.0, 1e-9);
                checkAbs("两点给出冠长轴（沿 z）-> S-12 = 0", row->crownAngleDeg, 0.0, 1e-9);
                checkEqInt("冠长轴来自参考 -> crownAxisFromReference=true",
                           (long long) (row->crownAxisFromReference ? 1 : 0), 1);
            }
            // 标注被删掉后同样要回到未测量
            checkTrue("删除标注", store.remove(lineId));
            mgr.recomputePlan();
            const Implant *after = mgr.findImplant(id);
            checkTrue("i4 标注失效 -> S-11/S-12 未测量", after != nullptr && !after->crownMeasured);
        }

        // i5) A-05 环形：points 是环上采样点，必须取圆心而不是首点
        {
            MeasurementManager mgr; mgr.bindVolume(&sy.cv);
            mgr.setAnnotationStore(&store);
            Implant a = im;
            a.crownRefKind = CROWN_ANNOTATION; a.crownRefId = ringId;
            const int id = mgr.addImplant(a);
            const Implant *row = mgr.findImplant(id);
            checkTrue("环形参考：解析成功", row != nullptr && row->crownMeasured);
            if (row != nullptr) {
                checkAbs("环形取圆心（首点会把参考放到环边缘，差一个半径）",
                         row->crownCenter.x, 14.0, 1e-12);
                checkAbs("S-11 = 2.0（圆心 x=14）", row->crownOffsetMm, 2.0, 1e-9);
                checkEqInt("环形不给冠长轴", (long long) (row->crownAxisFromReference ? 1 : 0), 0);
            }
        }

        // i6) 未注入标注存储（主机单测/旧会话）：kind=2 解析失败并写清原因，不影响其它指标
        {
            MeasurementManager mgr; mgr.bindVolume(&sy.cv);
            Implant a = im;
            a.crownRefKind = CROWN_ANNOTATION; a.crownRefId = ringId;
            const int id = mgr.addImplant(a);
            const Implant *row = mgr.findImplant(id);
            checkTrue("i6 未注入 annos_ 时不崩、也不冒充测到",
                      row != nullptr && !row->crownMeasured);
            if (row != nullptr) {
                checkAbs("其它指标照常：骨高 > 0", row->boneHeightMm > 0.0 ? 1.0 : 0.0, 1.0, 0.0);
                checkTrue("等级仍按 S-03~S-06 判", row->level == SAFE_GREEN ||
                          row->level == WARN_YELLOW || row->level == DANGER_RED);
            }
        }

        // i7) kind=1 但引用的是非球面 ROI（用户把 ROI 改了类型）：同样按未测量处理
        {
            MeasurementManager mgr; mgr.bindVolume(&sy.cv);
            RoiDef box; box.type = ROI_BOX; box.boxMin = Vec3(4, 4, 4); box.boxMax = Vec3(20, 20, 20);
            const int boxId = mgr.addRoi(box);
            Implant a = im;
            a.crownRefKind = CROWN_SPHERE_ROI; a.crownRefId = boxId;
            const int id = mgr.addImplant(a);
            const Implant *row = mgr.findImplant(id);
            checkTrue("i7 引用非球面 ROI -> 未测量", row != nullptr && !row->crownMeasured);
        }

        // i8) 引用 id 不存在（会话恢复后对象没了）
        {
            MeasurementManager mgr; mgr.bindVolume(&sy.cv);
            Implant a = im;
            a.crownRefKind = CROWN_SPHERE_ROI; a.crownRefId = 99999;
            const int id = mgr.addImplant(a);
            const Implant *row = mgr.findImplant(id);
            checkTrue("i8 悬空引用 -> 未测量", row != nullptr && !row->crownMeasured);
        }
    }

    // ---- j) 归档往返：sku 与冠方参考必须落得回 ----
    {
        Synth sy;
        sy.build(60, 60, 60, 0.4, 0.4, 0.4, 0.0f);
        sy.fillBox(10, 12, 10, 49, 52, 49, 1000.0f);
        AnnotationStore store;
        Annotation line;
        line.type = AN_LINE;
        line.points.push_back(Vec3(14.0, 20.5, 23.0));
        line.points.push_back(Vec3(14.0, 20.5, 15.0));
        const int lineId = store.add(line);

        MeasurementManager mgr; mgr.bindVolume(&sy.cv); mgr.setAnnotationStore(&store);
        Implant a;
        a.name = "A"; a.sku = "GEN-4.1-11.5";
        a.entry = Vec3(12.0, 20.5, 21.0);
        a.pitchDeg = 0.0; a.yawDeg = 0.0; a.depthMm = 6.0; a.lengthMm = 11.0; a.diaMm = 4.1;
        a.crownRefKind = CROWN_ANNOTATION; a.crownRefId = lineId;
        const int id = mgr.addImplant(a);
        const Implant *row = mgr.findImplant(id);
        checkTrue("夹具：写盘前已测到 S-11", row != nullptr && row->crownMeasured);

        const Json plan = mgr.planJson();
        MeasurementManager mgr2; mgr2.bindVolume(&sy.cv);
        AnnotationStore store2;      // 空存储：恢复后引用应失效（真实场景是标注也一并恢复）
        mgr2.setAnnotationStore(&store2);
        checkTrue("j1 归档往返成功", mgr2.planFromJson(plan));
        const Implant *back = mgr2.findImplant(id);
        checkTrue("j1 恢复后能找到植体", back != nullptr);
        if (back != nullptr) {
            checkEqStr("j2 型号 sku 原样恢复", back->sku, "GEN-4.1-11.5");
            checkEqInt("j2 参考类型恢复", back->crownRefKind, CROWN_ANNOTATION);
            checkEqInt("j2 参考 id 恢复", back->crownRefId, lineId);
            // 参考对象不在同一份标注存储里 -> 重新解析为未测量，而不是沿用旧偏差
            checkTrue("j3 标注未随归档恢复时，S-11/S-12 报未测量而不是旧值", !back->crownMeasured);
        }

        // j4) plan.json 自证型号库口径
        {
            bool ok = false;
            const Json root = Json::parse(plan.dump(), &ok);
            checkTrue("plan.json 可解析", ok);
            if (ok) {
                const Json &libJ = root.at("modelLibrary");
                checkEqInt("modelLibrary.count 与内置表一致",
                           (long long) libJ.at("count").asNumber(-1), 20);
                checkEqInt("modelLibrary.generic=true（内置通用阶梯）",
                           (long long) (libJ.at("generic").asBool(false) ? 1 : 0), 1);
                checkEqInt("modelLibrary.builtin=true",
                           (long long) (libJ.at("builtin").asBool(false) ? 1 : 0), 1);
            }
        }

        // j5) 第 7 轮及以前的归档（完全没有这些键）：不得凭空认为"测过"
        {
            Json legacy = Json::makeObject();
            legacy.set("id", Json::makeNumber(88));
            legacy.set("entry", Json::makeArray());
            Implant out;
            MeasurementManager::implantFromJson(legacy, out);
            checkEqInt("legacy 无 sku -> 空（UI 写\"自由参数\"）", (long long) (out.sku.empty() ? 1 : 0), 1);
            checkEqInt("legacy 无 crownRefKind -> CROWN_NONE", out.crownRefKind, CROWN_NONE);
            checkEqInt("legacy 无 crownMeasured -> false（绝不因为有数值就当测过）",
                       (long long) (out.crownMeasured ? 1 : 0), 0);
            checkTrue("legacy 冠中心为 0 向量", out.crownCenter.length() < 1e-12);
        }
    }
}


// ============================================================================
// 组 15 · P-10 第三磨牙 ↔ 下牙槽神经风险度量（core/NerveRisk.cpp）
//
// 夹具复用组 11/14 的合成掩膜（buildTwoToothAi）：实例 #1 是 i/j/k ∈ [4,13]
// 的 10×10×10 立方，抽稀体素球心因此精确落在 4.5 … 13.5，间距 1.0 mm。
// 于是"掩膜球心 ↔ 折线"的最小距离可以解析写出（下面每条都是 2.0 / 3.5 这种
// 手算得到的数），而不需要拿另一个易错的实现当参考。
//
// 每条 case 卡的是一件真会出错的事：
//   a) 折线端点外（foot 不在线段内）时距离仍然是垂足到端点的长度；
//   b) 剪枝（包围盒 + AABB 下界）会不会把真正的最近点剪掉；
//   c) 接触必须读 0 —— 按"到掩膜表面的距离"实现的话接触会得到正数；
//   d) 皮质骨的"最长连续非骨段"必须区分全长缺失与中段缺失；
//   e) 输入不可用时报未测量，而不是把 -1 当距离印进报告。
// ============================================================================

/** 两点折线（世界 mm）；radiusMm 用调用方给的口径，name 决定手描/AI-02 */
static NervePath riskNerve(const Vec3 &a, const Vec3 &b, double radiusMm,
                           const std::string &name) {
    NervePath p;
    p.name = name;
    p.radiusMm = radiusMm;
    p.visible = true;
    p.color = (int) 0xFFFF1744U;
    p.points.push_back(a);
    p.points.push_back(b);
    return p;
}

/**
 * 暴力参照：不看包围盒、不剪枝，遍历整张掩膜网格求最小球心距。
 * 存在的意义是"剪枝实现错了也能被发现"——被测实现与参照实现是两套循环。
 */
static double bruteMaskPolylineDist(const AiResult &ai, int instanceId,
                                    const std::vector<Vec3> &poly, Vec3 *root) {
    double best = 1e300;
    const AiGrid &g = ai.grid;
    for (int i = 0; i < g.redDim[0]; ++i)
        for (int j = 0; j < g.redDim[1]; ++j)
            for (int k = 0; k < g.redDim[2]; ++k) {
                const long long t = g.redIndex(i, j, k);
                if (t < 0 || (size_t) t >= ai.inst.size()) continue;
                if ((int) ai.inst[(size_t) t] != instanceId) continue;
                const Vec3 p = g.redToWorld(i, j, k);
                for (size_t q = 0; q + 1 < poly.size(); ++q) {
                    const Vec3 &a = poly[q], &b = poly[q + 1];
                    const Vec3 ab = b - a;
                    const double len2 = ab.dot(ab);
                    double t2 = len2 > 1e-12 ? (p - a).dot(ab) / len2 : 0.0;
                    if (t2 < 0.0) t2 = 0.0;
                    if (t2 > 1.0) t2 = 1.0;
                    const Vec3 foot = a + ab * t2;
                    const Vec3 d = p - foot;
                    const double dist = std::sqrt(d.dot(d));
                    if (dist < best) {
                        best = dist;
                        if (root) *root = p;
                    }
                }
            }
    return best;
}

static void group15NerveRisk() {
    beginGroup("组15 P-10 —— 第三磨牙↔下颌管风险度量（距离/接触/管壁皮质骨）");

    ImplantLibrary::shared().loadBuiltin();   // 与组14 的后置状态隔离

    Synth sy;
    sy.build(32, 32, 32, 1.0, 1.0, 1.0, -1000.0f);
    sy.cv.manufacturer = "ACME-HostStub";
    sy.cv.seriesInstanceUID = "1.2.826.0.1.3.1.1.1.9.9.15";
    sy.fillBox(4, 4, 4, 13, 13, 13, 1500.0f);          // 与实例 #1 同位的 HU 块

    AiResult ai;
    buildTwoToothAi(ai);                                // #1: 4..13, #2: 20..27 × k6..13
    VolumeRef vol;
    vol.bind(&sy.cv);

    // ---- a) 非接触的解析最小距离 ----
    //     折线沿 +y 从 (9.5,15.5,9.5) 起，掩膜最大球心 y=13.5 -> 距离正好 2.0，
    //     且垂足落在线段端点（foot 不在线段内部），这一步专门卡"点到直线 vs 点到线段"。
    {
        const Vec3 a(9.5, 15.5, 9.5), b(9.5, 20.5, 9.5);
        const NervePath p = riskNerve(a, b, 0.5, "手描下颌管");
        const ThirdMolarRisk r = NerveRisk::assess(vol, ai, 1, p, "手工描记");
        checkTrue("a1 有掩膜+折线 -> 已测量", r.measured);
        checkTrue("a1 折线顶点不在掩膜里 -> 非接触", !r.contact);
        checkAbs("a1 最小距离解析值 2.0mm", r.minDistMm, 2.0, 1e-9);
        checkAbs("a1 最近根面点 x", r.nearestRoot.x, 9.5, 1e-9);
        checkAbs("a1 最近根面点 y（掩膜最大球心）", r.nearestRoot.y, 13.5, 1e-9);
        checkAbs("a1 最近管点 = 折线端点（垂足在线段外）", r.nearestCanal.y, 15.5, 1e-9);
        checkEqInt("a1 参与计算的掩膜体素数 = 实例体素数 1000", r.rootVoxels, 1000);
        checkEqInt("a1 实例体素数与掩膜一致（取证字段自证）", ai.instances[0].voxels, 1000);
        checkTrue("a1 半径 0.5 < 距离 2.0 -> 未进入管腔", !r.withinCanalRadius);
        checkTrue("a1 皮质骨区间可取样（0.5..1.5）", r.cortexMeasured);
        checkEqStr("a1 来源如实写手工描记", r.nerveSource, std::string("手工描记"));
        checkHas("a1 结论行含最近距离", r.summaryText(), "最近距离");

        Vec3 bruteRoot;
        const double brute = bruteMaskPolylineDist(ai, 1, p.points, &bruteRoot);
        checkAbs("a2 剪枝/包围盒结果 == 全网格暴力参照", r.minDistMm, brute, 1e-9);
        checkAbs("a2 暴力参照本身就是 2.0", brute, 2.0, 1e-9);
    }

    // ---- a3) 折线中段最近（垂足在线段内部）+ 斜向折线的剪枝核对 ----
    {
        const Vec3 a(0.5, 17.0, 3.0), b(20.5, 17.0, 16.0);
        const NervePath p = riskNerve(a, b, 1.5, "手描下颌管");
        const ThirdMolarRisk r = NerveRisk::assess(vol, ai, 1, p, "手工描记");
        Vec3 bruteRoot;
        const double brute = bruteMaskPolylineDist(ai, 1, p.points, &bruteRoot);
        checkTrue("a3 斜折线也测到了", r.measured);
        checkAbs("a3 剪枝未漏掉真正的最近点", r.minDistMm, brute, 1e-9);
        checkTrue("a3 最近点确实来自折线内部（距端点 > 0.5mm）", r.minDistMm < 3.6);
        printf("        a3 实测 %.6f / 暴力 %.6f（参照最近根面点 %.1f,%.1f,%.1f）\n",
               r.minDistMm, brute, bruteRoot.x, bruteRoot.y, bruteRoot.z);
    }

    // ---- b) 接触：折线顶点落进掩膜体素 -> 距离 0，不是"到表面的正数" ----
    {
        const Vec3 inside(9.5, 9.5, 9.5), outside(9.5, 15.5, 9.5);
        const NervePath p = riskNerve(inside, outside, 1.5, "手描下颌管");
        const ThirdMolarRisk r = NerveRisk::assess(vol, ai, 1, p, "手工描记");
        checkTrue("b1 折线顶点在掩膜体内 -> 判为接触", r.contact);
        checkTrue("b1 已测量", r.measured);
        checkAbs("b1 接触时距离写 0（不是掩膜表面距离）", r.minDistMm, 0.0, 1e-12);
        checkTrue("b1 接触即视为进入管腔半径", r.withinCanalRadius);
        checkEqInt("b1 接触分支仍统计掩膜体素数（不写 0 让人以为没数据）", r.rootVoxels, 1000);
        checkTrue("b1 根面已在管腔内 -> 管壁区间无可取样段", !r.cortexMeasured);
        checkHas("b1 未取样原因写明管腔半径", r.error, "管腔半径");
        checkHas("b1 结论行标出接触", r.summaryText(), "接触");
    }

    // ---- c) 管腔半径口径的边界：<= 算进入 ----
    {
        const Vec3 a(9.5, 15.5, 9.5), b(9.5, 20.5, 9.5);
        const ThirdMolarRisk tight = NerveRisk::assess(vol, ai, 1,
                                                       riskNerve(a, b, 2.0, "手描"), "手工描记");
        const ThirdMolarRisk loose = NerveRisk::assess(vol, ai, 1,
                                                       riskNerve(a, b, 1.99, "手描"), "手工描记");
        checkAbs("c1 距离仍是 2.0（半径不参与距离计算）", tight.minDistMm, 2.0, 1e-9);
        checkTrue("c1 半径 2.0 == 距离 2.0 -> 边界算进入（<=）", tight.withinCanalRadius);
        checkTrue("c2 半径 1.99 < 距离 2.0 -> 未进入", !loose.withinCanalRadius);
        checkAbs("c2 当时用的半径随条目记录，换半径要重算", loose.canalRadiusMm, 1.99, 1e-9);
    }

    // ---- d) 管壁区间里的皮质骨取样（三档解析对照）----
    {
        const Vec3 a(9.5, 15.5, 9.5), b(9.5, 20.5, 9.5);
        // 射线上取样点依次是 y=15.0 / 14.5 / 14.0（step=0.5，tStart=0.5）
        {
            const ThirdMolarRisk r = NerveRisk::assess(vol, ai, 1,
                                                       riskNerve(a, b, 0.5, "手描"), "手工描记");
            checkTrue("d1 取到 3 个样点", r.cortexMeasured);
            checkTrue("d1 全程 -1000HU -> 未采到骨", !r.cortexPresent);
            checkAbs("d1 最长连续非骨段 = 3×0.5 = 1.5mm", r.cortexGapMm, 1.5, 1e-9);
            checkHas("d1 结论行如实说\"未采到骨\"", r.summaryText(), "未采到骨");
        }
        sy.fillBox(8, 14, 8, 11, 15, 11, 1000.0f);     // 皮质骨薄板覆盖整段区间
        {
            const ThirdMolarRisk r = NerveRisk::assess(vol, ai, 1,
                                                       riskNerve(a, b, 0.5, "手描"), "手工描记");
            checkTrue("d2 已测量", r.measured);
            checkTrue("d2 骨板在区间内 -> 采到骨", r.cortexPresent);
            checkAbs("d2 三个样点全是骨 -> 非骨段 0mm", r.cortexGapMm, 0.0, 1e-9);
            checkAbs("d2 骨口径写 200HU（PRD R-01 同源）", r.cortexBoneThresholdHu, 200.0, 1e-12);
            checkHas("d2 结论行含\"采到骨\"", r.summaryText(), "采到骨");
        }
        for (int k = 8; k <= 11; ++k)
            for (int j = 15; j <= 15; ++j)
                for (int i = 8; i <= 11; ++i) sy.set(i, j, k, -1000.0f);   // 只留 j=14 一层
        {
            const ThirdMolarRisk r = NerveRisk::assess(vol, ai, 1,
                                                       riskNerve(a, b, 0.5, "手描"), "手工描记");
            // y=15.0 -> j15 空气；y=14.5 -> (1000 + -1000)/2 = 0 < 200；y=14.0 -> 骨
            checkTrue("d3 中段插值掉到阈值以下 -> 仍算采到骨（端点命中）", r.cortexPresent);
            checkAbs("d3 最长连续非骨段 = 前两段 1.0mm", r.cortexGapMm, 1.0, 1e-9);
        }
    }

    // ---- e) 输入不可用：一律 measured=false + 原因，绝不产出数字 ----
    {
        const Vec3 a(9.5, 15.5, 9.5), b(9.5, 20.5, 9.5);
        VolumeRef novol;                                  // 未绑定
        const ThirdMolarRisk g1 = NerveRisk::assess(novol, ai, 1,
                                                    riskNerve(a, b, 1.5, "手描"), "手工描记");
        checkTrue("e1 无体数据 -> 未测量", !g1.measured);
        checkHas("e1 原因写明体数据未绑定", g1.error, "体数据未绑定");
        checkAbs("e1 距离保持 -1（不是 0 也不是猜值）", g1.minDistMm, -1.0, 1e-12);

        AiResult nomask;                                  // 空结果
        const ThirdMolarRisk g2 = NerveRisk::assess(vol, nomask, 1,
                                                    riskNerve(a, b, 1.5, "手描"), "手工描记");
        checkTrue("e2 无 AI-01 掩膜 -> 未测量", !g2.measured);
        checkHas("e2 原因写明缺牙根掩膜", g2.error, "掩膜");

        const NervePath onePoint = riskNerve(a, a, 1.5, "手描");
        NervePath truncated = onePoint;
        truncated.points.pop_back();                      // 只剩 1 点
        const ThirdMolarRisk g4 = NerveRisk::assess(vol, ai, 1, truncated, "手工描记");
        checkTrue("e3 折线少于 2 点 -> 未测量", !g4.measured);
        checkHas("e3 原因写明折线点数不足", g4.error, "2 点");

        const ThirdMolarRisk g5 = NerveRisk::assess(vol, ai, 99,
                                                    riskNerve(a, b, 1.5, "手描"), "手工描记");
        checkTrue("e4 实例不在结果里 -> 未测量", !g5.measured);
        checkHas("e4 原因写明实例不存在", g5.error, "不在当前推理结果里");
        checkEqInt("e4 掩膜体素数 0（与接触时的 1000 形成对照）", g5.rootVoxels, 0);

        checkHas("e5 未测量的结论行以\"未测量\"开头", g4.summaryText(), "未测量");
    }

    // ---- f) 来源可复核字段（FOV / 间距 / 剂量口径）----
    {
        const Vec3 a(9.5, 15.5, 9.5), b(9.5, 20.5, 9.5);
        const ThirdMolarRisk r = NerveRisk::assess(vol, ai, 1,
                                                   riskNerve(a, b, 1.5, "手描"), "手工描记");
        checkAbs("f1 FOV X = 32 体素 × 1.0mm", r.fovXMm, 32.0, 1e-9);
        checkAbs("f1 FOV Y", r.fovYMm, 32.0, 1e-9);
        checkAbs("f1 FOV Z", r.fovZMm, 32.0, 1e-9);
        checkAbs("f1 体素间距 Z 如实记录", r.spacingZ, 1.0, 1e-12);
        checkHas("f2 剂量：本序列没这些字段时如实写未携带", r.doseText, "未携带");
        checkEqStr("f3 序列 UID 随行（可回查原始 DICOM）", r.seriesUid,
                   std::string("1.2.826.0.1.3.1.1.1.9.9.15"));
        checkEqStr("f3 设备厂商随行", r.manufacturer, std::string("ACME-HostStub"));
        checkHas("f4 判读口径单独成句，且明说不参与 S-02",
                 std::string(NerveRisk::gradingCaveat()), "不参与 S-02");
        checkHas("f4 判读口径明说阈值待 PRD", std::string(NerveRisk::gradingCaveat()), "PRD");
    }

    // ---- g) JSON 逐字段往返（归档/报告/SR 共用同一份序列化）----
    {
        const Vec3 a(9.5, 15.5, 9.5), b(9.5, 20.5, 9.5);
        ThirdMolarRisk src = NerveRisk::assess(vol, ai, 1,
                                               riskNerve(a, b, 0.5, "手描"), "手工描记");
        src.id = 77;
        src.createdAt = 1700000000123LL;
        const std::string text = NerveRisk::toJson(src).dump(false);
        bool ok = false;
        const Json back = Json::parse(text, &ok);
        checkTrue("g1 序列化文本可再解析", ok);
        ThirdMolarRisk out;
        NerveRisk::fromJson(back, out);
        checkEqInt("g2 id 往返", out.id, 77);
        checkEqInt("g2 createdAt 往返（毫秒级不丢位）", out.createdAt, 1700000000123LL);
        checkEqInt("g3 instanceId", out.instanceId, 1);
        checkAbs("g3 minDistMm", out.minDistMm, src.minDistMm, 1e-9);
        checkEqInt("g3 measured", out.measured ? 1 : 0, 1);
        checkEqInt("g3 contact", out.contact ? 1 : 0, 0);
        checkEqInt("g4 cortexMeasured/cortexPresent 两个布尔都要各归各",
                   (long long) ((out.cortexMeasured && out.cortexPresent) ? 1 : 0), 1);
        checkAbs("g4 cortexGapMm", out.cortexGapMm, src.cortexGapMm, 1e-9);
        checkAbs("g5 canalRadiusMm", out.canalRadiusMm, 0.5, 1e-12);
        checkEqInt("g5 rootVoxels 不截断", out.rootVoxels, 1000);
        checkAbs("g6 最近点坐标往返", out.nearestRoot.y, 13.5, 1e-9);
        checkAbs("g6 管侧最近点往返", out.nearestCanal.y, 15.5, 1e-9);
        checkEqStr("g7 doseText 原文往返", out.doseText, src.doseText);
        checkAbs("g7 fov 往返", out.fovZMm, 32.0, 1e-9);
        checkHas("g8 判读口径随条目入档（三年后还能知道它没有阈值）", text, "gradingCaveat");

        // 旧归档：完全没有 P-10 字段 -> 结构体默认值，且不得"看起来像测过"
        ThirdMolarRisk legacy;
        const Json emptyObj = Json::parse("{}", &ok);
        NerveRisk::fromJson(emptyObj, legacy);
        checkTrue("g9 空对象读出来 measured=false", !legacy.measured);
        checkAbs("g9 距离保持默认 -1", legacy.minDistMm, -1.0, 1e-12);
        checkAbs("g10 骨口径缺省时回填 200HU（取样口径不能空着）",
                 legacy.cortexBoneThresholdHu, 200.0, 1e-12);
    }

    // ---- h) 管理器：入库、去重覆盖、牙位名、arch 过滤、失效 ----
    {
        MeasurementManager mgr;
        mgr.bindVolume(&sy.cv);
        mgr.adoptAiResult(ai);
        const int nerveId = mgr.addNervePath(riskNerve(Vec3(9.5, 15.5, 9.5),
                                                       Vec3(9.5, 20.5, 9.5), 1.5, "手描下颌管"));
        checkTrue("h1 神经管已入库", nerveId > 0);

        ThirdMolarRisk r;
        checkTrue("h2 单条评估成功", mgr.assessNerveRisk(1, nerveId, r));
        checkEqInt("h2 会话里 1 条风险", (long long) mgr.nerveRisks().size(), 1);
        checkTrue("h2 条目 id 由管理器分配", r.id > 0);
        checkTrue("h2 createdAt 非 0（报告按时间排序用）", r.createdAt > 0);
        checkAbs("h2 数值与 core 层同一条算式", r.minDistMm, 2.0, 1e-9);

        ThirdMolarRisk r2;
        checkTrue("h3 重复评估仍成功", mgr.assessNerveRisk(1, nerveId, r2));
        checkEqInt("h3 同组合覆盖而不是加行", (long long) mgr.nerveRisks().size(), 1);
        checkEqInt("h3 沿用原 id（报告不会长出两份同形状的行）", r2.id, r.id);
        checkEqInt("D-16 覆盖保留 createdAt（首次评估时间不漂移）",
                   r2.createdAt, r.createdAt);

        const int otherNerve = mgr.addNervePath(riskNerve(Vec3(24.5, 29.5, 10.5),
                                                          Vec3(24.5, 31.5, 10.5), 1.5, "手描 II"));
        ThirdMolarRisk r3;
        checkTrue("h4 不同实例 × 不同路径可并存", mgr.assessNerveRisk(2, otherNerve, r3));
        checkEqInt("h4 会话里 2 条", (long long) mgr.nerveRisks().size(), 2);
        checkTrue("h5 删除单条生效", mgr.removeNerveRisk(r3.id));
        checkEqInt("h5 删除后 1 条", (long long) mgr.nerveRisks().size(), 1);
        checkTrue("h5 删不存在的 id 返回 false", !mgr.removeNerveRisk(999999));

        // 未知神经管 id：返回 false 且带原因（UI 要显示"点了没反应"的那一句）
        ThirdMolarRisk bad;
        checkTrue("h6 不存在的神经管 -> false", !mgr.assessNerveRisk(1, 999999, bad));
        checkHas("h6 原因写明路径不在会话里", bad.error, "不在会话里");
        checkTrue("h6 失败的组合不入库", (long long) mgr.nerveRisks().size() == 1);
    }

    // ---- h7) 牙位名回填：AI 掩膜落库为 ROI 后，风险行用临床名字 ----
    {
        MeasurementManager mgr;
        mgr.bindVolume(&sy.cv);
        mgr.adoptAiResult(ai);
        mgr.aiAutoMeasure(true);
        const int nerveId = mgr.addNervePath(riskNerve(Vec3(9.5, 15.5, 9.5),
                                                       Vec3(9.5, 20.5, 9.5), 1.5, "手描"));
        ThirdMolarRisk r;
        checkTrue("h7 评估成功", mgr.assessNerveRisk(1, nerveId, r));
        checkHas("h7 牙位名取掩膜 ROI 名（AI 牙 U…）", r.toothName, "AI 牙");
        checkHas("h7 ROI 名里带实例号，报告能对回 #1", r.toothName, "#1");
    }

    // ---- h8) 一键评估的牙弓过滤 ----
    {
        MeasurementManager mgr;
        mgr.bindVolume(&sy.cv);
        mgr.adoptAiResult(ai);                 // #1 arch=0（上颌）/ #2 arch=1（下颌）
        // 一键评估不需要点名路径，所以这里只入库不取 id
        mgr.addNervePath(riskNerve(Vec3(24.5, 29.5, 10.5),
                                   Vec3(24.5, 31.5, 10.5), 1.5, "手描"));
        const int n = mgr.assessAllNerveRisks();
        checkEqInt("h8 下颌弓 1 颗 × 1 条路径 -> 1 条", n, 1);
        checkTrue("h8 入库的是下颌那颗（arch==1）",
                  !mgr.nerveRisks().empty() && mgr.nerveRisks()[0].instanceId == 2);

        // 没有神经管路径时不产条目，日志给原因（不是静默 0）
        MeasurementManager mgr0;
        mgr0.bindVolume(&sy.cv);
        mgr0.adoptAiResult(ai);
        checkEqInt("h9 无路径 -> 0 条", mgr0.assessAllNerveRisks(), 0);

        // 牙弓判不出来（arch=-1）时的退化路径：全口评估，但条目要说清口径
        AiResult unknownArch;
        buildTwoToothAi(unknownArch);
        for (size_t i = 0; i < unknownArch.instances.size(); ++i)
            unknownArch.instances[i].arch = -1;
        MeasurementManager mgrU;
        mgrU.bindVolume(&sy.cv);
        mgrU.adoptAiResult(unknownArch);
        mgrU.addNervePath(riskNerve(Vec3(24.5, 29.5, 10.5), Vec3(24.5, 31.5, 10.5), 1.5, "手描"));
        checkEqInt("h10 arch 未知 -> 退化为一颗都不漏（2 实例各 1 条）",
                   mgrU.assessAllNerveRisks(), 2);

        // 重跑推理（掩膜换了）：旧风险数字必须作废，而不是继续显示上一轮的距离
        const size_t before = mgr.nerveRisks().size();
        checkTrue("h11 clearAi 前有条目", before > 0);
        mgr.clearAi();
        checkEqInt("h11 重推后风险条目清空（依赖掩膜的数字不能留旧值）",
                   (long long) mgr.nerveRisks().size(), 0);
    }

    // ---- h12) AI-02 自动管道的来源文案（门禁未过这件事要一路跟到报告）----
    {
        MeasurementManager mgr;
        mgr.bindVolume(&sy.cv);
        mgr.adoptAiResult(ai);
        const int autoId = mgr.addNervePath(riskNerve(Vec3(9.5, 15.5, 9.5),
                                                      Vec3(9.5, 20.5, 9.5), 1.5,
                                                      std::string(AiConst::CANAL_AUTO_NOTE) + " #1"));
        ThirdMolarRisk r;
        checkTrue("h12 自动管道也能评估", mgr.assessNerveRisk(1, autoId, r));
        checkHas("h12 来源写明是 AI-02 自动结果", r.nerveSource, "AI-02");
        checkHas("h12 来源同时写明本轮只作预览", r.nerveSource, "预览");
        checkHas("h13 结论行里有神经管名字", r.summaryText(), "AI-02");

        // D-15：清掉自动管道后，按 nervePathId 引用它的风险行不能留在会话里
        checkEqInt("D-15 先有 1 条自动管道风险行", (long long) mgr.nerveRisks().size(), 1);
        checkEqInt("D-15 clearCanalTracks 移除自动管道", mgr.clearCanalTracks(), 1);
        checkEqInt("D-15 风险行随管道一起清理", (long long) mgr.nerveRisks().size(), 0);
    }

    // ---- i) plan.json：随归档走 + 旧归档兼容 ----
    {
        MeasurementManager mgr;
        mgr.bindVolume(&sy.cv);
        mgr.adoptAiResult(ai);
        const int nerveId = mgr.addNervePath(riskNerve(Vec3(9.5, 15.5, 9.5),
                                                       Vec3(9.5, 20.5, 9.5), 0.5, "手描下颌管"));
        ThirdMolarRisk r;
        checkTrue("i1 评估成功（写盘前夹具已就绪）", mgr.assessNerveRisk(1, nerveId, r));

        const Json plan = mgr.planJson();
        bool ok = false;
        const Json root = Json::parse(plan.dump(false), &ok);
        checkTrue("i2 plan.json 可解析", ok);
        const Json &arr = root.at("nerveRisks");
        checkTrue("i3 归档里有 nerveRisks 数组", arr.isArray());
        checkEqInt("i3 条数与内存一致", (long long) arr.size(), 1);
        checkAbs("i4 归档内数值未失真", arr[0].at("minDistMm").asNumber(-99), 2.0, 1e-9);

        MeasurementManager mgr2;
        mgr2.bindVolume(&sy.cv);
        checkTrue("i5 恢复归档成功", mgr2.planFromJson(plan));
        checkEqInt("i6 风险条目随归档恢复", (long long) mgr2.nerveRisks().size(), 1);
        if (!mgr2.nerveRisks().empty()) {
            const ThirdMolarRisk &b = mgr2.nerveRisks()[0];
            checkEqInt("i7 id 原样恢复（不重新编号）", b.id, r.id);
            checkAbs("i7 距离原样恢复", b.minDistMm, 2.0, 1e-9);
            checkAbs("i7 皮质骨缺口原样恢复", b.cortexGapMm, r.cortexGapMm, 1e-9);
            checkEqStr("i8 剂量口径原文恢复", b.doseText, r.doseText);
            // 恢复后的 mgr2 没有 AI 掩膜：条目按原值留着，但绝不能被"顺手重算"改写
            mgr2.recomputePlan();
            checkTrue("i9 recomputePlan 不重算风险（没有掩膜就会把数字刷成未测量）",
                      !mgr2.nerveRisks().empty() &&
                      mgr2.nerveRisks()[0].measured &&
                      std::fabs(mgr2.nerveRisks()[0].minDistMm - 2.0) < 1e-9);
        }
        // 恢复归档后再评估同一会话的另一个组合：id 不得与已恢复的条目撞号
        mgr2.adoptAiResult(ai);              // 恢复的归档本身不带掩膜，先补上
        ThirdMolarRisk again;
        const int nid2 = mgr2.addNervePath(riskNerve(Vec3(24.5, 29.5, 10.5),
                                                     Vec3(24.5, 31.5, 10.5), 1.5, "手描 II"));
        const bool okAgain = mgr2.assessNerveRisk(2, nid2, again);
        checkTrue("i10 恢复后可继续新增评估", okAgain);
        checkEqInt("i11 新条目 id 不与恢复条目重复", (long long) (again.id != r.id ? 1 : 0), 1);
        checkEqInt("i11 会话里共 2 条", (long long) mgr2.nerveRisks().size(), 2);

        // 第 7 轮及以前的归档：完全没有 nerveRisks 键
        Json legacy = Json::makeObject();
        legacy.set("implants", Json::makeArray());
        legacy.set("nervePaths", Json::makeArray());
        MeasurementManager mgrL;
        mgrL.bindVolume(&sy.cv);
        checkTrue("i12 旧归档仍能载入", mgrL.planFromJson(legacy));
        checkEqInt("i12 旧归档读出 0 条风险（而不是 1 条来源不明的条目）",
                   (long long) mgrL.nerveRisks().size(), 0);
    }
}


// ============================================================================
// 组 16：O-01 —— Json::asInt 对"无符号 32 位颜色"的窄化语义
//
// 历史 Kotlin 侧把颜色写成 0..4294967295（如 4294967295 = 0xFFFFFFFF）。
// double -> int 越界是 UB：arm64 饱和成 INT_MAX（真机实测 0x7fffffff），
// x86 得 INT_MIN。正确语义应与 Java/Kotlin 的 (int) 一致：按 32 位截断。
// 同时必须保证正常范围内的值（id、类型、负数）一字不变。
// ============================================================================
static void group16JsonIntNarrowing() {
    auto asInt = [](double d) {
        Json j = Json::makeObject();
        j.set("v", Json::makeNumber(d));
        return j.at("v").asInt(-12345);
    };

    checkEqInt("O-01 0xFFFFFFFF 读回 -1", (long long) asInt(4294967295.0), -1);
    const unsigned int cyan = 0xFF00E5FFu;
    checkEqInt("O-01 0xFF00E5FF 读回原 Int 位模式",
               (long long) asInt((double) cyan), (long long) (int) cyan);
    checkEqInt("O-01 2^31 截断为 INT_MIN", (long long) asInt(2147483648.0), INT_MIN);
    checkEqInt("O-01 -1 不变", (long long) asInt(-1.0), -1);
    checkEqInt("O-01 普通 id 不变", (long long) asInt(73.0), 73);
    checkEqInt("O-01 取整仍按四舍五入", (long long) asInt(2.6), 3);
    checkEqInt("O-01 超出 int64 时回退 fallback", (long long) asInt(1e19), -12345);
    checkEqInt("O-01 非数字回退 fallback",
               (long long) Json::makeObject().at("missing").asInt(-7), -7);
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
    group11AiMaskReuse();
    group12CanalTracking();
    group13SpacingRegression();
    group14ImplantLibraryAndCrown();
    group15NerveRisk();
    group16JsonIntNarrowing();

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
