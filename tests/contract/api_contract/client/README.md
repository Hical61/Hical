# 房间 API 客户端验证

先启动房间示例，把 `/openapi.json` 的响应存成文件。然后把文件路径、服务地址和一个空输出目录传给脚本。两种反射模式都跑同一份 `smoke.ts`。

```sh
python tests/contract/api_contract/client/run_client.py --base-url http://127.0.0.1:8080 \
    --openapi build/room-openapi.json --output-dir build/room-client \
    --generator-jar /path/to/openapi-generator-cli-7.17.0.jar
```

请使用 Node 20.16.0、npm 10.8.1、TypeScript 5.9.3 和 OpenAPI Generator 7.17.0。生成器需要 Java，验证时用的是 JRE 17.0.17+10。可以用 `OPENAPI_GENERATOR_JAR` 设置 JAR 路径，省去 `--generator-jar` 参数。也可以用 `HICAL_NODE`、`HICAL_JAVA` 和 `HICAL_NPM_CLI` 指定工具路径。

脚本按锁文件装依赖，生成 `typescript-fetch` SDK，再编译并运行 `smoke.ts`。测试会创建、查询和列出房间，核对 HTTP 状态和房间数据，也会读出列表中的 `rooms` 和 `total`。请求字段仍由服务端校验。生成文件、依赖和日志都在输出目录里。要重跑，传入同一份文档和另一个空目录即可。

退出码 `0` 表示通过。遇到错误时，可以按退出码查对应的日志。

- `10` 表示工具或依赖准备失败，查看 `versions.log` 和 `npm-ci.log`。
- `11` 表示 SDK 生成失败，查看 `generate.log`。
- `12` 表示 TypeScript 编译失败，查看 `tsc.log`。
- `13` 表示 HTTP 调用或断言失败，查看 `call.log`。

在列表兼容性试验中，SDK 应该能生成，但 `tsc.log` 会报找不到 `rooms` 和 `total`。
