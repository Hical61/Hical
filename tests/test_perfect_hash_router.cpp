/**
 * @file test_perfect_hash_router.cpp
 * @brief PerfectHashRouter 正确性与性能测试
 */

#include "core/PerfectHashRouter.h"
#include "core/Router.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include "core/MetaRoutes.h"
#include "core/Reflection.h"
#include "core/Coroutine.h"
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace hical;

namespace
{

	// ============ 编译期 PerfectHashRouter 正确性 ============

	TEST(PerfectHashRouterTest, CompileTime_AllMethodsMatch)
	{
		constexpr std::array<PerfectHashRouter<4>::Key, 4> keys = {{
			{HttpMethod::hGet, "/api/users", 0},
			{HttpMethod::hPost, "/api/users", 1},
			{HttpMethod::hPut, "/api/users/42", 2},
			{HttpMethod::hDelete, "/api/users/42", 3},
		}};
		constexpr auto router = PerfectHashRouter<4>(keys);

		auto* e = router.lookup(HttpMethod::hGet, "/api/users");
		ASSERT_NE(e, nullptr);
		EXPECT_EQ(e->index_, 0);

		e = router.lookup(HttpMethod::hPost, "/api/users");
		ASSERT_NE(e, nullptr);
		EXPECT_EQ(e->index_, 1);

		e = router.lookup(HttpMethod::hPut, "/api/users/42");
		ASSERT_NE(e, nullptr);
		EXPECT_EQ(e->index_, 2);

		e = router.lookup(HttpMethod::hDelete, "/api/users/42");
		ASSERT_NE(e, nullptr);
		EXPECT_EQ(e->index_, 3);
	}

	TEST(PerfectHashRouterTest, CompileTime_NotFoundReturnsNull)
	{
		constexpr std::array<PerfectHashRouter<2>::Key, 2> keys = {{
			{HttpMethod::hGet, "/api/hello", 0},
			{HttpMethod::hPost, "/api/data", 1},
		}};
		constexpr auto router = PerfectHashRouter<2>(keys);

		EXPECT_EQ(router.lookup(HttpMethod::hGet, "/api/nonexistent"), nullptr);
		EXPECT_EQ(router.lookup(HttpMethod::hPost, "/api/hello"), nullptr);
		EXPECT_EQ(router.lookup(HttpMethod::hDelete, "/api/xyz"), nullptr);
	}

	TEST(PerfectHashRouterTest, SamePathDifferentMethod)
	{
		constexpr std::array<PerfectHashRouter<4>::Key, 4> keys = {{
			{HttpMethod::hGet, "/api/item", 0},
			{HttpMethod::hPost, "/api/item", 1},
			{HttpMethod::hPut, "/api/item", 2},
			{HttpMethod::hDelete, "/api/item", 3},
		}};
		constexpr auto router = PerfectHashRouter<4>(keys);

		EXPECT_EQ(router.lookup(HttpMethod::hGet, "/api/item")->index_, 0);
		EXPECT_EQ(router.lookup(HttpMethod::hPost, "/api/item")->index_, 1);
		EXPECT_EQ(router.lookup(HttpMethod::hPut, "/api/item")->index_, 2);
		EXPECT_EQ(router.lookup(HttpMethod::hDelete, "/api/item")->index_, 3);
	}

	// ============ 运行时 RuntimePerfectHashLookup 正确性 ============

	TEST(RuntimePerfectHashLookupTest, BuildFromKeys_HitAllMethods)
	{
		std::vector<std::pair<HttpMethod, std::string_view>> keys = {
			{HttpMethod::hGet, "/api/v1/products"},
			{HttpMethod::hPost, "/api/v1/products"},
			{HttpMethod::hPut, "/api/v1/products/99"},
			{HttpMethod::hDelete, "/api/v1/products/99"},
		};

		auto lookup = RuntimePerfectHashLookup::buildFromKeys(keys);
		ASSERT_TRUE(lookup.valid());
		EXPECT_EQ(lookup.keyCount(), 4);

		EXPECT_EQ(lookup.lookup(HttpMethod::hGet, "/api/v1/products"), 0);
		EXPECT_EQ(lookup.lookup(HttpMethod::hPost, "/api/v1/products"), 1);
		EXPECT_EQ(lookup.lookup(HttpMethod::hPut, "/api/v1/products/99"), 2);
		EXPECT_EQ(lookup.lookup(HttpMethod::hDelete, "/api/v1/products/99"), 3);
	}

	TEST(RuntimePerfectHashLookupTest, Lookup_NotFoundReturnsSizeMax)
	{
		std::vector<std::pair<HttpMethod, std::string_view>> keys = {
			{HttpMethod::hGet, "/api/ping"},
		};

		auto lookup = RuntimePerfectHashLookup::buildFromKeys(keys);
		ASSERT_TRUE(lookup.valid());

		EXPECT_EQ(lookup.lookup(HttpMethod::hGet, "/api/nope"), SIZE_MAX);
		EXPECT_EQ(lookup.lookup(HttpMethod::hPost, "/api/ping"), SIZE_MAX);
	}

	TEST(RuntimePerfectHashLookupTest, BuildFromKeys_EmptyReturnsInvalid)
	{
		auto lookup = RuntimePerfectHashLookup::buildFromKeys({});
		EXPECT_FALSE(lookup.valid());
	}

	TEST(RuntimePerfectHashLookupTest, BuildFromKeys_SingleKey)
	{
		std::vector<std::pair<HttpMethod, std::string_view>> keys = {
			{HttpMethod::hGet, "/"},
		};

		auto lookup = RuntimePerfectHashLookup::buildFromKeys(keys);
		ASSERT_TRUE(lookup.valid());
		EXPECT_EQ(lookup.keyCount(), 1);
		EXPECT_EQ(lookup.lookup(HttpMethod::hGet, "/"), 0);
	}

	TEST(RuntimePerfectHashLookupTest, BuildFromKeys_DuplicateKeysReturnsInvalid)
	{
		/* 两个 key 完全相同（同方法+同路径），djb2 哈希值一样，
           任何种子下都会冲突，buildFromKeys 应返回无效对象 */
		std::vector<std::pair<HttpMethod, std::string_view>> keys = {
			{HttpMethod::hGet, "/api/dup"},
			{HttpMethod::hGet, "/api/dup"},
		};

		auto lookup = RuntimePerfectHashLookup::buildFromKeys(keys);
		EXPECT_FALSE(lookup.valid());
	}

	TEST(RuntimePerfectHashLookupTest, BuildFromKeys_ManyDistinctKeys)
	{
		/* 30 个不同路由键，验证种子搜索正常。
           先建好所有路径再构造 keys，避免 vector reallocate 导致 string_view 悬空 */
		std::vector<std::string> paths;
		for (int i = 0; i < 30; ++i)
		{
			paths.push_back("/api/route" + std::to_string(i));
		}

		std::vector<std::pair<HttpMethod, std::string_view>> keys;
		for (const auto& p : paths)
		{
			keys.emplace_back(HttpMethod::hGet, std::string_view(p));
		}

		auto lookup = RuntimePerfectHashLookup::buildFromKeys(keys);
		ASSERT_TRUE(lookup.valid());
		EXPECT_EQ(lookup.keyCount(), 30);

		/* 每个 key 都能正确命中 */
		for (size_t i = 0; i < keys.size(); ++i)
		{
			EXPECT_EQ(lookup.lookup(keys[i].first, keys[i].second), i)
				<< "key " << i << " (" << keys[i].second << ") not found";
		}
	}

	// ============ Router 集成：meta::registerRoutes 自动注入完美哈希 ============

	struct PhrTestHandler
	{
		HttpResponse getUsers(const HttpRequest&)
		{
			return HttpResponse::ok("users list");
		}
		HICAL_HANDLER(Get, "/api/users", getUsers)

		HttpResponse postUser(const HttpRequest&)
		{
			return HttpResponse::json({{"created", true}});
		}
		HICAL_HANDLER(Post, "/api/users", postUser)

		HttpResponse getUser(const HttpRequest& req)
		{
			return HttpResponse::ok("user " + std::string(req.param("id")));
		}
		HICAL_HANDLER(Get, "/api/users/{id}", getUser)

		HICAL_ROUTES(PhrTestHandler, getUsers, postUser, getUser)
	};

	TEST(PerfectHashIntegrationTest, MetaRoutes_StaticRoutesHitPerfectHash)
	{
		/* meta::registerRoutes 注册后自动注入完美哈希，dispatchSync 走完美哈希路径 */
		Router router;
		PhrTestHandler handler;
		meta::registerRoutes(router, handler);

		/* GET /api/users：sync handler 走 dispatchSync 完美哈希 */
		{
			HttpRequest req;
			req.setMethod(HttpMethod::hGet);
			req.setTarget("/api/users");

			auto result = router.dispatchSync(req);
			ASSERT_TRUE(result.has_value());
			EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
			EXPECT_EQ(result->body(), "users list");
		}

		/* POST /api/users：sync handler 走 dispatchSync 完美哈希 */
		{
			HttpRequest req;
			req.setMethod(HttpMethod::hPost);
			req.setTarget("/api/users");

			auto result = router.dispatchSync(req);
			ASSERT_TRUE(result.has_value());
			EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
		}
	}

	TEST(PerfectHashIntegrationTest, MetaRoutes_ParamRouteStillWorks)
	{
		/* 参数路由不在完美哈希表中，dispatchSync 仍走参数匹配路径 */
		Router router;
		PhrTestHandler handler;

		meta::registerRoutes(router, handler);

		HttpRequest req;
		req.setMethod(HttpMethod::hGet);
		req.setTarget("/api/users/42");

		auto result = router.dispatchSync(req);
		ASSERT_TRUE(result.has_value());
		EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
		EXPECT_EQ(result->body(), "user 42");
	}

	TEST(PerfectHashIntegrationTest, MetaRoutes_405MethodNotAllowed)
	{
		/* DELETE /api/users 未注册，应返回 405。
           dispatchSync 在 405 情况下返回 nullopt（需要构造响应体），
           走 dispatch 路径验证。 */
		boost::asio::io_context io;
		Router router;
		PhrTestHandler handler;

		meta::registerRoutes(router, handler);

		HttpRequest req;
		req.setMethod(HttpMethod::hDelete);
		req.setTarget("/api/users");

		std::optional<HttpResponse> result;
		bool done = false;

		boost::asio::co_spawn(
			io,
			[&]() -> Awaitable<void>
			{
				result = co_await router.dispatch(req);
				done = true;
			},
			boost::asio::detached);

		io.run();

		ASSERT_TRUE(done);
		ASSERT_TRUE(result.has_value());
		EXPECT_EQ(result->statusCode(), HttpStatusCode::hMethodNotAllowed);
	}

	TEST(PerfectHashIntegrationTest, CommandRegistrationStillWorks)
	{
		/* 命令式 router.get()/post() 注册，即使没有注入完美哈希也能正常工作 */
		Router router;

		router.get("/manual/hello",
				   [](const HttpRequest&) -> HttpResponse
				   {
					   return HttpResponse::ok("hello");
				   });
		router.post("/manual/data",
					[](const HttpRequest&) -> HttpResponse
					{
						return HttpResponse::ok("posted");
					});

		{
			HttpRequest req;
			req.setMethod(HttpMethod::hGet);
			req.setTarget("/manual/hello");

			auto result = router.dispatchSync(req);
			ASSERT_TRUE(result.has_value());
			EXPECT_EQ(result->body(), "hello");
		}

		{
			HttpRequest req;
			req.setMethod(HttpMethod::hPost);
			req.setTarget("/manual/data");

			auto result = router.dispatchSync(req);
			ASSERT_TRUE(result.has_value());
			EXPECT_EQ(result->body(), "posted");
		}
	}

	TEST(PerfectHashIntegrationTest, MixedMode_BothWork)
	{
		/* 混用：先 meta::registerRoutes（注入完美哈希），再命令式追加路由。
           追加的路由不在完美哈希表中，应走 unordered_map 回退路径。
           dispatchSync 覆盖 sync handler 场景节省协程开销。 */
		Router router;

		/* 1. HICAL_ROUTES 注册（自动注入完美哈希） */
		PhrTestHandler handler;
		meta::registerRoutes(router, handler);

		/* 2. 命令式追加额外路由（此时 staticRoutes_ 已有数据，追加的不会进完美哈希表） */
		router.get("/extra/hello",
				   [](const HttpRequest&) -> HttpResponse
				   {
					   return HttpResponse::ok("extra hello");
				   });

		/* 反射注册的静态路由：dispatchSync 完美哈希命中 */
		{
			HttpRequest req;
			req.setMethod(HttpMethod::hGet);
			req.setTarget("/api/users");

			auto result = router.dispatchSync(req);
			ASSERT_TRUE(result.has_value());
			EXPECT_EQ(result->body(), "users list");
		}

		/* 反射注册的参数路由：dispatchSync 参数匹配命中 */
		{
			HttpRequest req;
			req.setMethod(HttpMethod::hGet);
			req.setTarget("/api/users/7");

			auto result = router.dispatchSync(req);
			ASSERT_TRUE(result.has_value());
			EXPECT_EQ(result->body(), "user 7");
		}

		/* 命令式追加的路由：dispatchSync 回退到 unordered_map */
		{
			HttpRequest req;
			req.setMethod(HttpMethod::hGet);
			req.setTarget("/extra/hello");

			auto result = router.dispatchSync(req);
			ASSERT_TRUE(result.has_value());
			EXPECT_EQ(result->body(), "extra hello");
		}

		/* 未注册路由：dispatchSync 返回 nullopt（需走 dispatch 构造 404 响应） */
		{
			HttpRequest req;
			req.setMethod(HttpMethod::hGet);
			req.setTarget("/not/registered");

			auto result = router.dispatchSync(req);
			EXPECT_FALSE(result.has_value());
		}

		/* 未注册路由：走 dispatch 路径获取 404 响应 */
		{
			boost::asio::io_context io;
			HttpRequest req;
			req.setMethod(HttpMethod::hGet);
			req.setTarget("/not/registered");

			std::optional<HttpResponse> result;
			bool done = false;

			boost::asio::co_spawn(
				io,
				[&]() -> Awaitable<void>
				{
					result = co_await router.dispatch(req);
					done = true;
				},
				boost::asio::detached);

			io.run();

			ASSERT_TRUE(done);
			ASSERT_TRUE(result.has_value());
			EXPECT_EQ(result->statusCode(), HttpStatusCode::hNotFound);
		}
	}

	TEST(PerfectHashIntegrationTest, DispatchSync_PerfectHashPath)
	{
		/* dispatchSync 走完美哈希路径：sync handler 命中，不分配协程帧 */
		Router router;
		PhrTestHandler handler;
		meta::registerRoutes(router, handler);

		HttpRequest req;
		req.setMethod(HttpMethod::hGet);
		req.setTarget("/api/users");

		auto result = router.dispatchSync(req);
		ASSERT_TRUE(result.has_value());
		EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
		EXPECT_EQ(result->body(), "users list");
	}

	// ============ 性能测试 ============

	class PerfectHashPerfTest : public ::testing::Test
	{
	protected:
		static constexpr int kWarmupIterations = 500;
		static constexpr int kBenchIterations = 50000;

		/**
		 * @brief 创建含 count 个静态路由的 Router（命令式注册，无完美哈希）
		 */
		static Router createPlainRouter(int count)
		{
			Router router;
			for (int i = 0; i < count; ++i)
			{
				std::string path = "/api/v1/route" + std::to_string(i);
				router.get(path,
						   [](const HttpRequest&) -> HttpResponse
						   {
							   return HttpResponse::ok("ok");
						   });
			}
			return router;
		}

		/**
		 * @brief 创建含 count 个静态路由的 Router 并注入完美哈希
		 */
		static Router createPhrRouter(int count)
		{
			Router router;
			std::vector<std::pair<HttpMethod, std::string_view>> keys;
			std::vector<std::string> paths;
			keys.reserve(count);
			paths.reserve(count);

			for (int i = 0; i < count; ++i)
			{
				paths.push_back("/api/v1/route" + std::to_string(i));
				router.get(paths.back(),
						   [](const HttpRequest&) -> HttpResponse
						   {
							   return HttpResponse::ok("ok");
						   });
				keys.emplace_back(HttpMethod::hGet, paths.back());
			}

			auto lookup = RuntimePerfectHashLookup::buildFromKeys(keys);
			if (lookup.valid())
			{
				router.setPerfectHashLookup(std::move(lookup));
			}
			return router;
		}
	};

	TEST_F(PerfectHashPerfTest, DispatchSync_WithoutPerfectHash)
	{
		auto router = createPlainRouter(100);

		HttpRequest req;
		req.setMethod(HttpMethod::hGet);
		req.setTarget("/api/v1/route50");

		/* 预热 */
		for (int i = 0; i < kWarmupIterations; ++i)
		{
			(void)router.dispatchSync(req);
		}

		auto start = std::chrono::high_resolution_clock::now();
		for (int i = 0; i < kBenchIterations; ++i)
		{
			(void)router.dispatchSync(req);
		}
		auto end = std::chrono::high_resolution_clock::now();

		double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
		double nsPerOp = (totalMs * 1000000.0) / kBenchIterations;

		std::cout << "[dispatchSync-无完美哈希] " << kBenchIterations
				  << " \xE6\xAC\xA1\xE5\x88\x86\xE5\x8F\x91, \xE6\x80\xBB\xE8\x80\x97\xE6\x97\xB6: " << totalMs
				  << " ms, \xE6\xAF\x8F\xE6\xAC\xA1: " << nsPerOp << " ns\n";

		EXPECT_LT(nsPerOp, 1e6);
	}

	TEST_F(PerfectHashPerfTest, DispatchSync_WithPerfectHash)
	{
		auto router = createPhrRouter(100);

		HttpRequest req;
		req.setMethod(HttpMethod::hGet);
		req.setTarget("/api/v1/route50");

		/* 预热 */
		for (int i = 0; i < kWarmupIterations; ++i)
		{
			(void)router.dispatchSync(req);
		}

		auto start = std::chrono::high_resolution_clock::now();
		for (int i = 0; i < kBenchIterations; ++i)
		{
			(void)router.dispatchSync(req);
		}
		auto end = std::chrono::high_resolution_clock::now();

		double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
		double nsPerOp = (totalMs * 1000000.0) / kBenchIterations;

		std::cout << "[dispatchSync-有完美哈希] " << kBenchIterations
				  << " \xE6\xAC\xA1\xE5\x88\x86\xE5\x8F\x91, \xE6\x80\xBB\xE8\x80\x97\xE6\x97\xB6: " << totalMs
				  << " ms, \xE6\xAF\x8F\xE6\xAC\xA1: " << nsPerOp << " ns\n";

		EXPECT_LT(nsPerOp, 1e6);
	}

	TEST_F(PerfectHashPerfTest, DispatchSync_PerfectHashMissFallback)
	{
		/* 完美哈希未命中时回退到 unordered_map，性能不应退化太多 */
		auto router = createPhrRouter(100);

		/* 追加一个不在完美哈希表中的路由，验证回退路径性能 */
		router.get("/fallback/extra",
				   [](const HttpRequest&) -> HttpResponse
				   {
					   return HttpResponse::ok("extra");
				   });

		HttpRequest req;
		req.setMethod(HttpMethod::hGet);
		req.setTarget("/fallback/extra");

		for (int i = 0; i < kWarmupIterations; ++i)
		{
			(void)router.dispatchSync(req);
		}

		auto start = std::chrono::high_resolution_clock::now();
		for (int i = 0; i < kBenchIterations; ++i)
		{
			(void)router.dispatchSync(req);
		}
		auto end = std::chrono::high_resolution_clock::now();

		double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
		double nsPerOp = (totalMs * 1000000.0) / kBenchIterations;

		std::cout << "[dispatchSync-回退路径]   " << kBenchIterations
				  << " \xE6\xAC\xA1\xE5\x88\x86\xE5\x8F\x91, \xE6\x80\xBB\xE8\x80\x97\xE6\x97\xB6: " << totalMs
				  << " ms, \xE6\xAF\x8F\xE6\xAC\xA1: " << nsPerOp << " ns\n";

		/* 回退路径加上一次完美哈希 miss 的开销，但仍应在合理范围内 */
		EXPECT_LT(nsPerOp, 1e6);
	}

	TEST_F(PerfectHashPerfTest, DispatchSync_FirstAndLastHit)
	{
		/* 验证完美哈希下首条和末条性能一致（O(1) 无位置相关性） */
		auto router = createPhrRouter(100);

		HttpRequest reqFirst;
		reqFirst.setMethod(HttpMethod::hGet);
		reqFirst.setTarget("/api/v1/route0");

		HttpRequest reqLast;
		reqLast.setMethod(HttpMethod::hGet);
		reqLast.setTarget("/api/v1/route99");

		/* 首条 */
		for (int i = 0; i < kWarmupIterations; ++i)
		{
			(void)router.dispatchSync(reqFirst);
		}
		auto start = std::chrono::high_resolution_clock::now();
		for (int i = 0; i < kBenchIterations; ++i)
		{
			(void)router.dispatchSync(reqFirst);
		}
		auto end = std::chrono::high_resolution_clock::now();
		double firstNs =
			(std::chrono::duration<double, std::milli>(end - start).count() * 1000000.0) / kBenchIterations;

		/* 末条 */
		for (int i = 0; i < kWarmupIterations; ++i)
		{
			(void)router.dispatchSync(reqLast);
		}
		start = std::chrono::high_resolution_clock::now();
		for (int i = 0; i < kBenchIterations; ++i)
		{
			(void)router.dispatchSync(reqLast);
		}
		end = std::chrono::high_resolution_clock::now();
		double lastNs = (std::chrono::duration<double, std::milli>(end - start).count() * 1000000.0) / kBenchIterations;

		std::cout << "[dispatchSync-完美哈希首条] " << firstNs << " ns\n";
		std::cout << "[dispatchSync-完美哈希末条] " << lastNs << " ns\n";

		EXPECT_LT(firstNs, 1e6);
		EXPECT_LT(lastNs, 1e6);

		/* 首末条都能在合理时间内完成即说明 O(1) 查找无位置相关性 */
	}

	// ============ 多 Handler meta::registerRoutes 注入后性能 ============

	struct PhrBenchHandler
	{
		HttpResponse h0(const HttpRequest&)
		{
			return HttpResponse::ok("0");
		}
		HICAL_HANDLER(Get, "/b/0", h0)

		HttpResponse h1(const HttpRequest&)
		{
			return HttpResponse::ok("1");
		}
		HICAL_HANDLER(Get, "/b/1", h1)

		HttpResponse h2(const HttpRequest&)
		{
			return HttpResponse::ok("2");
		}
		HICAL_HANDLER(Get, "/b/2", h2)

		HttpResponse h3(const HttpRequest&)
		{
			return HttpResponse::ok("3");
		}
		HICAL_HANDLER(Get, "/b/3", h3)

		HttpResponse h4(const HttpRequest&)
		{
			return HttpResponse::ok("4");
		}
		HICAL_HANDLER(Get, "/b/4", h4)

		HttpResponse h5(const HttpRequest&)
		{
			return HttpResponse::ok("5");
		}
		HICAL_HANDLER(Get, "/b/5", h5)

		HttpResponse h6(const HttpRequest&)
		{
			return HttpResponse::ok("6");
		}
		HICAL_HANDLER(Get, "/b/6", h6)

		HttpResponse h7(const HttpRequest&)
		{
			return HttpResponse::ok("7");
		}
		HICAL_HANDLER(Get, "/b/7", h7)

		HttpResponse h8(const HttpRequest&)
		{
			return HttpResponse::ok("8");
		}
		HICAL_HANDLER(Get, "/b/8", h8)

		HttpResponse h9(const HttpRequest&)
		{
			return HttpResponse::ok("9");
		}
		HICAL_HANDLER(Get, "/b/9", h9)

		HICAL_ROUTES(PhrBenchHandler, h0, h1, h2, h3, h4, h5, h6, h7, h8, h9)
	};

	TEST_F(PerfectHashPerfTest, DispatchSync_MetaRoutesInjected)
	{
		/* 通过 meta::registerRoutes 自动注入完美哈希后的性能 */
		Router router;
		PhrBenchHandler handler;
		meta::registerRoutes(router, handler);

		HttpRequest req;
		req.setMethod(HttpMethod::hGet);
		req.setTarget("/b/5");

		for (int i = 0; i < kWarmupIterations; ++i)
		{
			(void)router.dispatchSync(req);
		}

		auto start = std::chrono::high_resolution_clock::now();
		for (int i = 0; i < kBenchIterations; ++i)
		{
			auto result = router.dispatchSync(req);
			ASSERT_TRUE(result.has_value());
			EXPECT_EQ(result->body(), "5");
		}
		auto end = std::chrono::high_resolution_clock::now();

		double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
		double nsPerOp = (totalMs * 1000000.0) / kBenchIterations;

		std::cout << "[dispatchSync-HICAL_ROUTES注入] " << kBenchIterations
				  << " \xE6\xAC\xA1\xE5\x88\x86\xE5\x8F\x91, \xE6\x80\xBB\xE8\x80\x97\xE6\x97\xB6: " << totalMs
				  << " ms, \xE6\xAF\x8F\xE6\xAC\xA1: " << nsPerOp << " ns\n";

		EXPECT_LT(nsPerOp, 1e6);
	}

} // namespace
