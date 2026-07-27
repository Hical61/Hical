/**
 * @file CompileTimeChain.h
 * @brief 编译期中间件调用链预构建
 * 提供 buildCompileTimeChain() 模板函数，接受编译期中间件条目类型列表，
 * 在编译期展开并生成与 buildOptimizedChain() 语义等价的 MiddlewareNext 调用链。
 * 连续 Sync 条目合并为单个协程帧，减少运行期堆分配。
 */

#pragma once

#include "Coroutine.h"
#include "HttpResponse.h"
#include "Middleware.h"
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace hical
{

	/**
	 * @brief 异步中间件编译期标签
	 * @tparam Handler 异步处理器（捕获列表为空的 lambda 或函数指针）
	 */
	template <auto Handler>
	struct CompileTimeAsyncMw
	{
		static constexpr auto mw = Handler;
	};

	/**
	 * @brief 同步前置中间件编译期标签（仅有 before）
	 * @tparam Before 前置处理器
	 */
	template <auto Before>
	struct CompileTimeSyncMw
	{
		static constexpr auto before = Before;
	};

	/**
	 * @brief 完整同步中间件编译期标签（before + after）
	 * @tparam Before 前置处理器
	 * @tparam After 后置处理器
	 */
	template <auto Before, auto After>
	struct CompileTimeSyncFullMw
	{
		static constexpr auto before = Before;
		static constexpr auto after = After;
	};

	namespace detail
	{

		/* ===== 类型特征 ===== */

		template <typename T>
		struct IsCompileTimeMw : std::false_type
		{
		};

		template <auto H>
		struct IsCompileTimeMw<CompileTimeAsyncMw<H>> : std::true_type
		{
		};

		template <auto B>
		struct IsCompileTimeMw<CompileTimeSyncMw<B>> : std::true_type
		{
		};

		template <auto B, auto A>
		struct IsCompileTimeMw<CompileTimeSyncFullMw<B, A>> : std::true_type
		{
		};

		template <typename T>
		struct IsSyncEntry : std::false_type
		{
		};

		template <auto B>
		struct IsSyncEntry<CompileTimeSyncMw<B>> : std::true_type
		{
		};

		template <auto B, auto A>
		struct IsSyncEntry<CompileTimeSyncFullMw<B, A>> : std::true_type
		{
		};

		template <typename T>
		inline constexpr bool kIsSyncEntry = IsSyncEntry<T>::value;

		/* ===== Sync 条目连续收集 ===== */

		/**
		 * @brief 从剩余条目中连续收集 Sync 条目
		 * @tparam Acc 已收集的 Sync 条目 tuple
		 * @tparam Remaining 剩余待处理条目 tuple
		 */
		template <typename Acc, typename Remaining>
		struct SyncRunCollector;

		/* 终止：剩余为空 */
		template <typename... Acc>
		struct SyncRunCollector<std::tuple<Acc...>, std::tuple<>>
		{
			using syncEntries = std::tuple<Acc...>;
			using remaining = std::tuple<>;
		};

		/* 下一个是 SyncMw */
		template <typename... Acc, auto B, typename... Rest>
		struct SyncRunCollector<std::tuple<Acc...>, std::tuple<CompileTimeSyncMw<B>, Rest...>>
		{
			using inner = SyncRunCollector<std::tuple<Acc..., CompileTimeSyncMw<B>>, std::tuple<Rest...>>;
			using syncEntries = typename inner::syncEntries;
			using remaining = typename inner::remaining;
		};

		/* 下一个是 SyncFullMw */
		template <typename... Acc, auto B, auto A, typename... Rest>
		struct SyncRunCollector<std::tuple<Acc...>, std::tuple<CompileTimeSyncFullMw<B, A>, Rest...>>
		{
			using inner = SyncRunCollector<std::tuple<Acc..., CompileTimeSyncFullMw<B, A>>, std::tuple<Rest...>>;
			using syncEntries = typename inner::syncEntries;
			using remaining = typename inner::remaining;
		};

		/* 下一个不是 Sync → 停止收集 */
		template <typename... Acc, typename NonSync, typename... Rest>
			requires(!kIsSyncEntry<NonSync>)
		struct SyncRunCollector<std::tuple<Acc...>, std::tuple<NonSync, Rest...>>
		{
			using syncEntries = std::tuple<Acc...>;
			using remaining = std::tuple<NonSync, Rest...>;
		};

		/* ===== 从条目列表中提取第一个"组" ===== */

		/**
		 * @brief 提取第一个有效的中间件组
		 * 如果是 Async 开头 → 提取为单条目 Async 组
		 * 如果是 Sync 开头 → 连续收集所有 Sync → 合并为一个 SyncGroup
		 */
		template <typename...>
		struct ExtractFirstGroup;

		/* 空列表 */
		template <>
		struct ExtractFirstGroup<>
		{
			static constexpr bool kIsEmpty = true;
		};

		/* Async 开头 */
		template <auto H, typename... Rest>
		struct ExtractFirstGroup<CompileTimeAsyncMw<H>, Rest...>
		{
			static constexpr bool kIsEmpty = false;
			static constexpr bool kIsAsync = true;
			static constexpr auto kHandler = H;
			using remaining = std::tuple<Rest...>;
		};

		/* Sync 开头 → 委托 SyncRunCollector 收集连续 Sync */
		template <typename First, typename... Rest>
			requires(kIsSyncEntry<First>)
		struct ExtractFirstGroup<First, Rest...>
		{
			static constexpr bool kIsEmpty = false;
			static constexpr bool kIsAsync = false;

			using run = SyncRunCollector<std::tuple<First>, std::tuple<Rest...>>;
			using syncEntries = typename run::syncEntries;
			using remaining = typename run::remaining;
		};

		/* 从 tuple 解包 */
		template <typename Tuple>
		struct ExtractFirstGroupFromTuple;

		template <typename... Ts>
		struct ExtractFirstGroupFromTuple<std::tuple<Ts...>> : ExtractFirstGroup<Ts...>
		{
		};

		/* ===== Sync 组构建 ===== */

		/**
		 * @brief 从 Sync 条目 tuple 构建合并的协程 lambda
		 * 将 N 个连续 Sync 条目（仅 before / before+after）合并为**单个协程帧**：
		 * - 前置处理器按注册顺序依次执行，任一拦截即短路返回
		 * - 后置处理器按注册逆序执行（洋葱模型语义）
		 */
		template <typename SyncTuple>
		MiddlewareNext buildSyncGroup(MiddlewareNext next)
		{
			return [&]<size_t... Is>(std::index_sequence<Is...>)
			{
				std::vector<SyncBeforeHandler> befores;
				std::vector<SyncAfterHandler> afters;

				/* 按注册顺序收集各 Sync 条目的 before/after */
				auto collectOne = [&]<typename T>()
				{
					/* T::before 是 static constexpr auto 成员，捕获列表为空的 lambda 可隐式转为 std::function */
					befores.push_back(T::before);
					if constexpr (requires { T::after; })
					{
						afters.push_back(T::after);
					}
					return 0;
				};
				((collectOne.template operator()<std::tuple_element_t<Is, SyncTuple>>()), ...);

				return MiddlewareNext {[befores = std::move(befores),
										afters = std::move(afters),
										next = std::move(next)](HttpRequest& r) -> Awaitable<HttpResponse>
									   {
										   /* 前置：按注册顺序执行 */
										   for (const auto& before : befores)
										   {
											   auto intercepted = before(r);
											   if (intercepted)
											   {
												   co_return std::move(*intercepted);
											   }
										   }

										   /* 唯一协程挂起点 */
										   auto res = co_await next(r);

										   /* 后置：按注册逆序执行（洋葱语义） */
										   for (int k = static_cast<int>(afters.size()) - 1; k >= 0; --k)
										   {
											   afters[k](r, res);
										   }

										   co_return res;
									   }};
			}(std::make_index_sequence<std::tuple_size_v<SyncTuple>> {});
		}

		/* ===== 主构建递归 ===== */

		/**
		 * @brief 从条目列表递归构建中间件调用链
		 * 每次从列表头部提取第一组（Async 单条目或 Sync 连续组），
		 * 递归构建剩余部分的链，再将当前组包裹在外层。
		 * 这自然形成注册顺序的外→内包裹（洋葱模型）。
		 */
		template <typename RemainingTuple>
		struct CompileTimeChainBuilder
		{
			static MiddlewareNext build(MiddlewareNext finalHandler)
			{
				using first = ExtractFirstGroupFromTuple<RemainingTuple>;

				if constexpr (first::kIsEmpty)
				{
					return finalHandler;
				}
				else
				{
					/* 先递归构建内层链（更靠近最终处理器） */
					auto inner = CompileTimeChainBuilder<typename first::remaining>::build(std::move(finalHandler));

					if constexpr (first::kIsAsync)
					{
						/* Async 条目：转发协程（不做 co_await，省一层协程帧） */
						auto mw = first::kHandler;
						return [mw, next = std::move(inner)](HttpRequest& r) -> Awaitable<HttpResponse>
						{
							return mw(r, next);
						};
					}
					else
					{
						/* 连续 Sync 条目：合并为单个协程帧 */
						return detail::buildSyncGroup<typename first::syncEntries>(std::move(inner));
					}
				}
			}
		};

	} // namespace detail

	/**
	 * @brief 编译期预构建中间件洋葱调用链
	 * 模板参数接受 CompileTimeAsyncMw / CompileTimeSyncMw / CompileTimeSyncFullMw 类型，
	 * 在编译期展开条目列表，连续 Sync 条目自动合并为单个协程帧，
	 * 语义与 MiddlewarePipeline::buildOptimizedChain() 完全一致。
	 * 用法示例：
	 * @code
	 * constexpr auto kLogMw = [](HttpRequest& r, MiddlewareNext n) -> Awaitable<HttpResponse> {
	 *     co_return co_await n(r);
	 * };
	 * constexpr auto kAuth = [](HttpRequest& r) -> SyncMiddlewareResult {
	 *     return std::nullopt;  // 放行
	 * };
	 * auto chain = buildCompileTimeChain<
	 *     CompileTimeAsyncMw<kLogMw>,
	 *     CompileTimeSyncMw<kAuth>
	 * >(finalHandler);
	 * @endcode
	 * @tparam Entries 编译期中间件条目类型列表（注册顺序，外层优先）
	 * @param finalHandler 最终处理器（路由分发等）
	 * @return 预构建的调用链
	 */
	template <typename... Entries>
	MiddlewareNext buildCompileTimeChain(MiddlewareNext finalHandler)
	{
		static_assert(sizeof...(Entries) > 0, "CompileTimeChain: at least one middleware entry is required");
		static_assert((detail::IsCompileTimeMw<Entries>::value && ...),
					  "CompileTimeChain: all template parameters must be CompileTimeAsyncMw, CompileTimeSyncMw, or "
					  "CompileTimeSyncFullMw");

		return detail::CompileTimeChainBuilder<std::tuple<Entries...>>::build(std::move(finalHandler));
	}

} // namespace hical
