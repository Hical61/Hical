/**
 * @file DbConnection.h
 * @brief 数据库连接抽象接口
 */

#pragma once

#include "DbResult.h"
#include "core/Coroutine.h"
#include <chrono>
#include <span>
#include <string>
#include <string_view>

namespace hical::db
{

	/**
	 * @brief 数据库连接抽象接口
	 * 所有数据库后端（MySQL、PostgreSQL 等）均实现此接口。
	 * 所有 I/O 操作均为协程化异步，不会阻塞 io_context 线程。
	 */
	class DbConnection
	{
	public:
		virtual ~DbConnection() = default;

		// ============ 查询/执行 ============

		/**
		 * @brief 异步执行 SQL 查询（SELECT）— 无参数化，仅限静态 SQL
		 * @param sql SQL 语句
		 * @return 查询结果集
		 * @warning 此重载不做参数转义，仅用于静态 SQL（DDL/SET 等）。
		 * 传入用户输入会导致 SQL 注入！请使用 query(sql, params) 版本。
		 */
		[[deprecated(
			"Unsafe: use query(sql, params) to prevent SQL injection")]] [[nodiscard]] virtual Awaitable<DbResult>
		query(std::string_view sql) = 0;

		/**
		 * @brief 异步参数化查询（防 SQL 注入）
		 * @param sql SQL 语句（参数用 ? 占位）
		 * @param params 参数值列表
		 * @return 查询结果集
		 */
		[[nodiscard]] virtual Awaitable<DbResult> query(std::string_view sql, std::span<const std::string> params) = 0;

		/**
		 * @brief 异步执行 SQL（INSERT/UPDATE/DELETE）— 无参数化，仅限静态 SQL
		 * @param sql SQL 语句
		 * @return 执行结果（affectedRows, insertId）
		 * @warning 此重载不做参数转义，仅用于静态 SQL（DDL/SET 等）。
		 * 传入用户输入会导致 SQL 注入！请使用 execute(sql, params) 版本。
		 */
		[[deprecated(
			"Unsafe: use execute(sql, params) to prevent SQL injection")]] [[nodiscard]] virtual Awaitable<DbResult>
		execute(std::string_view sql) = 0;

		/**
		 * @brief 异步参数化执行（防 SQL 注入）
		 * @param sql SQL 语句（参数用 ? 占位）
		 * @param params 参数值列表
		 * @return 执行结果（affectedRows, insertId）
		 */
		[[nodiscard]] virtual Awaitable<DbResult> execute(std::string_view sql,
														  std::span<const std::string> params) = 0;

		// ============ 事务控制 ============

		/**
		 * @brief 开始事务
		 */
		virtual Awaitable<void> beginTransaction() = 0;

		/**
		 * @brief 提交事务
		 */
		virtual Awaitable<void> commit() = 0;

		/**
		 * @brief 回滚事务
		 */
		virtual Awaitable<void> rollback() = 0;

		/**
		 * @brief 当前是否在事务中
		 */
		virtual bool inTransaction() const = 0;

		// ============ 连接状态 ============

		/**
		 * @brief 连接是否存活（本地状态判断，不发网络包）
		 */
		virtual bool isAlive() const = 0;

		/**
		 * @brief 异步 ping 检测连接活性（发网络包验证）
		 * @return true 表示连接正常
		 */
		[[nodiscard]] virtual Awaitable<bool> ping() = 0;

		/**
		 * @brief 后端名称
		 * @return "mysql", "pgsql", "sqlite" 等
		 */
		[[nodiscard]] virtual std::string_view backend() const = 0;

		/**
		 * @brief 最后活跃时间（用于空闲超时回收）
		 */
		[[nodiscard]] virtual std::chrono::steady_clock::time_point lastActiveTime() const = 0;

		/**
		 * @brief 最后一次 ping 成功的时间（用于健康检查宽限期判断）
		 */
		[[nodiscard]] virtual std::chrono::steady_clock::time_point lastPingTime() const = 0;

		/**
		 * @brief 更新最后活跃时间为当前时刻
		 */
		virtual void touch() = 0;
	};

} // namespace hical::db
