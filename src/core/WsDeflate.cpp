/**
 * @file WsDeflate.cpp
 * @brief permessage-deflate 压缩实现
 */

#include "WsDeflate.h"
#include <cstring>
#include <stdexcept>
#include <cstdint>
#include <zlib.h>

namespace hical
{

	struct WsDeflateContext::Impl
	{
		z_stream deflateStream {};
		z_stream inflateStream {};
		Config config;
		bool deflateInit = false;
		bool inflateInit = false;

		explicit Impl(const Config& cfg) : config(cfg)
		{
			// deflateInit2: 负窗口位数 = raw deflate（无 zlib/gzip header/trailer），符合 RFC 7692
			int ret = deflateInit2(&deflateStream,
								   config.compLevel,
								   Z_DEFLATED,
								   -config.serverMaxWindowBits,
								   config.memLevel,
								   Z_DEFAULT_STRATEGY);
			if (ret != Z_OK)
			{
				throw std::runtime_error("WsDeflateContext: deflateInit2 failed");
			}
			deflateInit = true;

			// inflateInit2: 同理，raw inflate
			ret = inflateInit2(&inflateStream, -config.clientMaxWindowBits);
			if (ret != Z_OK)
			{
				deflateEnd(&deflateStream);
				throw std::runtime_error("WsDeflateContext: inflateInit2 failed");
			}
			inflateInit = true;
		}

		~Impl()
		{
			if (deflateInit)
			{
				deflateEnd(&deflateStream);
			}
			if (inflateInit)
			{
				inflateEnd(&inflateStream);
			}
		}

		Impl(const Impl&) = delete;
		Impl& operator=(const Impl&) = delete;
	};

	WsDeflateContext::WsDeflateContext(const Config& config) : impl_(std::make_unique<Impl>(config))
	{
	}

	WsDeflateContext::~WsDeflateContext() = default;

	WsDeflateContext::WsDeflateContext(WsDeflateContext&& other) noexcept = default;

	WsDeflateContext& WsDeflateContext::operator=(WsDeflateContext&& other) noexcept = default;

	std::string WsDeflateContext::compress(std::string_view input)
	{
		auto& s = impl_->deflateStream;

		// server_no_context_takeover: 每条消息重置压缩状态
		if (impl_->config.serverNoContextTakeover)
		{
			deflateReset(&s);
		}

		// 输出缓冲区：预估压缩后大小（最坏情况下 deflate 输出略大于输入）
		std::string output;
		output.resize(deflateBound(&s, static_cast<uLong>(input.size())));

		s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
		s.avail_in = static_cast<uInt>(input.size());
		s.next_out = reinterpret_cast<Bytef*>(output.data());
		s.avail_out = static_cast<uInt>(output.size());

		int ret = deflate(&s, Z_SYNC_FLUSH);
		if (ret != Z_OK)
		{
			throw std::runtime_error("WsDeflateContext::compress: deflate failed");
		}

		size_t produced = output.size() - s.avail_out;
		output.resize(produced);

		// RFC 7692 §7.2.1: Z_SYNC_FLUSH 在输出末尾追加 0x00 0x00 0xFF 0xFF。
		// 压缩帧在发送前必须去除这 4 字节 sync marker。
		constexpr uint8_t kSyncMarker[] = {0x00, 0x00, 0xFF, 0xFF};
		if (output.size() >= 4 && std::memcmp(output.data() + output.size() - 4, kSyncMarker, 4) == 0)
		{
			output.resize(output.size() - 4);
		}

		return output;
	}

	std::string WsDeflateContext::decompress(std::string_view input, size_t maxOutputSize)
	{
		auto& s = impl_->inflateStream;

		// client_no_context_takeover: 每条消息重置解压状态
		if (impl_->config.clientNoContextTakeover)
		{
			inflateReset(&s);
		}

		// RFC 7692 §7.2.2: 解压前在输入末尾追加 sync marker
		std::string inputWithSync;
		inputWithSync.reserve(input.size() + 4);
		inputWithSync.append(input);
		inputWithSync.append("\x00\x00\xFF\xFF", 4);

		std::string output;
		constexpr size_t kChunkSize = 4096;
		output.resize(kChunkSize);

		s.next_in = reinterpret_cast<Bytef*>(inputWithSync.data());
		s.avail_in = static_cast<uInt>(inputWithSync.size());

		size_t totalProduced = 0;

		for (;;)
		{
			if (totalProduced >= output.size())
			{
				output.resize(output.size() * 2);
			}

			// zip bomb 防护：解压输出超过上限时中止
			if (maxOutputSize > 0 && totalProduced > maxOutputSize)
			{
				throw std::runtime_error("WsDeflateContext::decompress: output exceeds maxOutputSize");
			}

			s.next_out = reinterpret_cast<Bytef*>(output.data() + totalProduced);
			s.avail_out = static_cast<uInt>(output.size() - totalProduced);

			int ret = inflate(&s, Z_SYNC_FLUSH);
			totalProduced = output.size() - s.avail_out;

			if (ret == Z_STREAM_END || (ret == Z_OK && s.avail_in == 0 && s.avail_out > 0))
			{
				break;
			}
			if (ret == Z_OK && s.avail_out == 0)
			{
				continue; // 输出缓冲区满，扩容后继续
			}
			if (ret == Z_BUF_ERROR && s.avail_out == 0)
			{
				continue; // 需要更多输出空间
			}

			throw std::runtime_error("WsDeflateContext::decompress: inflate failed, ret=" + std::to_string(ret));
		}

		output.resize(totalProduced);
		return output;
	}

} // namespace hical
