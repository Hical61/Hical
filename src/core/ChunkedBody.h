/**
 * @file ChunkedBody.h
 * @brief Chunked Transfer-Encoding 响应体封装（RFC 7230 §4.1）
 */

#pragma once

#include "HttpTypes.h"
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hical
{

	/**
	 * @brief chunk 帧中的扩展参数（RFC 7230 §4.1.1）
	 * 形如: chunk-size;name=value\r\n
	 */
	struct ChunkExtension
	{
		std::string name;
		std::string value;
	};

	/**
	 * @brief Chunked Transfer-Encoding 响应体
	 * 支持两种使用模式：
	 * 1. 收集模式（Collect）：多次 write() 收集全部 chunk，end() 后通过 chunks() 取出
	 *    用于请求 handler 内全量计算后一次性发送
	 * 2. 流式模式（Stream）：write() 直接触发 chunk frame 发送（需要外部协程驱动）
	 *    用于 SSE、大文件流式输出等场景（后续切片实现）
	 * 非线程安全——单线程/协程内使用。
	 */
	class ChunkedBody
	{
	public:
		ChunkedBody() = default;

		/**
		 * @brief 写入一个数据块
		 * @param data 数据块内容
		 * 收集模式下追加到 chunks_ 列表
		 */
		void write(std::string_view data);

		/**
		 * @brief 结束分块传输
		 * @param trailers 可选 trailer 扩展（RFC 7230 §4.1.2）
		 * 之后再次调用 write() 会被忽略
		 */
		void end(const std::vector<ChunkExtension>& trailers = {});

		/**
		 * @brief 是否已结束
		 */
		bool finished() const noexcept
		{
			return finished_;
		}

		/**
		 * @brief 获取所有已收集的 chunks（收集模式使用）
		 * @return 所有写入的数据块列表（不含编码帧头尾）
		 */
		const std::vector<std::string>& chunks() const noexcept
		{
			return chunks_;
		}

		/**
		 * @brief 获取 trailer 扩展列表
		 */
		const std::vector<ChunkExtension>& trailers() const noexcept
		{
			return trailers_;
		}

	private:
		std::vector<std::string> chunks_;
		std::vector<ChunkExtension> trailers_;
		bool finished_ {false};
	};

	// ============ 自由函数（编码/序列化工具） ============

	/**
	 * @brief 序列化单个 chunk 帧
	 * @param data 数据块
	 * @return wire 格式：chunk-size\r\ndata\r\n
	 * 空 data 返回 0\r\n\r\n（终止帧）
	 */
	std::string serializeChunkFrame(std::string_view data);

	/**
	 * @brief 序列化终止帧（含可选 trailer）
	 * @param trailers trailer 扩展列表
	 * @return wire 格式：0\r\n[trailer]\r\n
	 */
	std::string serializeTrailerFrame(const std::vector<ChunkExtension>& trailers = {});

	/**
	 * @brief 将 ChunkedBody 全部序列化为 wire 字节流
	 * @param body ChunkedBody
	 * @return 完整的 chunked 编码字节流
	 */
	std::string serializeChunkedBody(const ChunkedBody& body);

	/**
	 * @brief 将 ChunkedBody 序列化到已预留空间的 string 中（单次分配优化版）
	 * @param out 输出 string（调用方应预分配足够容量，见 wireSize()）
	 * @param body ChunkedBody
	 */
	void serializeChunkedBodyTo(std::string& out, const ChunkedBody& body);

	/**
	 * @brief 预计算 ChunkedBody 序列化后的 wire 字节数
	 * @param body ChunkedBody
	 * @return wire 大小（字节）
	 */
	size_t chunkedBodyWireSize(const ChunkedBody& body) noexcept;

} // namespace hical
