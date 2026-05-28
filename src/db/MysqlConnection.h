/**
 * @file MysqlConnection.h
 * @brief Boost.MySQL 后端实现
 */

#pragma once

#ifdef HICAL_HAS_DATABASE

	#include "DbConfig.h"
	#include "DbConnection.h"
	#include "DbConnectionPool.h"
	#include "StmtCache.h"
	#include "core/Coroutine.h"
	#include <boost/mysql.hpp>
	#include <memory>
	#include <span>
	#include <string>
	#include <string_view>

namespace hical::db
{

	/**
	 * @brief MySQL 数据库连接实现(基于 boost::mysql::any_connection)
	 * 使用 Boost.MySQL 1.84+ 的 any_connection(类型擦除连接),
	 * 原生支持 co_await, 查询参数化使用 PreparedStatement(服务端预编译, 防注入)。
	 */
	class MysqlConnection : public DbConnection
	{
	public:
		~MysqlConnection() override;

		/**
		 * @brief 异步工厂方法: 创建并连接到 MySQL 服务器
		 * @param ioCtx io_context 引用
		 * @param config 数据库配置
		 * @return 已连接的 MysqlConnection
		 */
		[[nodiscard]] static Awaitable<std::shared_ptr<MysqlConnection>> create(boost::asio::io_context& ioCtx,
																				const DbConfig& config);

		/**
		 * @brief 创建 MysqlConnection 工厂函数(可传入 DbConnectionPool)
		 * @return DbConnectionFactory
		 */
		[[nodiscard]] static DbConnectionFactory makeFactory();

		// ============ DbConnection 接口实现 ============

		[[deprecated("Unsafe: use query(sql, params) to prevent SQL injection")]] [[nodiscard]] Awaitable<DbResult>
		query(std::string_view sql) override;
		[[nodiscard]] Awaitable<DbResult> query(std::string_view sql, std::span<const std::string> params) override;

		[[deprecated("Unsafe: use execute(sql, params) to prevent SQL injection")]] [[nodiscard]] Awaitable<DbResult>
		execute(std::string_view sql) override;
		[[nodiscard]] Awaitable<DbResult> execute(std::string_view sql, std::span<const std::string> params) override;

		Awaitable<void> beginTransaction() override;
		Awaitable<void> commit() override;
		Awaitable<void> rollback() override;
		bool inTransaction() const override;

		bool isAlive() const override;
		[[nodiscard]] Awaitable<bool> ping() override;

		[[nodiscard]] std::string_view backend() const override;

		[[nodiscard]] std::chrono::steady_clock::time_point lastActiveTime() const override;
		[[nodiscard]] std::chrono::steady_clock::time_point lastPingTime() const override;
		void touch() override;

	private:
		explicit MysqlConnection(boost::asio::io_context& ioCtx, size_t stmtCacheSize);

		/// 获取或准备 statement（使用缓存）
		Awaitable<boost::mysql::statement> getOrPrepare(std::string_view sql);

		/// 将 boost::mysql::results 转换为 DbResult
		static DbResult convertResults(const boost::mysql::results& boostResults);

		/**
		 * @brief 校验 charset 名称安全性
		 * 白名单：仅允许字母、数字、下划线。
		 * 此校验与 create() 中的 SET NAMES 拼接强耦合——
		 * 修改此校验规则前必须评估 SQL 注入风险。
		 * @param charset 字符集名称
		 * @throws std::invalid_argument 包含非法字符时
		 */
		static void validateCharset(const std::string& charset);

		boost::asio::io_context& ioCtx_;
		boost::mysql::any_connection conn_;
		StmtCache stmtCache_;
		bool alive_ = false;
		bool inTransaction_ = false;
		std::chrono::steady_clock::time_point lastActive_;
		std::chrono::steady_clock::time_point lastPing_;
	};

} // namespace hical::db

#endif // HICAL_HAS_DATABASE
