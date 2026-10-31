# 统一上游/Conan 的配置目标与系统头文件、库；仅在显式启用 HTTP/2 时加载。
include(FindPackageHandleStandardArgs)

find_package(nghttp2 1.70...<2 CONFIG QUIET HINTS ${Nghttp2_ROOT} ${NGHTTP2_ROOT})
if(NOT nghttp2_FOUND)
    find_package(libnghttp2 1.70...<2 CONFIG QUIET HINTS ${Nghttp2_ROOT} ${NGHTTP2_ROOT})
endif()
set(_nghttp2_target "")
foreach(_candidate nghttp2::nghttp2_static nghttp2::nghttp2 libnghttp2::nghttp2 libnghttp2::libnghttp2)
    if(TARGET ${_candidate})
        set(_nghttp2_target ${_candidate})
        break()
    endif()
endforeach()

if(_nghttp2_target)
    get_target_property(_nghttp2_includes ${_nghttp2_target} INTERFACE_INCLUDE_DIRECTORIES)
endif()
find_path(Nghttp2_INCLUDE_DIR NAMES nghttp2/nghttp2ver.h
    HINTS ${_nghttp2_includes} ${Nghttp2_ROOT} ${NGHTTP2_ROOT}
    PATH_SUFFIXES include)
if(Nghttp2_INCLUDE_DIR)
    file(STRINGS "${Nghttp2_INCLUDE_DIR}/nghttp2/nghttp2ver.h" _nghttp2_version_line
        REGEX "^#define NGHTTP2_VERSION \"[0-9]+\\.[0-9]+\\.[0-9]+\"")
    string(REGEX REPLACE ".*\"([0-9]+\\.[0-9]+\\.[0-9]+)\".*" "\\1"
        Nghttp2_VERSION "${_nghttp2_version_line}")
endif()

if(_nghttp2_target)
    # 配置目标的路径由其自身维护，不把系统库混入包管理器依赖图。
    set(Nghttp2_LIBRARY "${_nghttp2_target}")
    if(DEFINED nghttp2_VERSION)
        set(Nghttp2_VERSION "${nghttp2_VERSION}")
    elseif(DEFINED libnghttp2_VERSION)
        set(Nghttp2_VERSION "${libnghttp2_VERSION}")
    endif()
    set(_nghttp2_required Nghttp2_LIBRARY)
else()
    get_filename_component(_nghttp2_prefix "${Nghttp2_INCLUDE_DIR}" DIRECTORY)
    # 头文件前缀已是目标安装路径，避免 vcpkg 用 Debug 根目录再次重映射。
    find_library(Nghttp2_LIBRARY_RELEASE NAMES nghttp2 nghttp2_static
        HINTS "${_nghttp2_prefix}/lib" "${Nghttp2_ROOT}/lib" "${NGHTTP2_ROOT}/lib"
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(Nghttp2_LIBRARY_RELEASE NAMES nghttp2 nghttp2_static
        HINTS ${Nghttp2_ROOT} ${NGHTTP2_ROOT} PATH_SUFFIXES lib)
    find_library(Nghttp2_LIBRARY_DEBUG NAMES nghttp2 nghttp2_static nghttp2d nghttp2_staticd
        HINTS "${_nghttp2_prefix}/debug/lib" "${Nghttp2_ROOT}/debug/lib" "${NGHTTP2_ROOT}/debug/lib"
        NO_DEFAULT_PATH)
    include(SelectLibraryConfigurations)
    select_library_configurations(Nghttp2)
    set(_nghttp2_required Nghttp2_LIBRARY Nghttp2_INCLUDE_DIR)
endif()

find_package_handle_standard_args(Nghttp2
    REQUIRED_VARS ${_nghttp2_required}
    VERSION_VAR Nghttp2_VERSION HANDLE_VERSION_RANGE)

if(Nghttp2_FOUND AND NOT TARGET Nghttp2::Nghttp2)
    add_library(Nghttp2::Nghttp2 INTERFACE IMPORTED)
    if(_nghttp2_target)
        set_property(TARGET Nghttp2::Nghttp2 PROPERTY INTERFACE_LINK_LIBRARIES "${_nghttp2_target}")
    else()
        target_link_libraries(Nghttp2::Nghttp2 INTERFACE ${Nghttp2_LIBRARY})
        set_property(TARGET Nghttp2::Nghttp2 PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${Nghttp2_INCLUDE_DIR}")
        # MSYS2 的静态归档与导入库后缀不同，MSVC 静态包可显式设置该变量。
        if(WIN32 AND (Nghttp2_USE_STATIC_LIBS OR Nghttp2_LIBRARY MATCHES "(_static\\.lib|libnghttp2\\.a)$"))
            set_property(TARGET Nghttp2::Nghttp2 PROPERTY INTERFACE_COMPILE_DEFINITIONS NGHTTP2_STATICLIB)
        endif()
    endif()
    if(MSVC)
        # MSVC 没有 ssize_t；Codec 使用 nghttp2_ssize 和 *2 接口，不需要旧声明。
        set_property(TARGET Nghttp2::Nghttp2 APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS NGHTTP2_NO_SSIZE_T)
    endif()
endif()

mark_as_advanced(Nghttp2_INCLUDE_DIR Nghttp2_LIBRARY_RELEASE Nghttp2_LIBRARY_DEBUG)
unset(_candidate)
unset(_nghttp2_target)
unset(_nghttp2_includes)
unset(_nghttp2_version_line)
unset(_nghttp2_prefix)
unset(_nghttp2_required)
