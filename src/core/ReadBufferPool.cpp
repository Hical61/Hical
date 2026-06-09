/**
 * @file ReadBufferPool.cpp
 * @brief 读缓冲区 thread_local 对象池实现
 */

#include "ReadBufferPool.h"

namespace hical
{

	thread_local ReadBufferPool::PoolSlots ReadBufferPool::tlsPool;

	ReadBufferPool::BufferHandle ReadBufferPool::acquire()
	{
		if (tlsPool.count_ > 0)
		{
			std::string* buf = tlsPool.slots_[--tlsPool.count_];
			buf->clear();
			return BufferHandle(buf);
		}

		// 池空，新建一个，预留标准大小
		auto* buf = new std::string();
		buf->reserve(kBufferSize);
		return BufferHandle(buf);
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
			buf->clear();
			tlsPool.slots_[tlsPool.count_++] = buf;
		}
		else
		{
			delete buf;
		}
	}

} // namespace hical
