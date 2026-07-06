/**
 * @file HttpResponse.h
 * @brief HTTP 响应构建与序列化
 */

#pragma once

#include "ChunkedBody.h"
#include "Cookie.h"
#include "FixedBuffer.h"
#include "HeaderMap.h"
#include "HttpTypes.h"
#include <boost/json.hpp>
#include <charconv>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace hical
{

	/**
	 * @brief 文件体描述（Range 请求或大文件直传，延迟到 dispatch 层异步发送）
	 */
	struct FileBody
	{
		std::filesystem::path path;
		int64_t offset = 0; // 起始偏移
		int64_t length = 0; // 发送字节数
	};

	/**
	 * @brief HTTP 响应的原生内部表示
	 * 自研 HTTP 响应内部表示，零外部依赖。
	 * @warning serialize() / serializeHeadTo() 不做 CRLF 注入检查。
	 * 所有头部写入应通过 HttpResponse 的 setHeader() 方法（内含 CRLF 防护），
	 * 或确保数据源可信（如 picohttpparser 解析的请求头、框架内部设置的固定头部）。
	 * 直接操作 headers 的代码需自行保证不含 CR/LF。
	 */
	struct NativeResponse
	{
		HttpStatusCode status = HttpStatusCode::hOk;
		int httpVersionMinor = 1;
		HeaderMap headers;
		std::string body;
		std::optional<FileBody> fileBody; // Range 请求或大文件直传
		bool keepAlive = true;

		/// Chunked Transfer-Encoding 响应体（可选）
		std::optional<ChunkedBody> chunkedBody;

		/**
		 * @brief 是否包含文件体（dispatch 层据此选择文件发送路径）
		 */
		bool hasFileBody() const
		{
			return fileBody.has_value();
		}

		/**
		 * @brief 是否使用 Chunked Transfer-Encoding
		 */
		[[nodiscard]] bool hasChunkedBody() const
		{
			return chunkedBody.has_value();
		}

		/**
		 * @brief 标记 payload 需要重新计算（body/状态码/正文类型变化时调用）
		 * 下次 preparePayload() 会重新执行完整逻辑。
		 */
		void invalidatePayload() noexcept
		{
			payloadPrepared_ = false;
		}

		/**
		 * @brief 设置 Content-Length / Transfer-Encoding 头部
		 * 空 body 且状态码非 204/304 时设置 Content-Length: 0
		 * 幂等：频繁调用不会重复计算。
		 */
		void preparePayload()
		{
			if (payloadPrepared_)
			{
				return;
			}
			payloadPrepared_ = true;
			// Chunked body 路径：不设 Content-Length，改设 Transfer-Encoding
			if (chunkedBody.has_value())
			{
				headers.set("Transfer-Encoding", "chunked");
				// 删除可能残留的 Content-Length
				headers.erase("Content-Length");
				return;
			}

			// 文件体路径：Content-Length = fileBody->length
			if (fileBody.has_value())
			{
				char buf[20];
				auto [ptr, ec] = std::to_chars(buf, buf + 20, static_cast<size_t>(fileBody->length));
				headers.set("Content-Length", std::string_view(buf, static_cast<size_t>(ptr - buf)));
				return;
			}

			if (body.empty())
			{
				auto code = static_cast<unsigned>(status);
				if (code != 204 && code != 304)
				{
					headers.set("Content-Length", "0");
				}
			}
			else
			{
				char buf[20];
				auto [ptr, ec] = std::to_chars(buf, buf + 20, body.size());
				headers.set("Content-Length", std::string_view(buf, static_cast<size_t>(ptr - buf)));
			}
		}

		/**
		 * @brief 序列化为完整的 HTTP 响应字节流
		 * @return 包含状态行 + 头部 + 空行 + body 的字符串
		 */
		std::string serialize() const
		{
			std::string result;
			result.reserve(256 + body.size());

			// 状态行
			result.append("HTTP/1.");
			result += static_cast<char>('0' + httpVersionMinor);
			result += ' ';
			appendStatusCode(result);
			result.append("\r\n");

			// 头部
			for (const auto& [name, value] : headers)
			{
				result.append(name);
				result.append(": ");
				result.append(value);
				result.append("\r\n");
			}

			// 空行 + body
			result.append("\r\n");
			result.append(body);
			return result;
		}

		/**
		 * @brief 将状态行 + 头部序列化到栈缓冲区（零堆分配，用于 scatter-gather I/O）
		 * @param buf 输出缓冲区（512 字节栈缓冲，覆盖典型 3-5 个 header 的响应头）
		 */
		void serializeHeadTo(FixedBuffer<512>& buf) const
		{
			serializeStatusAndHeaders(buf);
			buf << "\r\n";
		}

		/**
		 * @brief 带预构建前缀的头部序列化，省掉通用头部（Server/Connection/Date）的逐字段格式化
		 * @param buf 输出缓冲区
		 * @param prefix 预拼好的通用头部 wire bytes
		 * @param prefixLen 前缀字节数
		 */
		void serializeHeadTo(FixedBuffer<512>& buf, const char* prefix, size_t prefixLen) const
		{
			serializeStatusAndHeaders(buf);
			buf.append(prefix, prefixLen);
			buf << "\r\n";
		}

	private:
		/// 状态行 + 用户头部的公共序列化逻辑（不含尾部空行）
		void serializeStatusAndHeaders(FixedBuffer<512>& buf) const
		{
			// 200 OK 快速路径
			if (status == HttpStatusCode::hOk && httpVersionMinor == 1)
			{
				buf.append("HTTP/1.1 200 OK\r\n", 17);
			}
			else
			{
				buf << "HTTP/1.";
				buf << static_cast<char>('0' + httpVersionMinor);
				buf << ' ';

				char codeBuf[4];
				auto [ptr, ec] = std::to_chars(codeBuf, codeBuf + 4, static_cast<unsigned>(status));
				buf.append(codeBuf, static_cast<size_t>(ptr - codeBuf));
				buf << ' ';
				buf << httpStatusCodeToString(status);
				buf << "\r\n";
			}

			// 用户 handler 设置的头部（Content-Type、Content-Length 等）
			for (const auto& [name, value] : headers)
			{
				buf << name;
				buf << ": ";
				buf << value;
				buf << "\r\n";
			}
		}

		void appendStatusCode(std::string& out) const
		{
			char buf[4];
			auto [ptr, ec] = std::to_chars(buf, buf + 4, static_cast<unsigned>(status));
			out.append(buf, static_cast<size_t>(ptr - buf));
			out += ' ';
			out.append(httpStatusCodeToString(status));
		}

		bool payloadPrepared_ = false;
	};

	/**
	 * @brief HTTP 响应封装
	 * 对自研 NativeResponse 的 hical 风格封装。
	 * 提供简洁的接口设置状态码、头部和消息体。
	 */
	class HttpResponse
	{
	public:
		HttpResponse();

		/**
		 * @brief 获取状态码
		 * @return HTTP 状态码
		 */
		HttpStatusCode statusCode() const;

		/**
		 * @brief 设置状态码
		 * @param code HTTP 状态码
		 */
		void setStatus(HttpStatusCode code);

		/**
		 * @brief 获取指定头部字段
		 * @param name 字段名
		 * @return 字段值
		 */
		std::string_view header(std::string_view name) const;

		/**
		 * @brief 设置头部字段
		 * @param name 字段名
		 * @param value 字段值
		 * @note string_view 可同时接受字面量、char*、std::string 作为实参，
		 *       避免 const std::string& 重载中字面量传参的临时 string 构造。
		 */
		void setHeader(std::string_view name, std::string_view value);

		/**
		 * @brief 获取消息体
		 * @return 消息体字符串
		 */
		const std::string& body() const;

		/**
		 * @brief 设置消息体（不动 Content-Type 头）
		 * @param body 消息体
		 */
		void setBody(const std::string& body);
		void setBody(std::string&& body);

		/**
		 * @brief 设置消息体并指定 Content-Type
		 * @param body 消息体
		 * @param contentType Content-Type 值（如 "application/json"）
		 */
		void setBody(const std::string& body, const std::string& contentType);
		void setBody(std::string&& body, const std::string& contentType);

		/**
		 * @brief 设置 JSON 消息体
		 * @param json JSON 值
		 */
		void setJsonBody(const boost::json::value& json);

		/**
		 * @brief 添加 Set-Cookie 响应头
		 * @param name Cookie 名称
		 * @param value Cookie 值
		 * @param options Cookie 设置选项（有效期、路径、HttpOnly 等）
		 * 可多次调用以设置多个 Cookie，每次追加一个 Set-Cookie 头。
		 */
		void setCookie(const std::string& name, const std::string& value, const CookieOptions& options = {});

		/**
		 * @brief 获取底层原生响应的引用
		 * @return NativeResponse 引用
		 */
		NativeResponse& native();
		const NativeResponse& native() const;

		// ============ 快捷工厂方法 ============

		/**
		 * @brief 返回 200 OK，默认 Content-Type 带 utf-8 编码
		 * @param body 响应体
		 * @return HttpResponse
		 */
		static HttpResponse ok(const std::string& body = "");

		/**
		 * @brief 返回 200 OK，自定 Content-Type（比如 text/html）
		 * @param body 响应体
		 * @param contentType Content-Type 值
		 * @return HttpResponse
		 */
		static HttpResponse ok(const std::string& body, const std::string& contentType);

		/**
		 * @brief 创建 JSON 200 OK 响应
		 * @param json JSON 值
		 * @return HttpResponse
		 */
		static HttpResponse json(const boost::json::value& json);

		/**
		 * @brief 创建 404 Not Found 响应
		 * @return HttpResponse
		 */
		static HttpResponse notFound();

		/**
		 * @brief 创建 400 Bad Request 响应
		 * @param message 错误信息
		 * @return HttpResponse
		 */
		static HttpResponse badRequest(const std::string& message = "Bad Request");

		/**
		 * @brief 创建 500 Internal Server Error 响应
		 * @return HttpResponse
		 */
		static HttpResponse serverError();

		/**
		 * @brief 创建重定向响应
		 * @param location 重定向目标 URL
		 * @param code 状态码（默认 302 Found，支持 301/302/307/308）
		 * @return HttpResponse
		 */
		static HttpResponse redirect(const std::string& location, HttpStatusCode code = HttpStatusCode::hFound);

		/**
		 * @brief 设置文件体（Range 响应等大文件场景，dispatch 层异步发送，不加载到内存）
		 * @param path 文件路径
		 * @param offset 起始偏移
		 * @param length 发送字节数
		 * @param contentType Content-Type
		 */
		void setFileBody(const std::filesystem::path& path,
						 int64_t offset,
						 int64_t length,
						 const std::string& contentType);

		/**
		 * @brief 是否包含文件体
		 */
		bool hasFileBody() const;

		/**
		 * @brief 创建 416 Range Not Satisfiable 响应
		 * @param fileSize 文件总大小（用于 Content-Range: bytes * /total）
		 * @return HttpResponse
		 */
		static HttpResponse rangeNotSatisfiable(std::uintmax_t fileSize);

		/**
		 * @brief 创建分块传输响应（Chunked Transfer-Encoding）
		 * handler 通过 chunkedBody() 获取 ChunkedBody 引用，多次写入数据后 end()
		 * @return HttpResponse
		 */
		static HttpResponse chunked();

		/**
		 * @brief 获取 ChunkedBody 引用
		 * @return ChunkedBody 引用
		 */
		[[nodiscard]] ChunkedBody& chunkedBody();

	private:
		NativeResponse res_;
	};

} // namespace hical
