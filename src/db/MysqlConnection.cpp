/**
 * @file MysqlConnection.cpp
 * @brief MySQL 连接实现
 */

#ifdef HICAL_HAS_DATABASE

	#include "MysqlConnection.h"
	#include <boost/asio/use_awaitable.hpp>
	#include <boost/mysql/any_connection.hpp>
	#include <boost/mysql/connect_params.hpp>
	#include <boost/mysql/results.hpp>
	#include <boost/mysql/statement.hpp>
	#include <charconv>
	#include <stdexcept>

namespace hical::db
{

	MysqlConnection::MysqlConnection(boost::asio::io_context& ioCtx, size_t stmtCacheSize)
		: ioCtx_(ioCtx)
		, conn_(ioCtx.get_executor())
		, stmtCache_(stmtCacheSize)
		, lastActive_(std::chrono::steady_clock::now())
	{
	}

	MysqlConnection::~MysqlConnection()
	{
		// 清理 statement 缓存（连接即将关闭，服务端会自动回收，无需异步 close）
		stmtCache_.clear();
	}

	void MysqlConnection::validateCharset(const std::string& charset)
	{
		// 白名单：仅允许字母、数字、下划线。
		// 此校验与 create() 中的 "SET NAMES '" + charset + "'" 拼接强耦合，
		// 修改此校验规则前必须评估 SQL 注入风险。
		for (char c : charset)
		{
			if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
			{
				throw std::invalid_argument("DbConfig: invalid charset '" + charset + "'");
			}
		}
	}

	Awaitable<std::shared_ptr<MysqlConnection>> MysqlConnection::create(boost::asio::io_context& ioCtx,
																		const DbConfig& config)
	{
		auto conn = std::shared_ptr<MysqlConnection>(new MysqlConnection(ioCtx, config.stmtCacheSize));

		// 构建连接参数
		boost::mysql::connect_params params;
		params.server_address.emplace_host_and_port(config.host, config.port);
		params.username = config.user;
		params.password = config.password;
		params.database = config.database;

		// 连接
		co_await conn->conn_.async_connect(params, boost::asio::use_awaitable);
		conn->alive_ = true;

		// 设置元数据模式为 full(获取列名等完整信息)
		conn->conn_.set_meta_mode(boost::mysql::metadata_mode::full);

		// 设置字符集(通过执行 SET NAMES)
		if (!config.charset.empty())
		{
			validateCharset(config.charset);
			boost::mysql::results charsetResult;
			std::string setNamesSQL = "SET NAMES '" + config.charset + "'";
			co_await conn->conn_.async_execute(setNamesSQL, charsetResult, boost::asio::use_awaitable);
		}

		conn->touch();
		co_return conn;
	}

	DbConnectionFactory MysqlConnection::makeFactory()
	{
		return [](boost::asio::io_context& ioCtx, const DbConfig& config) -> Awaitable<std::shared_ptr<DbConnection>>
		{
			co_return co_await MysqlConnection::create(ioCtx, config);
		};
	}

	// 以下实现中框架内部调用无参数化重载属于合法用途（静态 SQL），抑制 deprecated 警告
	#if defined(__GNUC__) || defined(__clang__)
		#pragma GCC diagnostic push
		#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	#elif defined(_MSC_VER)
		#pragma warning(push)
		#pragma warning(disable : 4996)
	#endif

	Awaitable<DbResult> MysqlConnection::query(std::string_view sql)
	{
		boost::mysql::results boostResults;
		co_await conn_.async_execute(std::string(sql), boostResults, boost::asio::use_awaitable);
		touch();
		co_return convertResults(boostResults);
	}

	Awaitable<DbResult> MysqlConnection::query(std::string_view sql, std::span<const std::string> params)
	{
		if (params.empty())
		{
			co_return co_await query(sql);
		}

		auto stmt = co_await getOrPrepare(sql);

		// 构建参数列表
		std::vector<boost::mysql::field_view> fields;
		fields.reserve(params.size());
		for (const auto& p : params)
		{
			fields.emplace_back(p);
		}

		boost::mysql::results boostResults;
		bool needRetry = false;
		try
		{
			co_await conn_.async_execute(stmt.bind(fields.begin(), fields.end()),
										 boostResults,
										 boost::asio::use_awaitable);
		}
		catch (...)
		{
			// statement 可能已失效（服务器重启等），标记重试
			stmtCache_.erase(sql);
			needRetry = true;
		}

		if (needRetry)
		{
			// 重新 prepare 并执行（在 catch 外，允许 co_await）
			auto freshStmt = co_await conn_.async_prepare_statement(std::string(sql), boost::asio::use_awaitable);

			co_await conn_.async_execute(freshStmt.bind(fields.begin(), fields.end()),
										 boostResults,
										 boost::asio::use_awaitable);

			// 重试成功，将新 statement 放回缓存
			auto evicted = stmtCache_.insert(std::string(sql), std::move(freshStmt));
			if (evicted)
			{
				try
				{
					co_await conn_.async_close_statement(*evicted, boost::asio::use_awaitable);
				}
				catch (...)
				{
				}
			}
		}

		touch();
		co_return convertResults(boostResults);
	}

	Awaitable<boost::mysql::statement> MysqlConnection::getOrPrepare(std::string_view sql)
	{
		// 查找缓存（透明哈希，无堆分配）
		auto* cached = stmtCache_.find(sql);
		if (cached)
		{
			co_return *cached;
		}

		// 缓存未命中：prepare 新 statement
		std::string sqlStr(sql);
		auto stmt = co_await conn_.async_prepare_statement(sqlStr, boost::asio::use_awaitable);

		// 放入缓存（可能淘汰旧条目）
		auto evicted = stmtCache_.insert(sqlStr, stmt);
		if (evicted)
		{
			// 异步关闭被淘汰的 statement
			try
			{
				co_await conn_.async_close_statement(*evicted, boost::asio::use_awaitable);
			}
			catch (...)
			{
			}
		}

		co_return stmt;
	}

	Awaitable<DbResult> MysqlConnection::execute(std::string_view sql)
	{
		return query(sql);
	}

	Awaitable<DbResult> MysqlConnection::execute(std::string_view sql, std::span<const std::string> params)
	{
		return query(sql, params);
	}

	#if defined(__GNUC__) || defined(__clang__)
		#pragma GCC diagnostic pop
	#elif defined(_MSC_VER)
		#pragma warning(pop)
	#endif

	Awaitable<void> MysqlConnection::beginTransaction()
	{
		boost::mysql::results r;
		co_await conn_.async_execute("START TRANSACTION", r, boost::asio::use_awaitable);
		inTransaction_ = true;
		touch();
	}

	Awaitable<void> MysqlConnection::commit()
	{
		boost::mysql::results r;
		co_await conn_.async_execute("COMMIT", r, boost::asio::use_awaitable);
		inTransaction_ = false;
		touch();
	}

	Awaitable<void> MysqlConnection::rollback()
	{
		boost::mysql::results r;
		co_await conn_.async_execute("ROLLBACK", r, boost::asio::use_awaitable);
		inTransaction_ = false;
		touch();
	}

	bool MysqlConnection::inTransaction() const
	{
		return inTransaction_;
	}

	bool MysqlConnection::isAlive() const
	{
		return alive_;
	}

	Awaitable<bool> MysqlConnection::ping()
	{
		try
		{
			co_await conn_.async_ping(boost::asio::use_awaitable);
			alive_ = true;
			lastPing_ = std::chrono::steady_clock::now();
			touch();
			co_return true;
		}
		catch (...)
		{
			alive_ = false;
			co_return false;
		}
	}

	std::string_view MysqlConnection::backend() const
	{
		return "mysql";
	}

	std::chrono::steady_clock::time_point MysqlConnection::lastActiveTime() const
	{
		return lastActive_;
	}

	std::chrono::steady_clock::time_point MysqlConnection::lastPingTime() const
	{
		return lastPing_;
	}

	void MysqlConnection::touch()
	{
		lastActive_ = std::chrono::steady_clock::now();
	}

	DbResult MysqlConnection::convertResults(const boost::mysql::results& boostResults)
	{
		DbResult result;

		if (!boostResults.has_value())
		{
			return result;
		}

		result.affectedRows = boostResults.affected_rows();
		result.insertId = boostResults.last_insert_id();

		auto meta = boostResults.meta();
		if (meta.empty())
		{
			return result;
		}

		result.columns.reserve(meta.size());
		for (const auto& col : meta)
		{
			auto sv = col.column_name();
			result.columns.emplace_back(sv.data(), sv.size());
		}

		auto rowsView = boostResults.rows();
		result.rows.reserve(rowsView.size());
		size_t colCount = result.columns.size();

		for (auto row : rowsView)
		{
			std::vector<std::string> dbRow;
			dbRow.reserve(colCount);
			for (size_t i = 0; i < colCount && i < row.size(); ++i)
			{
				const auto& field = row.at(i);

				if (field.is_null())
				{
					dbRow.emplace_back();
				}
				else if (field.is_int64())
				{
					char buf[24];
					auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), field.as_int64());
					dbRow.emplace_back(buf, ptr);
				}
				else if (field.is_uint64())
				{
					char buf[24];
					auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), field.as_uint64());
					dbRow.emplace_back(buf, ptr);
				}
				else if (field.is_double())
				{
					char buf[32];
					auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), field.as_double());
					dbRow.emplace_back(buf, ptr);
				}
				else if (field.is_string())
				{
					dbRow.emplace_back(field.as_string());
				}
				else if (field.is_blob())
				{
					auto blob = field.as_blob();
					dbRow.emplace_back(reinterpret_cast<const char*>(blob.data()), blob.size());
				}
				else if (field.is_date())
				{
					auto d = field.as_date();
					char buf[16];
					std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u", d.year(), d.month(), d.day());
					dbRow.emplace_back(buf);
				}
				else if (field.is_datetime())
				{
					auto dt = field.as_datetime();
					char buf[32];
					std::snprintf(buf,
								  sizeof(buf),
								  "%04u-%02u-%02u %02u:%02u:%02u",
								  dt.year(),
								  dt.month(),
								  dt.day(),
								  dt.hour(),
								  dt.minute(),
								  dt.second());
					dbRow.emplace_back(buf);
				}
				else if (field.is_time())
				{
					auto t = field.as_time();
					auto totalSecs = std::chrono::duration_cast<std::chrono::seconds>(t).count();
					bool negative = totalSecs < 0;
					if (negative)
					{
						totalSecs = -totalSecs;
					}
					auto hours = static_cast<long long>(totalSecs / 3600);
					auto minutes = static_cast<long long>((totalSecs % 3600) / 60);
					auto secs = static_cast<long long>(totalSecs % 60);
					char buf[24];
					if (negative)
					{
						std::snprintf(buf, sizeof(buf), "-%02lld:%02lld:%02lld", hours, minutes, secs);
					}
					else
					{
						std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", hours, minutes, secs);
					}
					dbRow.emplace_back(buf);
				}
				else
				{
					dbRow.emplace_back();
				}
			}
			result.rows.push_back(std::move(dbRow));
		}

		return result;
	}

} // namespace hical::db

#endif // HICAL_HAS_DATABASE
