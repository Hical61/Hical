# 告别手写 API 文档：Hical OpenAPI 自动生成 + Swagger UI 一键集成

> 你是否经历过这种情况：花了半天写好 Swagger 注解，一个需求变更，参数名改了，文档却忘了同步——测试组拿着旧文档联调，来回扯皮两小时？

API 文档和代码永远对不上，是后端开发者的经典痛点。本文介绍如何用 **Hical** 框架的 OpenAPI 模块，让文档从代码中**自动生成**，彻底消灭这个问题。

---

## 一、背景：OpenAPI 3.0 是什么

OpenAPI 3.0（即过去的 Swagger 规范）是描述 HTTP API 的行业标准 JSON/YAML 格式。有了它：

- Swagger UI / Redoc 可以直接渲染成可交互的文档页面
- 前端可以一键生成 TypeScript 类型定义
- QA 可以直接在浏览器里填参数发请求

手写 OpenAPI YAML 很繁琐，维护成本高。**Hical 的方案是：从 C++ 类型系统直接推导出 schema，标注一次，文档自动生成。**

---

## 二、三步集成概览

```
步骤 1  定义 DTO，加 HICAL_JSON + HICAL_SCHEMA_NAME
步骤 2  标注路由，加 HICAL_API + builder::*
步骤 3  main() 中 registerRoutesWithOpenApi + serveOpenApi
```

不需要任何新依赖，OpenAPI 模块默认随 Hical 一起编译（`HICAL_WITH_OPENAPI=ON` 是默认值），底层复用已有的 Boost.JSON。

---

## 三、步骤 1：定义 DTO

首先用 `HICAL_JSON` 宏让结构体支持 JSON 序列化，再用 `HICAL_SCHEMA_NAME` 注册类型名，这样嵌套引用时会生成 `$ref` 而不是把 schema 内联展开。

```cpp
#include "core/MetaJson.h"
#include "core/OpenApiSchema.h"

// 用户信息 DTO
struct UserDTO
{
    std::string name;
    int         age;
    std::string email;
    HICAL_JSON(UserDTO, REQUIRED(name), age, email)
};
HICAL_SCHEMA_NAME(UserDTO, "UserDTO")

// 创建用户的请求体
struct CreateUserRequest
{
    std::string name;
    int         age;
    std::string email;
    HICAL_JSON(CreateUserRequest, REQUIRED(name), REQUIRED(age), email)
};
HICAL_SCHEMA_NAME(CreateUserRequest, "CreateUserRequest")

// 统一错误响应
struct ErrorResponse
{
    int         code;
    std::string message;
    HICAL_JSON(ErrorResponse, code, message)
};
HICAL_SCHEMA_NAME(ErrorResponse, "ErrorResponse")
```

**类型映射规则**（由 `jsonSchema<T>()` 在编译期推导）：

| C++ 类型                                           | OpenAPI Schema                        |
| -------------------------------------------------- | ------------------------------------- |
| `std::string`                                      | `{"type":"string"}`                   |
| `bool`                                             | `{"type":"boolean"}`                  |
| `int` / `int32_t`                                  | `{"type":"integer","format":"int32"}` |
| `int64_t`                                          | `{"type":"integer","format":"int64"}` |
| `float`                                            | `{"type":"number","format":"float"}`  |
| `double`                                           | `{"type":"number","format":"double"}` |
| `std::vector<T>`                                   | `{"type":"array","items":{...}}`      |
| 嵌套 `HICAL_JSON` 结构体（有 `HICAL_SCHEMA_NAME`） | `{"$ref":"#/components/schemas/XXX"}` |
| 嵌套 `HICAL_JSON` 结构体（无名称）                 | 内联展开完整 schema                   |

`REQUIRED(field)` 会把该字段加入 schema 的 `required` 数组，与业务验证逻辑保持一致。

---

## 四、步骤 2：标注路由

在 Handler 类里，紧跟 `HICAL_HANDLER` 宏之后写 `HICAL_API`，用 `builder::*` 函数描述这个端点的元数据。最后用 `HICAL_ROUTES_WITH_API` 收集路由表（它是 `HICAL_ROUTES` 的超集，额外生成 API 标注表）。

```cpp
#include "core/MetaRoutes.h"
#include "core/OpenApiRegistry.h"

struct UserHandler
{
    // ---- GET /api/users ----
    HttpResponse listUsers(const HttpRequest& /*req*/)
    {
        boost::json::array arr;
        arr.push_back(toJson(UserDTO{"Alice", 30, "alice@example.com"}));
        return HttpResponse::json({{"users", arr}, {"total", 1}});
    }
    HICAL_HANDLER(Get, "/api/users", listUsers)
    HICAL_API(listUsers,
        builder::summary(info, "获取用户列表");
        builder::tags(info, {"users"});
        builder::response<UserDTO>(info, 200, "用户列表"))

    // ---- POST /api/users ----
    HttpResponse createUser(const HttpRequest& req)
    {
        auto body = req.readJson<CreateUserRequest>();
        UserDTO user{body.name, body.age, body.email};
        auto res = HttpResponse::json(toJson(user));
        res.setStatus(HttpStatusCode::hCreated);
        return res;
    }
    HICAL_HANDLER(Post, "/api/users", createUser)
    HICAL_API(createUser,
        builder::summary(info, "创建用户");
        builder::tags(info, {"users"});
        builder::request<CreateUserRequest>(info, "用户数据", true);
        builder::response<UserDTO>(info, 201, "创建成功");
        builder::responseDesc(info, 400, "请求体格式错误"))

    // ---- GET /api/users/{id} ----
    HttpResponse getUser(const HttpRequest& req)
    {
        auto id = req.param("id");
        UserDTO user{"User " + id, 20, id + "@example.com"};
        return HttpResponse::json(toJson(user));
    }
    HICAL_HANDLER(Get, "/api/users/{id}", getUser)
    HICAL_API(getUser,
        builder::summary(info, "按 ID 获取用户");
        builder::tags(info, {"users"});
        builder::pathParam(info, "id", "integer", "用户 ID");
        builder::response<UserDTO>(info, 200, "用户详情");
        builder::responseDesc(info, 404, "用户不存在"))

    HICAL_ROUTES_WITH_API(UserHandler, listUsers, createUser, getUser)
};
```

`HICAL_API` 宏在类内部生成一个静态 `RouteApiInfo` 常量，`builder::*` 函数都接受 `info` 这个局部变量（宏内已声明），语句之间用分号分隔，写起来像配置 DSL。

**builder 函数速查**：

| 函数                                               | 作用                          |
| -------------------------------------------------- | ----------------------------- |
| `builder::summary(info, "...")`                    | 端点一句话摘要                |
| `builder::description(info, "...")`                | 详细描述（支持 Markdown）     |
| `builder::tags(info, {"tag1", "tag2"})`            | 分组标签，Swagger UI 按此折叠 |
| `builder::request<T>(info, "描述", required)`      | 请求体 DTO，自动引用 schema   |
| `builder::response<T>(info, 状态码, "描述")`       | 有 body 的响应                |
| `builder::responseDesc(info, 状态码, "描述")`      | 无 body 的响应（204/404 等）  |
| `builder::pathParam(info, "name", "type", "描述")` | 路径参数说明                  |

---

## 五、步骤 3：在 main() 中暴露端点

```cpp
#include "core/HttpServer.h"
#include "core/OpenApiDocument.h"
#include "core/OpenApiEndpoint.h"

int main()
{
    HttpServer server(8080);
    auto registry = std::make_shared<OpenApiRegistry>();

    // 批量注册所有 DTO 的 schema 到 components/schemas
    std::unordered_map<std::string, boost::json::object> schemas;
    registerSchemas<UserDTO, CreateUserRequest, ErrorResponse>(schemas);
    for (auto& [name, schema] : schemas)
    {
        registry->addSchema(name, std::move(schema));
    }

    // 注册路由，同时把 HICAL_API 标注收集进 registry
    UserHandler handler;
    registerRoutesWithOpenApi(server.router(), handler, *registry);

    // 组装 OpenAPI 文档（惰性生成 + 缓存，按值返回线程安全）
    auto doc = std::make_shared<OpenApiDocument>(
        registry,
        OpenApiConfig{
            .title       = "用户服务 API",
            .version     = "1.0.0",
            .description = "演示 Hical 自动 OpenAPI 生成",
            .servers     = {{"http://localhost:8080", "本地开发服务器"}}
        });

    // 一键注册两个端点：
    //   GET /openapi.json  → OpenAPI 3.0 JSON spec
    //   GET /docs          → Swagger UI 交互页面
    serveOpenApi(server.router(), doc);

    server.start();
    return 0;
}
```

启动后：

```
GET http://localhost:8080/openapi.json   # 原始 JSON spec
GET http://localhost:8080/docs           # Swagger UI 页面
```

---

## 六、运行效果

访问 `/docs` 会看到 Swagger UI 标准界面，左侧按 tag 分组（`users`），每个端点展开后可以直接填参数、点 **Try it out** 发请求。

`/openapi.json` 返回的片段示例：

```json
{
  "openapi": "3.0.3",
  "info": { "title": "用户服务 API", "version": "1.0.0" },
  "paths": {
    "/api/users": {
      "get": {
        "summary": "获取用户列表",
        "tags": ["users"],
        "responses": {
          "200": {
            "description": "用户列表",
            "content": {
              "application/json": {
                "schema": { "$ref": "#/components/schemas/UserDTO" }
              }
            }
          }
        }
      },
      "post": {
        "summary": "创建用户",
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": { "$ref": "#/components/schemas/CreateUserRequest" }
            }
          }
        },
        "responses": {
          "201": { "description": "创建成功" },
          "400": { "description": "请求体格式错误" }
        }
      }
    }
  },
  "components": {
    "schemas": {
      "UserDTO": {
        "type": "object",
        "properties": {
          "name":  { "type": "string" },
          "age":   { "type": "integer", "format": "int32" },
          "email": { "type": "string" }
        },
        "required": ["name"]
      }
    }
  }
}
```

同路径下的不同 HTTP 方法（`GET /api/users` 和 `POST /api/users`）会自动合并到同一个 Path Item，符合 OpenAPI 3.0 规范。

---

## 七、高级用法

### 7.1 嵌套 DTO 与 $ref

当响应结构体里包含另一个标注了 `HICAL_SCHEMA_NAME` 的结构体时，schema 自动生成 `$ref`：

```cpp
struct AddressDTO
{
    std::string city;
    std::string street;
    HICAL_JSON(AddressDTO, city, street)
};
HICAL_SCHEMA_NAME(AddressDTO, "AddressDTO")

struct UserDetailDTO
{
    std::string name;
    AddressDTO  address;   // 嵌套 → 生成 $ref
    HICAL_JSON(UserDetailDTO, name, address)
};
HICAL_SCHEMA_NAME(UserDetailDTO, "UserDetailDTO")
```

调用 `collectSchemas<UserDetailDTO>(schemas)` 时会**递归**把 `AddressDTO` 也加入 `components/schemas`，无需手动处理嵌套依赖。

### 7.2 多响应码

一个端点可以声明任意多个响应码：

```cpp
HICAL_API(createOrder,
    builder::summary(info, "创建订单");
    builder::tags(info, {"orders"});
    builder::request<CreateOrderRequest>(info, "订单数据", true);
    builder::response<OrderDTO>(info, 201, "订单创建成功");
    builder::response<ErrorResponse>(info, 400, "参数错误");
    builder::responseDesc(info, 401, "未登录");
    builder::responseDesc(info, 429, "请求过于频繁"))
```

### 7.3 路径参数标注

`pathParam` 的第三个参数是 OpenAPI 类型字符串（`"integer"` / `"string"` / `"boolean"`），用于在 Swagger UI 里渲染正确的输入控件：

```cpp
HICAL_API(getUserOrders,
    builder::pathParam(info, "userId", "integer", "用户 ID");
    builder::pathParam(info, "status", "string",  "订单状态过滤"))
```

路径参数的存在由路由路径（`/api/users/{userId}/orders/{status}`）自动检测，`pathParam` 只是补充类型和描述。

---

## 八、与 Oat++ 的对比

Oat++ 是 C++ 生态里另一个有 Swagger 集成的框架，来看看标注风格的差异：

| 维度           | Hical                                    | Oat++                                            |
| -------------- | ---------------------------------------- | ------------------------------------------------ |
| 路由标注       | `HICAL_HANDLER` + `HICAL_API`            | `ENDPOINT_INFO` + `ENDPOINT`                     |
| Schema 生成    | 从 `HICAL_JSON` 自动推导                 | 需要继承 `oatpp::DTO` 并用 `DTO_FIELD`           |
| 响应声明       | `builder::response<T>(info, 200, "...")` | `info->addResponse<Object<T>>(Status::CODE_200)` |
| 新依赖         | 零（复用 Boost.JSON）                    | 引入 oatpp-swagger 库                            |
| 编译期类型检查 | `static_assert` + concepts               | 运行时反射                                       |
| 内存模型       | 零堆分配查找（透明哈希）                 | 智能指针 DTO 对象                                |

Hical 的核心差异在于 schema 是**编译期从类型信息推导**的，而不是运行时通过 DTO 对象的虚函数反射。这意味着写错类型在编译阶段就能发现，而不是部署后才看到错误的文档。

---

## 九、总结

Hical OpenAPI 模块的设计目标就是**让文档变成代码的副产品**：

- 定义 DTO 时顺手写 `HICAL_JSON`，schema 就有了
- 写完路由实现后紧跟一个 `HICAL_API` 块，文档就更新了
- `serveOpenApi` 一行，Swagger UI 就上线了

整个链路没有运行时解析、没有 YAML 文件、没有代码生成步骤。API 实现改了，下次请求 `/openapi.json` 时文档自动跟着变。

完整可运行示例见 `examples/openapi_server.cpp`。

---

*CMake 选项 `HICAL_WITH_OPENAPI=ON`（默认值）。*
