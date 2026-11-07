#!/usr/bin/env python3
"""安装 Hical 并移动安装目录，再用两个 CLI 入口分别生成、构建和测试工程。"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
COMMAND_TIMEOUT = 600
MOVE_RETRY_SECONDS = 5.0
MOVE_POLL_SECONDS = 0.1


def runCommand(command: list[str], cwd: Path, log, expected: int = 0, environment=None) -> str:
    """运行命令，把输出写入日志后返回。退出码不符时抛出异常。"""
    heading = f"\n执行：{subprocess.list2cmdline(command)}\n"
    print(heading, end="", flush=True)
    log.write(heading)
    log.flush()
    environment = dict(os.environ if environment is None else environment)
    environment["PYTHONUTF8"] = "1"
    try:
        result = subprocess.run(command, cwd=cwd, env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                                timeout=COMMAND_TIMEOUT, check=False)
    except subprocess.TimeoutExpired as error:
        partial = error.stdout or ""
        if isinstance(partial, bytes):
            partial = partial.decode("utf-8", errors="replace")
        print(partial, end="", flush=True)
        log.write(partial)
        log.flush()
        raise RuntimeError(f"命令运行超过 {COMMAND_TIMEOUT} 秒，日志见 {log.name}") from error
    print(result.stdout, end="", flush=True)
    log.write(result.stdout)
    log.flush()
    if result.returncode != expected:
        raise RuntimeError(f"命令退出码为 {result.returncode}，预期为 {expected}。日志见 {log.name}")
    return result.stdout


def runInstalled(launcher: Path, arguments: list[str], cwd: Path, log, expected: int = 0) -> str:
    """用参数列表直接运行安装后的 CLI，返回命令输出。"""
    environment = os.environ.copy()
    environment["PYTHONUTF8"] = "1"
    command = [str(launcher), *arguments]
    log.write(f"安装入口参数：{subprocess.list2cmdline(command)}\n")
    return runCommand(command, cwd, log, expected, environment)


def configureAndTest(cmake: str, ctest: str, project: Path, options, log) -> None:
    """用刚安装的 Hical 构建工程，确认 CTest 找到了路由测试再运行。"""
    build = project / "build"
    configure = [cmake, "-S", str(project), "-B", str(build), *options.cmake_args,
                 f"-Dhical_DIR={options.package_dir}", f"-DCMAKE_BUILD_TYPE={options.config}"]
    runCommand([*configure, "-DBUILD_TESTING=ON"], project, log)
    runCommand([cmake, "--build", str(build), "--config", options.config, "--parallel", "2"], project, log)
    discovery = runCommand([ctest, "--test-dir", str(build), "-C", options.config, "--show-only=json-v1"],
                           project, log)
    tests = json.loads(discovery).get("tests", [])
    expected = project.name.replace("-", "_") + ".routes"
    if not tests or expected not in [test.get("name") for test in tests]:
        raise RuntimeError(f"CTest 找不到路由测试 {expected}")
    runCommand([ctest, "--test-dir", str(build), "-C", options.config, "--no-tests=error",
                "--output-on-failure", "--timeout", "60"], project, log)


def findTools(cmakeName: str) -> tuple[str, str]:
    """返回 CMake 和 CTest 的路径，优先使用同一目录下的 CTest。"""
    cmake = shutil.which(cmakeName)
    if not cmake:
        raise RuntimeError(f"找不到 CMake：{cmakeName}")
    sibling = Path(cmake).resolve().with_name("ctest.exe" if os.name == "nt" else "ctest")
    ctest = str(sibling) if sibling.is_file() else shutil.which("ctest")
    if not ctest:
        raise RuntimeError("找不到 CTest。")
    return cmake, ctest


def movePrefix(prefix: Path, moved: Path) -> None:
    """移动安装目录。Windows 安装完有时还占着目录，遇到拒绝访问就等一会再试。"""
    deadline = time.monotonic() + MOVE_RETRY_SECONDS
    while True:
        try:
            prefix.rename(moved)
            return
        except PermissionError as error:
            remaining = deadline - time.monotonic()
            if os.name != "nt" or getattr(error, "winerror", None) != 5 or remaining <= 0:
                raise
            time.sleep(min(MOVE_POLL_SECONDS, remaining))


def verifyInstallation(options) -> None:
    """在新目录里安装 Hical 并检查生成的工程。失败后可以到该目录查日志。"""
    build = options.hical_build_dir.resolve(strict=True)
    if not (build / "CMakeCache.txt").is_file():
        raise RuntimeError(f"Hical 构建目录缺少 CMakeCache.txt：{build}")
    cmake, ctest = findTools(options.cmake)
    work = options.work_dir.resolve()
    work.mkdir(parents=True)
    prefix = work / ("install prefix" if options.ascii_build_paths else "安装 Hical 前缀")
    moved = work / ("moved prefix" if options.ascii_build_paths else "已搬移 Hical 前缀")
    with (work / "check.log").open("x", encoding="utf-8", newline="\n") as log:
        runCommand([cmake, "--install", str(build), "--config", options.config, "--prefix", str(prefix)],
                   work, log)
        if not prefix.resolve(strict=True).is_relative_to(work) or not moved.resolve().is_relative_to(work):
            raise RuntimeError("安装目录和搬移目标都必须在检查目录内。")
        movePrefix(prefix, moved)
        options.package_dir = moved / "lib/cmake/hical"
        if not (options.package_dir / "hicalConfig.cmake").is_file():
            raise RuntimeError(f"安装包缺少 hicalConfig.cmake：{options.package_dir}")
        launcher = moved / "bin" / ("hical.exe" if os.name == "nt" else "hical")
        if not launcher.is_file():
            raise RuntimeError("找不到安装后的 CLI。请用 -DHICAL_INSTALL_CLI=ON 重新配置 Hical。")
        projects = work / ("independent projects" if options.ascii_build_paths else "独立 工程")
        projects.mkdir()
        runCommand([sys.executable, str(REPOSITORY_ROOT / "scripts/hical.py"), "new", "source-api",
                    "--directory", str(projects)], moved, log)
        runInstalled(launcher, ["--help"], work, log)
        runInstalled(launcher, ["new", "installed-api", "--directory", str(projects)], work, log)
        runInstalled(launcher, ["gen"], work, log, expected=2)
        unicodeParent = work / "中文 生成目录"
        unicodeParent.mkdir()
        runInstalled(launcher, ["new", "unicode-api", "--directory", str(unicodeParent)], work, log)
        unicodeProject = unicodeParent / "unicode-api"
        expectedFiles = {"CMakeLists.txt", "README.md", ".gitignore", "src/app.h", "src/app.cpp",
                         "src/main.cpp", "tests/test_app.cpp"}
        actualFiles = {path.relative_to(unicodeProject).as_posix()
                       for path in unicodeProject.rglob("*") if path.is_file()}
        if actualFiles != expectedFiles:
            raise RuntimeError("中文目录下生成的工程文件不完整。")
        if os.name == "nt":
            runCommand([sys.executable, str(REPOSITORY_ROOT / "scripts/test_hical_cli_windows.py"),
                        "--launcher", str(launcher)], work, log)
        for name in ("source-api", "installed-api"):
            configureAndTest(cmake, ctest, projects / name, options, log)
        project = projects / "installed-api"
        noTests = project / "build-no-tests"
        runCommand([cmake, "-S", str(project), "-B", str(noTests), *options.cmake_args,
                    f"-Dhical_DIR={options.package_dir}", f"-DCMAKE_BUILD_TYPE={options.config}",
                    "-DBUILD_TESTING=OFF"], project, log)
        runCommand([cmake, "--build", str(noTests), "--config", options.config, "--parallel", "2"], project, log)
    print(f"CLI 安装检查通过，日志见 {work / 'check.log'}")


def main(argv: list[str] | None = None) -> int:
    """运行安装检查，成功返回 0，失败返回 1。`--` 后的参数传给生成工程的 CMake。"""
    arguments = list(sys.argv[1:] if argv is None else argv)
    separator = arguments.index("--") if "--" in arguments else len(arguments)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hical-build-dir", type=Path, required=True, help="已用 HICAL_INSTALL_CLI=ON 构建的 Hical 目录")
    parser.add_argument("--work-dir", type=Path, required=True, help="存放检查结果的目录，必须还未创建")
    parser.add_argument("--cmake", default="cmake", help="CMake 可执行文件，默认从 PATH 查找")
    parser.add_argument("--config", default="Release", help="构建、安装和测试使用的配置，默认 Release")
    parser.add_argument("--ascii-build-paths", action="store_true",
                        help="构建用的目录名改用 ASCII，另测中文目录下的工程生成")
    options = parser.parse_args(arguments[:separator])
    options.cmake_args = arguments[separator + 1:]
    try:
        verifyInstallation(options)
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f"CLI 集成检查失败：{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
