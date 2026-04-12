# portfile.cmake for hical

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Hical61/Hical
    REF v1.0.1
    SHA512 0
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DHICAL_BUILD_TESTS=OFF
        -DHICAL_BUILD_EXAMPLES=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(
    PACKAGE_NAME hical
    CONFIG_PATH lib/cmake/hical
)

# 清理 debug/include（避免重复）
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

# 安装 copyright
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
