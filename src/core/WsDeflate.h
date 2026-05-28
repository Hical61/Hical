/**
 * @file WsDeflate.h
 * @brief WebSocket permessage-deflate 压缩扩展
 */

#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace hical
{

	/**
	 * @brief permessage-deflate 压缩/解压上下文（RFC 7692）
	 * 使用 pimpl 模式隔离 zlib 头文件，避免 <zlib.h> 宏污染其他翻译单元。
	 * 每个 WebSocket 连接持有一个实例（如果启用了压缩）。
	 * zlib 状态可以跨消息保持（context takeover）以提高压缩率，
	 * 或每条消息重置（no context takeover）以节省内存。
	 */
	class WsDeflateContext
	{
	public:
		struct Config
		{
			int serverMaxWindowBits = 15;         ///< 压缩窗口大小（服务端发送用，8-15）
			int clientMaxWindowBits = 15;         ///< 解压窗口大小（接收客户端消息用，8-15）
			bool serverNoContextTakeover = false; ///< 每条消息后重置压缩状态
			bool clientNoContextTakeover = false; ///< 每条消息后重置解压状态
			int compLevel = 6;                    ///< zlib 压缩级别（1-9，6 是默认平衡点）
			int memLevel = 4;                     ///< zlib 内存级别（1-9）
		};

		explicit WsDeflateContext(const Config& config);
		~WsDeflateContext();

		// 禁止拷贝（zlib z_stream 状态不可拷贝）
		WsDeflateContext(const WsDeflateContext&) = delete;
		WsDeflateContext& operator=(const WsDeflateContext&) = delete;

		// 允许移动
		WsDeflateContext(WsDeflateContext&& other) noexcept;
		WsDeflateContext& operator=(WsDeflateContext&& other) noexcept;

		/**
		 * @brief 压缩消息（服务端发送前调用）
		 * 使用 Z_SYNC_FLUSH 刷新，并去除尾部 4 字节 sync marker (0x00 0x00 0xFF 0xFF)。
		 * 如果 serverNoContextTakeover，每次调用前自动重置 deflate 状态。
		 * @param input 待压缩的消息数据
		 * @return 压缩后的数据
		 */
		std::string compress(std::string_view input);

		/**
		 * @brief 解压消息（接收客户端压缩帧后调用）
		 * 在输入末尾追加 4 字节 sync marker 后执行 inflate。
		 * 如果 clientNoContextTakeover，每次调用前自动重置 inflate 状态。
		 * @param input 压缩的帧数据
		 * @param maxOutputSize 解压输出上限（防止 zip bomb DoS，0 表示不限制）
		 * @return 解压后的消息
		 * @throws std::runtime_error 解压失败或输出超过上限
		 */
		std::string decompress(std::string_view input, size_t maxOutputSize = 0);

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};

} // namespace hical
