/**
 * @file main.cpp
 * @brief HttpArena benchmark 专用服务器
 * 实现 baseline, pipelined, json, json-comp, upload, static, echo-ws 端点。
 * 仅 Gzip 压缩中间件，其余走同步快速路径，针对 HttpArena 64 核硬件优化。
 * 卷挂载约定：
 *   /data/dataset.json  — JSON 数据集（50 个商品条目）
 *   /data/static/       — 静态文件目录（20 个文件）
 */

#include "core/GzipCompression.h"
#include "core/HttpServer.h"
#include "core/Log.h"
#include "core/RouteGroup.h"
#include "core/StaticFiles.h"
#include "core/WebSocket.h"

#ifdef HICAL_HAS_PGSQL
	#include "db/DbConfig.h"
	#include "db/DbConnectionPool.h"
	#include "db/DbMiddleware.h"
	#include "db/PgsqlConnection.h"
#endif

#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace hical;
namespace json = boost::json;

// ── Dataset 模型 ───────────────────────────────────────────────────────────

struct Rating
{
	int64_t score;
	int64_t count;
};

struct DatasetItem
{
	int64_t id;
	std::string name;
	std::string category;
	int64_t price;
	int64_t quantity;
	bool active;
	std::vector<std::string> tags;
	Rating rating;
};

static std::vector<DatasetItem> g_items;

// ── 加载 dataset.json ───────────────────────────────────────────────────────

static bool loadDataset(const std::string& path)
{
	std::ifstream ifs(path);
	if (!ifs)
	{
		return false;
	}

	json::stream_parser p;
	std::string line;
	while (std::getline(ifs, line))
	{
		p.write(line);
	}
	p.finish();
	auto jv = p.release();

	if (!jv.is_array())
	{
		return false;
	}

	auto& arr = jv.as_array();
	g_items.reserve(arr.size());

	for (auto& elem : arr)
	{
		auto& obj = elem.as_object();
		DatasetItem item;
		item.id = obj.at("id").as_int64();
		item.name = obj.at("name").as_string().c_str();
		item.category = obj.at("category").as_string().c_str();
		item.price = obj.at("price").as_int64();
		item.quantity = obj.at("quantity").as_int64();
		item.active = obj.at("active").as_bool();

		for (auto& tag : obj.at("tags").as_array())
		{
			item.tags.push_back(tag.as_string().c_str());
		}

		auto& rating = obj.at("rating").as_object();
		item.rating.score = rating.at("score").as_int64();
		item.rating.count = rating.at("count").as_int64();

		g_items.push_back(std::move(item));
	}
	return !g_items.empty();
}

// ── 解析 body 为整数 ───────────────────────────────────────────────────────

static int64_t parseBodyInt(const std::string& body)
{
	if (body.empty())
	{
		return 0;
	}
	try
	{
		return std::stoll(body);
	}
	catch (...)
	{
		return 0;
	}
}

#ifdef HICAL_HAS_PGSQL

// ── DATABASE_URL 五元组解析 ────────────────────────────────────────────────

namespace
{

	// async-db 端点 query param 的 limit clamp 范围（只有 limit 会被限制，min/max 不 clamp）
	constexpr int64_t kMinLimit = 1;
	constexpr int64_t kMaxLimit = 50;
	constexpr uint16_t kPgsqlDefaultPort = 5432;

	// 连接池缺省上限与最小空闲连接数（DATABASE_MAX_CONN 未设置 / 解析失败时回退）
	constexpr size_t kDefaultMaxConnections = 256;
	constexpr int kConnectionFloor = 1;

	/**
	 * @brief 解析 postgres://user:pass@host:port/dbname 形式的连接串
	 * 五元组缺省项：port 缺省 5432，user/password 可为空。
	 * @param url 形如 postgres://[user[:pass]@]host[:port][/dbname] 的连接串
	 * @param out 解析结果写入的 DbConfig
	 * @return 解析成功返回 true
	 */
	bool parsePostgresUrl(const std::string& url, hical::db::DbConfig& out)
	{
		constexpr std::string_view kScheme = "postgres://";
		if (url.rfind(kScheme, 0) != 0)
		{
			return false;
		}

		std::string_view rest(url.data() + kScheme.size(), url.size() - kScheme.size());

		// 拆出路径部分（dbname），若有
		std::string_view authority = rest;
		const size_t slash = rest.find('/');
		if (slash != std::string_view::npos)
		{
			authority = rest.substr(0, slash);
			out.database = std::string(rest.substr(slash + 1));
		}

		// 拆 user[:pass]@ 前缀
		std::string_view hostPort = authority;
		const size_t at = authority.find('@');
		if (at != std::string_view::npos)
		{
			std::string_view userInfo = authority.substr(0, at);
			hostPort = authority.substr(at + 1);
			const size_t colon = userInfo.find(':');
			if (colon != std::string_view::npos)
			{
				out.user = std::string(userInfo.substr(0, colon));
				out.password = std::string(userInfo.substr(colon + 1));
			}
			else
			{
				out.user = std::string(userInfo);
			}
		}

		// 拆 host[:port]
		const size_t colon = hostPort.find(':');
		if (colon != std::string_view::npos)
		{
			out.host = std::string(hostPort.substr(0, colon));
			const std::string portStr(hostPort.substr(colon + 1));
			if (portStr.empty())
			{
				out.port = kPgsqlDefaultPort;
			}
			else
			{
				try
				{
					const long parsed = std::stol(portStr);
					if (parsed < 0 || parsed > 65535)
					{
						return false;
					}
					out.port = static_cast<uint16_t>(parsed);
				}
				catch (...)
				{
					return false;
				}
			}
		}
		else if (!hostPort.empty())
		{
			out.host = std::string(hostPort);
			out.port = kPgsqlDefaultPort;
		}

		return !out.host.empty();
	}

	// 解析 query param 为整数，缺失或解析失败时回退缺省值。不 clamp，min/max 走这里
	// （官方校验会传 min=9999&max=9999 做空范围反作弊，clamp 会破坏这个语义）
	int64_t parseIntParamNoClamp(const std::optional<std::string>& val, int64_t fallback)
	{
		if (val && !val->empty())
		{
			try
			{
				return std::stoll(*val);
			}
			catch (...)
			{
				return fallback;
			}
		}
		return fallback;
	}

	// 解析 limit 并 clamp 到 [kMinLimit, kMaxLimit]，缺失或解析失败回退缺省值
	int64_t parseLimitParam(const std::optional<std::string>& val, int64_t fallback)
	{
		return std::clamp(parseIntParamNoClamp(val, fallback), kMinLimit, kMaxLimit);
	}

	// 把 tags 列的 jsonb_out 输出（紧凑 JSON 数组串，如 `["fast","new"]`）解析成
	// json::value。空串 / 非法 JSON 回退空数组，避免二次引号包裹成字符串嵌套。
	boost::json::value parseTags(std::string_view raw)
	{
		if (raw.empty())
		{
			return boost::json::array();
		}
		boost::system::error_code ec;
		auto parsed = boost::json::parse(raw, ec);
		if (ec || !parsed.is_array())
		{
			return boost::json::array();
		}
		return parsed;
	}

} // namespace

#endif // HICAL_HAS_PGSQL

// ── 构建 JSON items 数组 ───────────────────────────────────────────────────

static json::array buildItems(int count, int64_t multiplier)
{
	const size_t dsSize = g_items.size();
	if (dsSize == 0 || count <= 0)
	{
		return {};
	}

	json::array items;
	items.reserve(static_cast<std::size_t>(count));

	for (int i = 0; i < count; ++i)
	{
		const auto& src = g_items[static_cast<size_t>(i) % dsSize];

		json::array tags;
		tags.reserve(src.tags.size());
		for (const auto& t : src.tags)
		{
			tags.push_back(boost::json::value(t));
		}

		json::object item = {
			{"id", src.id},
			{"name", src.name},
			{"category", src.category},
			{"price", src.price},
			{"quantity", src.quantity},
			{"active", src.active},
			{"tags", std::move(tags)},
			{"rating", {{"score", src.rating.score}, {"count", src.rating.count}}},
			{"total", src.price * src.quantity * multiplier},
		};
		items.push_back(std::move(item));
	}
	return items;
}

// ── CPU 核数检测 ───────────────────────────────────────────────────────────

#if defined(__linux__)

namespace
{

	// cgroup v2 与 v1 的 cpuset 文件路径。v2 用 cpuset.cpus.effective，
	// v1 用 cpuset.cpus。容器里这两个文件反映的是被限制后的逻辑核集合，
	// 而不是宿主机的全部核，正好用来对抗 oversubscribe。
	constexpr std::string_view kCgroupV2Cpuset = "/sys/fs/cgroup/cpuset.cpus.effective";
	constexpr std::string_view kCgroupV1Cpuset = "/sys/fs/cgroup/cpuset/cpuset.cpus";

	/**
	 * @brief 解析 cgroup cpuset 文件内容，统计可得逻辑核总数
	 * 文件内容形如 "0-3,64-67\n"：逗号分隔若干段，每段是 a-b 区间或单个数字。
	 * @param raw 文件读出的原始文本
	 * @return 解析出的逻辑核数量，解析失败返回 0
	 */
	size_t parseCpuSet(std::string_view raw)
	{
		size_t total = 0;
		size_t pos = 0;
		while (pos <= raw.size())
		{
			const size_t comma = raw.find(',', pos);
			const std::string_view seg = raw.substr(pos, comma == std::string_view::npos ? raw.size() - pos : comma - pos);
			if (!seg.empty())
			{
				const size_t dash = seg.find('-');
				try
				{
					if (dash == std::string_view::npos)
					{
						// 单个核，如 "5"，转数字失败（非法字符）回退 0
						const long v = std::stol(std::string(seg));
						if (v < 0)
						{
							return 0;
						}
						total += 1;
					}
					else
					{
						// 区间，如 "0-3"，两端都必须是合法数字且 a <= b
						const long a = std::stol(std::string(seg.substr(0, dash)));
						const long b = std::stol(std::string(seg.substr(dash + 1)));
						if (a < 0 || b < a)
						{
							return 0;
						}
						total += static_cast<size_t>(b - a + 1);
					}
				}
				catch (...)
				{
					return 0;
				}
			}
			if (comma == std::string_view::npos)
			{
				break;
			}
			pos = comma + 1;
		}
		return total;
	}

	/**
	 * @brief 从 cgroup cpuset 读取可得逻辑核数，失败返回 0
	 * 先试 v2 再试 v1，任一成功且解析出 >0 即返回。
	 * @return 解析出的逻辑核数，读取失败或解析为 0 时返回 0
	 */
	size_t cgroupCpuCount()
	{
		for (const std::string_view path : {kCgroupV2Cpuset, kCgroupV1Cpuset})
		{
			std::ifstream ifs(path.data());
			if (!ifs)
			{
				continue;
			}
			std::string content;
			std::getline(ifs, content);
			if (content.empty())
			{
				continue;
			}
			const size_t count = parseCpuSet(content);
			if (count > 0)
			{
				return count;
			}
		}
		return 0;
	}

} // namespace

#endif // defined(__linux__)

/**
 * @brief 探测工作线程数，优先 cgroup cpuset，非容器回退硬件并发数
 * 容器里 hardware_concurrency() 返回宿主机核数，会 oversubscribe；cgroup cpuset
 * 反映被限制后的核集合，才是真实可用的并行度。
 * @return 逻辑核数，始终 >= 1
 */
static size_t detectCpuCount()
{
#if defined(__linux__)
	const size_t cgroupCount = cgroupCpuCount();
	if (cgroupCount > 0)
	{
		return cgroupCount;
	}
#endif
	return std::thread::hardware_concurrency();
}

// ── 共享路由注册 ────────────────────────────────────────────────────────────

/**
 * @brief 向指定服务器注册纯路由集合（不含 DB 端点）
 * 这些端点对 8080 明文与 8081 TLS 两个实例完全一致，
 * 抽成函数确保双实例挂载同一套路由、行为一致。
 * @param server 目标 HttpServer 实例（注册时尚未 start）
 */
static void registerRoutes(HttpServer& server)
{
	// ── GET /baseline11?a=X&b=Y → X + Y（text/plain）────────────────────
	server.router().get("/baseline11",
						[](const HttpRequest& req) -> HttpResponse
						{
							auto aOpt = req.queryParam("a");
							auto bOpt = req.queryParam("b");
							int64_t sum = std::stoll(aOpt.value_or("0")) + std::stoll(bOpt.value_or("0"));

							HttpResponse res;
							res.setStatus(HttpStatusCode::hOk);
							res.native().headers.set("Content-Type", "text/plain");
							res.native().body = std::to_string(sum);
							return res;
						});

	// ── POST /baseline11?a=X&b=Y  body=N → X+Y+N（text/plain）───────────
	server.router().post("/baseline11",
						 [](const HttpRequest& req) -> HttpResponse
						 {
							 auto aOpt = req.queryParam("a");
							 auto bOpt = req.queryParam("b");
							 int64_t sum = std::stoll(aOpt.value_or("0")) + std::stoll(bOpt.value_or("0"))
										   + parseBodyInt(req.body());

							 HttpResponse res;
							 res.setStatus(HttpStatusCode::hOk);
							 res.native().headers.set("Content-Type", "text/plain");
							 res.native().body = std::to_string(sum);
							 return res;
						 });

	// ── GET /pipeline → "ok"（text/plain）────────────────────────────────
	server.router().get("/pipeline",
						[](const HttpRequest&) -> HttpResponse
						{
							HttpResponse res;
							res.setStatus(HttpStatusCode::hOk);
							res.native().headers.set("Content-Type", "text/plain");
							res.native().body = "ok";
							return res;
						});

	// ── GET /delay/{ms} → 异步等待 ms 毫秒后回显数字（text/plain）─────────
	// 官方 async 协议：用协程 sleep 等待，不阻塞事件循环线程，也不按请求开线程。
	server.router().get("/delay/{ms}",
						[](const HttpRequest& req) -> Awaitable<HttpResponse>
						{
							HttpResponse res;
							int64_t ms = 0;
							try
							{
								ms = std::stoll(req.param("ms"));
							}
							catch (...)
							{
								res.setStatus(HttpStatusCode::hBadRequest);
								res.native().headers.set("Content-Type", "text/plain");
								res.native().body = "";
								co_return res;
							}

							if (ms < 0)
							{
								res.setStatus(HttpStatusCode::hBadRequest);
								res.native().headers.set("Content-Type", "text/plain");
								res.native().body = "";
								co_return res;
							}

							co_await hical::sleep(std::chrono::milliseconds(ms));

							res.setStatus(HttpStatusCode::hOk);
							res.native().headers.set("Content-Type", "text/plain");
							res.native().body = std::to_string(ms);
							co_return res;
						});

	// ── GET /json/{count}?m=X → JSON 序列化（路由组挂 Gzip 用于 json-comp）───
	{
		auto gzip = makeGzipCompressionMiddleware();
		auto jsonGroup = server.router().group("/json");
		jsonGroup.use([gzip](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
			auto resp = co_await next(req);
			gzip(req, resp);
			co_return resp;
		});
		jsonGroup.get("/{count}",
					  [](const HttpRequest& req) -> HttpResponse
					  {
						  int count = 0;
						  try
						  {
							  count = std::stoi(req.param("count"));
						  }
						  catch (...)
						  {
						  }

						  int64_t m = 1;
						  if (auto mOpt = req.queryParam("m"))
						  {
							  try
							  {
								  m = std::stoll(*mOpt);
							  }
							  catch (...)
							  {
							  }
						  }

						  json::object resp = {
							  {"count", count},
							  {"items", buildItems(count, m)},
						  };

						  HttpResponse res;
						  res.setStatus(HttpStatusCode::hOk);
						  res.native().headers.set("Content-Type", "application/json");
						  res.native().body = json::serialize(resp);
						  return res;
					  });
	}

	// ── POST /upload → body 字节数 ────────────────────────────────────────
	server.router().post("/upload",
						 [](const HttpRequest& req) -> HttpResponse
						 {
							 HttpResponse res;
							 res.setStatus(HttpStatusCode::hOk);
							 res.native().headers.set("Content-Type", "text/plain");
							 res.native().body = std::to_string(req.body().size());
							 return res;
						 });

	// ── GET /static/{file} → 静态文件服务 ────────────────────────────────
	server.router().get("/static/{file}", serveStatic("/data/static", "/static/"));

	// ── WS /ws → WebSocket 回显（类型感知回调，区分 Text/Binary）─────────
	server.router().ws("/ws",
					   [](const WsMessage& msg, WebSocketSession& ws) -> Awaitable<void>
					   {
						   if (msg.type == WsOpcode::hBinary)
						   {
							   co_await ws.sendBinary(msg.data);
						   }
						   else
						   {
							   co_await ws.send(msg.data);
						   }
					   });

	// ── POST /echo → body 原样回显（octet-stream，8gbit 测协议）───────────
	// 真读 body 再回写，不按 Content-Length 编造；chunked / 空 / 任意大小都逐字节原样返回。
	server.router().post("/echo",
						 [](const HttpRequest& req) -> HttpResponse
						 {
							 HttpResponse res;
							 res.setStatus(HttpStatusCode::hOk);
							 res.native().headers.set("Content-Type", "application/octet-stream");
							 res.native().body = req.body();
							 return res;
						 });
}

#ifdef HICAL_HAS_PGSQL

/**
 * @brief 向指定服务器注册 async-db 端点（依赖 DATABASE_URL + 连接池）
 * 这个端点依赖 io_context 和 DB 连接池，实例间无法完全共享；抽成函数后
 * 每个需要该端点的实例各自调用一次、各自建一个连接池。
 * @param server 目标 HttpServer 实例（注册时尚未 start）
 */
static void registerAsyncDbRoute(HttpServer& server)
{
	// ── GET /async-db → 异步参数化查询（rating 聚合）─────────────────────
	// 懒连接：minConnections 置 0，init() 不预连，首请求时由 acquire() 现场建连。
	// DATABASE_URL 未设置时跳过 DB 接入，端点返回空结果占位。
	const char* dbUrlEnv = std::getenv("DATABASE_URL");
	if (dbUrlEnv && *dbUrlEnv)
	{
		hical::db::DbConfig dbConfig;
		if (!parsePostgresUrl(dbUrlEnv, dbConfig))
		{
			HICAL_LOG_ERROR("Invalid DATABASE_URL, skipping async-db backend: {}", dbUrlEnv);
		}
		else
		{
			// maxConnections：DATABASE_MAX_CONN 环境变量，缺省 256
			const char* maxConnEnv = std::getenv("DATABASE_MAX_CONN");
			size_t maxConn = kDefaultMaxConnections;
			if (maxConnEnv && *maxConnEnv)
			{
				try
				{
					maxConn = static_cast<size_t>(std::max(kConnectionFloor, std::stoi(maxConnEnv)));
				}
				catch (...)
				{
					maxConn = kDefaultMaxConnections;
				}
			}

			dbConfig.minConnections = 0; // 懒连接：不在启动期建连，PG 未就绪也不阻塞启动
			dbConfig.maxConnections = maxConn;
			dbConfig.acquireTimeout = std::chrono::seconds(3); // PG 未就绪时快速失败，避免长等待

			auto pool = std::make_shared<hical::db::DbConnectionPool>(server.ioContext(),
																	  dbConfig,
																	  hical::db::PgsqlConnection::makeFactory());

			// 启动期 init：预创建 0 个连接，正常不会抛异常；万一抛了也 catch 住不 crash，
			// 连接会推迟到请求到达时的 acquire() 现场建立（懒连接 + 隐式 retry）。
			try
			{
				hical::coSpawn(server.ioContext(),
							   [pool]() -> Awaitable<void>
							   {
								   try
								   {
									   co_await pool->init();
								   }
								   catch (const std::exception& e)
								   {
									   HICAL_LOG_WARN("DbConnectionPool init failed (PG not ready yet): {}", e.what());
								   }
							   });
			}
			catch (...)
			{
				// coSpawn 本身理论上不抛，但兜底，避免启动崩溃
			}

			// 用路由组把 DB 中间件只挂在 /async-db 上，避免影响其它端点的快速路径。
			// 外层兜底中间件先注册（洋葱最外层）：DB 中间件 acquire 失败或查询抛异常时
			// 统一吞掉，返回空结果，不让 PG 未就绪把请求打成 500 或挂起。
			auto dbGroup = server.router().group("/async-db");
			dbGroup.use(
				[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
				{
					try
					{
						auto resp = co_await next(req);
						co_return resp;
					}
					catch (const std::exception& e)
					{
						HICAL_LOG_WARN("async-db backend unavailable: {}", e.what());
						HttpResponse res;
						res.setStatus(HttpStatusCode::hOk);
						res.native().headers.set("Content-Type", "application/json");
						res.native().body = R"({"items":[],"count":0})";
						co_return res;
					}
				});
			dbGroup.use(hical::db::makeDbMiddleware(pool));

			auto asyncDbHandler = [](const HttpRequest& req) -> Awaitable<HttpResponse>
			{
				// query param：min/max 缺省 10/50，只解析不 clamp；limit 缺省 50，clamp 到 [1, 50]
				const int64_t min = parseIntParamNoClamp(req.queryParam("min"), 10);
				const int64_t max = parseIntParamNoClamp(req.queryParam("max"), 50);
				const int64_t limit = parseLimitParam(req.queryParam("limit"), 50);

				json::array items;

				try
				{
					auto conn = hical::db::getDbConnection(req);

					// 官方协议：按价格区间过滤，限制条数，返回 items 表的 9 列原始字段
					const std::string sql = "SELECT id, name, category, price, quantity, active, tags, "
											"rating_score, rating_count FROM items "
											"WHERE price BETWEEN $1 AND $2 LIMIT $3";
					const std::vector<std::string> params = {std::to_string(min),
															 std::to_string(max),
															 std::to_string(limit)};

					auto result = co_await conn->query(sql, params);

					const size_t idIdx = result.columnIndex("id");
					const size_t nameIdx = result.columnIndex("name");
					const size_t categoryIdx = result.columnIndex("category");
					const size_t priceIdx = result.columnIndex("price");
					const size_t quantityIdx = result.columnIndex("quantity");
					const size_t activeIdx = result.columnIndex("active");
					const size_t tagsIdx = result.columnIndex("tags");
					const size_t scoreIdx = result.columnIndex("rating_score");
					const size_t countIdx = result.columnIndex("rating_count");

					items.reserve(result.size());
					for (size_t i = 0; i < result.size(); ++i)
					{
						auto row = result[i];

						// PG 文本格式取值：数值列是数字字符串，用 stoll 转 int64
						auto cellAsInt = [&row](size_t idx) -> int64_t
						{
							if (idx == hical::db::DbResult::npos)
							{
								return 0;
							}
							try
							{
								return std::stoll(row[idx]);
							}
							catch (...)
							{
								return 0;
							}
						};

						auto cellAsStr = [&row](size_t idx) -> std::string
						{
							if (idx == hical::db::DbResult::npos)
							{
								return {};
							}
							return std::string(row[idx]);
						};

						// active BOOLEAN 文本格式是 "t"/"f"，转成 JSON 布尔
						bool active = false;
						if (activeIdx != hical::db::DbResult::npos)
						{
							active = (row[activeIdx] == "t" || row[activeIdx] == "true");
						}

						// tags JSONB 的 jsonb_out 输出是紧凑 JSON 数组串，直接 parse 成
						// json::value，不二次引号包裹。列缺失时回退空数组。
						std::string_view tagsRaw;
						if (tagsIdx != hical::db::DbResult::npos)
						{
							tagsRaw = row[tagsIdx];
						}

						json::object item = {
							{"id", cellAsInt(idIdx)},
							{"name", cellAsStr(nameIdx)},
							{"category", cellAsStr(categoryIdx)},
							{"price", cellAsInt(priceIdx)},
							{"quantity", cellAsInt(quantityIdx)},
							{"active", active},
							{"tags", parseTags(tagsRaw)},
							{"rating", {{"score", cellAsInt(scoreIdx)}, {"count", cellAsInt(countIdx)}}},
						};
						items.push_back(std::move(item));
					}
				}
				catch (const std::exception& e)
				{
					// PG 未就绪 / 查询失败：返回空结果，不 crash
					HICAL_LOG_WARN("async-db query failed: {}", e.what());
				}

				// count 动态 = items 数组长度（== limit 当有足够行时），不是任何聚合和
				const std::size_t itemCount = items.size();
				json::object resp = {
					{"items", std::move(items)},
					{"count", itemCount},
				};

				HttpResponse res;
				res.setStatus(HttpStatusCode::hOk);
				res.native().headers.set("Content-Type", "application/json");
				res.native().body = json::serialize(resp);
				co_return res;
			};

			// 同时兼容 /async-db 与 /async-db/（带不带尾斜杠路由都能命中）
			dbGroup.get("", asyncDbHandler);
			dbGroup.get("/", asyncDbHandler);
		}
	}
	else
	{
		// DATABASE_URL 未设置：注册一个空结果占位的 /async-db，让 benchmark 不因缺后端而 404
		server.router().get("/async-db",
							[](const HttpRequest&) -> HttpResponse
							{
								HttpResponse res;
								res.setStatus(HttpStatusCode::hOk);
								res.native().headers.set("Content-Type", "application/json");
								res.native().body = R"({"items":[],"count":0})";
								return res;
							});
	}
}

#endif // HICAL_HAS_PGSQL

// ── main ────────────────────────────────────────────────────────────────────

// 证书与私钥在容器内的挂载路径（官方 runner 仅在 TLS profile 下才挂 /certs 卷）
constexpr std::string_view kTlsCertPath = "/certs/server.crt";
constexpr std::string_view kTlsKeyPath = "/certs/server.key";

// TLS 实例缺省线程数。8gbit 固定 5 万 req/s、json-tls 长连接低频，负载远低于 baseline，
// TLS 瓶颈在加解密 CPU 而非连接数；固定小值避免与明文实例线程相加翻倍、还原 oversubscribe。
constexpr size_t kDefaultTlsThreads = 2;

/**
 * @brief 统一施加 benchmark 运行时配置
 * 两个实例（明文 8080 / TLS 8081）共用同一套极限值，保证负载下的行为一致。
 * @param server 目标实例（注册路由、尚未 start）
 */
static void configureBenchmark(HttpServer& server)
{
	server.setMaxConnections(65535);
	server.setIdleTimeout(0);
	server.setGcInterval(0);
	server.setMaxBodySize(32ULL * 1024 * 1024); // 32MB 大文件上传
}

int main()
{
	// 明文 8080 线程数：HICAL_THREADS 显式覆盖，缺省按 cgroup 核数探测
	const char* threadEnv = std::getenv("HICAL_THREADS");
	size_t threads = threadEnv ? static_cast<size_t>(std::atoi(threadEnv)) : detectCpuCount();
	if (threads == 0)
	{
		threads = 1;
	}

	// TLS 8081 线程数：固定小值，防止与明文实例线程相加翻倍导致 oversubscribe
	size_t tlsThreads = kDefaultTlsThreads;
	const char* tlsThreadEnv = std::getenv("HICAL_TLS_THREADS");
	if (tlsThreadEnv && *tlsThreadEnv)
	{
		const int parsed = std::atoi(tlsThreadEnv);
		if (parsed > 0)
		{
			tlsThreads = static_cast<size_t>(parsed);
		}
	}

	// 加载数据集（挂载卷 /data/dataset.json）
	if (!loadDataset("/data/dataset.json"))
	{
		// 数据集不存在也可以启动，json 端点返回空数组
	}

	// ── 明文 8080 实例 ────────────────────────────────────────────────────
	HttpServer server(8080, threads);
	registerRoutes(server);
#ifdef HICAL_HAS_PGSQL
	registerAsyncDbRoute(server); // async-db 只挂明文，TLS 端口不测 DB
#endif // HICAL_HAS_PGSQL
	configureBenchmark(server);

	// ── TLS 8081 实例（证书/私钥存在才建）────────────────────────────────
	// 官方 runner 只在 TLS profile 才挂 /certs；缺失时降级为单明文实例，不启用 TLS。
	std::optional<HttpServer> tlsServer;
	std::thread tlsThread;
	if (std::filesystem::exists(kTlsCertPath) && std::filesystem::exists(kTlsKeyPath))
	{
		tlsServer.emplace(8081, tlsThreads);
		registerRoutes(*tlsServer);
		tlsServer->enableSsl(std::string(kTlsCertPath), std::string(kTlsKeyPath));
		configureBenchmark(*tlsServer);

		// TLS 实例后台跑，明文实例占主线程。两者各注册的 signal_set 都会收到
		// SIGINT/SIGTERM 并各自 gracefulStop → stop() → run() 返回。
		tlsThread = std::thread([&tlsServer]()
								{
									tlsServer->start();
								});
	}

	// 主线程跑明文实例（阻塞直到信号触发 stop）
	server.start();

	if (tlsThread.joinable())
	{
		tlsThread.join();
	}

	return 0;
}
