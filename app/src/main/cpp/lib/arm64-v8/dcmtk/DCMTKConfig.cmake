#
# DCMTKConfig.cmake - DCMTK CMake configuration file for external projects
#


####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was DCMTKConfig.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

####################################################################################

# Basic version information
set(DCMTK_MAJOR_VERSION 3)
set(DCMTK_MINOR_VERSION 6)
set(DCMTK_BUILD_VERSION 9)

# DCMTK libraries and modules
set(DCMTK_MODULES "ofstd;oflog;oficonv;dcmdata;dcmimgle;dcmimage;dcmjpeg;dcmjpls;dcmtls;dcmnet;dcmsr;dcmsign;dcmwlm;dcmqrdb;dcmpstat;dcmrt;dcmiod;dcmfg;dcmseg;dcmtract;dcmpmap;dcmect;dcmapps")
set(DCMTK_LIBRARIES "ofstd;oflog;oficonv;dcmdata;i2d;dcmxml;dcmimgle;dcmimage;dcmjpeg;ijg8;ijg12;ijg16;dcmjpls;dcmtkcharls;dcmtls;dcmnet;dcmsr;cmr;dcmdsig;dcmwlm;dcmqrdb;dcmpstat;dcmrt;dcmiod;dcmfg;dcmseg;dcmtract;dcmpmap;dcmect")

# Optional DCMTK 3rd party libraries
set(DCMTK_WITH_TIFF OFF)
set(DCMTK_WITH_PNG OFF)
set(DCMTK_WITH_XML OFF)
set(DCMTK_WITH_ZLIB ON)
set(DCMTK_WITH_OPENSSL OFF)
set(DCMTK_WITH_SNDFILE OFF)
set(DCMTK_WITH_ICONV OFF)
set(DCMTK_WITH_STDLIBC_ICONV ON)
set(DCMTK_WITH_WRAP OFF)
set(DCMTK_WITH_OPENJPEG OFF)
set(DCMTK_WITH_DOXYGEN OFF)

# Dictionary-related

# Define the type of standard dictionary that we want to use:
#   0 - Do not load any default dictionary on startup
#   1 - Load builtin dictionary on startup
#   2 - Load external (i.e. file-based) dictionary on startup
set(DCM_DICT_DEFAULT 2)
set(DCM_DICT_USE_DCMDICTPATH 1)
set(DCMTK_ENABLE_PRIVATE_TAGS OFF)

# Compiler / standard library features
set(DCMTK_ENABLE_STL OFF)
set(DCMTK_ENABLE_STL_ALGORITHM INFERRED)
set(DCMTK_ENABLE_STL_LIST INFERRED)
set(DCMTK_ENABLE_STL_MAP INFERRED)
set(DCMTK_ENABLE_STL_MEMORY INFERRED)
set(DCMTK_ENABLE_STL_STACK INFERRED)
set(DCMTK_ENABLE_STL_STRING INFERRED)
set(DCMTK_ENABLE_STL_SYSTEM_ERROR INFERRED)
set(DCMTK_ENABLE_STL_TUPLE INFERRED)
set(DCMTK_ENABLE_STL_TYPE_TRAITS INFERRED)
set(DCMTK_ENABLE_STL_VECTOR INFERRED)
set(DCMTK_ENABLE_STL_ATOMIC INFERRED)

set(DCMTK_FORCE_FPIC_ON_UNIX OFF)

# DCMTK documentation
set(DCMTK_GENERATE_DOXYGEN_TAGFILE OFF)

# DCMTK shared libraries and build model
set(DCMTK_OVERWRITE_WIN32_COMPILER_FLAGS OFF)
set(DCMTK_COMPILE_WIN32_MULTITHREADED_DLL OFF)
set(DCMTK_SHARED_LIBRARIES OFF)
set(DCMTK_SINGLE_SHARED_LIBRARY OFF)

# DCMTK additional options
set(DCMTK_BUILD_APPS ON)
set(DCMTK_WITH_THREADS ON)
set(DCMTK_WIDE_CHAR_FILE_IO_FUNCTIONS OFF)
set(DCMTK_WIDE_CHAR_MAIN_FUNCTION OFF)
set(DCMTK_ENABLE_LFS lfs64)
set(DCMTK_ENABLE_CHARSET_CONVERSION DCMTK_CHARSET_CONVERSION_OFICONV)


# CMake builtins
set(DCMTK_CMAKE_BUILD_TYPE Release)
set(DCMTK_CMAKE_CXX_COMPILER "C:/Users/Fall/AppData/Local/Android/Sdk/ndk/25.1.8937393/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe")
set(DCMTK_CMAKE_CXX_STANDARD "11")

set(DCMTK_CMAKE_CXX_FLAGS -g -DANDROID -fdata-sections -ffunction-sections -funwind-tables -fstack-protector-strong -no-canonical-prefixes -D_FORTIFY_SOURCE=2 -Wformat -Werror=format-security  )
set(DCMTK_CMAKE_CXX_FLAGS_DEBUG -fno-limit-debug-info  -DDEBUG)
set(DCMTK_CMAKE_CXX_FLAGS_RELEASE -O3 -DNDEBUG )
set(DCMTK_CMAKE_CXX_FLAGS_MINSIZEREL -Os -DNDEBUG)
set(DCMTK_CMAKE_CXX_FLAGS_RELWITHDEBINFO -O2 -g -DNDEBUG)

set(DCMTK_CMAKE_C_FLAGS_DEBUG -fno-limit-debug-info  -DDEBUG)
set(DCMTK_CMAKE_C_FLAGS_RELEASE -O3 -DNDEBUG )
set(DCMTK_CMAKE_C_FLAGS_MINSIZEREL -Os -DNDEBUG)
set(DCMTK_CMAKE_C_FLAGS_RELWITHDEBINFO -O2 -g -DNDEBUG)

set(DCMTK_CMAKE_EXE_LINKER_FLAGS )
set(DCMTK_CMAKE_EXE_LINKER_FLAGS_DEBUG )
set(DCMTK_CMAKE_EXE_LINKER_FLAGS_RELEASE )
set(DCMTK_CMAKE_EXE_LINKER_FLAGS_MINSIZEREL )
set(DCMTK_CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO )

set(DCMTK_CMAKE_INSTALL_BINDIR bin)
set(DCMTK_CMAKE_INSTALL_SYSCONFDIR etc/dcmtk-3.6.9)
set(DCMTK_CMAKE_INSTALL_INCLUDEDIR include)
set(DCMTK_CMAKE_INSTALL_LIBDIR lib)
set(DCMTK_CMAKE_INSTALL_DATAROOTDIR share)

set(DCMTK_CMAKE_INSTALL_PREFIX "C:/Users/Fall/Desktop/dcmtk/install-android-arm64")


SET_AND_CHECK(DCMTK_TARGETS "${PACKAGE_PREFIX_DIR}/lib/cmake/dcmtk/DCMTKTargets.cmake")

####### Expanded from @DCMTK_CONFIG_CODE@ #######
list(APPEND DCMTK_INCLUDE_DIRS "${PACKAGE_PREFIX_DIR}/include")
##################################################

# Compatibility: This variable is deprecated
set(DCMTK_INCLUDE_DIR ${DCMTK_INCLUDE_DIRS})

if(NOT DCMTK_TARGETS_IMPORTED)
  set(DCMTK_TARGETS_IMPORTED 1)
  include(${DCMTK_TARGETS})
endif()
