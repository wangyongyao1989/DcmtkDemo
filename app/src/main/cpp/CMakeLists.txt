
cmake_minimum_required(VERSION 3.22.1)

project("dcmtkdemo")

# ========== 基础配置：与 DCMTK 编译参数保持一致 ==========
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -frtti -fexceptions")
set(ANDROID_STL c++_static)

# ========== DCMTK 头文件路径 ==========
include_directories(${CMAKE_SOURCE_DIR}/include)

# ========== 导入所有 DCMTK 静态库 ==========
# 基础工具库
add_library(lib_ofstd STATIC IMPORTED)
set_target_properties(lib_ofstd PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libofstd.a)

add_library(lib_oflog STATIC IMPORTED)
set_target_properties(lib_oflog PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/liboflog.a)

add_library(lib_oficonv STATIC IMPORTED)
set_target_properties(lib_oficonv PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/liboficonv.a)

# DICOM 核心数据解析
add_library(lib_dcmdata STATIC IMPORTED)
set_target_properties(lib_dcmdata PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libdcmdata.a)

# 图像处理基础
add_library(lib_dcmimgle STATIC IMPORTED)
set_target_properties(lib_dcmimgle PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libdcmimgle.a)

# JPEG 编解码 + IJG 库
add_library(lib_dcmjpeg STATIC IMPORTED)
set_target_properties(lib_dcmjpeg PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libdcmjpeg.a)

add_library(lib_ijg8 STATIC IMPORTED)
set_target_properties(lib_ijg8 PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libijg8.a)

add_library(lib_ijg12 STATIC IMPORTED)
set_target_properties(lib_ijg12 PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libijg12.a)

add_library(lib_ijg16 STATIC IMPORTED)
set_target_properties(lib_ijg16 PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libijg16.a)

# 放疗模块（不需要可以删掉）
add_library(lib_dcmrt STATIC IMPORTED)
set_target_properties(lib_dcmrt PROPERTIES IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libdcmrt.a)


add_library(${CMAKE_PROJECT_NAME} SHARED
        # List C/C++ source files with relative paths to this CMakeLists.txt.
        native-lib.cpp)


target_link_libraries(${CMAKE_PROJECT_NAME}
        # List libraries link to the target library
        # 高层模块
        lib_dcmrt
        lib_dcmjpeg
        lib_dcmimgle
        lib_dcmdata

        # 底层基础库
        lib_oflog
        lib_ofstd
        lib_oficonv

        # JPEG 底层依赖
        lib_ijg16
        lib_ijg12
        lib_ijg8

        # Android 系统依赖
        log
        android
        z
        m
        atomic
)