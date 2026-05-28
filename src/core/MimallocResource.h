/**
 * @file MimallocResource.h
 * @brief mimalloc 的 std::pmr::memory_resource 包装
 * 仅在 HICAL_USE_MIMALLOC=1 时编译，作为 PMR 树最底层 upstream 替代 new_delete_resource()。
 * mimalloc 在高并发小对象频繁分配/释放场景下相比系统 malloc 有显著性能优势。
 */

#pragma once

#ifdef HICAL_USE_MIMALLOC

	#include <memory_resource>
	#include <mimalloc.h>
	#include <new>

namespace hical
{

	/**
	 * @brief 基于 mimalloc 的 PMR memory_resource 实现
	 * 线程安全：mi_malloc_aligned / mi_free_aligned 内部线程安全。
	 * 无状态单例，满足 memory_resource 的 is_equal 语义。
	 */
	class MimallocResource : public std::pmr::memory_resource
	{
	protected:
		void* do_allocate(size_t bytes, size_t alignment) override
		{
			void* p = mi_malloc_aligned(bytes, alignment);
			if (!p)
			{
				throw std::bad_alloc();
			}
			return p;
		}

		void do_deallocate(void* p, size_t /*bytes*/, size_t alignment) override
		{
			mi_free_aligned(p, alignment);
		}

		bool do_is_equal(const memory_resource& other) const noexcept override
		{
			return this == &other;
		}
	};

	/**
	 * @brief 获取全局 MimallocResource 单例
	 * @return MimallocResource 引用（进程级唯一）
	 */
	inline MimallocResource& mimallocResource()
	{
		static MimallocResource instance;
		return instance;
	}

} // namespace hical

#endif // HICAL_USE_MIMALLOC
