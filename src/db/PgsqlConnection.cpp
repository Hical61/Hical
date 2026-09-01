/**
 * @file PgsqlConnection.cpp
 * @brief libpq 原生 C API 后端实现（非阻塞连接 + 静态 SQL 查询/执行）
 */

#ifdef HICAL_HAS_PGSQL

	#include "PgsqlConnection.h"
	#include "PgSocketAdapter.h"
	#include <boost/asio/use_awaitable.hpp>
	#include <stdexcept>
	#include <string>

namespace hical::db
{

	PgsqlConnection::PgsqlConnection(boost::asio::io_context& ioCtx, size_t stmtCacheSize)
		: ioCtx_(ioCtx), stmtCache_(stmtCacheSize), lastActive_(std::chrono::steady_clock::now())
	{
	}

	PgsqlConnection::~PgsqlConnection()
	{
		// 清理 statement 缓存（仅删映射条目，服务端由 PQfinish 统一回收）
		stmtCache_.clear();

		if (conn_ != nullptr)
		{
			PQfinish(conn_);
			conn_ = nullptr;
		}
	}

	namespace
	{
		/**
		 * @brief 转义 conninfo 中的单个值
		 * 跟随 libpq PQconnectdbParams 的转义规则：值用单引号包裹，
		 * 内部单引号用反斜杠转义，反斜杠本身也转义，空串写成空引号。
		 */
		std::string escapeConninfoValue(std::string_view value)
		{
			std::string escaped;
			escaped.reserve(value.size() + 2);
			escaped.push_back('\'');
			for (char c : value)
			{
				if (c == '\'' || c == '\\')
				{
					escaped.push_back('\\');
				}
				escaped.push_back(c);
			}
			escaped.push_back('\'');
			return escaped;
		}

		/**
		 * @brief 拼接 conninfo 字符串（keyword=value 格式，关键字之间空格分隔）
		 */
		std::string buildConnInfo(const DbConfig& config)
		{
			std::string conninfo;
			conninfo.reserve(128);

			conninfo += "host=";
			conninfo += escapeConninfoValue(config.host);
			conninfo += " port=";
			conninfo += std::to_string(config.port);
			conninfo += " user=";
			conninfo += escapeConninfoValue(config.user);

			if (!config.password.empty())
			{
				conninfo += " password=";
				conninfo += escapeConninfoValue(config.password);
			}

			if (!config.database.empty())
			{
				conninfo += " dbname=";
				conninfo += escapeConninfoValue(config.database);
			}

			return conninfo;
		}

		/**
		 * @brief 判断 SQL 文本是否显式含 RETURNING 关键字（大小写不敏感）
		 * 用来区分「INSERT ... RETURNING 拿 insertId」和「普通 SELECT」。
		 * 直接做子串匹配就够了，RETURNING 几乎不会出现在别的标识符里，
		 * 误判了顶多 insertId 填错，不致命。
		 */
		bool hasReturningKeyword(std::string_view sql)
		{
			auto upper = [](char c)
			{
				return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
			};

			for (size_t i = 0; i + 9 <= sql.size(); ++i)
			{
				if (upper(sql[i]) == 'R' && upper(sql[i + 1]) == 'E' && upper(sql[i + 2]) == 'T'
					&& upper(sql[i + 3]) == 'U' && upper(sql[i + 4]) == 'R' && upper(sql[i + 5]) == 'N'
					&& upper(sql[i + 6]) == 'I' && upper(sql[i + 7]) == 'N' && upper(sql[i + 8]) == 'G')
				{
					return true;
				}
			}
			return false;
		}

		/**
		 * @brief 从 PGresult 读取 affected rows
		 * PQcmdTuples 返回字符串，解析为无符号数；解析失败返回 0。
		 */
		uint64_t affectedRowsOf(PGresult* result)
		{
			const char* tuples = PQcmdTuples(result);
			if (tuples == nullptr || tuples[0] == '\0')
			{
				return 0;
			}

			uint64_t count = 0;
			for (const char* p = tuples; *p != '\0'; ++p)
			{
				if (*p < '0' || *p > '9')
				{
					return 0;
				}
				count = count * 10 + static_cast<uint64_t>(*p - '0');
			}
			return count;
		}

		/**
		 * @brief PGresult 的 RAII 守卫，析构时自动 PQclear
		 * 确保 co_await 异常路径（等待可读/可写时被取消或 socket 出错）下结果不泄漏。
		 */
		class PgResultGuard
		{
		public:
			explicit PgResultGuard(PGresult* result) : result_(result)
			{
			}

			PgResultGuard(const PgResultGuard&) = delete;
			PgResultGuard& operator=(const PgResultGuard&) = delete;

			~PgResultGuard()
			{
				if (result_ != nullptr)
				{
					PQclear(result_);
				}
			}

			[[nodiscard]] PGresult* get() const
			{
				return result_;
			}

			void reset(PGresult* result)
			{
				if (result_ != nullptr)
				{
					PQclear(result_);
				}
				result_ = result;
			}

		private:
			PGresult* result_;
		};
	} // namespace

	Awaitable<std::shared_ptr<PgsqlConnection>> PgsqlConnection::create(boost::asio::io_context& ioCtx,
																		const DbConfig& config)
	{
		std::shared_ptr<PgsqlConnection> conn(new PgsqlConnection(ioCtx, config.stmtCacheSize));

		std::string conninfo = buildConnInfo(config);
		PGconn* raw = PQconnectStart(conninfo.c_str());
		if (raw == nullptr)
		{
			// conninfo 非法（几乎不可能，关键字都是合法的），libpq 文档约定此时返回 nullptr
			throw std::runtime_error("PQconnectStart failed: unable to allocate PGconn");
		}
		conn->conn_ = raw;

		// 连接状态可能会推进多次，poll 结果决定下一步等待哪种 socket 事件。
		// 正常情况下走 READING/WRITING 分支交替向前，OK 时跳出。
		try
		{
			PgSocketAdapter adapter(ioCtx, raw);
			for (;;)
			{
				PostgresPollingStatusType status = PQconnectPoll(raw);
				switch (status)
				{
					case PGRES_POLLING_OK:
					{
						// 真正连接成功与否看 PQstatus；可能握手失败但协议走完
						if (PQstatus(raw) != CONNECTION_OK)
						{
							throw std::runtime_error(std::string("connection failed: ") + PQerrorMessage(raw));
						}
						conn->alive_ = true;
						conn->touch();
						co_return conn;
					}
					case PGRES_POLLING_READING:
						co_await adapter.waitReadable();
						break;
					case PGRES_POLLING_WRITING:
						co_await adapter.waitWritable();
						break;
					case PGRES_POLLING_FAILED:
						throw std::runtime_error(std::string("connection failed: ") + PQerrorMessage(raw));
					case PGRES_POLLING_ACTIVE:
						// 正在处理中，无 socket 事件可等，立刻再 poll 一次
						break;
					default:
						throw std::runtime_error("PQconnectPoll returned unexpected status");
				}
			}
		}
		catch (...)
		{
			// 连接失败：conn 的析构会 PQfinish 回收资源
			throw;
		}
	}

	DbConnectionFactory PgsqlConnection::makeFactory()
	{
		return [](boost::asio::io_context& ioCtx, const DbConfig& config) -> Awaitable<std::shared_ptr<DbConnection>>
		{
			co_return co_await PgsqlConnection::create(ioCtx, config);
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

	Awaitable<DbResult> PgsqlConnection::query(std::string_view sql)
	{
		// 发送：PQsendQuery 返回 0 表示入队失败
		std::string sqlStr(sql);
		if (PQsendQuery(conn_, sqlStr.c_str()) == 0)
		{
			throw std::runtime_error(std::string("PQsendQuery failed: ") + PQerrorMessage(conn_));
		}

		PgSocketAdapter adapter(ioCtx_, conn_);

		// 把发送缓冲区刷干：PQflush 返回 1 表示还有数据未发完，需等可写
		int flushStatus;
		while ((flushStatus = PQflush(conn_)) == 1)
		{
			co_await adapter.waitWritable();
		}
		if (flushStatus == -1)
		{
			throw std::runtime_error(std::string("PQflush failed: ") + PQerrorMessage(conn_));
		}

		// 收结果：消费所有可读数据，取出全部 PGresult 直到 PQgetResult 返回 nullptr
		PgResultGuard result(nullptr);
		for (;;)
		{
			if (PQconsumeInput(conn_) == 0)
			{
				throw std::runtime_error(std::string("PQconsumeInput failed: ") + PQerrorMessage(conn_));
			}

			if (PQisBusy(conn_))
			{
				co_await adapter.waitReadable();
				continue;
			}

			PGresult* got = PQgetResult(conn_);
			if (got == nullptr)
			{
				break;
			}

			// 只保留最后一个结果（惯例：单条语句只产生一个结果集）。
			// reset 会先 PQclear 旧的，再接管新的，异常路径下也不泄漏。
			result.reset(got);
		}

		if (result.get() == nullptr)
		{
			// 理论上 PQsendQuery 成功后必有结果；防御性兜底
			throw std::runtime_error("query produced no PGresult");
		}

		DbResult dbResult = convertResults(result.get(), hasReturningKeyword(sqlStr));
		touch();
		co_return dbResult;
	}

	Awaitable<DbResult> PgsqlConnection::query(std::string_view sql, std::span<const std::string> params)
	{
		// 没参数就直接走静态 SQL，省得为一个空查询去 prepare，NULL 语义也一致。
		if (params.empty())
		{
			co_return co_await query(sql);
		}

		// PostgreSQL 参数化用 $1 $2 ... 占位（与 MySQL 的 ? 不同），statement 名到 SQL
		// 的映射缓存在 stmtCache_，命中就不用重复 PQprepare。
		const bool detectReturning = hasReturningKeyword(sql);
		std::string stmtName = co_await getOrPrepare(sql);

		// 把 span 里的 std::string 参数整理成 libpq 需要的三个并行数组。
		// 全部走文本格式（paramFormats[] 置 0），std::string 直接 c_str()/size()。
		// 数组底层由 vector 持有，跨越 co_await 期间有效，不会悬垂。
		std::vector<const char*> paramValues;
		std::vector<int> paramLengths;
		std::vector<int> paramFormats;
		paramValues.reserve(params.size());
		paramLengths.reserve(params.size());
		paramFormats.reserve(params.size());
		for (const auto& p : params)
		{
			paramValues.push_back(p.c_str());
			paramLengths.push_back(static_cast<int>(p.size()));
			paramFormats.push_back(0);
		}

		bool needRetry = false;
		DbResult dbResult;
		try
		{
			dbResult = co_await executePrepared(stmtName, paramValues, paramLengths, paramFormats, detectReturning);
		}
		catch (...)
		{
			// statement 可能因服务器重启/超时失效（errCode 26000 invalid_sql_statement_name），
			// 清掉缓存里这条 SQL 的映射，重新 prepare 后再执行一次。
			stmtCache_.erase(sql);
			needRetry = true;
		}

		if (needRetry)
		{
			std::string freshName = co_await getOrPrepare(sql);
			dbResult = co_await executePrepared(freshName, paramValues, paramLengths, paramFormats, detectReturning);
		}

		touch();
		co_return dbResult;
	}

	Awaitable<DbResult> PgsqlConnection::execute(std::string_view sql)
	{
		return query(sql);
	}

	Awaitable<DbResult> PgsqlConnection::execute(std::string_view sql, std::span<const std::string> params)
	{
		return query(sql, params);
	}

	#if defined(__GNUC__) || defined(__clang__)
		#pragma GCC diagnostic pop
	#elif defined(_MSC_VER)
		#pragma warning(pop)
	#endif

	Awaitable<std::string> PgsqlConnection::getOrPrepare(std::string_view sql)
	{
		// 命中缓存：直接用已有的 statement 名字，省一次服务端 prepare 开销
		if (auto cached = stmtCache_.find(sql))
		{
			co_return *cached;
		}

		// 未命中：生成连接内唯一名字并 prepare，再写入缓存。
		// stmt_<counter> 单调递增，连接生命周期内不会重名；反斜杠无任何意义，
		// 名字仅供 libpq 内部标识，不参与 SQL 语义。
		std::string stmtName = "stmt_" + std::to_string(++stmtCounter_);

		std::string sqlStr(sql);
		// PQsendPrepare 用 0 个形参（paramTypes 传 nullptr）：libpq 会在首次执行时
		// 从实际绑定的参数文本推断各列类型，无需在 prepare 阶段给出完整类型 OID。
		if (PQsendPrepare(conn_, stmtName.c_str(), sqlStr.c_str(), 0, nullptr) == 0)
		{
			throw std::runtime_error(std::string("PQsendPrepare failed: ") + PQerrorMessage(conn_));
		}

		PgSocketAdapter adapter(ioCtx_, conn_);

		// 刷干发送缓冲区，等待可写直到发送完成
		int flushStatus;
		while ((flushStatus = PQflush(conn_)) == 1)
		{
			co_await adapter.waitWritable();
		}
		if (flushStatus == -1)
		{
			throw std::runtime_error(std::string("PQflush failed: ") + PQerrorMessage(conn_));
		}

		// 收结果拿最后一个 PGresult（prepare 只有一个结果）
		PgResultGuard result(nullptr);
		for (;;)
		{
			if (PQconsumeInput(conn_) == 0)
			{
				throw std::runtime_error(std::string("PQconsumeInput failed: ") + PQerrorMessage(conn_));
			}

			if (PQisBusy(conn_))
			{
				co_await adapter.waitReadable();
				continue;
			}

			PGresult* got = PQgetResult(conn_);
			if (got == nullptr)
			{
				break;
			}
			result.reset(got);
		}

		if (result.get() == nullptr)
		{
			throw std::runtime_error("prepare produced no PGresult");
		}

		ExecStatusType status = PQresultStatus(result.get());
		if (status != PGRES_COMMAND_OK)
		{
			throw std::runtime_error(std::string("prepare failed: ") + PQresultErrorMessage(result.get()));
		}

		stmtCache_.insert(std::move(sqlStr), stmtName);
		co_return stmtName;
	}

	Awaitable<DbResult> PgsqlConnection::executePrepared(const std::string& stmtName,
														 const std::vector<const char*>& paramValues,
														 const std::vector<int>& paramLengths,
														 const std::vector<int>& paramFormats,
														 bool detectReturning)
	{
		// 参数数组为空则传空指针（size 0 时 .data() 符合预期，但显式空指针更符合 libpq
		// 约定）；text 格式让每一列都走服务端解析，字符编码由 connection 层保证。
		const char* const* valuesPtr = paramValues.empty() ? nullptr : paramValues.data();
		const int* lengthsPtr = paramLengths.empty() ? nullptr : paramLengths.data();
		const int* formatsPtr = paramFormats.empty() ? nullptr : paramFormats.data();

		// resultFormat = 0（文本），与 convertResults 的文本解析约定一致
		if (PQsendQueryPrepared(conn_,
								stmtName.c_str(),
								static_cast<int>(paramValues.size()),
								valuesPtr,
								lengthsPtr,
								formatsPtr,
								0)
			== 0)
		{
			throw std::runtime_error(std::string("PQsendQueryPrepared failed: ") + PQerrorMessage(conn_));
		}

		PgSocketAdapter adapter(ioCtx_, conn_);

		// 刷干发送缓冲区
		int flushStatus;
		while ((flushStatus = PQflush(conn_)) == 1)
		{
			co_await adapter.waitWritable();
		}
		if (flushStatus == -1)
		{
			throw std::runtime_error(std::string("PQflush failed: ") + PQerrorMessage(conn_));
		}

		// 收结果，取最后一个 PGresult（多语句结果只保留最后，与静态查询一致）
		PgResultGuard result(nullptr);
		for (;;)
		{
			if (PQconsumeInput(conn_) == 0)
			{
				throw std::runtime_error(std::string("PQconsumeInput failed: ") + PQerrorMessage(conn_));
			}

			if (PQisBusy(conn_))
			{
				co_await adapter.waitReadable();
				continue;
			}

			PGresult* got = PQgetResult(conn_);
			if (got == nullptr)
			{
				break;
			}
			result.reset(got);
		}

		if (result.get() == nullptr)
		{
			throw std::runtime_error("PQsendQueryPrepared produced no PGresult");
		}

		co_return convertResults(result.get(), detectReturning);
	}

	// 事务命令都是固定关键字（BEGIN/COMMIT/ROLLBACK），不存在注入面，
	// 走无参数重载属于合法内部用法，这里同样抑制 deprecated 警告。
	#if defined(__GNUC__) || defined(__clang__)
		#pragma GCC diagnostic push
		#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	#elif defined(_MSC_VER)
		#pragma warning(push)
		#pragma warning(disable : 4996)
	#endif

	Awaitable<void> PgsqlConnection::beginTransaction()
	{
		// BEGIN 走静态 SQL 路径（内部是非阻塞 PQsendQuery + PgResultGuard），
		// 命令成功返回后本地状态才置 true，和 MysqlConnection 的 bool 跟踪一致。
		co_await query("BEGIN");
		inTransaction_ = true;
		touch();
	}

	Awaitable<void> PgsqlConnection::commit()
	{
		// COMMIT 失败会抛异常，此时事务尚未真正结束，inTransaction_ 保持 true，
		// 由上层决定重试还是 rollback，避免误以为已提交。成功才切回 false。
		co_await query("COMMIT");
		inTransaction_ = false;
		touch();
	}

	Awaitable<void> PgsqlConnection::rollback()
	{
		// ROLLBACK 命令失败会抛异常（与 MysqlConnection 对齐），此时本地 inTransaction_
		// 保持原状，由上层（如 DbConnectionPool::release 的兜底逻辑）决定丢弃连接还是重试。
		// 真正的「状态复位」只在 ROLLBACK 成功返回后发生，与 commit 的语义一致。
		co_await query("ROLLBACK");
		inTransaction_ = false;
		touch();
	}

	#if defined(__GNUC__) || defined(__clang__)
		#pragma GCC diagnostic pop
	#elif defined(_MSC_VER)
		#pragma warning(pop)
	#endif

	bool PgsqlConnection::inTransaction() const
	{
		return inTransaction_;
	}

	bool PgsqlConnection::isAlive() const
	{
		return alive_;
	}

	Awaitable<bool> PgsqlConnection::ping()
	{
		// 执行 SELECT 1 验证连接还活着。走参数化重载传空 span，
		// 既避开无参数重载的 deprecated 警告，也复用 query 的非阻塞流程。
		try
		{
			auto res = co_await query("SELECT 1", std::span<const std::string> {});
			(void)res;
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

	std::string_view PgsqlConnection::backend() const
	{
		return "pgsql";
	}

	std::chrono::steady_clock::time_point PgsqlConnection::lastActiveTime() const
	{
		return lastActive_;
	}

	std::chrono::steady_clock::time_point PgsqlConnection::lastPingTime() const
	{
		return lastPing_;
	}

	void PgsqlConnection::touch()
	{
		lastActive_ = std::chrono::steady_clock::now();
	}

	DbResult PgsqlConnection::convertResults(PGresult* result, bool detectReturning)
	{
		DbResult dbResult;

		if (result == nullptr)
		{
			return dbResult;
		}

		ExecStatusType status = PQresultStatus(result);
		int nrows = PQntuples(result);
		int nfields = PQnfields(result);

		// SQL 执行错误（语法错误、约束违反、表不存在等）返回 PGRES_FATAL_ERROR。
		// 必须抛异常传播，否则错误被静默吞掉变成空结果集，调用方会把「执行失败」
		// 误解成「查询成功返回 0 行」。与 MysqlConnection 靠 async_execute 抛
		// boost::system::system_error 的行为对齐。
		if (status == PGRES_FATAL_ERROR || status == PGRES_BAD_RESPONSE)
		{
			throw std::runtime_error(std::string("pgsql error: ") + PQresultErrorMessage(result));
		}

		// DML 命令（INSERT/UPDATE/DELETE 等）返回 PGRES_COMMAND_OK，没有结果集，
		// 用 PQcmdTuples 拿 affected rows。
		if (status == PGRES_COMMAND_OK)
		{
			dbResult.affectedRows = affectedRowsOf(result);
			return dbResult;
		}

		// 有结果集的场景（PGRES_TUPLES_OK）
		if (nfields <= 0)
		{
			return dbResult;
		}

		// INSERT ... RETURNING id 走的是 PGRES_TUPLES_OK，结果集第一行第一列就是
		// 新插入的主键。PostgreSQL 没有 MySQL 的 last_insert_id()，业务想拿 insertId
		// 必须显式写 RETURNING。这里只在 detectReturning 为真（SQL 显式含 RETURNING）
		// 时才解析第一行第一列；普通 SELECT 首列碰巧是整数不会被误填，从而避免
		// 调用方把一次普通查询误解成 INSERT 的返回。
		if (detectReturning && nrows > 0 && !PQgetisnull(result, 0, 0))
		{
			const char* first = PQgetvalue(result, 0, 0);
			uint64_t parsed = 0;
			bool valid = first[0] != '\0';
			for (const char* p = first; valid && *p != '\0'; ++p)
			{
				if (*p < '0' || *p > '9')
				{
					valid = false;
					break;
				}
				parsed = parsed * 10 + static_cast<uint64_t>(*p - '0');
			}
			if (valid)
			{
				dbResult.insertId = parsed;
			}
		}

		// 列名
		dbResult.columns.reserve(static_cast<size_t>(nfields));
		for (int col = 0; col < nfields; ++col)
		{
			const char* name = PQfname(result, col);
			dbResult.columns.emplace_back(name != nullptr ? name : "");
		}

		// 行数据（libpq 已格式化为文本），NULL 渲染为空串
		dbResult.rows.reserve(static_cast<size_t>(nrows));
		for (int row = 0; row < nrows; ++row)
		{
			std::vector<std::string> dbRow;
			dbRow.reserve(static_cast<size_t>(nfields));
			for (int col = 0; col < nfields; ++col)
			{
				if (PQgetisnull(result, row, col))
				{
					dbRow.emplace_back();
				}
				else
				{
					const char* value = PQgetvalue(result, row, col);
					int length = PQgetlength(result, row, col);
					dbRow.emplace_back(value, static_cast<size_t>(length));
				}
			}
			dbResult.rows.push_back(std::move(dbRow));
		}

		return dbResult;
	}

} // namespace hical::db

#endif // HICAL_HAS_PGSQL
