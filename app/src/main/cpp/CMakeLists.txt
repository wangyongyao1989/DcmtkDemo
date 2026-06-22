
cmake_minimum_required(VERSION 3.22.1)

project("dcmtkdemo")

# ========== 基础配置：与 DCMTK 编译参数保持一致 ==========
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -frtti -fexceptions")
set(ANDROID_STL c++_static)

# ========== DCMTK 头文件路径 ==========
include_directories(${CMAKE_SOURCE_DIR}/include)

# ========== 导入所有 DCMTK 静态库 ==========
# 注意：你的文件夹名是 arm64-v8，但 Android ABI 名通常是 arm64-v8a
if(${ANDROID_ABI} STREQUAL "arm64-v8a")
    set(DCMTK_ABI_DIR "arm64-v8")
else()
    set(DCMTK_ABI_DIR ${ANDROID_ABI})
endif()

file(GLOB DCMTK_STATIC_LIBS
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/${DCMTK_ABI_DIR}/*.a"
)

add_library(${CMAKE_PROJECT_NAME} SHARED
        # List C/C++ source files with relative paths to this CMakeLists.txt.
        native-lib.cpp)


target_link_libraries(${CMAKE_PROJECT_NAME}
        # List libraries link to the target library
        # DCMTK 静态库
        ${DCMTK_STATIC_LIBS}

        # Android 系统依赖
        log
        android
        z
        m
        atomic
)