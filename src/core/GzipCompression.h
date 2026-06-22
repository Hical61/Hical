/**
 * @file GzipCompression.h
 * @brief 响应压缩中间件（gzip）
 */

#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Middleware.h"
#include <string>
#include <string_view>

namespace hical
{

	/**
	 * @brief Gzip 压缩选项
	 */
	struct GzipCompressionOptions
	{
		/// 最小压缩阈值（字节），小于此大小的 body 不压缩
		size_t minSize = 1024;

		/// 流式压缩阈值（字节），大于此大小走 chunked 流式压缩
		size_t streamingThreshold = 65536; // 64KB

		/// zlib 压缩级别（1-9，6 是默认平衡点）
		int compressionLevel = 6;

		/// 异步压缩线程数。0 = 同步压缩（被调用线程上执行），>0 则在独立线程上执行
		/// 大 body（>= streamingThreshold）的 deflate 会挪到后台线程，不阻塞 IO 线程
		size_t asyncThreads = 2;
	};

	/**
	 * @brief 创建 gzip 响应压缩中间件（同步版，SyncAfterHandler）
	 * 自动检查请求的 Accept-Encoding: gzip，在响应时压缩 body
	 * 并设置 Content-Encoding: gzip。
	 * 小 body（< streamingThreshold）整体压缩后替换 body + 更新 Content-Length
	 * 大 body（>= streamingThreshold）走 chunked 流式压缩，逐块发送
	 * @warning 同步版在调用线程上执行 deflate，大 body 会阻塞 IO 线程。
	 *          对大 body 场景请使用 makeGzipCompressionMiddlewareAsync()。
	 * @param opts 压缩选项
	 * @return SyncAfterHandler 可传入 server.use() 的同步后置中间件
	 */
	SyncAfterHandler makeGzipCompressionMiddleware(GzipCompressionOptions opts = {});

	/**
	 * @brief 创建 gzip 响应压缩中间件（异步版，MiddlewareHandler）
	 * 和同步版效果一样，但大 body（>= streamingThreshold）的 deflate 压缩
	 * 在独立线程池中执行，不阻塞 IO 线程。
	 * 小 body（< streamingThreshold）仍在线程上同步压缩（延迟可忽略）。
	 * 通过 opts.asyncThreads 控制后台线程数（默认 2）。
	 * 使用示例：
	 * ```cpp
	 * server.use(makeGzipCompressionMiddlewareAsync({.asyncThreads = 4}));
	 * ```
	 * @param opts 压缩选项
	 * @return MiddlewareHandler 可传入 server.use() 的异步中间件
	 */
	MiddlewareHandler makeGzipCompressionMiddlewareAsync(GzipCompressionOptions opts = {});

} // namespace hical
