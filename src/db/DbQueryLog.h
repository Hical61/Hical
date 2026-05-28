/**
 * @file DbQueryLog.h
 * @brief 查询日志装饰器中间件
 */

#pragma once

#include "DbConnection.h"
#include "DbConnectionPool.h"
#include "core/Coroutine.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include "core/Middleware.h"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hical::db
{

	/**
	 * @brief 单条查询日志条目
	 */
	struct QueryLogEntry
	{
		std::string sql;
		std::chrono::microseconds duration {0};
		size_t rowCount = 0;
		uint64_t affectedRows = 0;
		bool isParameterized = false;
	};

	/**
	 * @brief 查询日志中间件选项
	 */
	struct QueryLogOptions
	{
		/// 请求完成后的回调（收到该请求的所有查询日志）
		using LogCallback = std::function<void(const HttpRequest& req, const std::vector<QueryLogEntry>& entries)>;
		LogCallback onRequestComplete;

		/// 慢查询阈值（超过此时长的查询触发 onSlowQuery 回调，0=禁用）
		std::chrono::microseconds slowQueryThreshold {0};

		/// 慢查询实时回调（每条慢查询立即触发）
		using SlowQueryCallback = std::function<void(const QueryLogEntry& entry)>;
		SlowQueryCallback onSlowQuery;
	};

	/// 请求属性键：查询日志条目列表
	static constexpr const char* hQueryLogKey = "hical.db.queryLog";

	/**
	 * @brief 创建查询日志中间件
	 * 必须注册在 makeDbMiddleware() **之后**，因为它需要请求中已有 DB 连接。
	 * 工作原理：用 LoggingDbConnection 装饰器包装真实连接，拦截 query/execute 记录指标。
	 * @param opts 配置选项
	 * @return MiddlewareHandler
	 */
	[[nodiscard]] MiddlewareHandler makeQueryLogMiddleware(QueryLogOptions opts = {});

} // namespace hical::db
