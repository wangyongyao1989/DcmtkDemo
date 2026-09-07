#!/bin/bash
# Android 平台 VTK 9.1.0 交叉编译脚本（CBCT 三维可视化专用裁剪版）
# 依据《Android平台VTK完整集成流程（适配CBCT三维可视化）》PDF 2.2 节定制，
# NDK 路径提取自本机 android_x264.sh（tag1：NDK 25.1.8937393）。
#
# 相对 PDF 原脚本的修正（PDF 原文部分选项为臆造，VTK 9.1.0 中不存在）：
#  - VTK_MODULE_ENABLE_VTK_Common   -> CommonCore（9.1.0 中无 "Common" 模块名）
#  - VTK_MODULE_ENABLE_VTK_MPR      -> 移除（9.1.0 无此模块；MPR 由 ImagingCore 的
#                                      vtkImageReslice 实现，另启用 RenderingImage
#                                      提供 vtkImageActor 切面显示）
#  - VTK_USE_QT=OFF                 -> 移除（9.1.0 无此开关；Qt 为模块制 GUISupportQt，
#                                      默认不启用，交叉编译 Android 不会拉入）
#  - VTK_USE_OPENGL_ES=ON           -> 移除（Android 下 VTK_OPENGL_USE_GLES 由
#                                      CMake/vtkOpenGLOptions.cmake 自动强制开启；
#                                      EGL 渲染窗口 vtkEGLRenderWindow 默认注册，
#                                      用于对接 Android Surface）
#  - VTK_USE_X=OFF                  -> 保留（真实存在，且 Android 下默认即 OFF）
#  - VTK_MODULE_ENABLE_VTK_IOImage=NO 保留（DCMTK 负责解析，VTK 无需图像 IO；
#                                      该模块仅出现在测试依赖中，关闭安全）
#  - VTK_GROUP_ENABLE_StandAlone/Rendering=DONT_WANT（9.1.0 顶层 CMakeLists
#                                      默认将这两组设为 WANT，导致全量构建；
#                                      改为 DONT_WANT 后仅编译显式 =YES 的模块
#                                      及其依赖，实现 PDF 要求的裁剪。组变量名
#                                      无 VTK_ 前缀，见 CMake/vtkModule.cmake）
#  - VTK_ENABLE_WRAPPING=OFF（交叉编译时 wrapping 需要宿主机 VTKCompileTools，
#                             Android 静态库无需 Java/Python 封装）
#  - VTK_MODULE_ENABLE_VTK_VolumeRendering -> RenderingVolume + RenderingVolumeOpenGL2
#                                      （9.1.0 中不存在 VTK::VolumeRendering 模块，
#                                      那是 9.2+ 的命名；9.1.0 体渲染拆为两个模块：
#                                      Rendering/Volume 含 vtkGPUVolumeRayCastMapper、
#                                      vtkFixedPointVolumeRayCastMapper（CPU 光线投射），
#                                      Rendering/VolumeOpenGL2 含 vtkSmartVolumeMapper、
#                                      vtkOpenGLGPUVolumeRayCastMapper。另注：PDF 4.2
#                                      伪代码中的 vtkVolumeRayCastMapper 为 VTK 8.x 类，
#                                      9.1.0 已移除，用上述两个 Mapper 替代）
set -e

# ---------- NDK 环境----------
export ANDROID_NDK=/Users/wangyao/Library/Android/sdk/ndk/25.1.8937393
export TOOLCHAIN=$ANDROID_NDK/build/cmake/android.toolchain.cmake
export ANDROID_ABI=arm64-v8a        # 仅编译 arm64-v8a（PDF 2.1：舍弃 32 位，精简体积）
export ANDROID_PLATFORM=android-24  # Android 7.0 (API 24) 及以上

# ---------- 目录 ----------
SRC_DIR="$(cd "$(dirname "$0")" && pwd)/vtk"
BUILD_DIR="$SRC_DIR/build-android-vtk"
INSTALL_PREFIX="$BUILD_DIR/install"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# ---------- 配置 ----------
cmake "$SRC_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
  -DANDROID_ABI="${ANDROID_ABI}" \
  -DANDROID_PLATFORM="${ANDROID_PLATFORM}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DVTK_BUILD_TESTING=OFF \
  -DVTK_BUILD_EXAMPLES=OFF \
  -DVTK_BUILD_DOCUMENTATION=OFF \
  -DVTK_ENABLE_REMOTE_MODULES=OFF \
  -DVTK_BUILD_ALL_MODULES=OFF \
  -DVTK_USE_X=OFF \
  -DVTK_ENABLE_WRAPPING=OFF \
  -DVTK_GROUP_ENABLE_StandAlone=DONT_WANT \
  -DVTK_GROUP_ENABLE_Rendering=DONT_WANT \
  -DCMAKE_C_FLAGS_RELEASE="-Os -DNDEBUG -g0" \
  -DCMAKE_CXX_FLAGS_RELEASE="-Os -DNDEBUG -g0" \
  -DCMAKE_CXX_STANDARD=11 \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
  -DVTK_MODULE_ENABLE_VTK_CommonCore=YES \
  -DVTK_MODULE_ENABLE_VTK_ImagingCore=YES \
  -DVTK_MODULE_ENABLE_VTK_ImagingGeneral=YES \
  -DVTK_MODULE_ENABLE_VTK_RenderingCore=YES \
  -DVTK_MODULE_ENABLE_VTK_RenderingOpenGL2=YES \
  -DVTK_MODULE_ENABLE_VTK_RenderingImage=YES \
  -DVTK_MODULE_ENABLE_VTK_RenderingVolume=YES \
  -DVTK_MODULE_ENABLE_VTK_RenderingVolumeOpenGL2=YES \
  -DVTK_MODULE_ENABLE_VTK_InteractionStyle=YES \
  -DVTK_MODULE_ENABLE_VTK_FiltersGeneric=NO \
  -DVTK_MODULE_ENABLE_VTK_IOImage=NO

# ---------- 编译并安装 ----------
# arm64-v8a 指令集基线即包含 NEON（PDF 6.2 的 NEON 加速随 ABI 自动生效）
# 注 1：本机 8GB 内存，-j8 并行 clang 会因内存压力偶发 Abort trap: 6，降为 -j4
# 注 2：NDK 工具链在 Release 下仍强制 -g，配合 -g0 去除调试信息，
#        缩小目标文件体积（兼顾客盘空间紧张与 PDF 6.1 的符号裁剪要求）
make -j4
make install

echo ""
echo "==== VTK Android 交叉编译完成 ===="
echo "头文件目录: $INSTALL_PREFIX/include"
echo "静态库目录: $INSTALL_PREFIX/lib"
