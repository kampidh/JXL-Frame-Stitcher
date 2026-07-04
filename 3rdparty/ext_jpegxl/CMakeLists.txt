SET(PREFIX_ext_jpegxl "${EXTPREFIX}" )

if (MINGW)
    include(CheckCXXSymbolExists)
    include(CheckCXXCompilerFlag)
    check_cxx_symbol_exists(PRIu64 "inttypes.h" CAN_BUILD_HIGHWAY)
    check_cxx_compiler_flag("-mavx512f" CAN_TARGET_AVX512F)
    if (NOT CAN_BUILD_HIGHWAY OR NOT CAN_TARGET_AVX512F)
        message(WARNING "Skipping libjxl, compiler cannot build highway.")
        return()
    endif()
endif()

ExternalProject_Add( ext_highway
    DOWNLOAD_DIR ${EXTERNALS_DOWNLOAD_DIR}
    URL https://github.com/google/highway/archive/refs/tags/1.2.0.tar.gz
    URL_HASH SHA256=7e0be78b8318e8bdbf6fa545d2ecb4c90f947df03f7aadc42c1967f019e63343

    INSTALL_DIR ${PREFIX_ext_jpegxl}
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${PREFIX_ext_jpegxl} -DCMAKE_BUILD_TYPE=${GLOBAL_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_CXX_FLAGS=${CMAKE_C_FLAGS}
        -DBUILD_TESTING=OFF
        -DBUILD_STATIC_LIBS=ON
        -DBUILD_SHARED_LIBS=OFF
        ${GLOBAL_PROFILE}

    UPDATE_COMMAND ""
)

ExternalProject_Add( ext_brotli
    DOWNLOAD_DIR ${EXTERNALS_DOWNLOAD_DIR}
    URL https://github.com/google/brotli/archive/v1.2.0.tar.gz
    URL_HASH SHA256=816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec

    INSTALL_DIR ${PREFIX_ext_jpegxl}
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${PREFIX_ext_jpegxl} -DCMAKE_BUILD_TYPE=${GLOBAL_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_CXX_FLAGS=${CMAKE_C_FLAGS}
        -DBUILD_TESTING=OFF
        -DBUILD_SHARED_LIBS=ON
        # -DBROTLI_BUILD_TOOLS=OFF
        ${GLOBAL_PROFILE}

    UPDATE_COMMAND ""
)

ExternalProject_Add( ext_jpegxl
    DOWNLOAD_DIR ${EXTERNALS_DOWNLOAD_DIR}
    URL https://github.com/libjxl/libjxl/archive/refs/tags/v0.12.0.tar.gz
    URL_HASH SHA256=03e9be69a30be4011f559da75328b6d7cea8ad921fabfbd551ce10bf45cdc992

    INSTALL_DIR ${PREFIX_ext_jpegxl}
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${PREFIX_ext_jpegxl}
        -DCMAKE_BUILD_TYPE=${GLOBAL_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_CXX_FLAGS=${CMAKE_C_FLAGS}
        -DBUILD_GMOCK=OFF
        -DINSTALL_GTEST=OFF
        -DBUILD_SHARED_LIBS=ON
        -DJPEGXL_ENABLE_DOXYGEN=OFF
        -DJPEGXL_ENABLE_MANPAGES=OFF
        -DJPEGXL_ENABLE_OPENEXR=OFF
        -DJPEGXL_ENABLE_TRANSCODE_JPEG=OFF
        -DJPEGXL_ENABLE_WASM_TRHEADS=OFF
        # -DJPEGXL_ENABLE_BOXES=OFF
        -DJPEGXL_ENABLE_BENCHMARK=OFF
        -DJPEGXL_ENABLE_COVERAGE=OFF
        -DJPEGXL_ENABLE_EXAMPLES=OFF
        -DJPEGXL_ENABLE_FUZZERS=OFF
        -DJPEGXL_ENABLE_JNI=OFF
        -DJPEGXL_ENABLE_SJPEG=OFF
        -DJPEGXL_ENABLE_SKCMS=OFF
        -DJPEGXL_ENABLE_TOOLS=OFF
        -DJPEGXL_ENABLE_VIEWERS=OFF
        -DJPEGXL_ENABLE_JPEGLI_LIBJPEG=OFF
        -DJPEGXL_ENABLE_JPEGLI=OFF
        -DSJPEG_ENABLE_SIMD=OFF
        "-DCMAKE_CXX_FLAGS:STRING=-DJXL_DEBUG_ON_ERROR -DJX_DEBUG_WARNING"
        ${GLOBAL_PROFILE}

    UPDATE_COMMAND ""
    DEPENDS ext_brotli ext_highway ext_lcms2
)
