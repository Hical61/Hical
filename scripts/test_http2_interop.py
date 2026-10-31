#!/usr/bin/env python3
"""驱动真实原生 HTTP/2 工具和公开 API 探针；prepare-only 不执行任何程序或网络请求。"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import ssl
import subprocess
import sys
import threading
import time
import traceback
import uuid
import xml.etree.ElementTree as ET


REPOSITORY = Path(__file__).resolve().parents[1]
HELLO = b"Hello, Hical!\n"
JSON_BODY = b'{"message":"hello","answer":42}'
H2SPEC_COMMIT = "70ac2294010887f48b18e2d64f5cccd48421fad1"
# 固定提交的普通/strict 案例身份集合；少一个或用新身份替换旧案例都必须失败。
H2SPEC_CASE_IDENTITIES = {
    False: (146, "6548da1fb73b37217b9aeb3f99e688cda4610be8c9ddf84b77ff5bd6f9fff4fd"),
    True: (147, "23de00ebfc6954aa2d5218fc24200674bcdbd9c3eaae43fa99f58044554f2f8a"),
}
H2SPEC_REVIEW_SIGNATURE = "338f12fdffa16db9f1824e8c0bfac16361a3ebf9acfab8449c309f430007f24e"
STANDARDS = {
    "suite": ["RFC 7540", "RFC 7541", "h2spec generic"],
    "suite_version": "h2spec 2.6.0", "suite_commit": H2SPEC_COMMIT,
    "rfc9113_claim": False,
    "readme": "https://github.com/summerwind/h2spec/blob/v2.6.0/README.md",
    "tracking_issue": "https://github.com/summerwind/h2spec/issues/128",
    "rfc9113_changes": "https://www.rfc-editor.org/rfc/rfc9113.html#appendix-B",
    "changes": ["TLS 1.3", "弃用旧优先级语义但保留相关线格式/收发规则", "弃用 HTTP/1.1 Upgrade，保留 prior knowledge",
                "字段校验及 4xx 错误报告调整", "连接专用字段及 Host/:authority 一致性", "HPACK 动态表更新澄清"],
    "failure_policy": "逐项记录并保留失败；RFC 版本差异必须人工逐项核对，不作通配豁免",
}


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_record(path: Path) -> dict:
    if not path.is_file():
        return {"path": str(path), "available": False}
    before = path.stat()
    with path.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError(f"留证时文件正在变化：{path}")
    return {"path": str(path), "available": True, "size": after.st_size, "sha256": digest}


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def source_records(root: Path) -> dict:
    files = {Path(__file__).resolve(), *REPOSITORY.joinpath("docker/test/http2").glob("*")}
    for directory in ("src", "cmake"):
        for path in root.joinpath(directory).rglob("*"):
            if path.is_file() and (path.suffix in (".h", ".hci", ".cpp", ".c", ".in", ".cmake") or path.name == "CMakeLists.txt"):
                files.add(path)
    for name in ("CMakeLists.txt", "tests/CMakeLists.txt", "conanfile.py", "vcpkg.json"):
        if root.joinpath(name).is_file():
            files.add(root / name)
    records = [file_record(path) for path in sorted(files) if path.is_file()]
    identity = sha256(json.dumps(records, sort_keys=True, ensure_ascii=False).encode("utf-8"))
    return {"root": str(root), "observation_identity": identity, "files": records,
            "notice": "记录当前文件哈希；源码与二进制的对应关系需结合构建日志，不能用 Git HEAD 代替未提交内容"}


def tool_argument(path: Path, executable: Path) -> str:
    """仅给 Cygwin 程序转换本机文件路径，不改全局 PATH 或安装挂载点。"""
    path = path.resolve()
    if os.name == "nt" and executable.parent.joinpath("cygwin1.dll").is_file():
        if not re.fullmatch(r"[A-Za-z]:", path.drive):
            raise ValueError("Cygwin 夹具只接受本机盘符路径，不推断 UNC 挂载")
        return "/cygdrive/" + path.drive[0].lower() + path.as_posix()[2:]
    return str(path)


class Runner:
    def __init__(self, output: Path, timeout: float):
        self.output = output
        self.timeout = timeout
        self.commands: list[dict] = []
        self.children: list[tuple[subprocess.Popen, dict, object, object]] = []

    def start(self, name: str, arguments: list[str], *, cwd: Path | None = None,
              env: dict | None = None, lifetime: float | None = None) -> tuple:
        number = len(self.commands)
        directory = self.output / "commands" / f"{number:03d}-{name}"
        directory.mkdir(parents=True)
        record = {"name": name, "argv": list(map(str, arguments)), "cwd": str(cwd or self.output),
                  "started_at_utc": utc_now(), "stdout": str(directory / "stdout.bin"),
                  "stderr": str(directory / "stderr.bin"), "timeout_seconds": lifetime or self.timeout}
        self.commands.append(record)
        stdout = (directory / "stdout.bin").open("wb")
        stderr = (directory / "stderr.bin").open("wb")
        options = {"start_new_session": True} if os.name != "nt" else {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
        try:
            process = subprocess.Popen(record["argv"], cwd=cwd or self.output, env=env, stdin=subprocess.DEVNULL,
                                       stdout=stdout, stderr=stderr, shell=False, **options)
        except BaseException:
            stdout.close()
            stderr.close()
            record["start_failed"] = True
            raise
        def expired():
            if process.poll() is None:
                record["timed_out"] = True
                try:
                    if os.name == "nt":
                        process.kill()
                    else:
                        os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass

        timer = threading.Timer(record["timeout_seconds"], expired)
        timer.daemon = True
        child = (process, record, stdout, stderr, timer)
        record["pid"] = process.pid
        self.children.append(child)
        timer.start()
        return child

    @staticmethod
    def stop(child: tuple) -> None:
        process, record, stdout, stderr, timer = child
        if record.get("reaped"):
            return
        timer.cancel()
        timer.join(timeout=1)
        if process.poll() is None:
            record["terminated_by_driver"] = True
            if os.name == "nt":
                process.terminate()
            else:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                if os.name == "nt":
                    process.kill()
                else:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                process.wait(timeout=3)
        else:
            process.wait(timeout=3)
        stdout.close()
        stderr.close()
        record.update(exit_code=process.returncode, ended_at_utc=utc_now(), reaped=True)
        record["raw_files"] = [file_record(Path(record[key])) for key in ("stdout", "stderr")]

    def run(self, name: str, arguments: list[str], *, timeout: float | None = None,
            cwd: Path | None = None, env: dict | None = None, check: bool = True) -> dict:
        child = self.start(name, arguments, cwd=cwd, env=env, lifetime=timeout or self.timeout)
        process, record, *_ = child
        record["timeout_seconds"] = timeout or self.timeout
        try:
            process.wait(timeout=record["timeout_seconds"])
        except subprocess.TimeoutExpired:
            record["timed_out"] = True
        finally:
            self.stop(child)
        if check and (record.get("timed_out") or record["exit_code"] != 0):
            raise RuntimeError(f"{name} 失败或超时；见 {Path(record['stderr']).parent}")
        return record

    def close(self) -> list[str]:
        errors = []
        for child in reversed(self.children):
            try:
                self.stop(child)
            except BaseException as error:
                errors.append(f"PID {child[0].pid} 回收失败：{error}")
        return errors


def wait_ready(child: tuple, predicate, timeout: float):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        if child[0].poll() is not None:
            raise RuntimeError(f"等待就绪时子进程退出：{child[1]['argv']}")
        try:
            result = predicate()
            if result:
                return result
        except (OSError, ValueError, ssl.SSLError) as error:
            last_error = str(error)
        # 这是按就绪条件轮询的上限间隔，不把固定等待当作业务已就绪。
        threading.Event().wait(min(0.025, max(0, deadline - time.monotonic())))
    raise TimeoutError(f"等待真实就绪超时；最后观察：{last_error}")


@contextmanager
def hical_server(args, runner: Runner, mode: str, payload: Path):
    directory = args.out / f"server-{mode}"
    directory.mkdir()
    ready, stop = directory / "ready.json", directory / "stop"
    child = runner.start(f"hical-{mode}", [str(args.server_driver), "--ready", str(ready), "--stop-file", str(stop),
        "--file", str(payload), "--cert", str(args.cert), "--key", str(args.key), "--mode", mode,
        "--tls", "on", "--lifetime-seconds", str(args.server_lifetime), "--slow-ms", "50"],
        lifetime=args.server_lifetime + 10)
    try:
        data = wait_ready(child, lambda: json.loads(ready.read_text(encoding="utf-8")) if ready.is_file() else None,
                          min(args.timeout, 30))
        if not data.get("ready") or not data.get("io_callback_executed") or not 1 <= data.get("port", 0) <= 65535:
            raise ValueError("服务端就绪文件未证明真实事件循环和 OS 分配端口")
        if data.get("mode") != mode or not data.get("tls") or (mode == "h2" and not data.get("compiled_http2")):
            raise ValueError("服务端探针模式或 TLS/H2 编译标识不符")
        if data.get("file_sha256") != file_record(payload)["sha256"]:
            raise ValueError("服务端文件夹具与驱动不一致")
        yield data
    finally:
        stop.write_bytes(b"stop\n")
        try:
            child[0].wait(timeout=5)
        except subprocess.TimeoutExpired:
            child[1]["graceful_stop_timed_out"] = True
        runner.stop(child)
        if child[1].get("graceful_stop_timed_out") or child[1].get("timed_out") or child[1]["exit_code"] != 0:
            raise RuntimeError("Hical 服务端没有正常在 stop-file 后退出，不能只凭响应成功判定收尾通过")


def read_har(path: Path, expected: dict[str, tuple[int, int | None]]) -> list:
    entries = json.loads(path.read_text(encoding="utf-8"))["log"]["entries"]
    if len(entries) != len(expected):
        raise ValueError(f"HAR 交易数不符：{len(entries)} != {len(expected)}")
    seen = set()
    for entry in entries:
        url = entry["request"]["url"]
        if url not in expected or url in seen:
            raise ValueError(f"HAR 含未预期或重复 URL：{url}")
        seen.add(url)
        status, size = expected[url]
        response = entry["response"]
        if response["status"] != status or response["httpVersion"] != "HTTP/2.0":
            raise ValueError(f"HAR 状态/协议不符：{url}")
        if size is not None and response["content"]["size"] != size:
            raise ValueError(f"HAR 完整 DATA 长度不符：{url}")
    return entries


def nghttp_options(args, har: Path) -> list[str]:
    result = [str(args.nghttp), "--timeout=15", "--har=" + tool_argument(har, args.nghttp)]
    if args.external_insecure:
        result.append("--no-verify-peer")
    return result


def tool_environment(args, tool: Path) -> dict:
    environment = os.environ.copy()
    environment["SSL_CERT_FILE"] = tool_argument(args.ca, tool)
    return environment


def nghttp_trailers(log: str, target: str) -> dict:
    """从逐流帧日志核对第二个 HEADERS；nghttp HAR 不保存 trailers。"""
    request_stream = None
    wanted_stream = None
    pending = {}
    blocks = {}
    trailers = {}
    for line in log.splitlines():
        sent = re.search(r"send HEADERS frame[^\n]*stream_id=(\d+)", line)
        if sent:
            request_stream = int(sent[1])
        if line.strip() == ":path: " + target:
            wanted_stream = request_stream
        field = re.search(r"recv \(stream_id=(\d+)\) ([a-z0-9_-]+): (.*)$", line)
        if field:
            pending.setdefault(int(field[1]), {})[field[2]] = field[3]
        frame = re.search(r"recv HEADERS frame <length=\d+, flags=0x([0-9a-fA-F]+), stream_id=(\d+)>", line)
        if frame:
            stream = int(frame[2])
            fields = pending.pop(stream, {})
            if blocks.get(stream, 0) and int(frame[1], 16) & 1:
                trailers[stream] = fields
            blocks[stream] = blocks.get(stream, 0) + 1
    if wanted_stream is None or wanted_stream not in trailers:
        raise ValueError(f"nghttp 帧日志未证明 {target} 的独立尾 HEADERS END_STREAM")
    return {"stream_id": wanted_stream, "fields": trailers[wanted_stream], "end_stream": True}


def check_server(args, runner: Runner, ready: dict, payload: Path) -> dict:
    directory = args.out / "nghttp-to-hical"
    directory.mkdir()
    base = f"https://127.0.0.1:{ready['port']}"
    contents = {"/hello": HELLO, "/json-dom": JSON_BODY, "/json-dto": JSON_BODY, "/json-writer": JSON_BODY,
                "/file.bin": payload.read_bytes(), "/range.bin": payload.read_bytes()[717:87718],
                "/gzip": payload.read_bytes(), "/slow": HELLO, "/trailer": HELLO}
    # nghttp -O 官方仍未实现。无 -v 时 stdout 才是纯完整 body；单独多流 HAR/帧日志证明复用。
    multi_paths = list(contents)
    har = directory / "multiplex.har"
    command = nghttp_options(args, har) + ["-v", "-n", "-H", "accept-encoding: identity"] + [base + path for path in multi_paths]
    record = runner.run("nghttp-multiplex", command, env=tool_environment(args, args.nghttp))
    log = Path(record["stdout"]).read_text(encoding="utf-8", errors="replace")
    stream_ids = sorted(set(map(int, re.findall(r"send HEADERS frame[^\n]*stream_id=(\d+)", log))))
    if "The negotiated protocol: h2" not in log or len(stream_ids) != len(multi_paths):
        raise ValueError("nghttp 日志未证明 ALPN h2 及多个独立请求 stream")
    if len(re.findall(r"\bConnected\b", log)) != 1:
        raise ValueError("nghttp 多流命令未证明只建立一条连接")
    trailer_evidence = nghttp_trailers(log, "/trailer")
    if trailer_evidence["fields"].get("x-interop-checksum") != sha256(HELLO):
        raise ValueError("nghttp 尾 HEADERS 的校验和不符")
    entries = read_har(har, {base + path: (206 if path == "/range.bin" else 200, len(contents[path])) for path in multi_paths})
    results = []
    for index, (path, expected) in enumerate(contents.items()):
        har = directory / f"body-{index}.har"
        encoding = "gzip" if path == "/gzip" else "identity"
        record = runner.run(f"nghttp-body-{index}", nghttp_options(args, har) + ["-H", "accept-encoding: " + encoding, base + path],
                            env=tool_environment(args, args.nghttp))
        actual = Path(record["stdout"]).read_bytes()
        entry = read_har(har, {base + path: (206 if path == "/range.bin" else 200, None if path == "/gzip" else len(expected))})[0]
        headers = {item["name"].lower(): item["value"] for item in entry["response"]["headers"]}
        if actual != expected:
            raise ValueError(f"nghttp 完整 body 不符：{path}；raw stdout 保留实际数据")
        if path == "/gzip" and headers.get("content-encoding") != "gzip":
            raise ValueError("gzip 路由没有实际发送 gzip 编码")
        if path == "/range.bin" and headers.get("content-range") != f"bytes 717-87717/{payload.stat().st_size}":
            raise ValueError("固定范围响应缺少正确 Content-Range")
        results.append({"path": path, "body_size": len(actual), "body_sha256": sha256(actual), "headers": headers,
                        "gzip_note": "nghttp 自动解压 stdout；同时核对实际 Content-Encoding" if path == "/gzip" else None})
    posted = args.out / "fixtures" / "posted.bin"
    har = directory / "post.har"
    record = runner.run("nghttp-post", nghttp_options(args, har) + ["-H", "accept-encoding: identity", "-d", tool_argument(posted, args.nghttp), base + "/echo"],
                        env=tool_environment(args, args.nghttp))
    read_har(har, {base + "/echo": (200, posted.stat().st_size)})
    if Path(record["stdout"]).read_bytes() != posted.read_bytes():
        raise ValueError("POST echo 没有返回完整上传体")
    return {"multiplex": {"streams": stream_ids, "transactions": len(entries), "connections": 1}, "body_checks": results,
            "post_body_sha256": file_record(posted)["sha256"], "trailer_evidence": trailer_evidence}


def run_client(args, runner: Runner, *, port: int, driver: str, mode: str, expected: Path,
               method: str = "GET", target: str = "/payload.bin", expect_protocol: str = "h2", count: int = 8,
               allow_failure: bool = False) -> tuple[dict, str]:
    run_id = uuid.uuid4().hex
    report_path = args.out / "client-reports" / f"{run_id}.json"
    command = [str(args.client_driver), "--host", "127.0.0.1", "--port", str(port), "--target", target,
               "--driver", driver, "--mode", mode, "--expect-protocol", expect_protocol, "--run-id", run_id,
               "--expected", str(expected), "--report", str(report_path), "--method", method,
               "--ca", str(args.ca), "--tls", "on", "--requests", str(count), "--concurrency", str(count), "--timeout-ms", "20000"]
    if method == "POST":
        command += ["--body", str(expected)]
    record = runner.run("client-" + driver + "-" + mode + "-" + method.lower(), command, check=not allow_failure)
    report = json.loads(report_path.read_text(encoding="utf-8"))
    if report["run_id"] != run_id or report["requests"] != count or len(report["results"]) != count:
        raise ValueError("客户端结构报告身份或数量不符")
    if allow_failure:
        if record.get("timed_out") or record["exit_code"] != 2 or report["ok"] or not all(item.get("protocol_negotiation_failed") for item in report["results"]):
            raise ValueError("强制 H2 对 H1 的拒绝没有明确归类为协议协商失败")
    elif not report["ok"] or not all(item.get("ok") and item.get("body_matches") and item.get("protocol") == expect_protocol for item in report["results"]):
        raise ValueError("客户端结构报告含响应、body 或协议失败")
    return report, run_id


def nghttpd_connections(log: str, run_id: str, count: int) -> dict:
    pattern = r"\[id=(\d+)\][^\n]*recv \(stream_id=(\d+)[^\n]*x-interop-run:\s*" + re.escape(run_id)
    pairs = {(int(session), int(stream)) for session, stream in re.findall(pattern, log)}
    sessions = {session for session, stream in pairs}
    if len(sessions) != 1 or len(pairs) != count:
        raise ValueError(f"nghttpd 对 {run_id} 未证明一连接 {count} 个真实 stream：{sorted(pairs)}")
    return {"run_id": run_id, "session_id": next(iter(sessions)), "stream_ids": sorted(stream for session, stream in pairs)}


def check_client(args, runner: Runner, payload: Path) -> dict:
    # nghttpd 未公布端口 0 的实际分配值；仅为此外部工具预留临时端口，若交接竞争则明确失败。
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    child = runner.start("nghttpd", [str(args.nghttpd), "-v", "-a", "127.0.0.1", "--echo-upload",
        "--trailer=x-upstream-proof: nghttpd", "-d", tool_argument(payload.parent, args.nghttpd), str(port),
        tool_argument(args.key, args.nghttpd), tool_argument(args.cert, args.nghttpd)], lifetime=args.server_lifetime)
    context = ssl.create_default_context(cafile=str(args.ca))
    context.set_alpn_protocols(["h2"])

    def ready():
        with socket.create_connection(("127.0.0.1", port), timeout=0.5) as raw:
            with context.wrap_socket(raw, server_hostname="127.0.0.1") as tls:
                if tls.selected_alpn_protocol() != "h2":
                    raise ValueError("nghttpd 就绪探测未协商出 h2")
                return {"port": port, "alpn": "h2", "certificate_verified": True}

    reports = []
    try:
        readiness = wait_ready(child, ready, min(args.timeout, 20))
        for driver in ("shared", "pool-lease", "pool"):
            for method in ("GET", "POST"):
                expected = payload if method == "GET" else args.out / "fixtures" / "posted.bin"
                report, run_id = run_client(args, runner, port=port, driver=driver, mode="h2", expected=expected, method=method)
                for item in report["results"]:
                    trailers = {field["name"].lower(): field["value"] for field in item.get("trailers", [])}
                    if trailers.get("x-upstream-proof") != "nghttpd":
                        raise ValueError("生产客户端没有保留 nghttpd 的完整尾字段")
                log = Path(child[1]["stdout"]).read_text(encoding="utf-8", errors="replace")
                proof = nghttpd_connections(log, run_id, 8)
                reports.append({"driver": driver, "method": method, "report": report, "connection_proof": proof})
        return {"readiness": readiness, "runs": reports}
    finally:
        runner.stop(child)


def junit_cases(path: Path) -> list[dict]:
    root = ET.parse(path).getroot()
    cases = []

    def visit(element, suites):
        tag = element.tag.rsplit("}", 1)[-1]
        if tag == "testsuite":
            suites = suites + [element.get("name", "")]
        if tag != "testcase":
            for child in element:
                visit(child, suites)
            return
        failure_details = []
        for child in element:
            if child.tag.rsplit("}", 1)[-1] not in ("failure", "error"):
                continue
            detail = {"kind": child.tag.rsplit("}", 1)[-1], "attributes": dict(child.attrib),
                      "payload": "".join(child.itertext())}
            if len(child):
                detail["child_xml"] = [ET.tostring(node, encoding="unicode") for node in child]
            failure_details.append(detail)
        failures = [item["payload"] or item["attributes"].get("message", "") for item in failure_details]
        skipped = ["".join(child.itertext()) or child.get("message", "")
                   for child in element if child.tag.rsplit("}", 1)[-1] == "skipped"]
        identity = " ".join(suites + [element.get("classname", ""), element.get("package", "")]).lower()
        standard = "RFC7541" if "hpack" in identity else "h2spec generic" if "generic" in identity else "RFC7540" if "http2" in identity or "hypertext transfer" in identity else "待按原始 suite 归类"
        cases.append({"suite": suites, "package": element.get("package", ""),
                      "classname": element.get("classname", ""), "name": element.get("name", ""),
                      "original_attributes": dict(element.attrib), "status": "failed" if failures else "skipped" if skipped else "passed",
                      "failures": failures, "failure_details": failure_details, "skipped": skipped, "standard": standard,
                      "rfc9113_review": "须逐项核对，未自动豁免" if failures or skipped else "仅表示该旧版用例通过"})

    visit(root, [])
    if not cases:
        raise ValueError("h2spec JUnit 没有任何 testcase，不能把零测试当通过")
    return cases


def h2spec_case_key(case: dict) -> tuple[str, str, str]:
    return (case.get("package", case.get("original_attributes", {}).get("package", "")),
            case["classname"], case.get("name", ""))


def canonical_hash(value: object) -> str:
    return sha256(json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))


def h2spec_signature(case: dict) -> dict:
    package, classname, name = h2spec_case_key(case)
    return {"package": package, "classname": classname, "name": name, "failure_details": case["failure_details"]}


def load_h2spec_review(path: Path | None) -> dict | None:
    """只接受已独立审查的七项固定签名，不从名称模式推导豁免。"""
    if path is None:
        return None
    raw = path.read_bytes()
    review = json.loads(raw)
    if not isinstance(review, dict):
        raise ValueError("h2spec 审查文件必须是 JSON 对象")
    if review.get("schema") != 1 or review.get("suite_version") != "2.6.0" or review.get("suite_commit") != H2SPEC_COMMIT:
        raise ValueError("h2spec 审查文件的 schema、版本或固定提交不符")
    entries = review.get("reviewed_differences", [])
    if not isinstance(entries, list) or any(not isinstance(entry, dict) or not all(
            name in entry for name in ("package", "classname", "name", "failure_details")) for entry in entries):
        raise ValueError("h2spec 审查文件的精确案例结构不完整")
    if len(entries) != 7 or len({h2spec_case_key(case) for case in entries}) != 7:
        raise ValueError("h2spec 审查文件必须恰好包含七项不同的精确案例")
    signatures = sorted((h2spec_signature(case) for case in entries), key=h2spec_case_key)
    if canonical_hash(signatures) != H2SPEC_REVIEW_SIGNATURE:
        raise ValueError("h2spec 审查文件改变了已核对的 package、classname 或完整失败载荷")
    for entry in entries:
        if not entry.get("reason") or not entry.get("boundary") or not entry.get("references"):
            raise ValueError("h2spec 每项差异必须保留规范依据、适用边界和引用")
    return {"file": {"path": str(path), "available": True, "size": len(raw), "sha256": sha256(raw)},
            "suite_version": review["suite_version"], "suite_commit": review["suite_commit"],
            "reviewed_differences": entries}


def evaluate_h2spec(raw_runs: list[dict], *, strict: bool, review: dict | None) -> dict:
    """校验执行证据，再对有效输入中的精确失败应用显式审查结论。"""
    issues = []
    full, corrected = raw_runs
    full_cases, sized_cases = full["cases"], corrected["cases"]
    expected_count, expected_identity = H2SPEC_CASE_IDENTITIES[strict]
    full_keys = [h2spec_case_key(case) for case in full_cases]
    if len(full_keys) != expected_count or len(set(full_keys)) != expected_count:
        issues.append("完整套件缺少、增加或重复 testcase")
    if canonical_hash(sorted(full_keys)) != expected_identity:
        issues.append("完整套件的精确案例身份集合与固定提交不符")
    expected_key = tuple(corrected["expected_case"])
    if len(sized_cases) != 1 or h2spec_case_key(sized_cases[0]) != expected_key or full_keys.count(expected_key) != 1:
        issues.append("精确输入复验未发现预期的唯一用例")
    for run in raw_runs:
        if run.get("parse_error"):
            issues.append(f"{run['profile']} JUnit 缺失或解析失败：{run['parse_error']}")
        if run["timed_out"] or run.get("start_failed") or run.get("terminated_by_driver"):
            issues.append(f"{run['profile']} 进程超时或异常终止")
        failures = any(case["status"] == "failed" for case in run["cases"])
        if run["exit_code"] != (1 if failures else 0):
            issues.append(f"{run['profile']} 退出码与固定 h2spec 的用例结果不符")
        if any(case["skipped"] or case["status"] == "skipped" for case in run["cases"]):
            issues.append(f"{run['profile']} 含 skipped 用例")
    if sized_cases and any(case["status"] != "passed" for case in sized_cases):
        issues.append("精确输入复验没有实际通过")
    input_failure = ("GOAWAY Frame (Error Code: FRAME_SIZE_ERROR)\nConnection closed\n"
                     "RST_STREAM Frame (length:4, flags:0x00, stream_id:1)"
                     if full["max_header_length"] < 4000 else
                     "HEADERS Frame (stream_id:1)\nRST_STREAM Frame (length:4, flags:0x00, stream_id:1)")
    for case in full_cases:
        if h2spec_case_key(case) == expected_key and case["status"] != "passed":
            if case["status"] != "failed" or case["failure_details"] != [
                    {"kind": "error", "attributes": {}, "payload": input_failure}]:
                issues.append("被替换输入案例出现了未经核对的新失败载荷")
    cases = [sized_cases[0] if h2spec_case_key(case) == expected_key and len(sized_cases) == 1 else case
             for case in full_cases]
    matched = []
    reviewed = {h2spec_case_key(entry): entry for entry in review["reviewed_differences"]} if review else {}
    for case in cases:
        if case["status"] == "passed":
            continue
        entry = reviewed.get(h2spec_case_key(case))
        if case["status"] == "failed" and not case["skipped"] and entry and h2spec_signature(case) == h2spec_signature(entry):
            matched.append({"package": entry["package"], "classname": entry["classname"], "name": entry["name"],
                            "reason": entry["reason"], "boundary": entry["boundary"], "references": entry["references"]})
        else:
            issues.append(f"未经精确审查接受的结果：{h2spec_case_key(case)!r}")
    raw_ok = not issues and all(case["status"] == "passed" for run in raw_runs for case in run["cases"])
    return {"standards": STANDARDS, "cases": cases, "raw_runs": raw_runs,
            "exit_code": full["exit_code"], "timed_out": any(run["timed_out"] for run in raw_runs),
            "executed_case_count": sum(len(run["cases"]) for run in raw_runs), "effective_case_count": len(cases),
            "raw_ok": raw_ok, "accepted": not issues,
            "accepted_with_reviewed_differences": not issues and bool(matched),
            "review": review, "matched_reviewed_differences": matched, "rejection_reasons": issues,
            "input_profile_adjustment": {"case": list(expected_key), "full_max_header_length": full["max_header_length"],
                                         "verified_max_header_length": corrected["max_header_length"],
                                         "notice": "完整原始结果保留；有效集合只使用该唯一案例的合适输入复验"}}


class H2SpecRejected(ValueError):
    def __init__(self, result: dict):
        super().__init__("h2spec 未获接受；原始失败和具体拒绝原因保留于 h2spec-cases.json")
        self.result = result


def check_h2spec(args, runner: Runner, ready: dict) -> dict:
    report_path = args.out / "h2spec-junit.xml"
    def command_for(specs, length, destination):
        command = [str(args.h2spec), *specs, "--tls", "--host", "127.0.0.1", "--port", str(ready["port"]),
                   "--timeout", "2", "--max-header-length", str(length), "--junit-report", str(destination)]
        if args.external_insecure:
            command.append("--insecure")
        if args.h2spec_strict:
            command.append("--strict")
        return command

    command = command_for(["http2", "hpack", "generic"], args.h2spec_max_header_length, report_path)
    record = runner.run("h2spec", command, timeout=args.h2spec_timeout, check=False)
    def raw_run(profile, length, record, path):
        result = {"profile": profile, "max_header_length": length, "exit_code": record["exit_code"],
                  "timed_out": record.get("timed_out", False), "start_failed": record.get("start_failed", False),
                  "terminated_by_driver": record.get("terminated_by_driver", False), "junit": file_record(path), "cases": []}
        try:
            result["cases"] = junit_cases(path)
        except (OSError, ValueError, ET.ParseError) as error:
            result["parse_error"] = str(error)
        return result

    raw_runs = [raw_run("full", args.h2spec_max_header_length, record, report_path)]
    # 两个旧用例共享 dummy 长度，却分别需要“预算内头块”和“超过 16KiB 的单帧”。
    # 精确重跑一项，保留原始整组结果；不把不同输入的失败当成生产正确性结论。
    if args.h2spec_max_header_length < 4000:
        target, length, package, name = ("http2/4.2/3", 4000, "http2/4.2",
                                        "Sends a large size HEADERS frame that exceeds the SETTINGS_MAX_FRAME_SIZE")
    else:
        target, length, package, name = ("http2/6.10/1", 3000, "http2/6.10",
                                        "Sends multiple CONTINUATION frames preceded by a HEADERS frame")
    corrected_path = args.out / "h2spec-sized-case-junit.xml"
    corrected_record = runner.run("h2spec-sized-case", command_for([target], length, corrected_path),
                                  timeout=args.h2spec_timeout, check=False)
    sized = raw_run(target, length, corrected_record, corrected_path)
    sized["expected_case"] = [package, name, ""]
    raw_runs.append(sized)
    result = evaluate_h2spec(raw_runs, strict=args.h2spec_strict, review=args.h2spec_review)
    write_json(args.out / "h2spec-cases.json", result)
    if not result["accepted"]:
        raise H2SpecRejected(result)
    return result


def check_h2load(args, runner: Runner, ready: dict) -> dict:
    record = runner.run("h2load", [str(args.h2load), "-n", "16", "-c", "1", "-m", "8", f"https://127.0.0.1:{ready['port']}/hello"])
    text = Path(record["stdout"]).read_text(encoding="utf-8", errors="replace")
    counts = re.search(r"requests:\s*(\d+) total,\s*(\d+) started,\s*(\d+) done,\s*(\d+) succeeded,\s*(\d+) failed,\s*(\d+) errored,\s*(\d+) timeout", text)
    if not counts or tuple(map(int, counts.groups())) != (16, 16, 16, 16, 0, 0, 0) or "Application protocol: h2" not in text:
        raise ValueError("h2load 未证明单连接多流的 16 个 H2 请求全部完成")
    return {"requests": 16, "connections": 1, "streams": 8, "note": "仅做互操作冒烟，不用于性能结论"}


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server-driver", "client-driver", "nghttp", "nghttpd", "h2load", "h2spec", "cert", "key", "ca"):
        parser.add_argument("--" + name, type=lambda value: Path(value).resolve())
    parser.add_argument("--out", type=lambda value: Path(value).resolve(), required=True)
    parser.add_argument("--source-root", type=lambda value: Path(value).resolve(), default=REPOSITORY)
    parser.add_argument("--build-source-identity", help="实际构建使用的源码版本或快照标识，不能用当前 HEAD 代替")
    parser.add_argument("--prepare-only", action="store_true", help="只冻结输入哈希，不启动任何子进程或网络")
    parser.add_argument("--section", action="append", choices=("server", "client", "h1", "h2spec", "h2load"))
    parser.add_argument("--timeout", type=float, default=40)
    parser.add_argument("--h2spec-timeout", type=float, default=180)
    parser.add_argument("--h2spec-max-header-length", type=int, default=3000,
                        help="每个 h2spec dummy 字段长度；默认两份加伪头仍处于服务端 8192 字节预算内")
    parser.add_argument("--h2spec-rfc9113-review", type=lambda value: Path(value).resolve(),
                        help="显式采用七项已独立审查的 RFC 差异；仍保留原始失败，默认不接受差异")
    parser.add_argument("--server-lifetime", type=int, default=900)
    parser.add_argument("--external-insecure", action="store_true", help="仅允许外部本地测试工具跳过测试证书信任；生产客户端仍验证 CA/主机")
    parser.add_argument("--h2spec-strict", action="store_true")
    parser.add_argument("--expected-nghttp-version", default="1.69.0")
    args = parser.parse_args()
    if not 1 <= args.timeout <= 300 or not 1 <= args.h2spec_timeout <= 900 or not 5 <= args.server_lifetime <= 1800:
        parser.error("timeout/server-lifetime 必须为有限合法范围")
    if not 1 <= args.h2spec_max_header_length <= 65536:
        parser.error("h2spec-max-header-length 必须位于 1～65536")
    args.section = args.section or ["server", "client", "h1", "h2spec", "h2load"]
    args.h2spec_review = load_h2spec_review(args.h2spec_rfc9113_review)
    if not args.prepare_only:
        for name in ("server_driver", "client_driver", "nghttp", "nghttpd", "h2load", "h2spec", "cert", "key", "ca"):
            path = getattr(args, name)
            if path is None or not path.is_file():
                parser.error(f"真实运行要求存在的 --{name.replace('_', '-')} 文件")
    return args


def main() -> int:
    args = arguments()
    if args.out.exists() and any(args.out.iterdir()):
        raise ValueError("输出目录非空，拒绝把旧结果混入本轮；请指定新的 --out")
    args.out.mkdir(parents=True, exist_ok=True)
    inputs = {}
    for name in ("server_driver", "client_driver", "nghttp", "nghttpd", "h2load", "h2spec", "cert", "key", "ca"):
        path = getattr(args, name)
        # 私钥只记录路径和哈希，不复制或输出内容。
        inputs[name] = file_record(path) if path else {"available": False, "path": None}
    report = {"schema": 1, "started_at_utc": utc_now(), "status": "prepared_not_executed", "task29_accepted": False,
              "inputs": inputs, "sources": source_records(args.source_root), "build_source_identity": args.build_source_identity,
              "standards": STANDARDS, "sections": args.section, "external_insecure": args.external_insecure,
              "h2spec_rfc9113_review": args.h2spec_review,
              "implementation_library_requirement": "nghttp2>=1.70,<2；外部工具1.69不是实现依赖", "stages": [], "commands": []}
    if os.name == "nt" and args.nghttp:
        local_nss = args.nghttp.parent.parent / "etc" / "nsswitch.conf"
        if local_nss.is_file():
            report["local_tool_nss_config"] = file_record(local_nss)
    write_json(args.out / "report.json", report)
    if args.prepare_only:
        print(f"已冻结准备报告：{args.out / 'report.json'}；未构建、未启动程序、未运行互操作。")
        return 0
    runner = Runner(args.out, args.timeout)
    failed = False

    def stage(name, action):
        nonlocal failed
        item = {"name": name, "started_at_utc": utc_now()}
        report["stages"].append(item)
        try:
            item["result"] = action()
            item["status"] = ("accepted_with_reviewed_differences"
                              if item["result"].get("accepted_with_reviewed_differences") else "passed")
        except Exception as error:
            failed = True
            item.update(status="failed", error=str(error), traceback=traceback.format_exc())
            if isinstance(error, H2SpecRejected):
                item["result"] = error.result
        item["ended_at_utc"] = utc_now()
        report["commands"] = runner.commands
        write_json(args.out / "report.json", report)

    try:
        versions = {}
        needed_tools = {"nghttp": "server", "nghttpd": "client", "h2load": "h2load", "h2spec": "h2spec"}
        for name in ("nghttp", "nghttpd", "h2load", "h2spec"):
            if needed_tools[name] not in args.section:
                continue
            record = runner.run(name + "-version", [str(getattr(args, name)), "--version"])
            observed = Path(record["stdout"]).read_text(encoding="utf-8", errors="replace").strip()
            versions[name] = observed
            if name == "h2spec":
                if observed != f"Version: 2.6.0 ({H2SPEC_COMMIT})":
                    raise ValueError("h2spec 必须为已固定的官方 2.6.0/提交")
            elif args.expected_nghttp_version not in observed:
                raise ValueError(f"{name} 工具版本与预期不符：{observed}")
        report["versions"] = versions
        fixture = args.out / "fixtures"
        fixture.mkdir()
        payload = fixture / "payload.bin"
        payload.write_bytes(bytes(range(256)) * 1024)
        (fixture / "posted.bin").write_bytes(bytes(range(256)) * 257)
        hello_file = fixture / "hello.txt"
        hello_file.write_bytes(HELLO)
        report["fixtures"] = [file_record(path) for path in sorted(fixture.iterdir())]
        if any(section in args.section for section in ("server", "h2spec", "h2load")):
            with hical_server(args, runner, "h2", payload) as ready:
                report["h2_ready"] = ready
                if "server" in args.section:
                    stage("nghttp-to-hical", lambda: check_server(args, runner, ready, payload))
                if "h2spec" in args.section:
                    stage("h2spec", lambda: check_h2spec(args, runner, ready))
                if "h2load" in args.section:
                    stage("h2load-smoke", lambda: check_h2load(args, runner, ready))
        if "client" in args.section:
            stage("hical-client-to-nghttpd", lambda: check_client(args, runner, payload))
        if "h1" in args.section:
            with hical_server(args, runner, "h1", payload) as ready:
                for mode in ("h1", "negotiate"):
                    stage("h1-fallback-" + mode, lambda mode=mode: run_client(args, runner, port=ready["port"], driver="shared",
                        mode=mode, expected=hello_file, expect_protocol="h1", target="/hello")[0])
                stage("h2-required-rejects-h1", lambda: run_client(args, runner, port=ready["port"], driver="shared",
                    mode="h2", expected=hello_file, target="/hello", count=1, allow_failure=True)[0])
    except BaseException as error:
        failed = True
        report["fatal_error"] = {"error": str(error), "traceback": traceback.format_exc()}
    finally:
        cleanup = runner.close()
        failed = failed or bool(cleanup)
        reviewed_differences = any(item["status"] == "accepted_with_reviewed_differences" for item in report["stages"])
        status = ("failed" if failed else "component_scenarios_accepted_with_reviewed_differences"
                  if reviewed_differences else "component_scenarios_passed")
        report.update(status=status, accepted_with_reviewed_differences=reviewed_differences and not failed,
                      status_explanation="有已审差异；原始 h2spec 失败没有改为通过" if reviewed_differences else "无已审差异被采用",
                      ended_at_utc=utc_now(),
                      commands=runner.commands, cleanup_errors=cleanup)
        write_json(args.out / "report.json", report)
    print(f"{report['status']}：{args.out / 'report.json'}；其他平台和检测结果需单独验收。")
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        if hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(encoding="utf-8")
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"错误：{error}", file=sys.stderr)
        sys.exit(1)
