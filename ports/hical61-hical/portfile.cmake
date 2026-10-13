vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Hical61/Hical
    REF "v${VERSION}"
    SHA512 174464e4030a4a27227a3492fe4521576d668d32ef62c33339803c04974d7304c4043de6821e565eb6b55577646812711d006691f6ffb977c6ab79bfce8018b3
    HEAD_REF main
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        database HICAL_WITH_DATABASE
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DHICAL_BUILD_TESTS=OFF
        -DHICAL_BUILD_EXAMPLES=OFF
        -DHICAL_USE_SYSTEM_PICOHTTPPARSER=ON
        ${FEATURE_OPTIONS}
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(
    PACKAGE_NAME hical
    CONFIG_PATH lib/cmake/hical
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
