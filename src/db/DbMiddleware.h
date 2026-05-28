/**
 * @file DbMiddleware.h
 * @brief HTTP 数据库中间件（自动事务）
 */

#pragma once

#include "DbConnection.h"
#include "DbConnectionPool.h"
#include "core/Coroutine.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include "core/Middleware.h"
#include <memory>
#include <stdexcept>

namespace hical::db
{

	/**
	 * @brief DB 中间件配置选项
	 */
	struct DbMiddlewareOptions
	{
		/// 是否为每个请求自动开启事务(成功自动提交, 异常自动回滚)
		bool autoTransaction = false;

		/// 是否将连接池对象也注入到请求属性中(供高级用法使用)
		bool injectPool = true;
	};

	/**
	 * @brief 创建数据库中间件
	 * 前置阶段: 从连接池异步获取连接, 注入 req.setAttribute()
	 * 后置阶段: 归还连接到池, 异常时自动回滚事务
	 * @param pool 连接池(shared_ptr, 可多个中间件共享同一个 pool)
	 * @param opts 中间件选项
	 * @return MiddlewareHandler 可传入 server.use() 的中间件
	 */
	inline MiddlewareHandler makeDbMiddleware(std::shared_ptr<DbConnectionPool> pool, DbMiddlewareOptions opts = {})
	{
		return [pool, opts](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			// 前置: 从连接池获取连接
			auto conn = co_await pool->acquire();
			req.setAttribute(DbConnectionPool::hConnKey, conn);

			if (opts.injectPool)
			{
				req.setAttribute(DbConnectionPool::hPoolKey, pool);
			}

			// 自动事务: 开启
			if (opts.autoTransaction)
			{
				co_await conn->beginTransaction();
			}

			std::exception_ptr eptr;
			HttpResponse res;
			try
			{
				// 执行后续中间件/路由
				res = co_await next(req);

				// 后置: 提交事务
				if (opts.autoTransaction && conn->inTransaction())
				{
					co_await conn->commit();
				}
			}
			catch (...)
			{
				eptr = std::current_exception();
			}

			// 异常时同步回滚（在 catch 外，允许 co_await）
			if (eptr && conn->inTransaction())
			{
				try
				{
					co_await conn->rollback();
				}
				catch (...)
				{
				}
			}

			// 归还连接
			pool->release(std::move(conn));

			if (eptr)
			{
				std::rethrow_exception(eptr);
			}

			co_return res;
		};
	}

	/**
	 * @brief 从请求属性中获取当前请求的数据库连接
	 * @param req HTTP 请求
	 * @return 数据库连接
	 * @throws std::runtime_error 如果请求中没有注入连接(未注册 DB 中间件)
	 */
	inline std::shared_ptr<DbConnection> getDbConnection(const HttpRequest& req)
	{
		auto conn = req.getAttribute<std::shared_ptr<DbConnection>>(DbConnectionPool::hConnKey);
		if (!conn)
		{
			throw std::runtime_error("No DB connection in request. "
									 "Did you register makeDbMiddleware()?");
		}
		return *conn;
	}

	/**
	 * @brief 从请求属性中获取连接池
	 * @param req HTTP 请求
	 * @return 连接池
	 * @throws std::runtime_error 如果请求中没有注入连接池
	 */
	inline std::shared_ptr<DbConnectionPool> getDbPool(const HttpRequest& req)
	{
		auto pool = req.getAttribute<std::shared_ptr<DbConnectionPool>>(DbConnectionPool::hPoolKey);
		if (!pool)
		{
			throw std::runtime_error("No DB pool in request. "
									 "Did you register makeDbMiddleware() with injectPool=true?");
		}
		return *pool;
	}

} // namespace hical::db
