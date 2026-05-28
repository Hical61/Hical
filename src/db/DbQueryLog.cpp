/**
 * @file DbQueryLog.cpp
 * @brief 查询日志装饰器中间件实现
 */

#include "DbQueryLog.h"
#include "DbMiddleware.h"
#include <utility>

namespace hical::db
{

	/**
	 * @brief 日志装饰器连接：包装真实连接，拦截 query/execute 记录指标
	 */
// 装饰器必须实现所有重载（含 deprecated），抑制内部调用的 deprecated 警告
#if defined(__GNUC__) || defined(__clang__)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
	#pragma warning(push)
	#pragma warning(disable : 4996)
#endif
	class LoggingDbConnection : public DbConnection
	{
	public:
		LoggingDbConnection(std::shared_ptr<DbConnection> real,
							std::shared_ptr<std::vector<QueryLogEntry>> log,
							std::chrono::microseconds slowThreshold,
							QueryLogOptions::SlowQueryCallback slowCb)
			: real_(std::move(real)), log_(std::move(log)), slowThreshold_(slowThreshold), slowCb_(std::move(slowCb))
		{
		}

		Awaitable<DbResult> query(std::string_view sql) override
		{
			auto start = std::chrono::steady_clock::now();
			auto result = co_await real_->query(sql);
			recordEntry(std::string(sql), start, result, false);
			co_return result;
		}

		Awaitable<DbResult> query(std::string_view sql, std::span<const std::string> params) override
		{
			auto start = std::chrono::steady_clock::now();
			auto result = co_await real_->query(sql, params);
			recordEntry(std::string(sql), start, result, true);
			co_return result;
		}

		Awaitable<DbResult> execute(std::string_view sql) override
		{
			auto start = std::chrono::steady_clock::now();
			auto result = co_await real_->execute(sql);
			recordEntry(std::string(sql), start, result, false);
			co_return result;
		}

		Awaitable<DbResult> execute(std::string_view sql, std::span<const std::string> params) override
		{
			auto start = std::chrono::steady_clock::now();
			auto result = co_await real_->execute(sql, params);
			recordEntry(std::string(sql), start, result, true);
			co_return result;
		}

		// ============ 直接转发到真实连接 ============

		Awaitable<void> beginTransaction() override
		{
			return real_->beginTransaction();
		}

		Awaitable<void> commit() override
		{
			return real_->commit();
		}

		Awaitable<void> rollback() override
		{
			return real_->rollback();
		}

		bool inTransaction() const override
		{
			return real_->inTransaction();
		}

		bool isAlive() const override
		{
			return real_->isAlive();
		}

		Awaitable<bool> ping() override
		{
			return real_->ping();
		}

		std::string_view backend() const override
		{
			return real_->backend();
		}

		std::chrono::steady_clock::time_point lastActiveTime() const override
		{
			return real_->lastActiveTime();
		}

		std::chrono::steady_clock::time_point lastPingTime() const override
		{
			return real_->lastPingTime();
		}

		void touch() override
		{
			real_->touch();
		}

	private:
		void recordEntry(std::string sql,
						 std::chrono::steady_clock::time_point start,
						 const DbResult& result,
						 bool parameterized)
		{
			auto elapsed =
				std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);

			QueryLogEntry entry {.sql = std::move(sql),
								 .duration = elapsed,
								 .rowCount = result.rows.size(),
								 .affectedRows = result.affectedRows,
								 .isParameterized = parameterized};

			// 慢查询回调
			if (slowThreshold_.count() > 0 && elapsed >= slowThreshold_ && slowCb_)
			{
				slowCb_(entry);
			}

			log_->push_back(std::move(entry));
		}

		std::shared_ptr<DbConnection> real_;
		std::shared_ptr<std::vector<QueryLogEntry>> log_;
		std::chrono::microseconds slowThreshold_;
		QueryLogOptions::SlowQueryCallback slowCb_;
	};
#if defined(__GNUC__) || defined(__clang__)
	#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
	#pragma warning(pop)
#endif

	// ============ 中间件工厂实现 ============

	MiddlewareHandler makeQueryLogMiddleware(QueryLogOptions opts)
	{
		return [opts = std::move(opts)](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			// 获取真实连接
			auto realConn = getDbConnection(req);

			// 创建日志收集器和装饰器
			auto logEntries = std::make_shared<std::vector<QueryLogEntry>>();
			auto loggingConn =
				std::make_shared<LoggingDbConnection>(realConn, logEntries, opts.slowQueryThreshold, opts.onSlowQuery);

			// 替换请求中的连接为装饰器
			req.setAttribute(DbConnectionPool::hConnKey, std::static_pointer_cast<DbConnection>(loggingConn));

			// 将日志条目列表存入请求属性
			req.setAttribute(hQueryLogKey, logEntries);

			std::exception_ptr eptr;
			HttpResponse res;
			try
			{
				res = co_await next(req);
			}
			catch (...)
			{
				eptr = std::current_exception();
			}

			// 无论成功还是异常，都恢复原始连接（确保 DbMiddleware 归还的是真实连接）
			req.setAttribute(DbConnectionPool::hConnKey, realConn);

			// 请求完成回调（无论成功或异常都触发，让用户决定如何处理）
			if (opts.onRequestComplete)
			{
				opts.onRequestComplete(req, *logEntries);
			}

			if (eptr)
			{
				std::rethrow_exception(eptr);
			}

			co_return res;
		};
	}

} // namespace hical::db
