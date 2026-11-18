"""联调用的测试服务，可让列表返回错误的数据结构或状态码，并按测试模式调整 OpenAPI。"""

from copy import deepcopy
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import sys


def run(mode, spec_path, room_path):
    spec = json.loads(Path(spec_path).read_bytes())
    room = json.loads(Path(room_path).read_bytes())
    if mode == "breaking":
        list_responses = spec["paths"]["/rooms"]["get"]["responses"]
        get_responses = spec["paths"]["/rooms/{id}"]["get"]["responses"]
        list_responses["200"]["content"] = deepcopy(get_responses["200"]["content"])

    class Fixture(BaseHTTPRequestHandler):
        def send_json(self, status, value):
            body = json.dumps(value, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/openapi.json":
                self.send_json(200, spec)
            elif self.path == "/rooms":
                payload = {"rooms": [room], "total": 1} if mode == "wrong-status" else room
                self.send_json(202 if mode == "wrong-status" else 200, payload)
            elif self.path == f"/rooms/{room['id']}":
                self.send_json(200, room)
            else:
                self.send_json(404, {"message": "房间不存在"})

        def do_POST(self):
            if self.path != "/rooms":
                self.send_json(404, {"message": "房间不存在"})
                return
            data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            room.update({"room_name": data["room_name"], "capacity": data["capacity"]})
            self.send_json(201, room)

    with HTTPServer(("127.0.0.1", 0), Fixture) as server:
        print(f"PORT={server.server_port}", flush=True)
        server.serve_forever()


if __name__ == "__main__":
    run(*sys.argv[1:])
