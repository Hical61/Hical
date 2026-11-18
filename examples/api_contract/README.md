# 房间 API 契约示例

这个示例用 `find_package(hical CONFIG REQUIRED)` 查找 Hical。先安装启用了 `HICAL_WITH_OPENAPI` 的 Hical，再把下面的安装前缀换成你的路径。C++20 宏轨和 C++26 原生轨共用一套房间业务代码。要验证两轨，请用各自的工具链安装 Hical，并为每轨指定单独的构建目录。

```sh
cmake -S examples/api_contract -B build/room-example -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/hical/install
cmake --build build/room-example --config Release
./build/room-example/api_contract_server 0
```

服务只监听 `127.0.0.1`。省略端口或传入 `0`，系统会选一个端口。服务开始监听后会打印 `PORT=<实际端口>`。如果在 Windows 上使用多配置生成器，去 `build/room-example/Release/` 找可执行文件。按 `Ctrl+C` 可以退出服务。

把下面的端口换成服务打印的值，再试试创建、查询和列出房间。每次启动时列表都为空，由同一个 I/O 线程读写。

```sh
base=http://127.0.0.1:8080
curl -fsS -H 'Content-Type: application/json' -d '{"room_name":"大厅","capacity":10}' "$base/rooms"
curl -fsS "$base/rooms/1"
curl -fsS "$base/rooms"
curl -fsS "$base/openapi.json"
```

创建房间时，`room_name` 必须填写且不能为空，`capacity` 必须是 `1` 到 `100` 之间的 32 位整数。创建成功返回 `201`，查询和列表成功返回 `200`。列表响应里有 `rooms` 和 `total`。如果请求体或房间编号不合法，服务返回 `400`。编号合法但找不到房间时，服务返回 `404`。两种错误响应都带有 `message`。

`dto.h` 里能看到两轨的 DTO 写法。`app.h` 负责存取房间，`app.cpp` 一起注册同步处理函数和对应的 OpenAPI 说明。示例生成文档时会补上房间编号的取值范围。如果同一个 DTO 的输入和输出生成了不同的 Schema，请使用不同的组件名，或直接内联 Schema。注册时，同名组件的内容必须一致。

也可以单独构建同步分派测试。它先在编译时检查哪些函数签名能注册，再通过 `Router` 的公开接口验证同步分派。

```sh
cmake -S tests/contract/api_contract/compile_cases -B build/room-compile-cases \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/hical/install
cmake --build build/room-compile-cases --config Release
ctest --test-dir build/room-compile-cases -C Release --output-on-failure
```

要跑完整联调，请在 Linux x86_64 上建一个 Python 3.14 虚拟环境，再装测试依赖。脚本会启动和关闭服务，发出实际 HTTP 请求，检查 OpenAPI，运行同一份 TypeScript 调用代码，并单独跑三个故障用例。Node、JRE 和生成器的版本见[客户端验证说明](../../tests/contract/api_contract/client/README.md)。

```sh
python -m pip install --require-hashes -r tests/contract/api_contract/requirements-linux.lock
python scripts/check_api_contract.py --server build/room-example/api_contract_server --output-dir build/room-evidence
```

每次运行请指定一个新输出目录。如果只想检查 HTTP 接口，加上 `--http-only` 即可，这时只需要 Python 标准库。
