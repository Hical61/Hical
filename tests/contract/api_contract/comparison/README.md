# 房间接口对照

先用同一份联调脚本检查 Hical 原型（宏轨和原生轨）、手写 Hical 和 oatpp。每次启动时房间列表都为空。手写版沿用原型的启动代码、DTO 和房间存储，路由和 OpenAPI 则单独注册。运行步骤见[房间示例](../../../../examples/api_contract/README.md)和 [oatpp 对照](oatpp/README.md)。

```sh
for mode in macro native; do
    cmake -S examples/api_contract -B "build/rooms-$mode" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++-16 \
        -DCMAKE_PREFIX_PATH="/path/to/hical/$mode/install;/path/to/boost/install"
    cmake --build "build/rooms-$mode" --parallel 2
done
cmake -S tests/contract/api_contract/comparison/hical_manual -B build/rooms-manual \
    -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++-16 \
    -DCMAKE_PREFIX_PATH="/path/to/hical/macro/install;/path/to/boost/install"
cmake --build build/rooms-manual --parallel 2
python scripts/check_api_contract.py --positive-only \
    --server build/rooms-manual/hical_rooms_manual --output-dir build/rooms-manual-evidence
```

压测前，请先结束其他构建和测试，再给服务和客户端选不同的 CPU。脚本先给每个服务建两个房间。预热后，用一个 wrk 线程和八条连接反复请求 `GET /rooms`。

每轮会换一个服务顺序。脚本会核对压测前后的响应，用同一个 Lua 回调数出实际返回了多少个 `200`，同时记录延迟、进程 CPU、RSS 和原始日志。这些数字对应房间列表接口，连接处理、序列化、线程模型和客户端回调都会影响结果。

```sh
python tests/contract/api_contract/comparison/measure.py \
    --wrk /path/to/wrk --server-cpu 0 --client-cpu 1 --rounds 5 --seconds 6 \
    --server hical-manual build/rooms-manual/hical_rooms_manual \
    --server hical-macro build/rooms-macro/api_contract_server \
    --server hical-native build/rooms-native/api_contract_server \
    --server oatpp build-oatpp-room-contract/oatpp_room_contract \
    --output-dir build/rooms-load-evidence
```
