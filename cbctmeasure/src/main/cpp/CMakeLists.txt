cmake_minimum_required(VERSION 3.22.1)

project("cbct_measure")

# ========== 基础配置：与 :cbctdeal / :dcmtk 模块保持一致 ==========
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -frtti -fexceptions")
set(ANDROID_STL c++_shared)

# ==========================================================================
# 依赖策略（对应 PRD 7.1 模块依赖关系 / 8.1 CMakeLists 扩展）
#
# 1) 不链接 VTK。
#    PRD 8.1 原方案是让本模块也链接 libvtk*.a 以便向共享渲染窗口 AddViewProp。
#    实测工程内每个模块各自静态链接一份 VTK（cbct_native.so 已链 43 个 .a），
#    若再链一份进 libcbct_measure.so，就会出现两套 VTK 单例：
#      - vtkInformationKey 在各模块内以"静态对象地址"作键，两套地址不同，
#        A 模块写入的属性 B 模块按自己的键查不到；
#      - 对象工厂 / vtkObject::GlobalWarningDisplay / 渲染缓存各自独立，
#        跨 .so 传递 vtkActor* 时 dynamic_cast(SafeDownCast) 与 RTTI 也会失效。
#    因此本模块改为：几何与测量计算全部在纯 C++ 里完成（精度可控、可离线单测），
#    世界<->屏幕换算与取帧通过 :cbctdeal 暴露的 POD 只读内省接口
#    （projectPoints / displayToRay / captureFrame）委托给真正持有相机的那份
#    VTK 实例，叠加图形由 Android Canvas 绘制在同一个渲染窗口的上层视图里。
#    这与 PRD 风险表 R-01「实现自定义射线-体素交叉算法替代 vtkCellPicker」
#    给出的缓解措施一致，且规避了 R-03（MPR 坐标逆映射误差）。
#
# 2) 链接 DCMTK 静态库（仅 DICOM SR 生成需要，见 core/SrReport.cpp）。
#    与 :cbctdeal 的做法相同：仅在构建层共享 :dcmtk 模块下的交叉编译产物，
#    Kotlin 层零依赖；DCMTK 对象（DSRDocument / DcmFileFormat）不出 .so 边界，
#    生成结果以 byte[] 交给 Kotlin，因此两套 DCMTK 单例不会互相干扰。
#    字典需自行注入（本模块的 dcmDataDict 是独立副本），见 SrReport::initDictionary。
#
# 3) CbctVolume.h 直接引用 :cbctdeal 的头文件，跨 .so 只"读"体数据指针，
#    不分配也不释放（释放仍由 CbctJni.releaseVolume 负责），
#    结构体定义与编译参数（NDK / c++_shared / arm64）与 :cbctdeal 完全一致，
#    因此内存布局一致。这一约定写在 include/VolumeRef.h 顶部，改动需同步。
# ==========================================================================
set(CBCTDEAL_CPP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../cbctdeal/src/main/cpp)
include_directories(${CBCTDEAL_CPP_ROOT}/include)
# 再上一层也加入：core/VolumeRef.cpp 以 #include "include/CbctVolume.h" 引用体数据结构，
# 与本工程"头文件一律带 include/ 前缀"的写法保持一致（只加 include/ 目录会让该写法失效）。
include_directories(${CBCTDEAL_CPP_ROOT})

set(DCMTK_CPP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../dcmtk/src/main/cpp)
include_directories(${DCMTK_CPP_ROOT})

# 本模块所有头文件都写成 #include "include/Xxx.h"（与 :cbctdeal 的写法一致），
# 根目录下的 JNI 源文件能靠"相对当前文件目录"解析，但 core/ 下的源文件
# 相对路径会落到 core/include/，因此必须把 cpp/ 本身加入搜索路径。
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# ONNX Runtime C API 头文件：从 AAR 的 headers/ 目录 vendor 进来（仅 4 个头，
# 约 240KB；AAR 本身没有 prefab 元数据，无法由 CMake 自动发现）。
# 注意只 include 不链接：运行时经 dlopen 获得，见 ai/OrtEngine.cpp。
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include)

file(GLOB DCMTK_STATIC_LIBS
        "${DCMTK_CPP_ROOT}/lib/${ANDROID_ABI}/*.a"
        "${DCMTK_CPP_ROOT}/lib/arm64-v8/*.a"   # 兼容旧路径名（与 :cbctdeal 一致）
)

if(NOT DCMTK_STATIC_LIBS)
    message(WARNING "DCMTK static libraries not found in ${DCMTK_CPP_ROOT}/lib/")
endif()

add_library(${CMAKE_PROJECT_NAME} SHARED
        # ---- JNI 桥接层：唯一接触 JNIEnv 的地方 ----
        measure-native-lib.cpp
        MeasureJniHelper.cpp
        # ---- 业务逻辑层：纯 C++，不依赖 JNI / VTK，可主机侧编译单测 ----
        core/VolumeRef.cpp
        core/Json.cpp
        core/MeasureMath.cpp
        core/RoiExtractor.cpp
        core/ImplantPlanner.cpp
        core/AnnotationStore.cpp
        core/MeasurePicker.cpp
        core/MeasurementManager.cpp
        # ---- AI 层（PRD 5.6 / 8.6）----
        # core/AiCore.cpp 与 core/AiEngine.cpp、core/AiPlanner.cpp 是纯 C++：
        # 预处理、阈值掩膜、连通域、轮廓、统计、种植位推荐全部可在主机侧编译单测
        # （见 src/host/build_and_run.sh），真机与主机共用同一份数值路径。
        core/AiCore.cpp
        core/AiEngine.cpp
        core/AiPlanner.cpp
        # ai/OrtEngine.cpp 是全工程唯一 include ONNX Runtime 头文件、唯一 dlopen
        # libonnxruntime.so 的翻译单元（理由见 ai/OrtEngine.h 顶部：1.17.0 的 AAR
        # 没有 prefab 模块，CMake 侧 find_package 走不通）。
        ai/OrtEngine.cpp
        # ---- JNI 桥接层（AI）：只做参数搬运与 JSON 序列化，不做计算 ----
        ai-native-lib.cpp
        # ---- 数据层：DICOM SR（仅此处依赖 DCMTK）----
        core/SrReport.cpp)

# 裁包体积：函数/数据分节。
# 同样不开 --gc-sections：DCMTK 数据字典与 dcmsr 的概念代码表是惰性初始化的
# 静态数据，链接期回收会让 SR 编码查询失败（原因见 :cbctdeal 的同名注释）。
#
# -O2：NDK 的 Debug 构型里 CMAKE_CXX_FLAGS_DEBUG 是空串（见
# .cxx/Debug/*/arm64-v8a/CMakeCache.txt），等于不带 -O 即 -O0。R-02 体积盒即使
# 已有角点权重快速路径，100 万体素在 -O0 下实测 analyze 仍要 73ms，达不到
# PRD PC-01「测量计算 <=50ms」。这里只在 Debug 构型补 -O2（target 级参数排在
# CMAKE_CXX_FLAGS_DEBUG 之后，能压过默认档位），不新增也不修改 CMAKE_BUILD_TYPE，
# :cbctdeal / :rawpixeldeal 的编译参数一字未动，渲染与像素算子行为零变化。
# -ffp-contract=off：clang 默认允许把 a*b+c 合并成 FMA，-O2 下会真的合并，
# 那样 AC-02 的均值/标准差与主机侧 -O0 单测就不是逐位可比了；关掉乘加收缩，
# 保证同一份体数据在真机与主机上得到完全相同的统计量。
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
        -ffunction-sections -fdata-sections
        $<$<CONFIG:Debug>:-O2>
        $<$<CONFIG:Debug>:-ffp-contract=off>)

target_link_libraries(${CMAKE_PROJECT_NAME}
        # DCMTK 静态库（循环依赖以 --start-group 解开）
        -Wl,--start-group
        ${DCMTK_STATIC_LIBS}
        -Wl,--end-group
        log
        android
        jnigraphics
        z
        m
        atomic
        # dlopen/dlsym：ONNX Runtime 经运行时装载（Android 上 libdl 是自 API 24
        # 起就存在的空 stub，符号实际在 libc 里，写上只为语义明确与低版本兼容）。
        dl
)
