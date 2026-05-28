/**
 * @file Error.h
 * @brief 统一错误码映射与 NetworkError
 */

#pragma once

#include <boost/system/error_code.hpp>
#include <cstdint>
#include <string>
#include <system_error>

namespace hical
{

	/**
	 * @brief 通用网络错误码枚举
	 * 将 Boost.Asio / 系统错误码统一映射为框架内部错误码，
	 * 使上层业务逻辑不直接依赖 Asio 错误码体系。
	 */
	enum class ErrorCode : uint32_t
	{
		hNoError = 0,

		// ============ 连接错误 ============

		/** 对端正常关闭连接（收到 EOF） */
		hEof,
		/** 连接被对端重置 */
		hConnectionReset,
		/** 连接被拒绝 */
		hConnectionRefused,
		/** 连接超时 */
		hTimedOut,
		/** 连接正在进行中 */
		hConnectionInProgress,
		/** 连接被中止 */
		hConnectionAborted,

		// ============ 地址错误 ============

		/** 地址已在使用中 */
		hAddressInUse,
		/** 地址不可用 */
		hAddressNotAvailable,
		/** 网络不可达 */
		hNetworkUnreachable,
		/** 主机不可达 */
		hHostUnreachable,

		// ============ 操作错误 ============

		/** 操作被取消 */
		hOperationAborted,
		/** 操作正在进行中 */
		hOperationInProgress,
		/** 管道破裂（向已关闭的连接写入） */
		hBrokenPipe,
		/** 权限不足 */
		hPermissionDenied,
		/** 文件描述符不足 */
		hTooManyOpenFiles,
		/** 资源暂时不可用（EAGAIN / EWOULDBLOCK） */
		hWouldBlock,

		// ============ SSL/TLS 错误 ============

		/** SSL 握手失败 */
		hSslHandshakeError,
		/** SSL 证书无效 */
		hSslInvalidCertificate,
		/** SSL 协议错误 */
		hSslProtocolError,

		// ============ 未知错误 ============

		/** 未知错误 */
		hUnknown = 0xFFFF
	};

	/**
	 * @brief 网络错误结构体
	 * 封装错误码和错误描述信息。
	 */
	struct NetworkError
	{
		ErrorCode code {ErrorCode::hNoError};
		std::string message;

		/**
		 * @brief 是否有错误
		 * @return true 如果有错误
		 */
		explicit operator bool() const
		{
			return code != ErrorCode::hNoError;
		}

		/**
		 * @brief 是否无错误
		 * @return true 如果无错误
		 */
		bool ok() const
		{
			return code == ErrorCode::hNoError;
		}

		/**
		 * @brief 是否为 EOF（对端正常关闭）
		 * @return true 如果是 EOF
		 */
		bool isEof() const
		{
			return code == ErrorCode::hEof;
		}

		/**
		 * @brief 是否为操作取消
		 * @return true 如果是取消
		 */
		bool isCancelled() const
		{
			return code == ErrorCode::hOperationAborted;
		}
	};

	/**
	 * @brief 将 boost::system::error_code 转换为 ErrorCode
	 * @param ec Boost 错误码
	 * @return 框架内部错误码
	 */
	ErrorCode fromBoostError(const boost::system::error_code& ec);

	/**
	 * @brief 将 std::error_code 转换为 ErrorCode（为未来迁移预留）
	 * @param ec 标准库错误码
	 * @return 框架内部错误码
	 */
	ErrorCode fromStdError(const std::error_code& ec);

	/**
	 * @brief 将 boost::system::error_code 转换为 NetworkError
	 * @param ec Boost 错误码
	 * @return 网络错误结构体
	 */
	NetworkError toNetworkError(const boost::system::error_code& ec);

	/**
	 * @brief 将 std::error_code 转换为 NetworkError（为未来迁移预留）
	 * @param ec 标准库错误码
	 * @return 网络错误结构体
	 */
	NetworkError toNetworkError(const std::error_code& ec);

	/**
	 * @brief 获取错误码的字符串描述
	 * @param code 错误码
	 * @return 错误描述
	 */
	const char* errorCodeToString(ErrorCode code);

} // namespace hical
