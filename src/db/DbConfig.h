/**
 * @file DbConfig.h
 * @brief 数据库连接池配置
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace hical::db
{

	/**
	 * @brief 数据库连接配置
	 * 包含服务器地址、认证信息、连接池参数等。
	 */
	struct DbConfig
	{
		std::string host = "127.0.0.1";
		uint16_t port = 3306;
		std::string user;
		std::string password;
		std::string database;
		std::string charset = "utf8mb4";

		// ============ 连接池参数 ============

		/// 最小空闲连接数（初始化时预创建）
		size_t minConnections = 2;

		/// 最大连接数（含活跃 + 空闲）
		size_t maxConnections = 16;

		/// 空闲连接超时回收时间
		std::chrono::seconds idleTimeout {300};

		/// 获取连接的超时时间（池满时等待上限）
		std::chrono::seconds acquireTimeout {5};

		/// 查询执行超时时间
		std::chrono::seconds queryTimeout {30};

		/// 断线后是否自动重连
		bool autoReconnect = true;

		/// 空闲连接回收检查间隔
		std::chrono::seconds idleCheckInterval {60};

		/// 后台健康检查间隔（定期 ping 空闲连接，0=禁用，使用 acquire 时 ping）
		std::chrono::seconds healthCheckInterval {30};

		/// acquire() 跳过 ping 的宽限期（连接最近被 ping 过则不再 ping）
		std::chrono::seconds pingGracePeriod {15};

		/// 每连接 PreparedStatement 缓存大小（0=禁用）
		size_t stmtCacheSize = 64;
	};

} // namespace hical::db
