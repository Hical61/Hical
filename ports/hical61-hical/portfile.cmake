vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Hical61/Hical
    REF "v${VERSION}"
    SHA512 e31f1de4e66a4cb055f764e05d846182d8cfac8bc1310534ce1062ad427e4c924bd98f392c2175473f5a5b91e2bd8586bbd647e55506a672dc6c3f579de9332b
    HEAD_REF main
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        database HICAL_WITH_DATABASE
        http-client HICAL_WITH_HTTP_CLIENT
        http2 HICAL_WITH_HTTP2
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
