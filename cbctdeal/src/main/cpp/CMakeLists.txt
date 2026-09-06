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

add_library(${CMAKE_PROJECT_NAME} SHARED
        cbct-native-lib.cpp
        CbctSeriesParser.cpp)

target_link_libraries(${CMAKE_PROJECT_NAME}
        # DCMTK 静态库（--start-group 解决 dcmjpeg/dcmjpls/dcmimage 之间的循环依赖）
        -Wl,--start-group
        ${DCMTK_STATIC_LIBS}
        -Wl,--end-group
        # Android 系统依赖
        log
        android
        jnigraphics
        z
        m
        atomic
)
