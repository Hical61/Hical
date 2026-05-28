/**
 * @file LogMiddleware.cpp
 * @brief 访问日志中间件实现
 */

#include "LogMiddleware.h"

#include "HttpRequest.h"
#include "HttpTypes.h"
#include "Log.h"
#include "LogChannel.h"
#include "LogRecord.h"

#include <chrono>
#include <random>
#include <stdexcept>

namespace hical
{

	namespace
	{
		/// trace-id 格式校验：仅接受 1-128 个 hex/alphanum/'-' 字符
		bool isValidTraceId(std::string_view id)
		{
			if (id.empty() || id.size() > 128)
			{
				return false;
			}
			for (unsigned char c : id)
			{
				bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-';
				if (!ok)
				{
					return false;
				}
			}
			return true;
		}
	} // namespace

	std::string generateTraceId()
	{
		// thread_local PRNG，trace-id 不需要密码学安全，够唯一就行
		thread_local std::mt19937_64 sRng(std::random_device {}());

		uint64_t hi = sRng();
		uint64_t lo = sRng();

		static constexpr char kHex[] = "0123456789abcdef";
		std::string result(32, '\0');

		// 高 64 位 → result[0..15]
		for (int i = 15; i >= 0; --i)
		{
			result[15 - i] = kHex[(hi >> (i * 4)) & 0xf];
		}
		// 低 64 位 → result[16..31]
		for (int i = 15; i >= 0; --i)
		{
			result[31 - i] = kHex[(lo >> (i * 4)) & 0xf];
		}
		return result;
	}

	std::string getTraceId(const HttpRequest& req)
	{
		auto val = req.getAttribute<std::string>(hTraceIdKey);
		return val.value_or("");
	}

	MiddlewareHandler makeLogMiddleware(LogMiddlewareOptions opts)
	{
		return [opts = std::move(opts)](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			// ---- before next() ----
			std::string traceId;
			if (opts.autoTraceId)
			{
				// 尝试从请求头提取，校验格式后采纳
				auto clientTraceId = req.header(opts.traceIdHeader);
				if (!clientTraceId.empty() && isValidTraceId(clientTraceId))
				{
					traceId = std::string(clientTraceId);
				}
				else
				{
					traceId = generateTraceId();
				}
				req.setAttribute(hTraceIdKey, traceId);
			}

			auto startTime = std::chrono::steady_clock::now();

			// ---- 调用后续中间件/路由 ----
			auto response = co_await next(req);

			// ---- after next() ----
			auto endTime = std::chrono::steady_clock::now();
			auto latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

			// 发射结构化访问日志到 access 通道
			if (auto ch = Logger::instance().channels().get(opts.accessLogChannel))
			{
				LogRecord record;
				record.level = LogLevel::hInfo;
				record.timestamp = std::chrono::system_clock::now();
				record.file = nullptr;
				record.line = 0;
				record.traceId = traceId;

				auto method = std::string(httpMethodToString(req.method()));
				auto path = std::string(req.path());
				auto status = static_cast<int>(response.statusCode());

				record.message = method + " " + path;
				record.fields["method"] = method;
				record.fields["path"] = path;
				record.fields["status"] = status;
				record.fields["latency_ms"] = latencyMs;

				if (!traceId.empty())
				{
					record.fields["trace_id"] = traceId;
				}

				ch->emit(record);
			}

			co_return response;
		};
	}

} // namespace hical
