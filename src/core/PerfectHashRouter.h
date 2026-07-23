/**
 * @file PerfectHashRouter.h
 * @brief 编译期完美哈希路由表（Multiply-Shift 完美哈希，编译期搜索种子，运行时一次乘法+位移）
 */

#pragma once

#include "HttpTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace hical
{

	namespace detail
	{

		/// constexpr: ceil to next power of 2
		inline constexpr size_t nextPow2(size_t x) noexcept
		{
			if (x == 0)
			{
				return 1;
			}
			--x;
			x |= x >> 1;
			x |= x >> 2;
			x |= x >> 4;
			x |= x >> 8;
			x |= x >> 16;
			if constexpr (sizeof(size_t) >= 8)
			{
				x |= x >> 32;
			}
			return x + 1;
		}

		/// constexpr: floor(log2(x)) for x > 0
		inline constexpr size_t log2Floor(size_t x) noexcept
		{
			size_t r = 0;
			while (x >>= 1)
			{
				++r;
			}
			return r;
		}

		/// constexpr: djb2 64-bit hash for path + method
		inline constexpr uint64_t hashPathMethod(std::string_view path, HttpMethod method) noexcept
		{
			uint64_t h = 5381;
			for (char c : path)
			{
				h = ((h << 5) + h) + static_cast<unsigned char>(c);
			}
			h = ((h << 5) + h) + static_cast<uint8_t>(method);
			h ^= h >> 33;
			h *= 0xFF51AFD7ED558CCDULL;
			h ^= h >> 33;
			return h;
		}

	} // namespace detail

	/**
	 * @brief 编译期完美哈希路由表
	 * Multiply-Shift 完美哈希：编译期暴力搜索一个 64-bit 奇数种子，
	 * 使 (baseHash * seed) >> shift 对所有键无冲突。
	 * 运行时 lookup() 只需一次 djb2 哈希 + 一次乘法 + 一次位移 + 字符串比较。
	 * 没有 std::hash、除法或取模运算。
	 * @tparam N 路由数量（1..256）
	 */
	template <size_t N>
	class PerfectHashRouter
	{
		static_assert(N > 0, "PerfectHashRouter 至少需要一个路由键");
		static_assert(N <= 256, "编译期完美哈希最多支持 256 个静态路由");

		// Table size: at least N * 1.5, rounded up to next power of 2
		// Multiply-shift uses top bits of the 64-bit product, so table must be a power of two
		static constexpr size_t kTableSize = detail::nextPow2(N > 1 ? (N + (N / 2)) : 2);
		static constexpr size_t kShift = 64 - detail::log2Floor(kTableSize);

	public:
		struct Key
		{
			HttpMethod method = HttpMethod::hGet;
			std::string_view path;
			size_t index_ = 0; ///< 映射回外部数组的下标
		};

		struct Entry
		{
			HttpMethod method = HttpMethod::hGet;
			std::string_view path;
			size_t index_ = 0; ///< 映射回外部数组的下标
		};

		/**
		 * @brief constexpr 构造：搜索无冲突种子并建表
		 * @param keys 编译期路由键数组
		 */
		constexpr explicit PerfectHashRouter(std::array<Key, N> keys) : keys_(keys), seed_ {}, entries_ {}
		{
			// 第一步：预计算所有 key 的 baseHash
			uint64_t hashes[N] {};
			for (size_t i = 0; i < N; ++i)
			{
				hashes[i] = detail::hashPathMethod(keys_[i].path, keys_[i].method);
			}

			// 第二步：搜索使所有 (hash * seed) >> shift 无冲突的奇数种子
			bool found = false;
			for (uint64_t cand = 1; cand < 100000 && !found; cand += 2)
			{
				std::array<int, kTableSize> occupied {};
				for (size_t i = 0; i < kTableSize; ++i)
				{
					occupied[i] = -1;
				}

				bool conflict = false;
				for (size_t i = 0; i < N && !conflict; ++i)
				{
					size_t slot = static_cast<size_t>((hashes[i] * cand) >> kShift);
					if (occupied[slot] >= 0)
					{
						conflict = true;
					}
					else
					{
						occupied[slot] = static_cast<int>(i);
					}
				}

				if (!conflict)
				{
					seed_ = cand;
					for (size_t i = 0; i < kTableSize; ++i)
					{
						entries_[i] = Entry {};
					}
					for (size_t i = 0; i < N; ++i)
					{
						size_t slot = static_cast<size_t>((hashes[i] * cand) >> kShift);
						entries_[slot] = {keys_[i].method, keys_[i].path, keys_[i].index_};
					}
					found = true;
				}
			}

			if (!found)
			{
				throw "PerfectHashRouter: 种子搜索失败，请检查路由键集合";
			}
		}

		/**
		 * @brief 运行时 O(1) 查找
		 * @param method HTTP 方法
		 * @param path 请求路径
		 * @return 命中的 Entry 指针，未找到返回 nullptr
		 * 运行时路径：djb2 哈希 path+method -> 乘以编译期种子 -> 右移取高位 -> 查表。
		 * 没有除法、没有取模、没有分支预测惩罚。
		 */
		[[nodiscard]] const Entry* lookup(HttpMethod method, std::string_view path) const noexcept
		{
			uint64_t h = detail::hashPathMethod(path, method);
			size_t slot = static_cast<size_t>((h * seed_) >> kShift);
			if (entries_[slot].path.data() != nullptr && entries_[slot].method == method && entries_[slot].path == path)
			{
				return &entries_[slot];
			}
			return nullptr;
		}

		/**
		 * @brief 获取哈希表大小
		 */
		[[nodiscard]] static constexpr size_t tableSize() noexcept
		{
			return kTableSize;
		}

		/**
		 * @brief 获取原始键数组
		 */
		[[nodiscard]] constexpr const std::array<Key, N>& keys() const noexcept
		{
			return keys_;
		}

	private:
		std::array<Key, N> keys_;
		uint64_t seed_;
		std::array<Entry, kTableSize> entries_;
	};

	/**
	 * @brief 运行时完美哈希查找器（类型擦除，不含 std::function 开销）
	 * 存储有 heap 分配的 entries 数组 + 运行时搜索到的种子，
	 * 查找逻辑内联，无虚函数调用。
	 * 由 MetaRoutes::registerRoutes() 构建，注入到 Router。
	 */
	class RuntimePerfectHashLookup
	{
	public:
		struct Entry
		{
			HttpMethod method = HttpMethod::hGet;
			std::string_view path;
			size_t index_ = 0; ///< 映射回 Router::RouteEntry* 数组的下标
		};

		RuntimePerfectHashLookup() = default;

		/**
		 * @brief 运行时构建：从 (method, path) 列表搜索种子并建表
		 * @param keys 路由键列表（method + path）
		 * @return 构建好的查找器，找不到种子时返回空（valid() == false）
		 */
		static RuntimePerfectHashLookup buildFromKeys(const std::vector<std::pair<HttpMethod, std::string_view>>& keys)
		{
			if (keys.empty() || keys.size() > 256)
			{
				return RuntimePerfectHashLookup {};
			}

			size_t N = keys.size();
			size_t tableSize = detail::nextPow2(N > 1 ? (N + (N / 2)) : 2);
			size_t shift = 64 - detail::log2Floor(tableSize);

			// 构建带 index 的 Entry 列表
			std::vector<Entry> entries;
			entries.reserve(N);
			for (size_t i = 0; i < N; ++i)
			{
				entries.push_back({keys[i].first, keys[i].second, i});
			}

			// 预计算哈希
			std::vector<uint64_t> hashes(N);
			for (size_t i = 0; i < N; ++i)
			{
				hashes[i] = detail::hashPathMethod(keys[i].second, keys[i].first);
			}

			// 搜索无冲突种子
			for (uint64_t cand = 1; cand < 100000; cand += 2)
			{
				std::vector<int> occupied(tableSize, -1);
				bool conflict = false;
				for (size_t i = 0; i < N && !conflict; ++i)
				{
					size_t slot = static_cast<size_t>((hashes[i] * cand) >> shift);
					if (occupied[slot] >= 0)
					{
						conflict = true;
					}
					else
					{
						occupied[slot] = static_cast<int>(i);
					}
				}

				if (!conflict)
				{
					return RuntimePerfectHashLookup(std::move(entries), cand, tableSize, shift);
				}
			}

			// 种子搜索失败（极少见），返回空
			return RuntimePerfectHashLookup {};
		}

		/**
		 * @brief 用运行时 keys + 搜索到的种子建表
		 * @param keys 路由键数组
		 * @param seed 已搜索好的无冲突种子
		 * @param tableSize 哈希表大小（2 的幂）
		 * @param shift 右移位数 (64 - log2(tableSize))
		 */
		RuntimePerfectHashLookup(std::vector<Entry> keys, uint64_t seed, size_t /* tableSize */, size_t shift)
			: keys_(std::move(keys)), seed_(seed), shift_(shift), entries_(1ULL << (64 - shift))
		{
			for (const auto& key : keys_)
			{
				uint64_t h = detail::hashPathMethod(key.path, key.method);
				size_t slot = static_cast<size_t>((h * seed_) >> shift_);
				entries_[slot] = key;
			}
		}

		/**
		 * @brief O(1) 查找，和编译期版本相同算法
		 * @return 命中时返回 index_，未命中返回 SIZE_MAX
		 */
		[[nodiscard]] size_t lookup(HttpMethod method, std::string_view path) const noexcept
		{
			if (seed_ == 0)
			{
				return SIZE_MAX;
			}
			uint64_t h = detail::hashPathMethod(path, method);
			size_t slot = static_cast<size_t>((h * seed_) >> shift_);
			if (entries_[slot].path.data() != nullptr && entries_[slot].method == method && entries_[slot].path == path)
			{
				return entries_[slot].index_;
			}
			return SIZE_MAX;
		}

		/**
		 * @brief 是否已初始化（含有效数据）
		 */
		[[nodiscard]] bool valid() const noexcept
		{
			return seed_ != 0;
		}

		/**
		 * @brief 原始键数量
		 */
		[[nodiscard]] size_t keyCount() const noexcept
		{
			return keys_.size();
		}

	private:
		std::vector<Entry> keys_;
		uint64_t seed_ = 0;
		size_t shift_ = 0;
		std::vector<Entry> entries_;
	};

} // namespace hical
