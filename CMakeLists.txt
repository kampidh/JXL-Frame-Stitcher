cmake_minimum_required(VERSION 3.16)

project(JXLFrameStitching VERSION 0.3.1 LANGUAGES CXX)

if (POLICY CMP0002)
    cmake_policy(SET CMP0002 NEW)
endif()

if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

LIST (APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

set(TOP_INST_DIR ${CMAKE_SOURCE_DIR}/i/${CMAKE_BUILD_TYPE})
set(EXTPREFIX "${TOP_INST_DIR}")
set(CMAKE_PREFIX_PATH "${EXTPREFIX}")
set(CMAKE_INSTALL_PREFIX "${EXTPREFIX}")

# find_package(LCMS2 2.13 REQUIRED)
find_package(JPEGXL 0.7.0 REQUIRED)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
        jxlutils.h
)

configure_file(jxfrstchconfig.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/jxfrstchconfig.h)

set(app_icon_resource_windows "${CMAKE_CURRENT_SOURCE_DIR}/resources/jxlframesticthing.rc")

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(JXLFrameStitching
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
        jxlutils.h
        ${app_icon_resource_windows}
        utils/jxlencoderobject.h utils/jxlencoderobject.cpp
        utils/jxldecoderobject.h utils/jxldecoderobject.cpp
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET JXLFrameStitching APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(JXLFrameStitching SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(JXLFrameStitching
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_link_libraries(JXLFrameStitching PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)

include_directories(${JPEGXL_INCLUDE_DIRS})
target_link_libraries(JXLFrameStitching PRIVATE ${JPEGXL_LIBRARIES})

# include_directories(${LCMS2_INCLUDE_DIRS})
# target_link_libraries(JXLFrameStitching PRIVATE ${LCMS2_LIBRARIES})

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.JXLFrameStitching)
endif()
set_target_properties(JXLFrameStitching PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS JXLFrameStitching
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(JXLFrameStitching)
endif()
