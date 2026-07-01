
cmake_minimum_required(VERSION 3.22.1)

project("dcmtk_native")

# ========== 基础配置：与 DCMTK 编译参数保持一致 ==========
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -frtti -fexceptions")
set(ANDROID_STL c++_static)

# ========== DCMTK 头文件路径 ==========
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# ========== 导入所有 DCMTK 静态库 ==========
# 注意：Android ABI 通常是 arm64-v8a, armeabi-v7a 等
# 这里尝试从 lib 目录下查找对应 ABI 的所有 .a 静态库
file(GLOB DCMTK_STATIC_LIBS
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/${ANDROID_ABI}/*.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/arm64-v8/*.a" # 兼容旧路径名
)

# 调试信息：如果为空则说明没找到库
if(NOT DCMTK_STATIC_LIBS)
    message(WARNING "DCMTK static libraries not found in ${CMAKE_CURRENT_SOURCE_DIR}/lib/${ANDROID_ABI}/")
endif()

add_library(${CMAKE_PROJECT_NAME} SHARED
        # List C/C++ source files with relative paths to this CMakeLists.txt.
        native-lib.cpp
        ProgressScu.cpp
        PacsClient.cpp
        DicomFileIO.cpp)


target_link_libraries(${CMAKE_PROJECT_NAME}
        # List libraries link to the target library
        # DCMTK 静态库
        # 使用 --start-group/--end-group 解决 dcmjpeg/dcmimage/dcmimgle/ijg* 之间的循环依赖
        -Wl,--start-group
        ${DCMTK_STATIC_LIBS}
        -Wl,--end-group

        # Android 系统依赖
        log
        android
        z
        m
        atomic
)