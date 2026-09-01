/**
 * @file PgsqlConnection.h
 * @brief libpq 原生 C API 后端实现
 */

#pragma once

#ifdef HICAL_HAS_PGSQL

	#include "DbConfig.h"
	#include "DbConnection.h"
	#include "DbConnectionPool.h"
	#include "PgStmtCache.h"
	#include "core/Coroutine.h"
	#include <libpq-fe.h>
	#include <chrono>
	#include <memory>
	#include <span>
	#include <string>
	#include <string_view>
	#include <vector>

namespace hical::db
{

	/**
	 * @brief PostgreSQL 数据库连接实现(基于 libpq 原生 C API)
	 * PGconn 是 libpq 暴露的 C 结构, 不能包智能指针, 析构时用 PQfinish 手动释放。
	 * 查询参数化使用 PQexecPrepared(服务端预编译), 与 MySQL 的 PreparedStatement 对齐。
	 */
	class PgsqlConnection : public DbConnection
	{
	public:
		~PgsqlConnection() override;

		/**
		 * @brief 异步工厂方法: 创建并连接到 PostgreSQL 服务器
		 * @param ioCtx io_context 引用
		 * @param config 数据库配置
		 * @return 已连接的 PgsqlConnection
		 */
		[[nodiscard]] static Awaitable<std::shared_ptr<PgsqlConnection>> create(boost::asio::io_context& ioCtx,
																				const DbConfig& config);

		/**
		 * @brief 创建 PgsqlConnection 工厂函数(可传入 DbConnectionPool)
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
		explicit PgsqlConnection(boost::asio::io_context& ioCtx, size_t stmtCacheSize);

		/// 获取或准备 statement（使用缓存），返回 statement 名
		Awaitable<std::string> getOrPrepare(std::string_view sql);

		/// 执行已 prepare 的 statement 并返回结果
		Awaitable<DbResult> executePrepared(const std::string& stmtName,
											const std::vector<const char*>& paramValues,
											const std::vector<int>& paramLengths,
											const std::vector<int>& paramFormats,
											bool detectReturning);

		/// 将 PGresult* 转换为 DbResult，detectReturning 控制是否识别第一行第一列为 insertId
		static DbResult convertResults(PGresult* result, bool detectReturning);

		boost::asio::io_context& ioCtx_;
		PGconn* conn_ = nullptr;
		PgStmtCache stmtCache_;
		/// 预处理语句名字计数器（用于生成连接内唯一的 stmt_<n>）
		uint64_t stmtCounter_ = 0;
		bool alive_ = false;
		bool inTransaction_ = false;
		std::chrono::steady_clock::time_point lastActive_;
		std::chrono::steady_clock::time_point lastPing_;
	};

} // namespace hical::db

#endif // HICAL_HAS_PGSQL
