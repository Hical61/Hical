/**
 * @file Concepts.h
 * @brief C++20 Concepts 编译期后端约束
 */

#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <memory_resource>
#include <chrono>
#include <cstdint>

namespace hical
{

	// 前向声明
	class EventLoop;
	class TcpConnection;
	class Timer;

	/**
	 * @brief 事件循环接口约束
	 * 要求事件循环类型提供完整的生命周期管理、任务调度、
	 * 定时器和 pmr 分配器支持。
	 */
	template <typename T>
	concept EventLoopLike = requires(T loop, std::function<void()> func, double delay) {
		// 生命周期
		{ loop.run() } -> std::same_as<void>;
		{ loop.stop() } -> std::same_as<void>;
		{ loop.isRunning() } -> std::convertible_to<bool>;

		// 任务调度
		{ loop.post(func) } -> std::same_as<void>;
		{ loop.dispatch(func) } -> std::same_as<void>;

		// 定时器
		{ loop.runAfter(delay, func) } -> std::convertible_to<uint64_t>;
		{ loop.runEvery(delay, func) } -> std::convertible_to<uint64_t>;
		{ loop.cancelTimer(uint64_t {}) } -> std::same_as<void>;

		// 线程属性
		{ loop.isInLoopThread() } -> std::convertible_to<bool>;
		{ loop.index() } -> std::convertible_to<size_t>;

		// pmr 分配器
		{ loop.allocator() } -> std::same_as<std::pmr::polymorphic_allocator<std::byte>>;
	};

	/**
	 * @brief TCP 连接接口约束
	 * 要求连接类型提供数据收发、连接控制和状态查询能力。
	 */
	template <typename T>
	concept TcpConnectionLike = requires(T conn, const char* data, size_t len, const std::string& msg) {
		// 数据发送
		{ conn.send(data, len) } -> std::same_as<void>;
		{ conn.send(msg) } -> std::same_as<void>;

		// 连接控制
		{ conn.shutdown() } -> std::same_as<void>;
		{ conn.close() } -> std::same_as<void>;

		// 状态查询
		{ conn.connected() } -> std::convertible_to<bool>;
		{ conn.disconnected() } -> std::convertible_to<bool>;
		{ conn.bytesSent() } -> std::convertible_to<size_t>;
		{ conn.bytesReceived() } -> std::convertible_to<size_t>;
	};

	/**
	 * @brief 定时器接口约束
	 * 要求定时器类型提供取消、状态查询和间隔信息。
	 */
	template <typename T>
	concept TimerLike = requires(T timer) {
		// 定时器控制
		{ timer.cancel() } -> std::same_as<void>;
		{ timer.isActive() } -> std::convertible_to<bool>;
		{ timer.isRepeating() } -> std::convertible_to<bool>;
		{ timer.interval() } -> std::convertible_to<double>;
	};

	/**
	 * @brief 网络后端统一约束（C++20 Concept）
	 * 定义了网络后端必须满足的接口要求，支持"双模"运行时切换。
	 * 任何满足此约束的类型都可以作为 hical 的网络后端。
	 * 后端需提供三个关联类型：
	 * - EventLoopType  — 满足 EventLoopLike 约束的事件循环
	 * - ConnectionType — 满足 TcpConnectionLike 约束的 TCP 连接
	 * - TimerType      — 满足 TimerLike 约束的定时器
	 * 用法示例：
	 * ```cpp
	 * template <NetworkBackend Backend>
	 * class GenericServer {
	 *     using Loop = typename Backend::EventLoopType;
	 *     using Conn = typename Backend::ConnectionType;
	 * };
	 * // 使用 Asio 后端
	 * GenericServer<AsioBackend> server;
	 * ```
	 * 当前仅提供 AsioBackend 实现。未来可添加轻量后端用于极致性能场景。
	 */
	template <typename T>
	concept NetworkBackend =
		requires {
			// 必须有三个关联类型
			typename T::EventLoopType;
			typename T::ConnectionType;
			typename T::TimerType;
		} && EventLoopLike<typename T::EventLoopType> && TcpConnectionLike<typename T::ConnectionType>
		&& TimerLike<typename T::TimerType>;

} // namespace hical

// ============ Asio 后端定义 ============
// 放在 namespace 外的 include guard 内，避免循环依赖。
// AsioBackend 通过前向声明引用 Asio 适配层类型。

namespace hical
{

	class AsioEventLoop;
	class AsioTimer;

	// GenericConnection 的默认实例化类型（PlainConnection）
	// 由 GenericConnection.h 定义为：
	//   using PlainConnection = GenericConnection<tcp::socket>;
	// 这里用前向声明避免拉入重量级头文件。
	class TcpConnection;

	/**
	 * @brief Asio 网络后端类型定义
	 * 将 Boost.Asio 适配层的三个核心类型打包为一个后端定义，
	 * 满足 NetworkBackend concept 约束。
	 * 这是 hical 的默认后端，适用于开发和生产环境。
	 */
	struct AsioBackend
	{
		using EventLoopType = AsioEventLoop;
		using ConnectionType = TcpConnection;
		using TimerType = AsioTimer;
	};

} // namespace hical
