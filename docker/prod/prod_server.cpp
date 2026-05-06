/**
 * @file prod_server.cpp
 * @brief Hical 生产级 HTTP 服务器入口
 *
 * 演示完整的生产中间件栈：CORS → 日志 → Session → DB → 业务路由。
 * 包含健康检查、Prometheus 指标、日志管理端点。
 *
 * 编译方式：
 *   cmake -B build -DHICAL_BUILD_DEPLOY=ON -DHICAL_WITH_DATABASE=ON ...
 *   cmake --build build --target prod_server
 */

#include "core/HttpServer.h"
#include "core/AsyncFileSink.h"
#include "core/Cors.h"
#include "core/Log.h"
#include "core/LogAdmin.h"
#include "core/LogFormatter.h"
#include "core/LogMiddleware.h"
#include "core/LogSink.h"
#include "core/Session.h"
#include "core/StaticFiles.h"
#include "core/WebSocket.h"

#ifdef HICAL_HAS_DATABASE
	#include "db/DbConfig.h"
	#include "db/DbConnectionPool.h"
	#include "db/DbMiddleware.h"
	#include "db/DbQueryLog.h"
	#include "db/MysqlConnection.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

using namespace hical;

// ============ 环境变量辅助 ============

static std::string getEnv(const char* name, const std::string& fallback = "")
{
	const char* val = std::getenv(name);
	return val ? std::string(val) : fallback;
}

static uint16_t getEnvUint16(const char* name, uint16_t fallback)
{
	auto str = getEnv(name);
	if (str.empty())
	{
		return fallback;
	}
	return static_cast<uint16_t>(std::stoi(str));
}

static size_t getEnvSizeT(const char* name, size_t fallback)
{
	auto str = getEnv(name);
	if (str.empty())
	{
		return fallback;
	}
	return static_cast<size_t>(std::stoul(str));
}

// ============ 简易 Prometheus 指标 ============

struct Metrics
{
	std::atomic<uint64_t> totalRequests {0};
	std::atomic<uint64_t> totalErrors {0};
	std::atomic<uint64_t> activeConnections {0};

	struct PathStats
	{
		uint64_t count = 0;
		double totalLatencyMs = 0;
	};
	std::mutex mtx;
	std::unordered_map<std::string, PathStats> pathStats;

	void record(const std::string& path, double latencyMs, int status)
	{
		totalRequests.fetch_add(1, std::memory_order_relaxed);
		if (status >= 500)
		{
			totalErrors.fetch_add(1, std::memory_order_relaxed);
		}
		std::lock_guard lock(mtx);
		auto& s = pathStats[path];
		s.count++;
		s.totalLatencyMs += latencyMs;
	}

	std::string toPrometheus() const
	{
		std::string out;
		out += "# HELP http_requests_total Total HTTP requests\n";
		out += "# TYPE http_requests_total counter\n";
		out += "http_requests_total " + std::to_string(totalRequests.load()) + "\n";
		out += "# HELP http_errors_total Total HTTP 5xx errors\n";
		out += "# TYPE http_errors_total counter\n";
		out += "http_errors_total " + std::to_string(totalErrors.load()) + "\n";
		out += "# HELP http_active_connections Current active connections\n";
		out += "# TYPE http_active_connections gauge\n";
		out += "http_active_connections " + std::to_string(activeConnections.load()) + "\n";
		return out;
	}
};

static Metrics g_metrics;

// ============ 日志初始化 ============

static void setupLogging()
{
	auto& logger = Logger::instance();

	auto logLevel = getEnv("LOG_LEVEL", "INFO");
	if (logLevel == "TRACE")
	{
		logger.setLevel(LogLevel::hTrace);
	}
	else if (logLevel == "DEBUG")
	{
		logger.setLevel(LogLevel::hDebug);
	}
	else if (logLevel == "WARN")
	{
		logger.setLevel(LogLevel::hWarn);
	}
	else if (logLevel == "ERROR")
	{
		logger.setLevel(LogLevel::hError);
	}
	else
	{
		logger.setLevel(LogLevel::hInfo);
	}

	auto logFormat = getEnv("LOG_FORMAT", "json");

	if (logFormat == "json")
	{
		logger.setFormatter(std::make_shared<JsonFormatter>());
	}
	else
	{
		logger.setFormatter(std::make_shared<TextFormatter>());
	}

	auto logOutput = getEnv("LOG_OUTPUT", "stderr");

	if (logOutput == "stderr")
	{
		logger.setSink(std::make_shared<StderrSink>());
	}
	else if (logOutput == "file")
	{
		auto logDir = getEnv("LOG_DIR", "/var/log/hical");
		AsyncFileSink::Options opts;
		opts.file.basePath = logDir + "/app.log";
		opts.file.maxFileSize = 100 * 1024 * 1024; // 100MB
		opts.file.maxFiles = 10;
		opts.bufferSize = 4 * 1024 * 1024; // 4MB
		auto sink = std::make_shared<AsyncFileSink>(opts);
		logger.setSink(sink);
	}
}

#ifdef HICAL_HAS_DATABASE
// ============ 数据库配置 ============

static db::DbConfig buildDbConfig()
{
	db::DbConfig cfg;
	cfg.host = getEnv("MYSQL_HOST", "127.0.0.1");
	cfg.port = getEnvUint16("MYSQL_PORT", 3306);
	cfg.user = getEnv("MYSQL_USER", "hical");
	cfg.password = getEnv("MYSQL_PASSWORD", "");
	cfg.database = getEnv("MYSQL_DATABASE", "hical_prod");
	cfg.charset = "utf8mb4";
	cfg.minConnections = getEnvSizeT("DB_MIN_CONNS", 2);
	cfg.maxConnections = getEnvSizeT("DB_MAX_CONNS", 16);
	return cfg;
}
#endif

// ============ main ============

int main()
{
	try
	{
		setupLogging();

		auto port = getEnvUint16("PORT", 8080);
		auto threads = getEnvSizeT("IO_THREADS", std::thread::hardware_concurrency());
		if (threads == 0)
		{
			threads = 1;
		}

		HttpServer server(port, threads);

		// ---- 服务器参数 ----
		server.setMaxBodySize(10 * 1024 * 1024);  // 10MB
		server.setMaxConnections(10000);
		server.setIdleTimeout(60.0);
		server.setShutdownTimeout(30.0);
		server.setGcInterval(60.0);

		// ---- 全局错误处理 ----
		server.setErrorHandler(
			[](const std::exception& e, const HttpRequest& req) -> HttpResponse
			{
				HICAL_LOG_ERROR("Unhandled exception: {} {} - {}", httpMethodToString(req.method()),
								req.path(), e.what());
				auto res = HttpResponse::json({{"error", "Internal Server Error"}});
				res.setStatus(HttpStatusCode::hInternalServerError);
				return res;
			});

		// ============ 中间件栈（注册顺序 = 执行顺序）============

		// 1. CORS
		CorsOptions corsOpts;
		corsOpts.allowedOrigins = {getEnv("CORS_ORIGIN", "*")};
		corsOpts.allowCredentials = false;
		server.use("cors", makeCorsMiddleware(corsOpts));

		// 2. 指标收集
		server.use("metrics",
				   [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
				   {
					   g_metrics.activeConnections.fetch_add(1, std::memory_order_relaxed);
					   auto start = std::chrono::steady_clock::now();

					   auto res = co_await next(req);

					   auto elapsed = std::chrono::steady_clock::now() - start;
					   double ms = std::chrono::duration<double, std::milli>(elapsed).count();
					   g_metrics.record(std::string(req.path()), ms, static_cast<int>(res.statusCode()));
					   g_metrics.activeConnections.fetch_sub(1, std::memory_order_relaxed);

					   co_return res;
				   });

		// 3. 日志中间件（trace-id + 访问日志）
		server.use("log", makeLogMiddleware());

		// 4. Session 中间件
		SessionOptions sessionOpts;
		sessionOpts.cookieName = "HICAL_SID";
		sessionOpts.maxAge = 3600;
		sessionOpts.secure = true;
		sessionOpts.httpOnly = true;
		auto sessionMgr = std::make_shared<SessionManager>(sessionOpts);
		server.use("session", makeSessionMiddleware(sessionMgr));

#ifdef HICAL_HAS_DATABASE
		// 5. 数据库中间件
		auto dbConfig = buildDbConfig();
		auto dbPool = std::make_shared<db::DbConnectionPool>(
			server.ioContext(), dbConfig, db::MysqlConnection::makeFactory());

		db::DbMiddlewareOptions dbOpts;
		dbOpts.autoTransaction = false;
		dbOpts.injectPool = true;
		server.use("db", db::makeDbMiddleware(dbPool, dbOpts));

		// 6. 查询日志中间件（注册在 DB 中间件之后）
		db::QueryLogOptions queryLogOpts;
		queryLogOpts.slowQueryThreshold = std::chrono::microseconds(500'000); // 500ms
		queryLogOpts.onSlowQuery = [](const db::QueryLogEntry& entry)
		{
			HICAL_LOG_WARN("Slow query ({}ms): {}", entry.duration.count() / 1000.0, entry.sql);
		};
		server.use("queryLog", db::makeQueryLogMiddleware(queryLogOpts));
#endif

		// ============ 基础设施端点 ============

		// 健康检查（liveness）
		server.router().get("/health",
							[](const HttpRequest&) -> HttpResponse
							{
								return HttpResponse::json({{"status", "ok"}});
							});

#ifdef HICAL_HAS_DATABASE
		// 健康检查（readiness，含数据库连通性）
		server.router().get("/health/ready",
							[dbPool](const HttpRequest&) -> Awaitable<HttpResponse>
							{
								try
								{
									auto conn = co_await dbPool->acquire();
									co_await conn->ping();
									dbPool->release(std::move(conn));
									co_return HttpResponse::json({
										{"status", "ok"},
										{"db", "connected"},
									});
								}
								catch (...)
								{
									auto res = HttpResponse::json({
										{"status", "degraded"},
										{"db", "disconnected"},
									});
									res.setStatus(HttpStatusCode::hServiceUnavailable);
									co_return res;
								}
							});
#endif

		// Prometheus 指标
		server.router().get("/metrics",
							[](const HttpRequest&) -> HttpResponse
							{
								auto body = g_metrics.toPrometheus();
								auto res = HttpResponse::ok(std::move(body));
								res.setHeader("Content-Type", "text/plain; version=0.0.4");
								return res;
							});

		// 日志管理端点（GET/PUT /admin/log-level）
		registerLogAdminEndpoints(server.router(), "/admin");

		// ============ 静态文件 ============

		server.router().get("/static/{path}", serveStatic("./public", "/static/"));

		// ============ 业务路由示例（CRUD 用户）============

#ifdef HICAL_HAS_DATABASE
		// GET /api/users — 查询所有用户
		server.router().get("/api/users",
							[](const HttpRequest& req) -> Awaitable<HttpResponse>
							{
								auto conn = db::getDbConnection(req);
								auto result = co_await conn->query("SELECT id, name, email, created_at FROM users",
																   {});
								boost::json::array users;
								for (const auto& row : result.rows)
								{
									users.push_back(boost::json::object {
										{"id", row[0]},
										{"name", row[1]},
										{"email", row[2]},
										{"created_at", row[3]},
									});
								}
								co_return HttpResponse::json(boost::json::object {{"users", users}});
							});

		// GET /api/users/{id} — 查询单个用户
		server.router().get("/api/users/{id}",
							[](const HttpRequest& req) -> Awaitable<HttpResponse>
							{
								auto conn = db::getDbConnection(req);
								std::vector<std::string> params = {req.param("id")};
								auto result = co_await conn->query(
									"SELECT id, name, email, created_at FROM users WHERE id = ?", params);
								if (result.rows.empty())
								{
									auto res = HttpResponse::json({{"error", "User not found"}});
									res.setStatus(HttpStatusCode::hNotFound);
									co_return res;
								}
								const auto& row = result.rows[0];
								co_return HttpResponse::json(boost::json::object {
									{"id", row[0]},
									{"name", row[1]},
									{"email", row[2]},
									{"created_at", row[3]},
								});
							});

		// POST /api/users — 创建用户
		server.router().post("/api/users",
							 [](const HttpRequest& req) -> Awaitable<HttpResponse>
							 {
								 auto body = boost::json::parse(req.body()).as_object();
								 auto name = std::string(body.at("name").as_string());
								 auto email = std::string(body.at("email").as_string());

								 auto conn = db::getDbConnection(req);
								 std::vector<std::string> params = {name, email};
								 auto result =
									 co_await conn->execute("INSERT INTO users (name, email) VALUES (?, ?)", params);

								 auto res = HttpResponse::json(
									 boost::json::object {
										 {"id", std::to_string(result.insertId)},
										 {"name", name},
										 {"email", email},
									 });
								 res.setStatus(HttpStatusCode::hCreated);
								 co_return res;
							 });

		// DELETE /api/users/{id} — 删除用户
		server.router().del("/api/users/{id}",
							[](const HttpRequest& req) -> Awaitable<HttpResponse>
							{
								auto conn = db::getDbConnection(req);
								std::vector<std::string> params = {req.param("id")};
								auto result = co_await conn->execute("DELETE FROM users WHERE id = ?", params);
								if (result.affectedRows == 0)
								{
									auto res = HttpResponse::json({{"error", "User not found"}});
									res.setStatus(HttpStatusCode::hNotFound);
									co_return res;
								}
								co_return HttpResponse::json({{"deleted", true}});
							});
#else
		// 无数据库时的占位路由
		server.router().get("/api/users",
							[](const HttpRequest&) -> HttpResponse
							{
								return HttpResponse::json(boost::json::object {
									{"message", "Database not enabled. Build with -DHICAL_WITH_DATABASE=ON"},
								});
							});
#endif

		// ============ WebSocket Echo ============

		server.router().ws(
			"/ws/echo",
			[](const std::string& msg, WebSocketSession& ws) -> Awaitable<void>
			{
				co_await ws.send("Echo: " + msg);
			},
			[](WebSocketSession& ws) -> Awaitable<void>
			{
				co_await ws.send("Connected to Hical production server!");
			});

		// ============ 启动 ============

		HICAL_LOG_INFO("Hical production server starting on port {} with {} IO threads", port, threads);
		HICAL_LOG_INFO("Endpoints: /health /health/ready /metrics /admin/log-level /api/users /ws/echo /docs");

#ifdef HICAL_HAS_DATABASE
		// 初始化连接池（需要在 io_context 运行后执行，通过 co_spawn）
		boost::asio::co_spawn(
			server.ioContext(),
			[&dbPool]() -> Awaitable<void>
			{
				co_await dbPool->init();
				HICAL_LOG_INFO("Database connection pool initialized");
			},
			boost::asio::detached);
#endif

		server.start();
	}
	catch (const std::exception& e)
	{
		std::cerr << "Fatal: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}
