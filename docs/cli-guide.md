# CLI 脚手架

用 `hical new` 可以建一个能直接编译、测试和运行的 C++20 HTTP 工程。生成器用 Python 3.10 或更新版本的标准库。生成之后，构建和测试需要 CMake 3.28+、C++ 工具链和已安装的 Hical 2.7.2 或同主版本的更新版本，无需 Python 或 GTest。

## 从源码调用

在 Hical 仓库根目录运行这条命令。

```sh
python scripts/hical.py new demo-api --directory /path/to/projects
```

在 Linux 或 macOS 上也可以用 `python3`。`--directory` 指向已存在的父目录，省略就用当前目录。从别的目录运行时，写脚本的绝对路径即可。这一步只生成文件，构建和初始化 Git 留给你自己操作。

项目名长 1～64 个字符，以英文字母开头，后面可以用英文字母、数字、下划线和连字符。`CON` 等 Windows 设备名在所有平台都不能用。父目录可以带中文和空格。

目标路径得空出来。即使那里是空目录、文件或链接，生成器也会报错。中途写入失败时，它只清理这次创建的文件和空目录。如果目录里还有别的文件，会保留它们并提示到哪里查看。

## 安装 `hical` 命令

在现有 Hical 构建目录里打开 `HICAL_INSTALL_CLI`，再编译和安装。

```sh
cmake -S . -B build -DHICAL_INSTALL_CLI=ON
cmake --build build --config Release
cmake --install build --config Release --prefix /path/to/hical-prefix
```

如果是第一次配置，按你的环境补上生成器、编译器、依赖前缀或包管理器工具链。`HICAL_INSTALL_CLI` 默认关闭，vcpkg 和 Conan 包也沿用默认安装内容。配置时不用找 Python，运行生成器时才需要它。

安装完，把 `<安装前缀>/bin` 加进当前终端的 PATH，就可以直接用 `hical`。

```sh
hical --help
hical new demo-api --directory /path/to/projects
```

POSIX 入口从 PATH 找 `python3`。Windows 的 `hical.exe` 从 PATH 找 `python.exe`，直接传 Unicode 参数。它用现有的 C 工具链编译，只调用 Windows 系统 API。在 PowerShell 里也可以用完整路径调用。

```powershell
& 'C:\hical-prefix\bin\hical.exe' new demo-api --directory 'F:\我的项目\R&D'
```

脚本和模板都在 `share/hical/cli/`。要搬目录，整个安装前缀一起搬，保留 `bin` 和 `share` 的相对位置，并确保链接时还能找到 Hical 的依赖。

Windows 现在安装的是 `hical.exe`。它把收到的参数直接交给 Python，避免目录里的 `&`、`%` 等字符再被批处理解释一遍。如果沿用旧安装目录，留下的 `hical.cmd` 可能按 `PATHEXT` 顺序先被找到。遇到这种情况，可以直接调用 `hical.exe`，或者删掉旧的 `hical.cmd`。

提示沿用终端或管道当前的编码。显示不了的路径字符会写成反斜杠转义，工程文件照常用 UTF-8，退出码也照常返回。

`--help` 返回 `0`，参数有误或子命令不支持时返回 `2`，模板、路径及 I/O 出错时返回 `1`。目前只有 `new`，`gen`、`dev`、热重载、依赖下载和合并已有工程都还没加。

## 构建和运行生成的工程

```text
demo-api/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── src/
│   ├── app.h
│   ├── app.cpp
│   └── main.cpp
└── tests/
    └── test_app.cpp
```

进入生成的目录，用 Hical 安装包配置、编译并运行测试。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/hical-prefix
cmake --build build --config Release
ctest --test-dir build -C Release -N
ctest --test-dir build -C Release --no-tests=error --output-on-failure
```

模板用 `find_package(hical 2.7.2 CONFIG REQUIRED)` 找包，并链接 `hical::hical_core`。这个目标会带上依赖和 ABI 相关的编译定义。请使用和 Hical 安装包兼容的工具链，别混用 MinGW 与 MSVC 的库，也别另设冲突的 Asio 线程宏。如果 CMake 找不到包，检查 `CMAKE_PREFIX_PATH` 是否指向实际安装前缀，或把 `hical_DIR` 指到安装后的 `lib/cmake/hical`。其他接入方式见 [集成指南](integration_guide.md)。

目录名里的连字符在 CMake 目标名里会换成下划线，所以 `demo-api` 的服务目标叫 `demo_api_server`。单配置构建后运行 `./build/demo_api_server`，Windows 上是 `build\demo_api_server.exe`。Visual Studio 多配置构建的 Release 程序在 `build\Release\demo_api_server.exe`。

服务监听 `127.0.0.1:8080`。访问 `GET /` 会收到 `200` 和 `Hical 已启动`，正文末尾有换行。按 Ctrl+C 即可退出。如果端口被占用，改一下 `src/main.cpp` 里的 `kDefaultPort`，然后重新编译。

服务和测试用的是同一套路由。CTest 用例直接调用同步分发，无需启动服务，Release 构建也会检查状态码和完整响应。只想编译服务时，可以设 `-DBUILD_TESTING=OFF`。

本地验证时遇到过 MinGW 链接器读不到中文安装路径下的静态库。如果碰到同样的问题，把安装前缀和编译目录换成 ASCII 路径即可，路径里可以有空格。生成器可以正常写入中文目录。

## 开发者验证

先用 Python 自带的 `unittest` 跑生成器测试。

```sh
python -m unittest discover -s scripts -p test_hical_cli.py -v
```

要单独检查 Windows 入口的 PowerShell 和 cmd 参数传递，可以传入已安装的 `hical.exe`。下一步的完整检查也会在 Windows 上跑这组测试。

```powershell
python scripts/test_hical_cli_windows.py --launcher 'C:\hical-prefix\bin\hical.exe'
```

Hical 打开 CLI 安装并编译好后，可以检查安装、搬移以及生成的工程能否独立构建。`--work-dir` 要指定一个还不存在的目录。失败时，日志和工程会留在里面，方便排查。

```sh
python scripts/check_hical_cli.py --hical-build-dir build --work-dir build-cli-check --config Release
```

传给 CMake 的生成器、编译器或工具链参数放在 `--` 后面。下面是 POSIX shell 的写法。

```sh
python scripts/check_hical_cli.py --hical-build-dir build --work-dir build-cli-check \
  --config Release -- -G Ninja '-DCMAKE_CXX_COMPILER=g++'
```

在 MinGW 上，可以传 `--ascii-build-paths`，并把 `--work-dir` 放在 ASCII 路径下。这样安装和编译子目录只用 ASCII 字符，可以带空格。脚本还会单独测试在中文目录下生成工程。生成的工程要和 Hical 的 ABI 匹配。如果 Hical 开了 Sanitizer，也给测试工程传相同的编译和链接选项。

本地用 MSYS2 MinGW64 验证了编译、安装搬移、Windows 和 POSIX shell 入口，也实际发了 HTTP 请求并用 Ctrl+C 退出。Linux、MSVC、最低 CMake 和本机未能运行的符号链接用例还要看 CI 结果。
