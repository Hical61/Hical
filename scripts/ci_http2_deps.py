#!/usr/bin/env python3
"""在 CI 工作区准备固定 HTTP/2 依赖；默认只写计划，--build 才下载和构建。"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile


WORKSPACE = Path(__file__).resolve().parents[1]
SOURCES = {
    "boost": {
        "version": "1.90.0", "directory": "boost_1_90_0",
        "url": "https://archives.boost.io/release/1.90.0/source/boost_1_90_0.tar.bz2",
        "sha256": "49551aff3b22cbc5c5a9ed3dbc92f0e23ea50a0f7325b0d198b705e8ee3fc305",
        "hash_source": "https://archives.boost.io/release/1.90.0/source/boost_1_90_0.tar.bz2.json",
    },
    "nghttp2": {
        "version": "1.70.0", "directory": "nghttp2-1.70.0",
        "url": "https://github.com/nghttp2/nghttp2/releases/download/v1.70.0/nghttp2-1.70.0.tar.xz",
        "sha256": "e05cb1388eaca3830aded4ccf20044b6e1ac1a61411dcca11b0437c4285c8bc2",
        "hash_source": "nghttp2 1.70.0 官方发布归档，已在本地复核 SHA256",
    },
}
H2SPEC_COMMIT = "70ac2294010887f48b18e2d64f5cccd48421fad1"
# SHA 相同不代表旧解包树完整保留了权限；规则升级后不能直接复用旧树。
EXTRACTION_VERSION = "permission-v1"
FILE_PERMISSION_MASK = 0o777


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def execute(args, command: list[str], name: str, *, cwd: Path | None = None, env: dict | None = None) -> str:
    logs = args.root / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    path = logs / (name + ".log")
    record = {"argv": command, "cwd": str(cwd or WORKSPACE), "log": str(path), "timeout_seconds": 1800}
    args.report["commands"].append(record)
    write_json(args.root / "manifest.json", args.report)
    with path.open("wb") as output:
        result = subprocess.run(command, cwd=cwd or WORKSPACE, env=env, stdout=output,
                                stderr=subprocess.STDOUT, timeout=1800, check=False)
    record["exit_code"] = result.returncode
    write_json(args.root / "manifest.json", args.report)
    if result.returncode:
        print(path.read_text(encoding="utf-8", errors="replace")[-12000:], file=sys.stderr)
        raise RuntimeError(f"{name} 失败，exit={result.returncode}；完整日志：{path}")
    return path.read_text(encoding="utf-8", errors="replace")


def archive(args, source: dict) -> Path:
    downloads = args.root / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    path = downloads / source["url"].rsplit("/", 1)[-1]
    if not path.is_file():
        temporary = path.with_name(path.name + ".part")
        execute(args, [shutil.which("curl") or "curl", "--http1.1", "--fail", "--show-error", "--location",
                       "--proto", "=https", "--proto-redir", "=https", "--retry", "3", "--retry-all-errors",
                       "--connect-timeout", "30", "--max-time", "1200", "--output", str(temporary), source["url"]],
                "download-" + source["directory"])
        if sha256(temporary) != source["sha256"]:
            raise ValueError(f"下载归档 SHA256 不符：{temporary}")
        temporary.replace(path)
    if sha256(path) != source["sha256"]:
        raise ValueError(f"现有归档 SHA256 不符，拒绝复用：{path}")
    return path


def unpack(args, source: dict) -> Path:
    """安全提取指定归档并返回源码根目录，只复用同一解包规则的完整缓存。"""
    path = archive(args, source)
    destination = args.root / "src"
    root = destination / source["directory"]
    marker = root / ".hical-archive-sha256"
    extraction_marker = root / ".hical-extraction-version"
    if root.exists():
        if marker.is_file() and marker.read_text(encoding="utf-8").strip() == source["sha256"]:
            if extraction_marker.is_file() and extraction_marker.read_text(encoding="utf-8").strip() == EXTRACTION_VERSION:
                return root
            raise ValueError(f"源码目录的解包规则已升级，拒绝复用旧树；请指定新的 --root：{root}")
        raise ValueError(f"源码目录不是完整的本次冻结归档，拒绝覆盖：{root}")
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(path, "r:*") as packed:
        for item in packed:
            relative = PurePosixPath(item.name)
            if relative.is_absolute() or ".." in relative.parts or not relative.parts or relative.parts[0] != source["directory"]:
                raise ValueError(f"归档路径超出固定源码目录：{item.name}")
            # 不提取链接或设备。文档/测试不参与 JSON 库构建，也避免 Windows 深层测试路径。
            if not item.isfile():
                continue
            if source["directory"].startswith("boost_") and any(part in ("doc", "docs", "test", "tests", "example", "examples", "benchmark", "benchmarks") for part in relative.parts[1:-1]):
                continue
            target = destination.joinpath(*relative.parts)
            target.resolve().relative_to(destination.resolve())
            target.parent.mkdir(parents=True, exist_ok=True)
            incoming = packed.extractfile(item)
            if incoming is None:
                raise ValueError(f"不可读归档普通成员：{item.name}")
            with incoming, target.open("xb") as outgoing:
                shutil.copyfileobj(incoming, outgoing)
            # 保留普通读写和执行位，不继承 setuid/setgid/sticky 等特殊权限。
            target.chmod(item.mode & FILE_PERMISSION_MASK)
    extraction_marker.write_text(EXTRACTION_VERSION + "\n", encoding="utf-8")
    marker.write_text(source["sha256"] + "\n", encoding="utf-8")
    return root


def prefix_is_ready(args, *, with_interop: bool) -> bool:
    """检查已安装前缀是否完整。CI 缓存命中的就是这一份，没必要重新编一遍 Boost。"""
    boost_config = args.prefix / "lib/cmake/Boost-1.90.0/BoostConfig.cmake"
    boost_header = args.prefix / "include/boost/version.hpp"
    nghttp2_header = args.prefix / "include/nghttp2/nghttp2ver.h"
    if not (boost_config.is_file() and boost_header.is_file() and nghttp2_header.is_file()):
        return False
    if not re.search(r"#define BOOST_VERSION\s+109000\b", boost_header.read_text(encoding="utf-8", errors="replace")):
        return False
    if '#define NGHTTP2_VERSION "1.70.0"' not in nghttp2_header.read_text(encoding="utf-8", errors="replace"):
        return False
    # 光有头文件说明不了链接得上，静态库也得在。
    # 名字随工具链变：b2 在 MSVC 下出 boost_json.lib，Unix/MinGW 出 libboost_json.a；
    # nghttp2 的 CMake install 同理。用前缀通配避开这份差异。
    suffix = ".lib" if args.compiler == "msvc" else ".a"
    for component in ("boost_json", "nghttp2"):
        if not any((args.prefix / "lib").glob("*" + component + suffix)):
            return False
    # interop 用例要跑这四个工具，少一个都跑不起来。
    if with_interop:
        tools = ("h2spec", "nghttp", "nghttpd", "h2load")
        if not all((args.prefix / "bin" / name).is_file() for name in tools):
            return False
    return True


def build_boost(args) -> None:
    source = unpack(args, SOURCES["boost"])
    compiler = shutil.which(args.cxx)
    if compiler is None:
        raise ValueError(f"找不到指定编译器：{args.cxx}")
    config = args.root / "user-config.jam"
    if args.compiler == "msvc":
        version = re.fullmatch(r"(\d+\.\d+)(?:\.\d+)*", os.environ.get("VCToolsVersion", "").strip())
        if version is None:
            raise ValueError("MSVC 环境缺少有效的 VCToolsVersion")
        vc_install_dir = os.environ.get("VCINSTALLDIR")
        if not vc_install_dir:
            raise ValueError("MSVC 环境缺少 VCINSTALLDIR")
        setup = Path(vc_install_dir) / "Auxiliary/Build/vcvarsall.bat"
        if not setup.is_file():
            raise ValueError(f"找不到 MSVC 环境脚本：{setup}")
        # 编译器和环境都已选好，不让旧版 Boost 再猜 VS 版本和安装路径。
        config.write_text(f'using msvc : {version.group(1)} : "{Path(compiler).as_posix()}" : <setup>"{setup.as_posix()}" ;\n', encoding="utf-8")
        toolset = "msvc-" + version.group(1)
    else:
        config.write_text(f'using {args.compiler} : hical : "{Path(compiler).as_posix()}" ;\n', encoding="utf-8")
        toolset = args.compiler + "-hical"
    if os.name == "nt":
        execute(args, [os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", "bootstrap.bat", args.compiler], "boost-bootstrap", cwd=source)
    else:
        execute(args, ["bash", "./bootstrap.sh", "--with-toolset=" + args.compiler], "boost-bootstrap", cwd=source)
    engine = source / ("b2.exe" if os.name == "nt" else "b2")
    user_config = ["--user-config=" + config.as_posix()]
    flags = [] if args.sanitizer == "none" else ["-fsanitize=" + args.sanitizer, "-fno-omit-frame-pointer"]
    command = [str(engine), "-j" + str(args.jobs), "--ignore-site-config", "--with-json", "--layout=system", "--prefix=" + args.prefix.as_posix(),
               "--libdir=" + (args.prefix / "lib").as_posix(), "--build-dir=" + (args.root / "boost-build").as_posix(),
               "toolset=" + toolset, "variant=" + ("debug" if args.configuration == "Debug" else "release"),
               "link=static", "runtime-link=shared", "threading=multi", "address-model=64", "cxxstd=20", *user_config]
    if flags:
        command += ["cxxflags=" + " ".join(flags), "linkflags=" + " ".join(flags)]
    execute(args, command + ["install"], "boost-install", cwd=source)
    header = args.prefix / "include/boost/version.hpp"
    if not re.search(r"#define BOOST_VERSION\s+109000\b", header.read_text()):
        raise ValueError("安装前缀的 Boost 不是 1.90.0")
    if not args.prefix.joinpath("lib/cmake/Boost-1.90.0/BoostConfig.cmake").is_file():
        raise ValueError("固定 Boost 前缀未导出 BoostConfig，禁止回退到系统 apt Boost")


def build_nghttp2(args) -> None:
    source = unpack(args, SOURCES["nghttp2"])
    build = args.root / "nghttp2-build"
    command = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
               "-DCMAKE_BUILD_TYPE=" + args.configuration, "-DCMAKE_C_COMPILER=" + args.cc,
               "-DCMAKE_CXX_COMPILER=" + args.cxx, "-DCMAKE_INSTALL_PREFIX=" + str(args.prefix),
               "-DCMAKE_INSTALL_LIBDIR=lib", "-DBUILD_STATIC_LIBS=ON", "-DBUILD_SHARED_LIBS=OFF",
               "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
               "-DENABLE_STATIC_CRT=OFF", "-DBUILD_TESTING=OFF", "-DENABLE_FAILMALLOC=OFF", "-DENABLE_DOC=OFF",
               "-DENABLE_EXAMPLES=OFF", "-DENABLE_HPACK_TOOLS=OFF", "-DENABLE_HTTP3=OFF",
               "-DENABLE_LIB_ONLY=" + ("OFF" if args.interop_tools else "ON"),
               "-DENABLE_APP=" + ("ON" if args.interop_tools else "OFF")]
    if args.sanitizer != "none":
        command += ["-DCMAKE_C_FLAGS=-fsanitize=" + args.sanitizer + " -fno-omit-frame-pointer",
                    "-DCMAKE_CXX_FLAGS=-fsanitize=" + args.sanitizer + " -fno-omit-frame-pointer"]
    execute(args, command, "nghttp2-configure")
    execute(args, ["cmake", "--build", str(build), "--config", args.configuration, "--parallel", str(args.jobs)], "nghttp2-build")
    execute(args, ["cmake", "--install", str(build), "--config", args.configuration], "nghttp2-install")
    version = args.prefix / "include/nghttp2/nghttp2ver.h"
    if '#define NGHTTP2_VERSION "1.70.0"' not in version.read_text():
        raise ValueError("安装前缀的实现库不是 nghttp2 1.70.0")


def build_h2spec(args) -> None:
    source = args.h2spec_source
    if source is None or not source.is_dir():
        raise ValueError("互操作工具要求先 checkout 官方固定提交并传 --h2spec-source")
    observed = execute(args, ["git", "-C", str(source), "rev-parse", "HEAD"], "h2spec-source-identity").strip()
    if observed != H2SPEC_COMMIT:
        raise ValueError("h2spec checkout 与固定官方提交不符")
    environment = os.environ.copy()
    environment.update(GO111MODULE="on", GOTOOLCHAIN="local", GOWORK="off", CGO_ENABLED="0",
                       GOPATH=str(args.root / "go"), GOMODCACHE=str(args.root / "go/mod"), GOCACHE=str(args.root / "go/cache"))
    go_version = execute(args, ["go", "version"], "go-version", env=environment).strip()
    execute(args, ["go", "build", "-mod=readonly", "-ldflags", "-X main.VERSION=2.6.0 -X main.COMMIT=" + H2SPEC_COMMIT,
                   "-o", str(args.prefix / "bin/h2spec"), "cmd/h2spec/h2spec.go"], "h2spec-build", cwd=source, env=environment)
    execute(args, ["go", "mod", "verify"], "h2spec-module-verify", cwd=source, env=environment)
    output = execute(args, [str(args.prefix / "bin/h2spec"), "--version"], "h2spec-version")
    if "2.6.0" not in output or H2SPEC_COMMIT not in output:
        raise ValueError("h2spec 二进制版本/提交不符")
    args.report["h2spec"] = {"commit": H2SPEC_COMMIT, "go_version": go_version, "go_sum_sha256": sha256(source / "go.sum"),
                             "binary_sha256": sha256(args.prefix / "bin/h2spec"), "version_output": output.strip()}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=lambda value: Path(value).resolve(), required=True)
    parser.add_argument("--compiler", choices=("gcc", "clang", "msvc"), required=True)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--configuration", choices=("Release", "Debug", "RelWithDebInfo"), default="Release")
    parser.add_argument("--sanitizer", choices=("none", "address,undefined", "thread"), default="none")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--interop-tools", action="store_true")
    parser.add_argument("--h2spec-source", type=lambda value: Path(value).resolve())
    args = parser.parse_args()
    args.root.relative_to(WORKSPACE)
    if args.root == WORKSPACE or not 1 <= args.jobs <= 16:
        parser.error("依赖 root 必须为仓库内独立子目录；jobs 必须在1～16之间")
    if args.interop_tools and (os.name == "nt" or args.sanitizer != "none"):
        parser.error("互操作工具仅在独立的 Linux 非 Sanitizer job 构建")
    args.prefix = args.root / "prefix"
    args.report = {"schema": 1, "sources": SOURCES, "compiler": args.compiler, "cc": args.cc, "cxx": args.cxx,
                   "configuration": args.configuration, "sanitizer": args.sanitizer, "prefix": str(args.prefix),
                   "interop_tools": args.interop_tools, "status": "plan_only", "commands": []}
    args.root.mkdir(parents=True, exist_ok=True)
    write_json(args.root / "manifest.json", args.report)
    if args.build:
        try:
            # CI 会把整个 prefix 缓存下来，命中时跳过下载和解包，只走这段校验。
            if prefix_is_ready(args, with_interop=args.interop_tools):
                args.report["status"] = "reused"
            else:
                build_boost(args)
                build_nghttp2(args)
                if args.interop_tools:
                    build_h2spec(args)
                args.report["status"] = "built"
                args.report["installed_files"] = [{"path": str(path.relative_to(args.prefix)), "sha256": sha256(path)}
                                                  for path in sorted(args.prefix.rglob("*")) if path.is_file() and path.suffix in (".a", ".lib")]
        except BaseException as error:
            args.report.update(status="failed", error=str(error))
            raise
        finally:
            write_json(args.root / "manifest.json", args.report)
    print(json.dumps({"status": args.report["status"], "prefix": str(args.prefix)}, ensure_ascii=False))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, tarfile.TarError, subprocess.TimeoutExpired) as error:
        print(f"错误：{error}", file=sys.stderr)
        sys.exit(1)
