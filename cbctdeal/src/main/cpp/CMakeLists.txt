cmake_minimum_required(VERSION 3.22.1)

project("cbct_native")

# ========== 基础配置：与 dcmtk 模块保持一致 ==========
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -frtti -fexceptions")
set(ANDROID_STL c++_shared)

# ========== DCMTK 头文件与静态库 ==========
# 模块解耦说明：cbctdeal 与 dcmtk 模块在 Kotlin 层零依赖，
# 仅在构建层共享 dcmtk 模块下的 DCMTK 预编译静态库与头文件，
# 各自链接进自己的 so（cbct_native.so），运行时互不影响。
set(DCMTK_CPP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../dcmtk/src/main/cpp)
include_directories(${DCMTK_CPP_ROOT})

file(GLOB DCMTK_STATIC_LIBS
        "${DCMTK_CPP_ROOT}/lib/${ANDROID_ABI}/*.a"
        "${DCMTK_CPP_ROOT}/lib/arm64-v8/*.a"   # 兼容旧路径名
)

if(NOT DCMTK_STATIC_LIBS)
    message(WARNING "DCMTK static libraries not found in ${DCMTK_CPP_ROOT}/lib/${ANDROID_ABI}/")
endif()

# ========== VTK 9.1.0 头文件与静态库（Android arm64-v8a 交叉编译产物） ==========
# 产物来源：doc/android_vtk.sh（NDK r25 / android-24 / OpenGL ES / 裁剪版）。
# include/vtk-9.1 平铺头文件目录 + lib/libvtk*.a 全量静态库。
set(VTK_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${VTK_ROOT}/include/vtk-9.1)

# 全量收集 VTK 模块库与第三方库（均带 libvtk 前缀），
# --start-group/--end-group 让链接器自动解静态库间循环依赖（VTK 模块间相互引用）。
file(GLOB VTK_STATIC_LIBS "${VTK_ROOT}/lib/libvtk*.a")

if(NOT VTK_STATIC_LIBS)
    message(WARNING "VTK static libraries not found in ${VTK_ROOT}/lib/")
endif()

add_library(${CMAKE_PROJECT_NAME} SHARED
        cbct-native-lib.cpp
        CbctSeriesParser.cpp
        CbctVtkRenderer.cpp)

# 裁包体积（PDF 6.1）：函数/数据分节 + 链接期未引用段回收。
# 注意：暂不启用 --gc-sections —— DCMTK 内置数据字典等静态数据表
# 仅被惰性初始化路径引用，链接期回收会导致 JPEG 解压时
# "Tag not found in data dictionary"（VR 查询失败）。
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
        -ffunction-sections -fdata-sections)

target_link_libraries(${CMAKE_PROJECT_NAME}
        # DCMTK 静态库（--start-group 解决 dcmjpeg/dcmjpls/dcmimage 之间的循环依赖）
        -Wl,--start-group
        ${DCMTK_STATIC_LIBS}
        -Wl,--end-group
        # VTK 静态库（模块间循环依赖同样以分组解决）
        -Wl,--start-group
        ${VTK_STATIC_LIBS}
        -Wl,--end-group
        # Android 系统依赖（EGL/GLES：VTK OpenGL ES 渲染窗口所需）
        log
        android
        jnigraphics
        EGL
        GLESv2
        GLESv3
        z
        m
        atomic
)
