/**
 * @file Error.cpp
 * @brief 统一错误码映射实现
 */

#include "Error.h"
#include <boost/asio/error.hpp>
#include <cerrno>

#ifdef _WIN32
	#include <winerror.h>
#endif

namespace hical
{

	ErrorCode fromBoostError(const boost::system::error_code& ec)
	{
		if (!ec)
		{
			return ErrorCode::hNoError;
		}

		// Asio 错误码（boost::asio::error 命名空间）
		if (ec == boost::asio::error::eof)
		{
			return ErrorCode::hEof;
		}
		if (ec == boost::asio::error::connection_reset)
		{
			return ErrorCode::hConnectionReset;
		}
		if (ec == boost::asio::error::connection_refused)
		{
			return ErrorCode::hConnectionRefused;
		}
		if (ec == boost::asio::error::timed_out)
		{
			return ErrorCode::hTimedOut;
		}
		if (ec == boost::asio::error::operation_aborted)
		{
			return ErrorCode::hOperationAborted;
		}
		if (ec == boost::asio::error::connection_aborted)
		{
			return ErrorCode::hConnectionAborted;
		}
		if (ec == boost::asio::error::network_unreachable)
		{
			return ErrorCode::hNetworkUnreachable;
		}
		if (ec == boost::asio::error::host_unreachable)
		{
			return ErrorCode::hHostUnreachable;
		}
		if (ec == boost::asio::error::address_in_use)
		{
			return ErrorCode::hAddressInUse;
		}
		if (ec == boost::asio::error::already_started)
		{
			return ErrorCode::hOperationInProgress;
		}
		if (ec == boost::asio::error::broken_pipe)
		{
			return ErrorCode::hBrokenPipe;
		}
		if (ec == boost::asio::error::access_denied)
		{
			return ErrorCode::hPermissionDenied;
		}
		if (ec == boost::asio::error::no_descriptors)
		{
			return ErrorCode::hTooManyOpenFiles;
		}
		if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again)
		{
			return ErrorCode::hWouldBlock;
		}
		if (ec == boost::asio::error::in_progress)
		{
			return ErrorCode::hConnectionInProgress;
		}

		// 系统错误码补充映射
		auto value = ec.value();
		auto& category = ec.category();

		if (category == boost::system::system_category() || category == boost::system::generic_category())
		{
#ifdef _WIN32
			// Windows 特有错误码
			switch (value)
			{
				case WSAECONNRESET:
					return ErrorCode::hConnectionReset;
				case WSAECONNREFUSED:
					return ErrorCode::hConnectionRefused;
				case WSAETIMEDOUT:
					return ErrorCode::hTimedOut;
				case WSAECONNABORTED:
					return ErrorCode::hConnectionAborted;
				case WSAENETUNREACH:
					return ErrorCode::hNetworkUnreachable;
				case WSAEHOSTUNREACH:
					return ErrorCode::hHostUnreachable;
				case WSAEADDRINUSE:
					return ErrorCode::hAddressInUse;
				case WSAEADDRNOTAVAIL:
					return ErrorCode::hAddressNotAvailable;
				case WSAEACCES:
					return ErrorCode::hPermissionDenied;
				case WSAEWOULDBLOCK:
					return ErrorCode::hWouldBlock;
				case WSAEINPROGRESS:
					return ErrorCode::hConnectionInProgress;
				default:
					break;
			}
#else
			// POSIX 错误码
			switch (value)
			{
				case ECONNRESET:
					return ErrorCode::hConnectionReset;
				case ECONNREFUSED:
					return ErrorCode::hConnectionRefused;
				case ETIMEDOUT:
					return ErrorCode::hTimedOut;
				case ECONNABORTED:
					return ErrorCode::hConnectionAborted;
				case ENETUNREACH:
					return ErrorCode::hNetworkUnreachable;
				case EHOSTUNREACH:
					return ErrorCode::hHostUnreachable;
				case EADDRINUSE:
					return ErrorCode::hAddressInUse;
				case EADDRNOTAVAIL:
					return ErrorCode::hAddressNotAvailable;
				case EACCES:
					return ErrorCode::hPermissionDenied;
				case EMFILE:
				case ENFILE:
					return ErrorCode::hTooManyOpenFiles;
				case EAGAIN:
					return ErrorCode::hWouldBlock;
				case EPIPE:
					return ErrorCode::hBrokenPipe;
				case EINPROGRESS:
					return ErrorCode::hConnectionInProgress;
				default:
					break;
			}
#endif
		}

		return ErrorCode::hUnknown;
	}

	NetworkError toNetworkError(const boost::system::error_code& ec)
	{
		if (!ec)
		{
			return {ErrorCode::hNoError, ""};
		}

		return {fromBoostError(ec), ec.message()};
	}

	const char* errorCodeToString(ErrorCode code)
	{
		switch (code)
		{
			case ErrorCode::hNoError:
				return "No error";
			case ErrorCode::hEof:
				return "End of file";
			case ErrorCode::hConnectionReset:
				return "Connection reset by peer";
			case ErrorCode::hConnectionRefused:
				return "Connection refused";
			case ErrorCode::hTimedOut:
				return "Connection timed out";
			case ErrorCode::hConnectionInProgress:
				return "Connection in progress";
			case ErrorCode::hConnectionAborted:
				return "Connection aborted";
			case ErrorCode::hAddressInUse:
				return "Address already in use";
			case ErrorCode::hAddressNotAvailable:
				return "Address not available";
			case ErrorCode::hNetworkUnreachable:
				return "Network unreachable";
			case ErrorCode::hHostUnreachable:
				return "Host unreachable";
			case ErrorCode::hOperationAborted:
				return "Operation aborted";
			case ErrorCode::hOperationInProgress:
				return "Operation in progress";
			case ErrorCode::hBrokenPipe:
				return "Broken pipe";
			case ErrorCode::hPermissionDenied:
				return "Permission denied";
			case ErrorCode::hTooManyOpenFiles:
				return "Too many open files";
			case ErrorCode::hWouldBlock:
				return "Resource temporarily unavailable";
			case ErrorCode::hSslHandshakeError:
				return "SSL handshake error";
			case ErrorCode::hSslInvalidCertificate:
				return "SSL invalid certificate";
			case ErrorCode::hSslProtocolError:
				return "SSL protocol error";
			case ErrorCode::hUnknown:
				return "Unknown error";
		}
		return "Unknown error";
	}

	ErrorCode fromStdError(const std::error_code& ec)
	{
		if (!ec)
		{
			return ErrorCode::hNoError;
		}

		// 通过 std::errc 枚举映射平台无关的 POSIX 错误码
		if (ec == std::errc::connection_reset)
		{
			return ErrorCode::hConnectionReset;
		}
		if (ec == std::errc::connection_refused)
		{
			return ErrorCode::hConnectionRefused;
		}
		if (ec == std::errc::timed_out)
		{
			return ErrorCode::hTimedOut;
		}
		if (ec == std::errc::operation_canceled)
		{
			return ErrorCode::hOperationAborted;
		}
		if (ec == std::errc::connection_aborted)
		{
			return ErrorCode::hConnectionAborted;
		}
		if (ec == std::errc::network_unreachable)
		{
			return ErrorCode::hNetworkUnreachable;
		}
		if (ec == std::errc::host_unreachable)
		{
			return ErrorCode::hHostUnreachable;
		}
		if (ec == std::errc::address_in_use)
		{
			return ErrorCode::hAddressInUse;
		}
		if (ec == std::errc::address_not_available)
		{
			return ErrorCode::hAddressNotAvailable;
		}
		if (ec == std::errc::already_connected)
		{
			return ErrorCode::hOperationInProgress;
		}
		if (ec == std::errc::broken_pipe)
		{
			return ErrorCode::hBrokenPipe;
		}
		if (ec == std::errc::permission_denied)
		{
			return ErrorCode::hPermissionDenied;
		}
		if (ec == std::errc::too_many_files_open || ec == std::errc::too_many_files_open_in_system)
		{
			return ErrorCode::hTooManyOpenFiles;
		}
		if (ec == std::errc::operation_would_block || ec == std::errc::resource_unavailable_try_again)
		{
			return ErrorCode::hWouldBlock;
		}
		if (ec == std::errc::operation_in_progress)
		{
			return ErrorCode::hConnectionInProgress;
		}

		// EOF 没有对应的 std::errc，通过原始值判断
		auto value = ec.value();
		auto& category = ec.category();

		if (category == std::generic_category() || category == std::system_category())
		{
#ifdef _WIN32
			switch (value)
			{
				case WSAECONNRESET:
					return ErrorCode::hConnectionReset;
				case WSAECONNREFUSED:
					return ErrorCode::hConnectionRefused;
				case WSAETIMEDOUT:
					return ErrorCode::hTimedOut;
				case WSAECONNABORTED:
					return ErrorCode::hConnectionAborted;
				case WSAENETUNREACH:
					return ErrorCode::hNetworkUnreachable;
				case WSAEHOSTUNREACH:
					return ErrorCode::hHostUnreachable;
				case WSAEADDRINUSE:
					return ErrorCode::hAddressInUse;
				case WSAEADDRNOTAVAIL:
					return ErrorCode::hAddressNotAvailable;
				case WSAEACCES:
					return ErrorCode::hPermissionDenied;
				case WSAEWOULDBLOCK:
					return ErrorCode::hWouldBlock;
				case WSAEINPROGRESS:
					return ErrorCode::hConnectionInProgress;
				default:
					break;
			}
#else
			switch (value)
			{
				case ECONNRESET:
					return ErrorCode::hConnectionReset;
				case ECONNREFUSED:
					return ErrorCode::hConnectionRefused;
				case ETIMEDOUT:
					return ErrorCode::hTimedOut;
				case ECONNABORTED:
					return ErrorCode::hConnectionAborted;
				case ENETUNREACH:
					return ErrorCode::hNetworkUnreachable;
				case EHOSTUNREACH:
					return ErrorCode::hHostUnreachable;
				case EADDRINUSE:
					return ErrorCode::hAddressInUse;
				case EADDRNOTAVAIL:
					return ErrorCode::hAddressNotAvailable;
				case EACCES:
					return ErrorCode::hPermissionDenied;
				case EMFILE:
				case ENFILE:
					return ErrorCode::hTooManyOpenFiles;
				case EAGAIN:
					return ErrorCode::hWouldBlock;
				case EPIPE:
					return ErrorCode::hBrokenPipe;
				case EINPROGRESS:
					return ErrorCode::hConnectionInProgress;
				default:
					break;
			}
#endif
		}

		return ErrorCode::hUnknown;
	}

	NetworkError toNetworkError(const std::error_code& ec)
	{
		if (!ec)
		{
			return {ErrorCode::hNoError, ""};
		}

		return {fromStdError(ec), ec.message()};
	}

} // namespace hical
