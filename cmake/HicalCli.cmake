# 入口从 bin 找 share 里的脚本，整个安装目录可以一起搬走。
set(HICAL_CLI_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../scripts")
set(HICAL_CLI_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/hical-cli")

if(WIN32)
    # 直接启动 Python，避免 cmd.exe 把项目路径里的 &、% 和 ^ 当成批处理语法。
    # 入口只用 C 和 Win32 API，省掉框架库和 C++ 运行库依赖。
    add_executable(hical_cli_launcher "${HICAL_CLI_SOURCE_DIR}/cli/hical_windows.c")
    set_target_properties(hical_cli_launcher PROPERTIES
        OUTPUT_NAME hical
        RUNTIME_OUTPUT_DIRECTORY "${HICAL_CLI_BINARY_DIR}"
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    if(MSVC)
        target_compile_options(hical_cli_launcher PRIVATE /utf-8)
    elseif(MINGW)
        target_link_options(hical_cli_launcher PRIVATE -municode -static-libgcc)
    endif()
    install(TARGETS hical_cli_launcher RUNTIME DESTINATION bin)
else()
    configure_file(
        "${HICAL_CLI_SOURCE_DIR}/cli/hical.in"
        "${HICAL_CLI_BINARY_DIR}/hical"
        @ONLY NEWLINE_STYLE UNIX
    )
    install(PROGRAMS "${HICAL_CLI_BINARY_DIR}/hical" DESTINATION bin)
endif()
install(FILES "${HICAL_CLI_SOURCE_DIR}/hical.py" DESTINATION share/hical/cli)
install(
    DIRECTORY "${HICAL_CLI_SOURCE_DIR}/templates/minimal"
    DESTINATION share/hical/cli/templates
    FILES_MATCHING PATTERN "*.in"
)
