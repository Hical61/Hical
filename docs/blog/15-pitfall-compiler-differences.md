# MSVC / GCC / Clang 三平台 C++20 编译差异实录

## 引言

Hical 从第一天起就要求在 GCC 14+、Clang 20+、MSVC 2022+ 三个编译器上通过 CI。框架大量使用了 C++20 新特性：Concepts、`co_await` 协程、PMR 内存池、`std::format`、`__VA_OPT__` 递归宏。

> 三平台兼容的代价就是踩三倍的坑。

这篇文章记录了开发 Hical 过程中遇到的**编译器差异踩坑**——每个坑按统一结构展开：**现象 → 最小复现 → 根因 → 解决方案**。

---

## 目录

- [MSVC / GCC / Clang 三平台 C++20 编译差异实录](#msvc--gcc--clang-三平台-c20-编译差异实录)
  - [引言](#引言)
  - [目录](#目录)
  - [坑 1：模板参数推导差异——GCC 过、MSVC 报错](#坑-1模板参数推导差异gcc-过msvc-报错)
  - [坑 2：Concepts 约束检查时机差异](#坑-2concepts-约束检查时机差异)
  - [坑 3：`__VA_OPT__` 宏展开行为差异](#坑-3__va_opt__-宏展开行为差异)
  - [坑 4：PMR allocator 传播行为差异](#坑-4pmr-allocator-传播行为差异)
  - [坑 5：`std::format` 可用性与行为差异](#坑-5stdformat-可用性与行为差异)
  - [坑 6：协程 promise\_type 与异常处理差异](#坑-6协程-promise_type-与异常处理差异)
  - [坑 7：链接顺序敏感——Windows 特有的 ws2\_32 问题](#坑-7链接顺序敏感windows-特有的-ws2_32-问题)
  - [经验总结：三平台兼容清单](#经验总结三平台兼容清单)
    - [编译器设置](#编译器设置)
    - [C++20 特性](#c20-特性)
    - [协程](#协程)
    - [一般性](#一般性)

---

## 坑 1：模板参数推导差异——GCC 过、MSVC 报错

**现象**：一段在 GCC 14 上完美编译的透明哈希代码，在 MSVC 上报 `C2672: no matching overloaded function found`。

**最小复现**：

Hical 的路由系统使用透明哈希（`is_transparent`）实现零分配的 `string_view` 查找：

```cpp
// Router.h — 透明哈希
struct RouteKeyHash
{
    using is_transparent = void;
    size_t operator()(const RouteKey& key) const { /* hash method+path */ }
    size_t operator()(const RouteKeyView& key) const { /* hash method+path_view */ }
};

struct RouteKeyEqual
{
    using is_transparent = void;
    bool operator()(const RouteKey& a, const RouteKey& b) const;
    bool operator()(const RouteKeyView& a, const RouteKey& b) const;
    // ❌ 如果缺少下面这个重载，GCC/Clang 不报错，MSVC 报错
    // bool operator()(const RouteKey& a, const RouteKeyView& b) const;
};

std::unordered_map<RouteKey, RouteHandler, RouteKeyHash, RouteKeyEqual> staticRoutes_;
```

**根因**：MSVC 的模板实例化策略更"急切"——即使某些 `operator()` 重载在实际代码路径中不会被调用，MSVC 也会在模板定义时尝试实例化所有可能的组合。GCC/Clang 采用"懒实例化"，只检查实际用到的路径。

C++20 标准对 `is_transparent` 异构查找的要求是：Hash 和 Equal 类型必须对异构键类型提供 `operator()`。但标准没有明确规定需要覆盖所有排列组合——这给了实现留了空间，也导致了跨编译器差异。

**解决方案**：提供所有排列组合的 `operator()`，宁可冗余：

```cpp
// Hical 的做法——三种比较组合全部覆盖
struct RouteKeyEqual
{
    using is_transparent = void;

    bool operator()(const RouteKey& a, const RouteKey& b) const
    {
        return a.method == b.method && a.path == b.path;
    }
    bool operator()(const RouteKeyView& a, const RouteKey& b) const
    {
        return a.method == b.method && a.path == b.path;
    }
    bool operator()(const RouteKey& a, const RouteKeyView& b) const
    {
        return a.method == b.method && a.path == b.path;
    }
};
```

**经验**：涉及 `is_transparent` 异构查找时，始终提供所有排列组合的 `operator()`。代码多几行，但三平台一致。

---

## 坑 2：Concepts 约束检查时机差异

**现象**：一个 concept 约束的函数模板在 GCC 上编译正常，Clang 上报 `constraints not satisfied`，而代码逻辑完全相同。

**场景**：Hical 用 Concepts 定义了网络后端约束：

```cpp
// Concepts.h — 事件循环约束
template <typename T>
concept EventLoopLike = requires(T loop, std::function<void()> func, double delay) {
    { loop.run() } -> std::same_as<void>;
    { loop.stop() } -> std::same_as<void>;
    { loop.post(func) } -> std::same_as<void>;
    { loop.dispatch(func) } -> std::same_as<void>;
    { loop.runAfter(delay, func) } -> std::convertible_to<uint64_t>;
    { loop.cancelTimer(uint64_t{}) } -> std::same_as<void>;
    { loop.isInLoopThread() } -> std::convertible_to<bool>;
    { loop.index() } -> std::convertible_to<size_t>;
    { loop.allocator() } -> std::same_as<std::pmr::polymorphic_allocator<std::byte>>;
};

// 组合约束
template <typename T>
concept NetworkBackend =
    requires {
        typename T::EventLoopType;
        typename T::ConnectionType;
        typename T::TimerType;
    } && EventLoopLike<typename T::EventLoopType>
      && TcpConnectionLike<typename T::ConnectionType>
      && TimerLike<typename T::TimerType>;
```

**根因**：C++20 标准对 concept 约束检查的时机有模糊地带。GCC 和 Clang 对"约束的规范化（normalization）"处理不同：

- **GCC**：在模板实例化时才检查约束
- **Clang**：在声明时就检查约束的"可满足性"（subsumption），对依赖名字（dependent name）的解析更严格

当 concept 内部使用 `requires` 表达式且涉及依赖名字（如 `typename T::EventLoopType`）时，两个编译器的解析顺序可能不一致。

**解决方案**：

1. **concept 只做存在性检查**——保持 requires 表达式简单直接
2. **避免嵌套 requires**——不在 concept 里做复杂的 SFINAE 或类型推导
3. **复杂类型检查放到函数体内**——用 `static_assert`

```cpp
// ✅ concept 保持简单
template <typename T>
concept EventLoopLike = requires(T loop, std::function<void()> func) {
    { loop.run() } -> std::same_as<void>;
    // 只检查接口存在和返回类型
};

// ❌ 避免在 concept 里做复杂逻辑
template <typename T>
concept Bad = requires(T t) {
    requires std::derived_from<typename T::Inner, SomeBase>;
    requires sizeof(T) > 64;  // 这类约束放到 static_assert 里
};
```

**经验**：CI 矩阵必须同时包含 GCC + Clang + MSVC。concept 写完后在三个编译器上都跑一遍，不能只在一个上面验证。

---

## 坑 3：`__VA_OPT__` 宏展开行为差异

**现象**：`HICAL_JSON` 宏在 GCC/Clang 上正确展开所有字段，MSVC 上编译失败，报错指向宏展开后的意外逗号。

**背景**：Hical 的 JSON 反射宏使用 `__VA_OPT__` 实现递归遍历，支持任意数量的字段：

```cpp
// MetaJson.h — 递归展开
#define HICAL_JSON_FOR_EACH_(macro, T, a, ...) \
    macro(T, a) __VA_OPT__(, HICAL_JSON_FE_AGAIN_ HICAL_JSON_PARENS_(macro, T, __VA_ARGS__))

#define HICAL_JSON_FE_AGAIN_() HICAL_JSON_FOR_EACH_
```

**根因**：`__VA_OPT__` 是 C++20 新增的预处理器特性。MSVC 有两个预处理器：

| 预处理器             | 启用方式           | `__VA_OPT__` 支持 |
| -------------------- | ------------------ | ----------------- |
| 传统预处理器（默认） | 默认               | ❌ 不支持          |
| 符合标准的预处理器   | `/Zc:preprocessor` | ✅ 支持            |

即使启用了 `/Zc:preprocessor`，MSVC 早期版本（19.28 之前）的递归宏展开也有 bug——递归深度达到某个阈值时，展开结果不正确。

**解决方案**：

第一步：CMakeLists.txt 中强制启用符合标准的预处理器：

```cmake
if (MSVC)
    target_compile_options(hical_core PRIVATE /Zc:preprocessor)
endif()
```

第二步：多层 `EXPAND` 宏突破递归深度限制：

```cpp
// MetaJson.h — 5 层 EXPAND，支持 3^5 = 243 个字段
#define HICAL_JSON_EXPAND_(...) HICAL_JSON_EXP4_(HICAL_JSON_EXP4_(__VA_ARGS__))
#define HICAL_JSON_EXP4_(...) HICAL_JSON_EXP3_(HICAL_JSON_EXP3_(__VA_ARGS__))
#define HICAL_JSON_EXP3_(...) HICAL_JSON_EXP2_(HICAL_JSON_EXP2_(__VA_ARGS__))
#define HICAL_JSON_EXP2_(...) HICAL_JSON_EXP1_(HICAL_JSON_EXP1_(__VA_ARGS__))
#define HICAL_JSON_EXP1_(...) __VA_ARGS__
```

每层 EXPAND 将上一层的"延迟展开标记"替换为实际内容。5 层嵌套意味着宏处理器会扫描 32 遍（2^5），足以展开绝大多数字段数量。

第三步：编译期字段校验保底：

```cpp
// 即使宏展开出问题，static_assert 也会在编译期报错
#define HICAL_JSON_MAKE_FIELD_(T, field, ...)                         \
    ([]()                                                             \
    {                                                                 \
        static_assert(                                                \
            requires { std::declval<T>().field; },                    \
            "HICAL_JSON: field '" #field "' does not exist in " #T); \
        return ::hical::meta::detail::makeField<T>(__VA_ARGS__);     \
    }())
```

**经验**：
- MSVC 用 `__VA_OPT__` 必须加 `/Zc:preprocessor`——这是最容易忘的一步
- 递归宏要多层 EXPAND 保底
- 宏展开后加 `static_assert` 做编译期兜底校验

---

## 坑 4：PMR allocator 传播行为差异

**现象**：同一份使用 `std::pmr::vector` 的代码，在不同标准库实现下，实际走 PMR 还是走默认 new/delete 的行为不一致。

**最小复现**：

```cpp
std::pmr::vector<std::pmr::string> v(&myPool);
v.emplace_back("hello world, this is a long string that exceeds SSO");
// 这个 string 的堆分配走 myPool 吗？
```

**根因**：C++ 标准规定 PMR 容器的**嵌套容器不会自动继承父容器的分配器**（除非使用 `uses_allocator` 协议）。但不同标准库的实现细节有差异：

| 行为                                  | libstdc++ (GCC)            | libc++ (Clang) | MSVC STL     |
| ------------------------------------- | -------------------------- | -------------- | ------------ |
| SSO 阈值                              | 通常 15 字节               | 通常 22 字节   | 通常 15 字节 |
| `vector::emplace_back` 传播 allocator | 是（uses_allocator 检测）  | 是             | 是           |
| `boost::json::object` 内部字符串      | 使用 json 自己的 allocator | 同左           | 同左         |

表面上行为一致，但 SSO 阈值不同意味着**同一个字符串在某些平台上走 PMR，在另一些平台上走 SSO 不分配**。这不会导致错误，但会导致性能特征在不同平台上不一致——给 benchmark 带来困惑。

更隐蔽的情况是从 PMR 容器中拷贝出来的值：

```cpp
auto& pool = getRequestPool();
std::pmr::vector<std::pmr::string> headers(&pool);
headers.push_back("Content-Type: text/html");

// ❌ 从 PMR 容器拷贝出来的 string 走的是默认 allocator
std::string copied = headers[0];  // 这个 string 不在 pool 里

// ❌ 更隐蔽：auto 推导不带 allocator
auto val = headers[0];  // auto = std::pmr::string，但如果赋值给 std::string 就脱离 PMR
```

**解决方案**：不依赖隐式传播行为，关键路径上显式构造：

```cpp
// ✅ 显式传播——三平台行为一致
auto& pool = getRequestPool();
std::pmr::vector<std::pmr::string> headers(&pool);
headers.emplace_back(std::pmr::string("Content-Type: text/html", &pool));
```

**经验**：
- PMR 的 allocator 传播不要依赖隐式行为——显式传播虽然啰嗦但跨平台一致
- SSO 阈值在不同标准库实现中不同——benchmark PMR 收益时要注意这个变量
- 从 PMR 容器中拷贝出来的对象不再走 PMR——避免无意中"脱离"池分配

---

## 坑 5：`std::format` 可用性与行为差异

**现象**：Hical 的日志宏基于 `std::format`，在 GCC 14 + libstdc++ 上完整可用，但在某些 Clang + libc++ 版本上自定义 `formatter` 特化有问题。

**Hical 的用法**：

```cpp
// Log.h — 编译期格式检查
template <typename... Args>
void logFmt(LogLevel level,
            const char* file,
            int line,
            std::format_string<Args...> fmt,  // 编译期校验
            Args&&... args)
{
    auto msg = std::format(fmt, std::forward<Args>(args)...);
    // ...
}

// 用户侧
HICAL_LOG_INFO("server started on port={}", 8080);
// 如果参数类型不匹配，编译期就报错
```

**各编译器支持状态**（截至 2025）：

| 特性                             | GCC 14 | Clang 20         | MSVC 19.36+ |
| -------------------------------- | ------ | ---------------- | ----------- |
| `std::format` 基本功能           | ✅      | ✅                | ✅           |
| `std::format_string` 编译期检查  | ✅      | ✅                | ✅           |
| 自定义 `formatter` 特化          | ✅      | ⚠️ 某些版本有 bug | ✅           |
| `std::format_to` 输出到 iterator | ✅      | ✅                | ✅           |

**解决方案**：

1. 基本的 `std::format` 在三平台上已经足够稳定——放心用
2. 保留流式 API 作为备选，不强制依赖 `std::format`：

```cpp
// 流式 API 不依赖 std::format，用 FixedBuffer + operator<< 实现
HICAL_LOG_INFO_STREAM << "port=" << port << " threads=" << numThreads;

// 内部实现用 FixedBuffer 栈缓冲 + std::to_chars
template <typename T>
FixedBuffer& formatInteger(T val)
{
    char tmp[32];
    auto [ptr, ec] = std::to_chars(tmp, tmp + 32, val);
    if (ec == std::errc{})
        append(tmp, static_cast<size_t>(ptr - tmp));
    return *this;
}
```

3. 避免为框架内部类型做 `formatter` 特化——内部用 `to_string()` 转换后再传给 `std::format`

**经验**：`std::format` 的基本功能已经三平台可靠，可以作为首选 API。但自定义 `formatter` 特化要谨慎，提供流式 API 备选是好策略。

---

## 坑 6：协程 promise_type 与异常处理差异

**现象**：一个 `detached` 协程在 GCC 上异常被静默吞掉，在 MSVC 上触发了 `std::terminate`。

**根因**：`boost::asio::co_spawn` 的第三个参数（completion handler）决定了异常的传播方式：

```cpp
// detached：异常在不同编译器/Asio 版本上行为不一致
boost::asio::co_spawn(io_ctx, myCoroutine(), boost::asio::detached);

// 显式处理异常的方式
boost::asio::co_spawn(io_ctx, myCoroutine(),
    [](std::exception_ptr eptr)
    {
        if (eptr)
        {
            try { std::rethrow_exception(eptr); }
            catch (const std::exception& e)
            {
                HICAL_LOG_ERROR("coroutine failed: {}", e.what());
            }
        }
    });
```

**解决方案**：Hical 的策略是——对所有"不应该抛异常"的协程，都显式提供异常处理器，而非依赖 `detached` 的行为：

```cpp
// TcpServer.cpp — accept 循环的异常处理
boost::asio::co_spawn(
    baseLoop_->getIoContext(),
    [this, aliveFlag]() -> Awaitable<void>
    {
        co_await acceptLoop();
    },
    [](std::exception_ptr) {});  // 显式吞掉——因为 acceptLoop 内部已处理
```

**经验**：不要依赖 `detached` 的异常行为——它在不同编译器和 Asio 版本上不一致。总是显式提供 completion handler。

---

## 坑 7：链接顺序敏感——Windows 特有的 ws2_32 问题

**现象**：在 Linux 上编译通过的代码，在 Windows（MSVC 和 MSYS2）上报大量 `unresolved external symbol`，全部指向 Winsock API。

**根因**：Windows 的网络 API（WSAStartup、socket、connect 等）在 `ws2_32.lib` 和 `mswsock.lib` 中。Boost.Asio 在 Windows 上依赖这些库，但 CMake 不会自动链接它们。

**解决方案**：

```cmake
# CMakeLists.txt — Windows 特有链接
if(WIN32)
    target_link_libraries(hical_core PUBLIC ws2_32 mswsock)
    # 测试也需要
    foreach(test_target ${ALL_TESTS})
        target_link_libraries(${test_target} PRIVATE ws2_32 mswsock)
    endforeach()
endif()
```

更隐蔽的问题是链接顺序——在某些 MinGW 工具链上，`ws2_32` 必须在 Boost 库之后：

```cmake
# ❌ 可能失败（某些 MinGW 版本）
target_link_libraries(myapp ws2_32 Boost::system Boost::beast)

# ✅ ws2_32 放在依赖链末尾
target_link_libraries(myapp Boost::system Boost::beast ws2_32 mswsock)
```

**经验**：Windows 上用 Boost.Asio 必须手动链接 `ws2_32` + `mswsock`，且注意链接顺序。这是每个 Asio 新手在 Windows 上必踩的第一个坑。

---

## 经验总结：三平台兼容清单

基于 Hical 的开发经验，整理了一份三平台兼容检查清单：

### 编译器设置

- [ ] MSVC 添加 `/Zc:preprocessor`（`__VA_OPT__` 必需）
- [ ] MSVC 添加 `/Zc:__cplusplus`（否则 `__cplusplus` 永远报 199711）
- [ ] CI 矩阵覆盖 GCC + Clang + MSVC
- [ ] Windows 链接 `ws2_32` + `mswsock`

### C++20 特性

- [ ] `is_transparent` 异构查找：提供所有比较组合
- [ ] Concepts：只做存在性检查，复杂逻辑用 `static_assert`
- [ ] `std::format`：基本功能可靠，自定义 `formatter` 谨慎
- [ ] PMR：不依赖隐式 allocator 传播，显式传递
- [ ] `__VA_OPT__` 递归宏：多层 EXPAND + 编译期校验

### 协程

- [ ] `co_spawn` 不用 `detached`，显式提供异常处理器
- [ ] `co_await` 后检查对象存活性
- [ ] `catch` 里不能 `co_await`——用 `exception_ptr` 中转

### 一般性

- [ ] 不依赖未定义行为的编译器差异——写明确的、标准保证的代码
- [ ] 静态分析（clang-tidy）开启但非阻塞——捕获潜在问题但不阻断 CI
- [ ] 格式检查（clang-format）统一版本——不同版本对同一配置的格式化结果可能不同

---

> **踩坑实录五部曲**：[编译差异](#)（本文） | [Asio 协程](16-pitfall-asio-coroutine.md) | [Boost.MySQL](17-pitfall-boost-mysql.md) | [日志系统](18-pitfall-logging-system.md) | [PMR 内存池](19-pitfall-pmr-memory-pool.md)
