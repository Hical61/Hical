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
#include "core/StaticFiles.h"
#include "core/WebSocket.h"
#include <boost/json.hpp>
#include <cstdlib>
#include <fstream>
#include <string>
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

// ── main ────────────────────────────────────────────────────────────────────

int main()
{
	const char* threadEnv = std::getenv("HICAL_THREADS");
	size_t threads = threadEnv ? static_cast<size_t>(std::atoi(threadEnv)) : std::thread::hardware_concurrency();
	if (threads == 0)
	{
		threads = 1;
	}

	HttpServer server(8080, threads);

	// 加载数据集（挂载卷 /data/dataset.json）
	if (!loadDataset("/data/dataset.json"))
	{
		// 数据集不存在也可以启动，json 端点返回空数组
	}

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

	// ── GET /json/{count}?m=X → JSON 序列化 ──────────────────────────────
	server.router().get("/json/{count}",
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

	// ── 中间件 ─────────────────────────────────────────────────────────────
	// Gzip 压缩，用于 json-comp 测试
	server.use(makeGzipCompressionMiddleware());

	// ── benchmark 极致配置 ────────────────────────────────────────────────
	server.setMaxConnections(65535);
	server.setIdleTimeout(0);
	server.setGcInterval(0);
	server.setMaxBodySize(32ULL * 1024 * 1024); // 32MB 大文件上传

	server.start();
	return 0;
}
