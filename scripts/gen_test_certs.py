#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 SSL 测试用的自签证书。

tests/test_ssl_connection.cpp 拿的是相对路径 test_server.crt / test_server.key，
而 ctest 跑测试时的工作目录是测试可执行文件所在的构建目录（默认 build/tests），
所以这里默认就把证书落到 build/tests，跟测试进程的 CWD 对上。

生成的是用完即弃的自签证书，本地调试够用，别拿去干别的。

用法:
    python scripts/gen_test_certs.py                      # 生成到 build/tests
    python scripts/gen_test_certs.py --out-dir build-cov/tests
    python scripts/gen_test_certs.py --force              # 已存在也重新生成
    HICAL_OPENSSL=/path/to/openssl python scripts/gen_test_certs.py

找不到 openssl 时脚本以非零码退出，不会静默放过——证书缺失会让 SSL 用例 SKIP，
静默跳过等于没有回归保护。
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys

# Windows CI 上 stdout 被重定向时默认不是 UTF-8，中文输出会直接抛 UnicodeEncodeError，
# 这里强制成 UTF-8，别让日志编码把脚本本身搞挂
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

# 仓库根目录（本脚本在 scripts/ 下）
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CERT_NAME = "test_server.crt"
KEY_NAME = "test_server.key"

# Windows 上 openssl CLI 常见的落点，PATH 里找不到时挨个试
WINDOWS_OPENSSL_HINTS = [
    r"C:\Program Files\OpenSSL-Win64\bin\openssl.exe",
    r"C:\Program Files\OpenSSL\bin\openssl.exe",
    r"C:\Program Files\Git\usr\bin\openssl.exe",
    r"C:\Program Files\Git\mingw64\bin\openssl.exe",
    r"C:\msys64\mingw64\bin\openssl.exe",
    r"C:\Strawberry\c\bin\openssl.exe",
]


def find_openssl():
    """定位 openssl 可执行文件，找不到返回 None。"""
    # 显式指定优先
    override = os.environ.get("HICAL_OPENSSL")
    if override:
        if os.path.isfile(override):
            return override
        print(f"[错误] HICAL_OPENSSL 指向的文件不存在：{override}", file=sys.stderr)
        sys.exit(2)

    found = shutil.which("openssl")
    if found:
        return found

    if os.name == "nt":
        candidates = list(WINDOWS_OPENSSL_HINTS)
        # OpenSSL 装在 Program Files 下但带版本后缀的情况
        candidates += glob.glob(r"C:\Program Files\OpenSSL*\bin\openssl.exe")
        for path in candidates:
            if os.path.isfile(path):
                return path

    return None


def certs_look_ok(cert_path, key_path):
    """证书和私钥都在、且非空，才算可用。"""
    return all(
        os.path.isfile(p) and os.path.getsize(p) > 0 for p in (cert_path, key_path)
    )


def generate(openssl, cert_path, key_path):
    """调 openssl 生成自签证书，失败直接抛 CalledProcessError。"""
    # 自签、无口令、CN=localhost，附上 SAN 免得现代客户端直接拒
    cmd = [
        openssl,
        "req",
        "-x509",
        "-newkey",
        "rsa:2048",
        "-nodes",
        "-keyout",
        key_path,
        "-out",
        cert_path,
        "-days",
        "365",
        "-subj",
        "/CN=localhost",
        "-addext",
        "subjectAltName=DNS:localhost,IP:127.0.0.1",
    ]
    subprocess.run(cmd, check=True, capture_output=True, text=True)


def main():
    parser = argparse.ArgumentParser(description="生成 SSL 测试自签证书")
    parser.add_argument(
        "--out-dir",
        default=os.path.join(PROJECT_ROOT, "build", "tests"),
        help="证书输出目录，默认 build/tests（可相对当前目录）",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="即使证书已存在也重新生成",
    )
    args = parser.parse_args()

    out_dir = os.path.abspath(args.out_dir)
    cert_path = os.path.join(out_dir, CERT_NAME)
    key_path = os.path.join(out_dir, KEY_NAME)

    # 已经有了就不折腾，省得每次 CI 重跑都重新签
    if not args.force and certs_look_ok(cert_path, key_path):
        print(f"[跳过] 证书已存在：{cert_path}")
        return 0

    openssl = find_openssl()
    if not openssl:
        print(
            "[错误] 找不到 openssl 可执行文件。装一个，或用 HICAL_OPENSSL 指过去。",
            file=sys.stderr,
        )
        return 1

    try:
        os.makedirs(out_dir, exist_ok=True)
    except OSError as e:
        print(f"[错误] 创建输出目录失败：{out_dir}（{e}）", file=sys.stderr)
        return 1

    print(f"[信息] 使用 openssl：{openssl}")
    try:
        generate(openssl, cert_path, key_path)
    except subprocess.CalledProcessError as e:
        print(f"[错误] openssl 生成证书失败（退出码 {e.returncode}）", file=sys.stderr)
        if e.stderr:
            print(e.stderr.strip(), file=sys.stderr)
        return 1
    except OSError as e:
        print(f"[错误] 无法执行 openssl：{openssl}（{e}）", file=sys.stderr)
        return 1

    # 再确认一遍产物真的落盘了，别信 openssl 自己的退出码
    if not certs_look_ok(cert_path, key_path):
        print("[错误] openssl 跑完却没生成出证书/私钥", file=sys.stderr)
        return 1

    print(f"[完成] 证书：{cert_path}")
    print(f"[完成] 私钥：{key_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
