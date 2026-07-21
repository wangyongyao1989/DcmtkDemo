
cmake_minimum_required(VERSION 3.22.1)

project("rawpixeldeal_native")

# ========== 基础配置 ==========
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -frtti -fexceptions")
set(ANDROID_STL c++_shared)

# ========== OpenCV 头文件路径 ==========
# include 目录位于 rawpixeldeal/src/main/cpp/include
set(OPENCV_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# ========== 导入 OpenCV 预编译动态库 ==========
# Android 交叉编译后的 libopencv_java4.so 位于 libs/<ABI>/ 下
set(OPENCV_SO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}")
set(OPENCV_SO_PATH "${OPENCV_SO_DIR}/libopencv_java4.so")

if(NOT EXISTS "${OPENCV_SO_PATH}")
    message(WARNING "OpenCV shared library not found at ${OPENCV_SO_PATH}")
endif()

add_library(opencv_java4 SHARED IMPORTED)
set_target_properties(opencv_java4 PROPERTIES
        IMPORTED_LOCATION "${OPENCV_SO_PATH}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENCV_INCLUDE_DIR}"
)

# ========== 链接参数：解决 OpenCV 在 Android 上常见的 liblog / libjnigraphics 依赖 ==========
set(OPENCV_LINK_LIBS
        android
        log
        z
        m
        jnigraphics
)

# ========== 业务动态库：rawpixeldeal_native ==========
add_library(${CMAKE_PROJECT_NAME} SHARED
        native-lib.cpp
        MedicalCTPreprocess.cpp
        JniHelper.cpp
        CtSeriesProcessor.cpp
        XrayProcessor.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        ${OPENCV_INCLUDE_DIR}
)

target_link_libraries(${CMAKE_PROJECT_NAME}
        opencv_java4
        ${OPENCV_LINK_LIBS}
)
