/**
 * @file WriteNode.h
 * @brief 多态写缓冲节点（支持内存数据和文件发送）
 * 设计灵感来自 Trantor 网络库的 BufferNode 多态设计。
 * 将发送数据抽象为节点，支持在发送队列中混合不同类型的数据源：
 * - MemoryWriteNode：内存中的字符串数据
 * - FileWriteNode：文件路径 + 偏移 + 长度（支持后续零拷贝优化）
 * writeLoop 根据节点类型选择最优发送策略：
 * - 连续内存节点 → scatter-gather I/O 批量发送
 * - 文件节点 → 分块读取并发送（后续可优化为 sendfile/TransmitFile）
 */

#pragma once

#include "PmrBuffer.h"
#include <boost/asio/buffer.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace hical
{

	/**
	 * @brief 写缓冲节点基类
	 */
	class WriteNode
	{
	public:
		virtual ~WriteNode() = default;

		/**
		 * @brief 节点数据大小（字节）
		 * @return 总字节数
		 */
		virtual size_t size() const = 0;

		/**
		 * @brief 是否为文件节点
		 * @return true 如果是文件节点
		 */
		virtual bool isFile() const
		{
			return false;
		}

		/**
		 * @brief 获取内存数据的 const_buffer 视图（仅内存节点有效）
		 * @return const_buffer，文件节点返回空 buffer
		 */
		virtual boost::asio::const_buffer asBuffer() const
		{
			return {};
		}

		WriteNode() = default;
		WriteNode(const WriteNode&) = delete;
		WriteNode& operator=(const WriteNode&) = delete;
	};

	/**
	 * @brief 内存数据写节点
	 * 持有 shared_ptr<string>，支持零拷贝共享。
	 */
	class MemoryWriteNode : public WriteNode
	{
	public:
		explicit MemoryWriteNode(std::shared_ptr<std::string> data) : data_(std::move(data))
		{
		}

		explicit MemoryWriteNode(std::string&& data) : data_(std::make_shared<std::string>(std::move(data)))
		{
		}

		size_t size() const override
		{
			return data_->size();
		}

		boost::asio::const_buffer asBuffer() const override
		{
			return boost::asio::buffer(*data_);
		}

		boost::asio::const_buffer buffer() const
		{
			return boost::asio::buffer(*data_);
		}

		const std::string& data() const
		{
			return *data_;
		}

	private:
		std::shared_ptr<std::string> data_;
	};

	/**
	 * @brief PMR 缓冲区写节点
	 * 直接持有 PmrBuffer（move 语义），避免 PmrBuffer → string 的数据拷贝。
	 */
	class PmrBufferWriteNode : public WriteNode
	{
	public:
		explicit PmrBufferWriteNode(PmrBuffer&& buffer) : buffer_(std::move(buffer))
		{
		}

		size_t size() const override
		{
			return buffer_.readableBytes();
		}

		boost::asio::const_buffer asBuffer() const override
		{
			return boost::asio::buffer(buffer_.peek(), buffer_.readableBytes());
		}

		boost::asio::const_buffer buffer() const
		{
			return boost::asio::buffer(buffer_.peek(), buffer_.readableBytes());
		}

	private:
		PmrBuffer buffer_;
	};

	/**
	 * @brief 文件写节点
	 * 持有文件路径、偏移量和长度。
	 * writeLoop 中分块读取并异步发送。
	 * 后续可优化为 sendfile(Linux) / TransmitFile(Windows) 零拷贝。
	 */
	class FileWriteNode : public WriteNode
	{
	public:
		/**
		 * @brief 构造文件写节点
		 * @param path 文件路径
		 * @param offset 起始偏移量
		 * @param length 发送长度（-1 表示到文件末尾）
		 */
		FileWriteNode(std::filesystem::path path, int64_t offset = 0, int64_t length = -1)
			: path_(std::move(path)), offset_(offset)
		{
			if (length < 0)
			{
				auto fileSize = std::filesystem::file_size(path_);
				length_ = static_cast<int64_t>(fileSize) - offset_;
			}
			else
			{
				length_ = length;
			}
		}

		size_t size() const override
		{
			return static_cast<size_t>(length_);
		}

		bool isFile() const override
		{
			return true;
		}

		const std::filesystem::path& path() const
		{
			return path_;
		}

		int64_t offset() const
		{
			return offset_;
		}

		int64_t length() const
		{
			return length_;
		}

	private:
		std::filesystem::path path_;
		int64_t offset_;
		int64_t length_;
	};

} // namespace hical
