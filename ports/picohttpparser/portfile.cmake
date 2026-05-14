vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO h2o/picohttpparser
    REF 066d2b1e9ab820703db0837571f3b6f353f3d0f4
    SHA512 f3781cbb4e9e190df38c3fe7fa80ba69bf6f9dbafb158e0426dd4604f2f1ba794450679005a38d0f9f1dad0696e2f22b8b086b2d7d08a0f99bb4fd3b0f7ed5d8
    HEAD_REF master
)

# 上游没有 CMakeLists.txt，使用 overlay
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME picohttpparser CONFIG_PATH lib/cmake/picohttpparser)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
