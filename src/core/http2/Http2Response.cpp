/**
 * @file Http2Response.cpp
 * @brief HTTP/2 响应头映射、正文长度校验和输出提交。
 */

#include "Http2Response.h"

#ifdef HICAL_HAS_HTTP2

	#include "Http2Headers.h"
	#include <array>
	#include <limits>
	#include <new>
	#include <optional>
	#include <string>
	#include <utility>

namespace hical::detail
{
	namespace
	{
		constexpr unsigned kMinimumHttpStatus = 100;
		constexpr unsigned kMinimumFinalStatus = 200;
		constexpr unsigned kMaximumHttpStatus = 599;

		NetworkError responseError(int32_t streamId, std::string message, bool resource = false)
		{
			return {resource ? ErrorCode::hResourceLimitExceeded : ErrorCode::hInvalidConfiguration,
					std::move(message),
					Http2ErrorInfo {static_cast<uint32_t>(Http2Code::hInternalError), streamId}};
		}

		NetworkError appendHeader(Http2Headers& headers,
								  Http2Codec::Header field,
								  int32_t streamId,
								  const Http2Options& options)
		{
			const auto remaining = options.maxHeaderBytes - headers.decodedBytes();
			if (headers.decodedFieldCount() >= options.maxHeaderFields
				|| Http2Options::kHeaderFieldOverheadBytes > remaining
				|| field.name.size() > remaining - Http2Options::kHeaderFieldOverheadBytes
				|| field.value.size() > remaining - Http2Options::kHeaderFieldOverheadBytes - field.name.size())
			{
				return responseError(streamId, "HTTP/2 响应头超过字段数量或 header-list 字节预算", true);
			}
			// 在分配小写名称前已检查整个字段预算；值由 Http2Headers 再严格校验并复制。
			std::string name(field.name);
			for (char& byte : name)
			{
				if (byte >= 'A' && byte <= 'Z')
				{
					byte = static_cast<char>(byte + ('a' - 'A'));
				}
			}
			return headers.append(name, field.value);
		}

		NetworkError prepareHeaders(Http2Headers& headers,
									const NativeResponse& response,
									int32_t streamId,
									const Http2Options& options,
									size_t payloadBytes)
		{
			const auto status = static_cast<unsigned>(response.status);
			if (auto error = headers.append(":status", std::to_string(status)))
			{
				return error;
			}
			// 304 的长度是调用者提供的表示元数据；其他普通响应只发送本次最终 payload 的唯一长度。
			const bool keepLength = response.status == HttpStatusCode::hNotModified;
			for (const auto& [name, value] : response.headers)
			{
				if (response.hasChunkedBody() && HeaderMap::iequals(name, "transfer-encoding")
					&& HeaderMap::iequals(value, "chunked"))
				{
					// 只移除 ChunkedBody 自带的 H1 分块标记；普通响应和其他传输编码仍需严格校验。
					continue;
				}
				if (!keepLength && HeaderMap::iequals(name, "content-length"))
				{
					continue;
				}
				if (auto error = appendHeader(headers, {name, value}, streamId, options))
				{
					return error;
				}
			}
			if (response.status != HttpStatusCode::hNoContent && !keepLength)
			{
				if (auto error = headers.append("content-length", std::to_string(payloadBytes)))
				{
					return error;
				}
			}
			return headers.seal();
		}

		NetworkError payloadLength(const NativeResponse& response, int32_t streamId, size_t& length)
		{
			if (response.hasFileBody())
			{
				const auto& file = *response.fileBody;
				if (file.offset < 0 || file.length < 0
					|| file.length > std::numeric_limits<int64_t>::max() - file.offset
					|| static_cast<uint64_t>(file.length) > std::numeric_limits<size_t>::max())
				{
					return responseError(streamId, "HTTP/2 文件范围无效或长度溢出");
				}
				length = static_cast<size_t>(file.length);
				return {};
			}
			length = response.body.size();
			if (!response.hasChunkedBody())
			{
				return {};
			}
			length = 0;
			for (const auto& chunk : response.chunkedBody->chunks())
			{
				if (chunk.size() > std::numeric_limits<size_t>::max() - length)
				{
					return responseError(streamId, "HTTP/2 收集式 body 的总长度溢出", true);
				}
				length += chunk.size();
			}
			return {};
		}

		NetworkError prepareTrailers(Http2Headers& trailers,
									 const NativeResponse& response,
									 int32_t streamId,
									 const Http2Options& options)
		{
			for (const auto& [name, value] : response.trailers)
			{
				if (auto error = appendHeader(trailers, {name, value}, streamId, options))
				{
					return error;
				}
			}
			if (response.hasChunkedBody())
			{
				for (const auto& field : response.chunkedBody->trailers())
				{
					if (auto error = appendHeader(trailers, {field.name, field.value}, streamId, options))
					{
						return error;
					}
				}
			}
			return trailers.seal();
		}

		void cancelFailed(Http2Output::OpenResult& result)
		{
			if (result.producer)
			{
				result.producer->cancel();
				result.producer.reset();
			}
		}

		void queueBody(Http2Output::OpenResult& result,
					   NativeResponse& response,
					   std::span<const Http2Codec::Header> trailers)
		{
			if (result.error)
			{
				return;
			}
			// appendChunks 在 Output 内先计费再聚合；不借用输入跨异步，也不受源块数量影响。
			result.error = response.hasChunkedBody() ? result.producer->appendChunks(response.chunkedBody->chunks())
													 : result.producer->appendOwned(std::move(response.body));
			if (!result.error)
			{
				result.error = result.producer->finish(trailers);
			}
			if (result.error)
			{
				cancelFailed(result);
			}
			else
			{
				response.chunkedBody.reset();
			}
		}

		NetworkError normalizeResponse(int32_t streamId, HttpResponse& response, const Http2Options& options)
		{
			if (auto error = options.validationError(); !error.empty())
			{
				return responseError(streamId, std::string(error));
			}
			const auto status = static_cast<unsigned>(response.statusCode());
			if (streamId <= 0 || status < kMinimumHttpStatus || status > kMaximumHttpStatus)
			{
				return responseError(streamId, "HTTP/2 响应流 ID 或 HTTP 状态码无效");
			}
			if (status < kMinimumFinalStatus)
			{
				response = HttpResponse::serverError();
			}
			if (response.native().hasFileBody() && response.native().hasChunkedBody())
			{
				return responseError(streamId, "HTTP/2 文件体和收集式 body 不能同时存在");
			}
			if (response.native().hasChunkedBody() && !response.native().chunkedBody->finished())
			{
				return responseError(streamId, "HTTP/2 收集式 ChunkedBody 必须先调用 end，不能等待后续生产");
			}
			return {};
		}
	} // namespace

	Http2Response::Result Http2Response::submit(Http2Output& output,
												int32_t streamId,
												HttpMethod requestMethod,
												HttpResponse&& response,
												const Http2Options& options)
	{
		Result result;
		result.streamId = streamId;
		try
		{
			if (auto error = normalizeResponse(streamId, response, options))
			{
				result.error = std::move(error);
				return result;
			}
			Http2Headers headers(Http2Codec::HeaderKind::hResponse, streamId, options);
			size_t payloadBytes = 0;
			if (auto error = payloadLength(response.native(), streamId, payloadBytes))
			{
				result.error = std::move(error);
				return result;
			}
			if (auto error = prepareHeaders(headers, response.native(), streamId, options, payloadBytes))
			{
				result.error = std::move(error);
				return result;
			}
			std::optional<Http2Headers> trailers;
			if (!response.native().trailers.empty()
				|| (response.native().hasChunkedBody() && !response.native().chunkedBody->trailers().empty()))
			{
				trailers.emplace(Http2Codec::HeaderKind::hTrailers, streamId, options);
				if (auto error = prepareTrailers(*trailers, response.native(), streamId, options))
				{
					result.error = std::move(error);
					return result;
				}
			}
			const auto statusText = std::to_string(static_cast<unsigned>(response.statusCode()));
			std::array<Http2Codec::Header, Http2Options::kMaxHeaderFields> fields;
			fields[0] = {":status", statusText};
			size_t fieldCount = 1;
			for (auto field : headers.fields())
			{
				fields[fieldCount++] = field;
			}
			const bool bodyForbidden = requestMethod == HttpMethod::hHead
									   || response.statusCode() == HttpStatusCode::hNoContent
									   || response.statusCode() == HttpStatusCode::hNotModified;
			if (bodyForbidden || (payloadBytes == 0 && !trailers))
			{
				result = output.submitResponseHeaders(streamId, std::span(fields).first(fieldCount));
				if (!result.error)
				{
					response.native().chunkedBody.reset();
					response.native().fileBody.reset();
				}
				return result;
			}
			result = output.submitResponse(streamId, std::span(fields).first(fieldCount));
			if (!result.error && response.native().hasFileBody())
			{
				result.file =
					Http2FileBody::create(streamId,
										  result.producer,
										  std::move(*response.native().fileBody),
										  options,
										  trailers ? trailers->fields() : std::span<const Http2Codec::Header> {});
				if (!result.file)
				{
					result.error = responseError(streamId, "HTTP/2 文件任务超过连接或服务配额", true);
					cancelFailed(result);
				}
				else
				{
					response.native().fileBody.reset();
				}
				return result;
			}
			queueBody(result,
					  response.native(),
					  trailers ? trailers->fields() : std::span<const Http2Codec::Header> {});
			return result;
		}
		catch (const std::bad_alloc&)
		{
			cancelFailed(result);
			result.error = {ErrorCode::hResourceLimitExceeded,
							{},
							Http2ErrorInfo {static_cast<uint32_t>(Http2Code::hInternalError), streamId}};
			return result;
		}
	}
} // namespace hical::detail

#endif
