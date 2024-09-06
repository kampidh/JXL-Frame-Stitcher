SET(PREFIX_ext_lcms2 "${EXTPREFIX}" )

SET(lcms2_args
    -Dfastfloat=false
    -Dutils=false
)

ExternalProject_Add( ext_lcms2
    DOWNLOAD_DIR ${EXTERNALS_DOWNLOAD_DIR}
    URL https://github.com/mm2/Little-CMS/releases/download/lcms2.16/lcms2-2.16.tar.gz
    URL_HASH SHA256=d873d34ad8b9b4cea010631f1a6228d2087475e4dc5e763eb81acc23d9d45a51

    INSTALL_DIR ${PREFIX_ext_lcms2}

    CONFIGURE_COMMAND
        ${Meson_EXECUTABLE} setup ${lcms2_args} --prefix=<INSTALL_DIR> <BINARY_DIR> <SOURCE_DIR>
    BUILD_COMMAND
        ${Ninja_EXECUTABLE} -C <BINARY_DIR>
    INSTALL_COMMAND
        ${Ninja_EXECUTABLE} -C <BINARY_DIR> install

    UPDATE_COMMAND ""
)

