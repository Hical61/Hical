/**
 * @file ReadBufferPool.cpp
 * @brief 读缓冲区 thread_local 对象池实现
 */

#include "ReadBufferPool.h"
#include <memory>

namespace hical
{

	thread_local ReadBufferPool::PoolSlots ReadBufferPool::tlsPool;

	ReadBufferPool::BufferHandle ReadBufferPool::acquire()
	{
		auto handle = takeBuffer();
		handle.get().clear();
		return handle;
	}

	ReadBufferPool::BufferHandle ReadBufferPool::acquireForRead(size_t size)
	{
		auto handle = takeBuffer();
		// 同尺寸 resize 不写旧内容；扩容时 string 会初始化新增部分，始终是合法可写存储。
		handle.get().resize(size);
		return handle;
	}

	ReadBufferPool::BufferHandle ReadBufferPool::takeBuffer()
	{
		if (tlsPool.count_ > 0)
		{
			std::string* buf = tlsPool.slots_[--tlsPool.count_];
			return BufferHandle(buf);
		}

		// 池空，新建一个，预留标准大小
		auto buf = std::make_unique<std::string>();
		buf->reserve(kBufferSize);
		return BufferHandle(buf.release());
	}

	void ReadBufferPool::returnBuffer(std::string* buf)
	{
		// 超大 buffer 直接丢弃，避免异常大请求撑大池子
		if (buf->capacity() > kMaxReturnSize)
		{
			delete buf;
			return;
		}

		if (tlsPool.count_ < kMaxPooled)
		{
			// 保留可写尺寸供 acquireForRead 复用；普通 acquire 仍会先 clear。
			tlsPool.slots_[tlsPool.count_++] = buf;
		}
		else
		{
			delete buf;
		}
	}

} // namespace hical
