/**
 * @file WsFrame.h
 * @brief WebSocket RFC 6455 帧解析与构建
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace hical
{

	// WebSocket 操作码 (RFC 6455 §5.2)
	enum class WsOpcode : uint8_t
	{
		hContinuation = 0x0,
		hText = 0x1,
		hBinary = 0x2,
		// 0x3-0x7: 保留给未来的非控制帧
		hClose = 0x8,
		hPing = 0x9,
		hPong = 0xA
		// 0xB-0xF: 保留给未来的控制帧
	};

	// WebSocket 关闭状态码 (RFC 6455 §7.4.1)
	enum class WsCloseCode : uint16_t
	{
		hNormal = 1000,
		hGoingAway = 1001,
		hProtocolError = 1002,
		hUnsupportedData = 1003,
		// 1004: 保留
		hNoStatusReceived = 1005, // 不能在 close 帧中发送
		hAbnormalClosure = 1006,  // 不能在 close 帧中发送
		hInvalidPayload = 1007,
		hPolicyViolation = 1008,
		hMessageTooBig = 1009,
		hMandatoryExtension = 1010,
		hInternalError = 1011
	};

	// 解析后的帧头
	struct WsFrameHeader
	{
		bool fin = false;
		bool rsv1 = false;
		bool rsv2 = false;
		bool rsv3 = false;
		WsOpcode opcode = WsOpcode::hText;
		bool masked = false;
		uint64_t payloadLength = 0;
		uint8_t maskKey[4] = {};
		size_t headerSize = 0; // 帧头总字节数 (2~14)
	};

	/**
	 * @brief 解析 WebSocket 帧头（RFC 6455 §5.2）
	 * 增量解析友好：数据不足时返回 nullopt，调用方可继续等待更多数据再重试。
	 * 帧头布局：
	 *   Byte 0: FIN(1) + RSV1(1) + RSV2(1) + RSV3(1) + opcode(4)
	 *   Byte 1: MASK(1) + payload_len(7)
	 *   Extended payload length: 若 payload_len==126 则后跟 2 字节 uint16 (big-endian)
	 *                             若 payload_len==127 则后跟 8 字节 uint64 (big-endian)
	 *   Masking key: 若 MASK==1 则后跟 4 字节
	 * @param data  输入缓冲区指针
	 * @param size  缓冲区可用字节数
	 * @return 解析成功时返回填充好的 WsFrameHeader；数据不足时返回 nullopt
	 */
	inline std::optional<WsFrameHeader> parseWsFrameHeader(const uint8_t* data, size_t size)
	{
		// 至少需要 2 字节才能确定帧头的最小结构
		if (size < 2)
		{
			return std::nullopt;
		}

		WsFrameHeader hdr;

		const uint8_t byte0 = data[0];
		const uint8_t byte1 = data[1];

		hdr.fin = (byte0 & 0x80) != 0;
		hdr.rsv1 = (byte0 & 0x40) != 0;
		hdr.rsv2 = (byte0 & 0x20) != 0;
		hdr.rsv3 = (byte0 & 0x10) != 0;
		hdr.opcode = static_cast<WsOpcode>(byte0 & 0x0F);
		hdr.masked = (byte1 & 0x80) != 0;

		const uint8_t rawLen = byte1 & 0x7F;

		size_t extLenBytes = 0;
		if (rawLen == 126)
		{
			extLenBytes = 2;
		}
		else if (rawLen == 127)
		{
			extLenBytes = 8;
		}

		const size_t maskBytes = hdr.masked ? 4 : 0;
		hdr.headerSize = 2 + extLenBytes + maskBytes;

		if (size < hdr.headerSize)
		{
			return std::nullopt;
		}

		// 解析 extended payload length (big-endian via memcpy，不依赖平台字节序)
		if (extLenBytes == 0)
		{
			hdr.payloadLength = rawLen;
		}
		else if (extLenBytes == 2)
		{
			hdr.payloadLength = (static_cast<uint64_t>(data[2]) << 8) | static_cast<uint64_t>(data[3]);
		}
		else // extLenBytes == 8
		{
			uint8_t tmp[8];
			std::memcpy(tmp, data + 2, 8);
			hdr.payloadLength = (static_cast<uint64_t>(tmp[0]) << 56) | (static_cast<uint64_t>(tmp[1]) << 48)
								| (static_cast<uint64_t>(tmp[2]) << 40) | (static_cast<uint64_t>(tmp[3]) << 32)
								| (static_cast<uint64_t>(tmp[4]) << 24) | (static_cast<uint64_t>(tmp[5]) << 16)
								| (static_cast<uint64_t>(tmp[6]) << 8) | static_cast<uint64_t>(tmp[7]);
		}

		if (hdr.masked)
		{
			std::memcpy(hdr.maskKey, data + 2 + extLenBytes, 4);
		}

		return hdr;
	}

	/**
	 * @brief 原地解除 WebSocket payload 的 XOR masking（RFC 6455 §5.3）
	 * 快速路径：按 4 字节对齐批量 XOR uint32_t，尾部逐字节处理。
	 * 同一函数既用于 mask 也用于 unmask（XOR 互逆）。
	 * @param data    payload 数据指针（原地修改）
	 * @param size    payload 字节数
	 * @param maskKey 4 字节 mask key
	 */
	inline void unmaskPayload(uint8_t* data, size_t size, const uint8_t maskKey[4])
	{
		// 将 4 字节 mask key 拼成 uint32_t，用于批量 XOR
		uint32_t mask32 = 0;
		std::memcpy(&mask32, maskKey, 4);

		size_t i = 0;

		// 4 字节对齐快速路径
		for (; i + 4 <= size; i += 4)
		{
			uint32_t chunk = 0;
			std::memcpy(&chunk, data + i, 4);
			chunk ^= mask32;
			std::memcpy(data + i, &chunk, 4);
		}

		// 处理尾部不足 4 字节的剩余部分
		for (; i < size; ++i)
		{
			data[i] ^= maskKey[i % 4];
		}
	}

	/**
	 * @brief 构造服务端发送的 WebSocket 帧（不带 mask）
	 * 服务端向客户端发送时不得 mask（RFC 6455 §5.1）。
	 * rsv1=true 表示 permessage-deflate 压缩帧。
	 * @param opcode   帧操作码
	 * @param payload  帧载荷数据
	 * @param fin      是否为消息的最后一帧，默认 true
	 * @param rsv1     RSV1 位，压缩帧时置 true，默认 false
	 * @return 完整帧字节串（帧头 + 载荷）
	 */
	inline std::string buildWsFrame(WsOpcode opcode, std::string_view payload, bool fin = true, bool rsv1 = false)
	{
		const uint64_t payloadLen = payload.size();

		// 预计算帧头大小
		size_t headerSize = 2;
		if (payloadLen > 65535)
		{
			headerSize += 8;
		}
		else if (payloadLen > 125)
		{
			headerSize += 2;
		}

		std::string frame;
		frame.resize(headerSize + payloadLen);
		auto* out = reinterpret_cast<uint8_t*>(frame.data());

		// Byte 0: FIN + RSV1 + opcode
		out[0] = static_cast<uint8_t>((fin ? 0x80U : 0x00U) | (rsv1 ? 0x40U : 0x00U)
									  | (static_cast<uint8_t>(opcode) & 0x0FU));

		// Byte 1 + extended payload length (big-endian, MASK=0)
		if (payloadLen <= 125)
		{
			out[1] = static_cast<uint8_t>(payloadLen);
		}
		else if (payloadLen <= 65535)
		{
			out[1] = 126;
			const auto len16 = static_cast<uint16_t>(payloadLen);
			out[2] = static_cast<uint8_t>(len16 >> 8);
			out[3] = static_cast<uint8_t>(len16 & 0xFF);
		}
		else
		{
			out[1] = 127;
			out[2] = static_cast<uint8_t>(payloadLen >> 56);
			out[3] = static_cast<uint8_t>((payloadLen >> 48) & 0xFF);
			out[4] = static_cast<uint8_t>((payloadLen >> 40) & 0xFF);
			out[5] = static_cast<uint8_t>((payloadLen >> 32) & 0xFF);
			out[6] = static_cast<uint8_t>((payloadLen >> 24) & 0xFF);
			out[7] = static_cast<uint8_t>((payloadLen >> 16) & 0xFF);
			out[8] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
			out[9] = static_cast<uint8_t>(payloadLen & 0xFF);
		}

		if (payloadLen > 0)
		{
			std::memcpy(out + headerSize, payload.data(), payloadLen);
		}

		return frame;
	}

	/**
	 * @brief 构造 WebSocket close 帧的载荷（RFC 6455 §5.5.1）
	 * 格式：2 字节状态码（big-endian）+ 可选 UTF-8 reason 字符串。
	 * reason 最长 123 字节（close 帧 payload 上限 125 字节，减去状态码 2 字节）。
	 * @param code    关闭状态码
	 * @param reason  可选关闭原因（超长部分截断）
	 * @return close 帧的载荷字节串
	 */
	inline std::string buildClosePayload(WsCloseCode code, std::string_view reason = {})
	{
		constexpr size_t kMaxReasonLen = 123;
		if (reason.size() > kMaxReasonLen)
		{
			reason = reason.substr(0, kMaxReasonLen);
		}

		std::string payload;
		payload.resize(2 + reason.size());

		const auto codeVal = static_cast<uint16_t>(code);
		payload[0] = static_cast<char>(codeVal >> 8);
		payload[1] = static_cast<char>(codeVal & 0xFF);

		if (!reason.empty())
		{
			std::memcpy(payload.data() + 2, reason.data(), reason.size());
		}

		return payload;
	}

	/**
	 * @brief 构造带 mask 的 WebSocket 帧（客户端用，RFC 6455 §5.1）
	 * 仅供测试客户端使用。客户端发送帧必须 mask（RFC 6455 §5.3）。
	 * maskKey 由调用方提供（通常来自安全随机源）。
	 * @param opcode   帧操作码
	 * @param payload  帧载荷（将被 XOR mask 后写入帧）
	 * @param maskKey  4 字节 mask key
	 * @param fin      是否为消息的最后一帧，默认 true
	 * @param rsv1     RSV1 位，压缩帧时置 true，默认 false
	 * @return 完整帧字节串（帧头 + maskKey + 已 mask 的载荷）
	 */
	inline std::string buildMaskedWsFrame(WsOpcode opcode,
										  std::string_view payload,
										  const uint8_t maskKey[4],
										  bool fin = true,
										  bool rsv1 = false)
	{
		const uint64_t payloadLen = payload.size();

		size_t headerSize = 2 + 4; // 2 基础 + 4 maskKey
		if (payloadLen > 65535)
		{
			headerSize += 8;
		}
		else if (payloadLen > 125)
		{
			headerSize += 2;
		}

		std::string frame;
		frame.resize(headerSize + payloadLen);
		auto* out = reinterpret_cast<uint8_t*>(frame.data());

		// Byte 0: FIN + RSV1 + opcode
		out[0] = static_cast<uint8_t>((fin ? 0x80U : 0x00U) | (rsv1 ? 0x40U : 0x00U)
									  | (static_cast<uint8_t>(opcode) & 0x0FU));

		// Byte 1 + extended payload length (MASK=1)
		size_t maskKeyOffset = 2;
		if (payloadLen <= 125)
		{
			out[1] = static_cast<uint8_t>(0x80U | payloadLen);
		}
		else if (payloadLen <= 65535)
		{
			out[1] = static_cast<uint8_t>(0x80U | 126U);
			const auto len16 = static_cast<uint16_t>(payloadLen);
			out[2] = static_cast<uint8_t>(len16 >> 8);
			out[3] = static_cast<uint8_t>(len16 & 0xFF);
			maskKeyOffset = 4;
		}
		else
		{
			out[1] = static_cast<uint8_t>(0x80U | 127U);
			out[2] = static_cast<uint8_t>(payloadLen >> 56);
			out[3] = static_cast<uint8_t>((payloadLen >> 48) & 0xFF);
			out[4] = static_cast<uint8_t>((payloadLen >> 40) & 0xFF);
			out[5] = static_cast<uint8_t>((payloadLen >> 32) & 0xFF);
			out[6] = static_cast<uint8_t>((payloadLen >> 24) & 0xFF);
			out[7] = static_cast<uint8_t>((payloadLen >> 16) & 0xFF);
			out[8] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
			out[9] = static_cast<uint8_t>(payloadLen & 0xFF);
			maskKeyOffset = 10;
		}

		// 写入 mask key
		std::memcpy(out + maskKeyOffset, maskKey, 4);

		// 复制 payload 并原地 XOR mask
		if (payloadLen > 0)
		{
			uint8_t* payloadOut = out + headerSize;
			std::memcpy(payloadOut, payload.data(), payloadLen);
			unmaskPayload(payloadOut, payloadLen, maskKey);
		}

		return frame;
	}

} // namespace hical
