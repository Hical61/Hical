#!/usr/bin/env python3
"""生成带 CMake 和 CTest 的 Hical 最小工程。"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


TEMPLATE_ROOT = Path(__file__).resolve().parent / "templates" / "minimal"
TEMPLATE_FILES = (
    "CMakeLists.txt", "README.md", ".gitignore", "src/app.h", "src/app.cpp",
    "src/main.cpp", "tests/test_app.cpp",
)
RESERVED_NAMES = {"CON", "PRN", "AUX", "NUL", "CLOCK$", "CONIN$", "CONOUT$"} | {
    f"{prefix}{number}" for prefix in ("COM", "LPT") for number in range(1, 10)
}


class ScaffoldError(Exception):
    """模板、路径或文件操作出错，工程无法继续生成。"""


class ChineseParser(argparse.ArgumentParser):
    """把 argparse 的用法和常见报错改成中文。"""

    def format_usage(self):
        return super().format_usage().replace("usage: ", "用法：", 1)

    def format_help(self):
        return super().format_help().replace("usage: ", "用法：", 1)

    def error(self, message):
        if message.startswith("the following arguments are required: "):
            message = "缺少必需参数：" + message.split(": ", 1)[1]
        elif message.startswith("unrecognized arguments: "):
            message = "无法识别的参数：" + message.split(": ", 1)[1]
        elif "invalid choice:" in message:
            message = "不支持这个子命令，目前只有 new。"
        elif not re.search(r"[\u4e00-\u9fff]", message):
            message = "参数无效，请使用 --help 查看用法。"
        else:
            message = re.sub(r"^argument ([^:]+): ", r"参数 \1：", message)
        self.print_usage(sys.stderr)
        self.exit(2, f"{self.prog}：参数错误：{message}\n")


def validateName(name: str) -> str:
    """检查 name 能否用作项目目录名，通过就返回原名。"""
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]{0,63}", name):
        raise argparse.ArgumentTypeError("项目名要以英文字母开头，长度为 1～64 个字符，可用英文字母、数字、下划线和连字符。")
    if name.upper() in RESERVED_NAMES:
        raise argparse.ArgumentTypeError("项目名不能用 Windows 保留的设备名。")
    return name


def renderTemplates(name: str, templateRoot: Path | None = None) -> list[tuple[Path, str]]:
    """用 name 填好模板，返回各文件的相对路径和内容。

    templateRoot 没有指定时，从脚本旁边的 templates/minimal 读取。
    """
    validateName(name)
    templateRoot = TEMPLATE_ROOT if templateRoot is None else Path(templateRoot)
    rendered = []
    for relative in TEMPLATE_FILES:
        content = (templateRoot / (relative + ".in")).read_text(encoding="utf-8")
        content = content.replace("@PROJECT_NAME@", name).replace("@TARGET_NAME@", name.replace("-", "_"))
        rendered.append((Path(relative), content))
    return rendered


def rollbackFiles(files: list[Path], directories: list[Path]) -> list[Path]:
    """按清单删除刚建的文件和空目录，返回没删掉的路径。"""
    remaining = []
    for path in reversed(files):
        try:
            path.unlink()
        except FileNotFoundError:
            pass
        except OSError:
            remaining.append(path)
    for path in reversed(directories):
        try:
            path.rmdir()
        except FileNotFoundError:
            pass
        except OSError:
            remaining.append(path)
    return remaining


def generateProject(name: str, parent: Path, templateRoot: Path | None = None) -> Path:
    """在 parent 下用模板创建名为 name 的工程，返回新目录。

    templateRoot 可用来换模板目录。写入失败时只清理本次创建的文件和空目录。
    """
    validateName(name)
    try:
        parent = Path(parent).resolve(strict=True)
    except RuntimeError as error:
        # Python 3.10～3.12 遇到链接环时抛 RuntimeError，3.13 起改为 OSError。
        raise ScaffoldError(f"父目录的符号链接无法解析：{parent}") from error
    if not parent.is_dir():
        raise ScaffoldError(f"父路径不是目录：{parent}")
    rendered = renderTemplates(name, templateRoot)
    target = parent / name
    try:
        target.mkdir()
    except FileExistsError as error:
        raise ScaffoldError(f"目标已存在，未写入文件：{target}") from error

    files = []
    directories = [target]
    try:
        for relative, content in rendered:
            path = target / relative
            if path.parent not in directories:
                path.parent.mkdir()
                directories.append(path.parent)
            stream = path.open("x", encoding="utf-8", newline="\n")
            files.append(path)
            with stream:
                stream.write(content)
    except BaseException as error:
        remaining = rollbackFiles(files, directories)
        residual = f"。还有文件或目录无法清理，见 {target}" if remaining else ""
        if isinstance(error, KeyboardInterrupt):
            raise ScaffoldError(f"操作已取消{residual}") from error
        raise ScaffoldError(f"写入工程失败：{error}{residual}") from error
    return target


def makeParser() -> ChineseParser:
    """返回 hical 的参数解析器，包含 new 和 --help。"""
    parser = ChineseParser(prog="hical", description="生成带 CMake 和 CTest 的 Hical 最小工程。",
                           add_help=False, allow_abbrev=False)
    parser._positionals.title = "位置参数"
    parser._optionals.title = "选项"
    parser.add_argument("-h", "--help", action="help", help="显示帮助并退出")
    commands = parser.add_subparsers(dest="command", title="子命令", metavar="{new}", required=True)
    new = commands.add_parser("new", help="创建最小 C++20 工程", description="在已有目录下新建工程。遇到同名文件或目录时会报错。",
                              add_help=False, allow_abbrev=False)
    new._positionals.title = "位置参数"
    new._optionals.title = "选项"
    new.add_argument("-h", "--help", action="help", help="显示帮助并退出")
    new.add_argument("name", type=validateName, help="项目名，例如 demo-api")
    new.add_argument("--directory", metavar="父目录", default=".", help="工程建在哪个目录下，默认是当前目录")
    return parser


def main(argv: list[str] | None = None) -> int:
    """读取 argv 并生成工程。省略 argv 时用命令行参数。

    成功返回 0，生成失败返回 1，参数错误时以 2 退出。
    """
    # 避免路径里有 GBK 等编码不支持的字符时，打印提示反而报错。
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(errors="backslashreplace")
    if sys.version_info < (3, 10):
        print("请用 Python 3.10 或更新版本运行 hical。", file=sys.stderr)
        return 1
    args = makeParser().parse_args(argv)
    try:
        target = generateProject(args.name, Path(args.directory))
    except (ScaffoldError, OSError, UnicodeError) as error:
        print(f"生成失败：{error}", file=sys.stderr)
        return 1
    print(f"已创建工程：{target}")
    print("进入工程目录，按 README.md 配好 Hical 安装前缀和工具链，再运行：")
    print("  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release")
    print("  cmake --build build --config Release")
    print("  ctest --test-dir build -C Release --no-tests=error --output-on-failure")
    return 0


if __name__ == "__main__":
    sys.exit(main())
