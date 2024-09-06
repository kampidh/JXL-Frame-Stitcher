#
# Some 3rd party dependencies are borrowed
# from Krita deps build
#
# JPEG XL build is kinda broken on Qt's MingW toolkit
# So I build it separately with gcc llvm 15
#

cmake_minimum_required(VERSION 3.16.0)

project(jxlframestitching-deps)

if (POLICY CMP0002)
    cmake_policy(SET CMP0002 NEW)
endif()

if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(TOP_INST_DIR ${CMAKE_SOURCE_DIR}/../i/${CMAKE_BUILD_TYPE})
set(EXTPREFIX "${TOP_INST_DIR}")
set(CMAKE_PREFIX_PATH "${EXTPREFIX}")

find_program(Meson_EXECUTABLE meson)
if(NOT Meson_EXECUTABLE)
    message(FATAL_ERROR "Meson required")
endif()

find_program(Ninja_EXECUTABLE ninja)
if(NOT Ninja_EXECUTABLE)
    message(FATAL_ERROR "Ninja required")
endif()

include (ExternalProject)

message( STATUS "CMAKE_GENERATOR: ${CMAKE_GENERATOR}")
message( STATUS "CMAKE_CL_64: ${CMAKE_CL_64}")
message("CMAKE BUILD TYPE: ${CMAKE_BUILD_TYPE}")

set(GLOBAL_BUILD_TYPE ${CMAKE_BUILD_TYPE})
set(GLOBAL_PROFILE ${GLOBAL_PROFILE} -DBUILD_TESTING=false)

add_subdirectory(ext_lcms2)
add_subdirectory(ext_jpegxl)
