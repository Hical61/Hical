# oatpp 房间接口对照

这个对照服务用 oatpp DTO 读写数据，让 oatpp-swagger 根据端点说明生成 OpenAPI。它提供相同的三个房间接口，还能通过 `GET /openapi.json` 取文档。服务监听 `127.0.0.1`。省略端口或传 `0`，系统会选一个端口。服务开始监听后会打印 `PORT=<实际端口>`。

请在 Linux 上用 GCC 16、CMake 和 Ninja。给准备脚本指定一个新目录。脚本会下载 oatpp 和 oatpp-swagger 的 1.3.1 源码，核对 SHA-256，再构建并安装 Release 静态库。两份上游包在 CMake 里仍报 `1.3.0`，所以脚本用提交号和摘要固定版本。如果已经准备好依赖，直接用对应的安装前缀配置服务。

```sh
source_dir=tests/contract/api_contract/comparison/oatpp
oatpp_root="$HOME/hical-contract-oatpp-1.3.1"
bash "$source_dir/prepare_dependencies.sh" "$oatpp_root"
cmake -S "$source_dir" -B build-oatpp-room-contract -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++-16 \
    -DCMAKE_PREFIX_PATH="$oatpp_root/install" \
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
cmake --build build-oatpp-room-contract --parallel 2
```

用房间示例联调时的 Node、Java 和 OpenAPI Generator 环境，再跑一遍同样的 HTTP、Schema 和 TypeScript SDK 检查。请指定一个新的输出目录。

```sh
python scripts/check_api_contract.py --positive-only \
    --server build-oatpp-room-contract/oatpp_room_contract \
    --output-dir build-oatpp-contract-evidence
```

这个服务用自己的 Int32 解码器检查 JSON 里的整数写法和取值范围。小数、指数写法和超出范围的值都会被拒绝。文档从 DTO 读取必填规则，再给 Swagger 模型补上容量和路径参数的范围。这个版本没有 `minLength`，因此用正则 `^[\\s\\S]+$` 表示名称不能为空，换行也算字符。

这个对照沿用 oatpp 的默认对象解析规则。实测 `{"room_name":"A","capacity":1,}` 多了尾逗号，`{"room_name":"A" "capacity":1}` 少了字段间的逗号，两种写法都返回 `201`。oatpp 对照还会检查请求的 `Content-Type`。Hical 示例调用 `readJson` 直接解析请求体。联调用 `application/json` 发送两边都支持的合法请求，也会发送测试脚本里固定的错误输入。
