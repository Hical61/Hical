#!/usr/bin/env python3
"""启动房间示例，通过 HTTP 检查接口、OpenAPI 文档和生成的客户端。"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import re
import subprocess
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener


HTTP_TIMEOUT = 3
READY_TIMEOUT = 15
API_ROOT = Path(__file__).resolve().parents[1] / "tests/contract/api_contract"


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, *_args, **_kwargs):
        return None


OPENER = build_opener(ProxyHandler({}), NoRedirect())


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def request(base, method, route, payload=None):
    headers = {"Content-Type": "application/json"} if payload is not None else {}
    req = Request(base + route, method=method, data=payload, headers=headers)
    try:
        response = OPENER.open(req, timeout=HTTP_TIMEOUT)
    except HTTPError as error:
        response = error
    with response:
        content_type = response.headers.get_content_type()
        require(content_type == "application/json", f"{method} {route}: {content_type}")
        return response.status, json.load(response)


def json_bytes(value):
    return json.dumps(value, ensure_ascii=False).encode("utf-8")


def save_json(path, value):
    path.write_bytes(json_bytes(value) + b"\n")


def check_operations(document):
    paths = document["paths"]
    for route, verb, operation, statuses in (
        ("/rooms", "post", "createRoom", {"201", "400"}),
        ("/rooms/{id}", "get", "getRoom", {"200", "400", "404"}),
        ("/rooms", "get", "listRooms", {"200"}),
    ):
        declared = paths[route][verb]
        require(declared["operationId"] == operation and set(declared["responses"]) == statuses,
                f"{verb} {route} 的操作名或响应状态码与预期不同")
    parameters = paths["/rooms/{id}"]["get"]["parameters"]
    identifier = next((p for p in parameters if p["in"] == "path" and p["name"] == "id"), None)
    require(identifier and identifier["required"]
            and identifier["schema"]["type"] == "integer"
            and identifier["schema"]["format"] == "int32"
            and identifier["schema"]["minimum"] == 1
            and identifier["schema"]["maximum"] == 2147483647,
            "文档里的房间编号应为必填的 int32，范围是 1 到 2147483647")


@contextmanager
def serve(command, evidence):
    evidence.mkdir()
    log_path = evidence / "server.log"
    with log_path.open("wb") as stream:
        proc = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=stream)
        try:
            deadline = time.monotonic() + READY_TIMEOUT
            while True:
                require(proc.poll() is None, f"服务提前退出，退出码是 {proc.returncode}。请看 {log_path}")
                text = log_path.read_text(encoding="utf-8", errors="replace")
                port = re.search(r"(?m)^PORT=(\d{1,5})\s*$", text)
                if port and 1 <= int(port[1]) <= 65535:
                    base = f"http://127.0.0.1:{port[1]}"
                    try:
                        status, document = request(base, "GET", "/openapi.json")
                        require(status == 200 and isinstance(document.get("paths"), dict),
                                "服务没有返回有效的 OpenAPI 文档")
                        break
                    except (URLError, TimeoutError, ConnectionError):
                        pass
                require(time.monotonic() < deadline, f"等到超时，服务仍没就绪。请看 {log_path}")
                time.sleep(0.05)
            document_path = evidence / "openapi.json"
            save_json(document_path, document)
            yield base, document_path, document, proc
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)


def exercise(base, request_case):
    """按固定预期检查房间业务，也试试类型不对或超出范围的输入。"""
    def call(method, route, status, payload=None):
        received, body = request_case(base, method, route, payload)
        require(received == status, f"{method} {route}: {received} != {status}, {body!r}")
        if status in (400, 404):
            require(isinstance(body, dict) and set(body) == {"message"}
                    and isinstance(body["message"], str) and body["message"],
                    f"{method} {route} 的错误响应应只有一条非空 message，实际收到 {body!r}")
        return body

    def list_equals(body, rooms):
        require(isinstance(body, dict) and set(body) == {"rooms", "total"}
                and body["rooms"] == rooms and type(body["total"]) is int
                and body["total"] == len(rooms), f"返回的 rooms 或 total 与预期不同，实际收到 {body!r}")

    list_equals(call("GET", "/rooms", 200), [])
    for bad in (b"{", b"[]", b"{}", b'{"room_name":"A"}', b'{"capacity":1}',
                b'{"room_name":"","capacity":1}', b'{"room_name":5,"capacity":1}',
                b'{"room_name":null,"capacity":1}', b'{"room_name":"A","capacity":null}',
                b'{"room_name":"A","capacity":0}', b'{"room_name":"A","capacity":101}',
                b'{"room_name":"A","capacity":1.5}', b'{"room_name":"A","capacity":1.0}',
                b'{"room_name":"A","capacity":1e0}', b'{"room_name":"A","capacity":4294967297}',
                b'{"room_name":"A","capacity":"1"}'):
        call("POST", "/rooms", 400, bad)
    for invalid_id in ("0", "-1", "x", "1x", "2147483648"):
        call("GET", f"/rooms/{invalid_id}", 400)
    call("GET", "/rooms/2147483647", 404)
    list_equals(call("GET", "/rooms", 200), [])
    rooms = []
    for name, cap in (("大厅", 1), ("满容量", 100)):
        body = call("POST", "/rooms", 201, json_bytes({"room_name": name, "capacity": cap}))
        require(isinstance(body, dict) and set(body) == {"id", "room_name", "capacity"}
                and type(body["id"]) is int and 1 <= body["id"] <= 2147483647
                and type(body["capacity"]) is int and body["capacity"] == cap
                and body["room_name"] == name and all(r["id"] != body["id"] for r in rooms),
                f"创建接口返回的房间与预期不同，实际收到 {body!r}")
        require(call("GET", f"/rooms/{body['id']}", 200) == body, "查询结果和刚创建的房间不同")
        rooms.append(body)
    list_equals(call("GET", "/rooms", 200), rooms)
    return rooms


def contract_checks(document_path):
    """用固定版本 Schemathesis 4.0.0 检查响应状态码和 Schema。"""
    import schemathesis
    from schemathesis.specs.openapi.checks import status_code_conformance, response_schema_conformance

    spec = schemathesis.openapi.from_path(str(document_path))
    checks = [status_code_conformance, response_schema_conformance]

    def validate(base, method, route, payload=None):
        path = "/rooms/{id}" if route.startswith("/rooms/") else route
        options = {}
        if path == "/rooms/{id}":
            options["path_parameters"] = {"id": route.rsplit("/", 1)[1]}
        if payload is not None:
            options.update(body=payload, media_type="application/json")
        case = spec[path][method].Case(**options)
        response = case.call(base_url=base, timeout=HTTP_TIMEOUT, allow_redirects=False)
        case.validate_response(response, checks=checks)
        return response.status_code, response.json()

    return validate


def sdk(base, document_path, output, breaking=False):
    command = [sys.executable, str(API_ROOT / "client/run_client.py"),
               "--base-url", base, "--openapi", str(document_path),
               "--output-dir", str(output)]
    # 客户端脚本给 npm、生成、编译和调用分别设了超时，超时后会清理对应进程。
    result = subprocess.run(command, check=False)
    if not breaking:
        require(result.returncode == 0, f"客户端验证失败，退出码是 {result.returncode}。请看 {output}")
        return
    require(result.returncode == 12, f"改坏列表接口后，客户端本应在 tsc 编译时失败，实际退出码是 {result.returncode}")
    compiler = (output / "tsc.log").read_text(encoding="utf-8")
    for prop in ("rooms", "total"):
        pattern = rf"^smoke\.ts\(\d+,\d+\): error TS2339: Property '{prop}' does not exist on type "
        require(re.search(pattern, compiler, re.MULTILINE),
                f"tsc 没有报告 {prop} 字段缺失（TS2339）")


def negative_cases(document_path, room, evidence):
    from schemathesis.core.failures import FailureGroup

    snapshot = evidence / "fixture-room.json"
    save_json(snapshot, room)
    for mode, expected in (("wrong-shape", "JsonSchemaError"),
                           ("wrong-status", "UndefinedStatusCode"),
                           ("breaking", None)):
        command = [sys.executable, str(API_ROOT / "response_fixture.py"), mode,
                   str(document_path), str(snapshot)]
        with serve(command, evidence / mode) as (base, spec_path, _, _):
            validate = contract_checks(spec_path)
            if expected:
                require(spec_path.read_bytes() == document_path.read_bytes(),
                        f"{mode} 应继续使用原文档，但文档内容变了")
                try:
                    validate(base, "GET", "/rooms")
                except FailureGroup as failure:
                    names = [type(error).__name__ for error in failure.exceptions]
                    require(expected in names, f"{mode} 应报 {expected}，实际报了 {names}")
                    (evidence / mode / "expected-failure.log").write_text(
                        f"{names}\n{failure}\n", encoding="utf-8")
                else:
                    raise RuntimeError(f"{mode} 的错误响应没有被文档检查拦住")
            else:
                require(validate(base, "GET", "/rooms") == (200, room),
                        "改坏列表接口后，服务没有按新文档返回单个房间")
                sdk(base, spec_path, evidence / mode / "sdk", breaking=True)
            print(f"已确认负例：{mode}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True, help="要启动的房间服务程序")
    parser.add_argument("--output-dir", type=Path, required=True, help="保存本次结果的新目录")
    parser.add_argument("--positive-only", action="store_true", help="只检查正常的 HTTP、Schema 和 SDK")
    parser.add_argument("--http-only", action="store_true", help="只用 Python 标准库检查 HTTP 接口")
    args = parser.parse_args()
    output = args.output_dir.resolve()
    output.mkdir()
    command = [str(args.server.resolve()), "0"]
    if args.http_only:
        with serve(command, output / "http") as (base, _, _, _):
            exercise(base, request)
        print("房间接口 HTTP 验收通过")
        return

    with serve(command, output / "http") as (base, document_path, document, _):
        check_operations(document)
        validate = contract_checks(document_path)
        rooms = exercise(base, request)
        for route in ("/rooms", f"/rooms/{rooms[0]['id']}", "/rooms/0", "/rooms/2147483647"):
            require(validate(base, "GET", route) == request(base, "GET", route),
                    f"{route} 的 Schema 检查和直接 HTTP 调用返回了不同结果")
        status, created = validate(base, "POST", "/rooms",
                                   json_bytes({"room_name": "schema", "capacity": 3}))
        require(status == 201 and created["room_name"] == "schema", "创建成功时应返回 201 和刚提交的房间名")
        require(validate(base, "POST", "/rooms",
                         json_bytes({"room_name": "", "capacity": 3}))[0] == 400,
                "创建空名称房间时应返回文档里声明的 400")
    # 给客户端启动一个空服务，这样它只依赖自己创建的房间。
    with serve(command, output / "client-service") as (base, spec_path, _, _):
        sdk(base, spec_path, output / "sdk")
    if not args.positive_only:
        negative_cases(document_path, rooms[0], output)
    print("房间接口 HTTP、Schema 与生成客户端验收通过")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
