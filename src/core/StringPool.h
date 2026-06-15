/**
 * @file StringPool.h
 * @brief thread_local size-class 字符串对象池
 */

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>

namespace hical
{

	/**
	 * @brief 分级字符串对象池
	 * 按大小分 5 档（256/512/1K/2K/4K），每档 thread_local 最多缓存 32 个。
	 * 热路径上省掉 malloc/free，跟 MpscNodePool 一个思路。
	 * 超过 4096 的不池化，走正常 make_shared。
	 * 跨线程注意：池是 thread_local 的，释放时 string 归还到执行释放的那个线程的池。
	 * 如果 send 和 writeLoop 不在同一线程，池化就白搭了（退化成普通 new/delete）。
	 * 好在 HTTP handler 里 send response 本身就跑在 IO 线程上，跟 writeLoop 同线程，
	 * 所以主流场景池化是生效的。
	 */
	class StringPool
	{
	public:
		/// 拿一个容量 >= requiredSize 的池化 string，用完自动还回池
		static std::shared_ptr<std::string> acquire(size_t requiredSize);

	private:
		static constexpr size_t kNumClasses = 5;
		static constexpr size_t kClassSizes[kNumClasses] = {256, 512, 1024, 2048, 4096};
		static constexpr size_t kMaxPooled = 32;

		struct SizeClass
		{
			std::string* slots[kMaxPooled] {};
			size_t count = 0;
		};

		struct Pool
		{
			SizeClass classes[kNumClasses];

			~Pool()
			{
#ifndef __MINGW32__
				// MinGW 下 thread_local 析构在 DLL TLS 回调里跑，那时 CRT 堆
				// 可能已销毁，delete 下去就崩。进程退出 OS 会帮我们收。
				for (auto& cls : classes)
				{
					for (size_t i = 0; i < cls.count; ++i)
					{
						delete cls.slots[i];
					}
				}
#endif
			}
		};

		static Pool& threadPool()
		{
			thread_local Pool pool;
			return pool;
		}

		/// 找到刚好能装下 requiredSize 的档位，找不到返回 kNumClasses
		static size_t findClass(size_t requiredSize)
		{
			for (size_t i = 0; i < kNumClasses; ++i)
			{
				if (kClassSizes[i] >= requiredSize)
				{
					return i;
				}
			}
			return kNumClasses;
		}

		/// 还回池里（shared_ptr 的 custom deleter 触发）
		static void release(std::string* str, size_t classIdx)
		{
			auto& pool = threadPool();
			auto& cls = pool.classes[classIdx];
			str->clear();
			if (cls.count < kMaxPooled)
			{
				cls.slots[cls.count++] = str;
			}
			else
			{
				delete str;
			}
		}
	};

	inline std::shared_ptr<std::string> StringPool::acquire(size_t requiredSize)
	{
		size_t classIdx = findClass(requiredSize);

		// 超大字符串不池化
		if (classIdx == kNumClasses)
		{
			return std::make_shared<std::string>();
		}

		auto& pool = threadPool();
		auto& cls = pool.classes[classIdx];

		std::string* raw = nullptr;
		if (cls.count > 0)
		{
			raw = cls.slots[--cls.count];
		}
		else
		{
			raw = new std::string();
			raw->reserve(kClassSizes[classIdx]);
		}

		// 带 custom deleter 的 shared_ptr，释放时归还到池
		return std::shared_ptr<std::string>(raw,
											[classIdx](std::string* p)
											{
												release(p, classIdx);
											});
	}

} // namespace hical
