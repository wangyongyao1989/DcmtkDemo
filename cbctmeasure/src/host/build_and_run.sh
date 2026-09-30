#!/bin/sh
# ============================================================================
# :cbctmeasure 纯 C++ 业务核心（core/）的主机侧精度验收单测（PRD §6 AC-01~AC-04）
#
# 只用 macOS 自带的 clang（c++），不碰 Gradle / NDK / CMake / 第三方测试框架，
# 也不链 :cbctdeal 的原生库和 DCMTK —— 体数据由 test_main.cpp 里的 Synth 合成，
# CbctVolume / android_log 用 src/host/stub 下的主机桩件顶替。
#
# 用法（仓库根目录或任意目录均可）：
#   sh cbctmeasure/src/host/build_and_run.sh
# 输出：每条 case 一行 PASS/FAIL；结尾打印 AC-01~AC-04 最坏实测误差。
# 退出码：0 = 全过；非 0 = 有 FAIL（或有 crash）。
#
# 说明：core/SrReport.cpp 是唯一引用 DCMTK 的翻译单元，主机侧刻意排除，
# 因此本套用例不覆盖 SR 导出（那部分需要真机/DCMTK 环境）。
# ============================================================================

# 定位仓库根：本脚本永远在 <root>/cbctmeasure/src/host/ 下
SELF=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SELF/../../.." && pwd)

HOST_INC="$SELF/stub"                 # 必须排在最前，遮住真机 include/CbctVolume.h
CPP_INC="$ROOT/cbctmeasure/src/main/cpp"
CORE="$ROOT/cbctmeasure/src/main/cpp/core"

OUT=${CBCT_MEASURE_AC_BIN:-/tmp/measure_ac_tests}
STD=${CBCT_MEASURE_AC_STD:-c++11}     # core 只用 C++11；传 c++17 也可

# ASAN=1 时用地址检查器跑，定位崩溃/越界（macOS clang 自带 -fsanitize=address）
EXTRA=""
if [ "$ASAN" = "1" ]; then
    EXTRA="-fsanitize=address -fno-omit-frame-pointer -g"
    OUT="$OUT.asan"
fi

set -e
c++ -std=$STD -Wall -Wno-unused-function $EXTRA \
    -I "$HOST_INC" -I "$CPP_INC" -I "$CPP_INC/include" \
    "$SELF/test_main.cpp" \
    "$CORE/Json.cpp" \
    "$CORE/MeasureMath.cpp" \
    "$CORE/VolumeRef.cpp" \
    "$CORE/RoiExtractor.cpp" \
    "$CORE/ImplantPlanner.cpp" \
    "$CORE/AnnotationStore.cpp" \
    "$CORE/MeasurePicker.cpp" \
    "$CORE/MeasurementManager.cpp" \
    -o "$OUT"

echo "built: $OUT"
"$OUT"
