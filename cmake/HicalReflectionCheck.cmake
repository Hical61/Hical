# 编译一小段用到反射和注解值的 C++ 代码。反射选项只加给这次检查。
function(hical_check_native_reflection)
    # GNU 的 cxx_std_26 从 CMake 3.30 才开始列出来。更早的版本会把它当成「编译器不支持」，
    # 报出来的错会让人去查编译器，方向完全错了。这里先按版本给出准确的诊断。
    if(CMAKE_VERSION VERSION_LESS 3.30 AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
        message(FATAL_ERROR
            "HICAL_ENABLE_REFLECTION=ON 需要 CMake >= 3.30，GNU 工具链的 cxx_std_26 从 3.30 才有。"
            "当前用 CMake ${CMAKE_VERSION} 配 ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}。"
            "请升级 CMake，或设置 HICAL_ENABLE_REFLECTION=OFF。")
    endif()
    if(NOT "cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
        message(FATAL_ERROR
            "HICAL_ENABLE_REFLECTION=ON 需要能配置 C++26 的 CMake 和编译器。"
            "当前使用 CMake ${CMAKE_VERSION} 和 ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}。")
    endif()

    # 只在本函数里把探测目标设为静态库，外部检查和 C 编译器检查继续用原来的设置。
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    message(STATUS "检查 C++26 原生反射与注解值支持")
    try_compile(hicalReflectionSupported
        SOURCE_FROM_CONTENT hical_reflection_probe.cpp [=[
#include <version>

#if !defined(__cpp_impl_reflection) || __cpp_impl_reflection < 202506L
#error "__cpp_impl_reflection 必须至少为 202506L"
#endif
#if !defined(__cpp_lib_reflection) || __cpp_lib_reflection < 202506L
#error "__cpp_lib_reflection 必须至少为 202506L"
#endif

#include <meta>

struct ReflectionTag
{
    int value;
};

struct ReflectionProbe
{
    [[= ReflectionTag {7}]] int value = 7;
};

constexpr auto kMembers = std::define_static_array(std::meta::nonstatic_data_members_of(
    ^^ReflectionProbe, std::meta::access_context::unprivileged()));
static_assert(kMembers.size() == 1);
constexpr auto kAnnotation = std::meta::annotations_of(kMembers[0])[0];
static_assert([:std::meta::constant_of(kAnnotation):].value == 7);
static_assert([]
{
    ReflectionProbe probe;
    template for (constexpr auto member : kMembers)
    {
        probe.[:member:] += 1;
    }
    return probe.value;
}() == 8);
]=]
        NO_CACHE
        LOG_DESCRIPTION "检查 Hical 能否使用 C++26 反射、注解值和静态成员访问"
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        COMPILE_DEFINITIONS -freflection
        OUTPUT_VARIABLE hicalReflectionOutput)

    file(WRITE "${CMAKE_BINARY_DIR}/CMakeFiles/HicalReflectionCheck.log" "${hicalReflectionOutput}")
    if(NOT hicalReflectionSupported)
        message(FATAL_ERROR
            "HICAL_ENABLE_REFLECTION=ON 要求 __cpp_impl_reflection 和 __cpp_lib_reflection 都 >=202506L。"
            "编译器还要支持反射、注解值和静态成员访问，例如 GCC 16 加 -freflection。"
            "当前的 ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} 没有通过编译检查。"
            "请换用支持这些功能的工具链，或设置 HICAL_ENABLE_REFLECTION=OFF。"
            "探测日志：${CMAKE_BINARY_DIR}/CMakeFiles/HicalReflectionCheck.log")
    endif()
    message(STATUS "C++26 原生反射与注解值编译检查通过")
endfunction()
