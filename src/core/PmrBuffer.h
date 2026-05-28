/**
 * @file PmrBuffer.h
 * @brief PMR 内存池缓冲区类型别名
 */

#pragma once

#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <vector>

namespace hical
{

	/**
	 * @brief 基于 pmr 的统一缓冲区
	 * hical 统一缓冲区，使用 pmr 分配器管理内存。
	 * 支持 prepend 区域和自动扩容，底层使用 std::pmr::vector。
	 */
	class PmrBuffer
	{
	public:
		static constexpr size_t hDefaultSize = 2048;
		static constexpr size_t hPrependSize = 8;

		/**
		 * @brief 构造函数
		 * @param allocator pmr 分配器
		 * @param initialSize 初始大小
		 */
		explicit PmrBuffer(std::pmr::polymorphic_allocator<std::byte> allocator = {}, size_t initialSize = hDefaultSize)
			: buffer_(hPrependSize + initialSize, allocator)
			, initialCapacity_(initialSize)
			, readIndex_(hPrependSize)
			, writeIndex_(hPrependSize)
		{
		}

		// ============ 读取接口 ============

		/**
		 * @brief 获取可读数据起始指针
		 * @return 数据指针
		 */
		const char* peek() const
		{
			return reinterpret_cast<const char*>(begin() + readIndex_);
		}

		/**
		 * @brief 获取可读字节数
		 * @return 字节数
		 */
		size_t readableBytes() const
		{
			return writeIndex_ - readIndex_;
		}

		/**
		 * @brief 消费指定字节数
		 * @param len 字节数
		 */
		void retrieve(size_t len)
		{
			if (len > readableBytes())
			{
				throw std::out_of_range("PmrBuffer::retrieve: len exceeds readable bytes");
			}
			if (len < readableBytes())
			{
				readIndex_ += len;
			}
			else
			{
				retrieveAll();
			}
		}

		/**
		 * @brief 消费到指定位置
		 * @param end 结束位置指针
		 */
		void retrieveUntil(const char* end)
		{
			if (peek() > end || end > beginWrite())
			{
				throw std::out_of_range("PmrBuffer::retrieveUntil: invalid end pointer");
			}
			retrieve(end - peek());
		}

		/**
		 * @brief 消费所有数据
		 */
		void retrieveAll()
		{
			// 缓冲区膨胀超过初始容量 2 倍时缩容，避免内存浪费
			// 使用构造时的 initialCapacity_ 作为基准，而非固定 hDefaultSize，
			// 防止大初始容量的 buffer 被错误缩容后反复扩容
			if (buffer_.size() > (initialCapacity_ + hPrependSize) * 2)
			{
				buffer_.resize(initialCapacity_ + hPrependSize);
			}
			readIndex_ = hPrependSize;
			writeIndex_ = hPrependSize;
		}

		/**
		 * @brief 读取指定字节数并返回字符串
		 * @param len 字节数
		 * @return 字符串
		 */
		std::string read(size_t len)
		{
			if (len > readableBytes())
			{
				throw std::out_of_range("PmrBuffer::read: len exceeds readable bytes");
			}
			std::string result(peek(), len);
			retrieve(len);
			return result;
		}

		/**
		 * @brief 读取所有数据并返回字符串
		 * @return 字符串
		 */
		std::string readAll()
		{
			return read(readableBytes());
		}

		/**
		 * @brief 查找 CRLF（\r\n）
		 * @return CRLF 位置指针，未找到返回 nullptr
		 */
		const char* findCRLF() const
		{
			const char* crlf = std::search(peek(), beginWrite(), hCrlf, hCrlf + 2);
			return crlf == beginWrite() ? nullptr : crlf;
		}

		/**
		 * @brief 查找 EOL（\n）
		 * @return EOL 位置指针，未找到返回 nullptr
		 */
		const char* findEOL() const
		{
			const void* eol = std::memchr(peek(), '\n', readableBytes());
			return static_cast<const char*>(eol);
		}

		// ============ 写入接口 ============

		/**
		 * @brief 获取可写区域起始指针
		 * @return 指针
		 */
		char* beginWrite()
		{
			return reinterpret_cast<char*>(begin() + writeIndex_);
		}

		const char* beginWrite() const
		{
			return reinterpret_cast<const char*>(begin() + writeIndex_);
		}

		/**
		 * @brief 获取可写字节数
		 * @return 字节数
		 */
		size_t writableBytes() const
		{
			return buffer_.size() - writeIndex_;
		}

		/**
		 * @brief 标记已写入字节数
		 * @param len 字节数
		 */
		void hasWritten(size_t len)
		{
			if (len > writableBytes())
			{
				throw std::out_of_range("PmrBuffer::hasWritten: len exceeds writable bytes");
			}
			writeIndex_ += len;
		}

		/**
		 * @brief 追加数据
		 * @param data 数据指针
		 * @param len 数据长度
		 */
		void append(const char* data, size_t len)
		{
			ensureWritableBytes(len);
			std::copy(data, data + len, reinterpret_cast<char*>(begin() + writeIndex_));
			hasWritten(len);
		}

		/**
		 * @brief 追加字符串
		 * @param str 字符串
		 */
		void append(const std::string& str)
		{
			append(str.data(), str.size());
		}

		/**
		 * @brief 追加另一个缓冲区
		 * @param buf 缓冲区
		 */
		void append(const PmrBuffer& buf)
		{
			append(buf.peek(), buf.readableBytes());
		}

		/**
		 * @brief 确保有足够的可写空间
		 * @param len 需要的字节数
		 */
		void ensureWritableBytes(size_t len)
		{
			if (writableBytes() < len)
			{
				makeSpace(len);
			}
			assert(writableBytes() >= len && "makeSpace logic error: insufficient space after expansion");
		}

		// ============ 交换 ============

		/**
		 * @brief 与另一个缓冲区交换
		 * @param rhs 另一个缓冲区
		 * @throw std::logic_error 当两个缓冲区的分配器不同时抛出
		 */
		void swap(PmrBuffer& rhs)
		{
			// pmr::vector::swap 在分配器不相等时行为未定义，此处做防御性检查
			if (buffer_.get_allocator() != rhs.buffer_.get_allocator())
			{
				throw std::logic_error("PmrBuffer::swap: cannot swap buffers with different allocators");
			}
			buffer_.swap(rhs.buffer_);
			std::swap(readIndex_, rhs.readIndex_);
			std::swap(writeIndex_, rhs.writeIndex_);
		}

		// ============ 分配器访问 ============

		/**
		 * @brief 获取底层分配器
		 * @return pmr 分配器
		 */
		std::pmr::polymorphic_allocator<std::byte> get_allocator() const
		{
			return buffer_.get_allocator();
		}

	private:
		std::byte* begin()
		{
			return buffer_.data();
		}

		const std::byte* begin() const
		{
			return buffer_.data();
		}

		void makeSpace(size_t len)
		{
			if (writableBytes() + prependableBytes() < len + hPrependSize)
			{
				// 扩容：优先 2 倍增长减少频繁扩容
				size_t newLen;
				if (buffer_.size() * 2 > writeIndex_ + len)
				{
					newLen = buffer_.size() * 2;
				}
				else
				{
					newLen = writeIndex_ + len;
				}
				buffer_.resize(newLen);
			}
			else
			{
				// 移动数据到前面
				size_t readable = readableBytes();
				std::copy(begin() + readIndex_, begin() + writeIndex_, begin() + hPrependSize);
				readIndex_ = hPrependSize;
				writeIndex_ = readIndex_ + readable;
			}
		}

		size_t prependableBytes() const
		{
			return readIndex_;
		}

		std::pmr::vector<std::byte> buffer_;
		size_t initialCapacity_; ///< 构造时传入的初始容量（缩容基准）
		size_t readIndex_;
		size_t writeIndex_;

		static constexpr const char hCrlf[] = "\r\n";
	};

} // namespace hical
