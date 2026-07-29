cmake_minimum_required(VERSION 3.22.1)

project("rawpixeldeal_native")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -frtti -fexceptions")
set(ANDROID_STL c++_shared)

set(OPENCV_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(OPENCV_SO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI}")
set(OPENCV_SO_PATH "${OPENCV_SO_DIR}/libopencv_java4.so")

add_library(opencv_java4 SHARED IMPORTED)
set_target_properties(opencv_java4 PROPERTIES
        IMPORTED_LOCATION "${OPENCV_SO_PATH}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENCV_INCLUDE_DIR}"
)

set(OPENCV_LINK_LIBS android log z m jnigraphics)

add_library(${CMAKE_PROJECT_NAME} SHARED
        native-lib.cpp
        MedicalCTPreprocess.cpp
        JniHelper.cpp
        CtSeriesProcessor.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        ${OPENCV_INCLUDE_DIR}
)

target_link_libraries(${CMAKE_PROJECT_NAME}
        opencv_java4
        ${OPENCV_LINK_LIBS}
)
