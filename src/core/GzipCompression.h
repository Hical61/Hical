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

		/// 大响应阈值（字节）：HTTP/1 在此阈值起编码 chunked，HTTP/2 始终保存原始 gzip
		size_t streamingThreshold = 65536; // 64KB

		/// zlib 压缩级别（1-9，6 是默认平衡点）
		/// 注意：json-comp 场景按带宽调整后计分，压得越小反而分越高，
		/// 调 level 要按总分算，别只盯 CPU 开销
		int compressionLevel = 6;

		/// 异步压缩线程数。0 = 同步压缩（被调用线程上执行），>0 则在独立线程上执行
		/// 大 body（>= streamingThreshold）的 deflate 会挪到后台线程，不阻塞 IO 线程
		size_t asyncThreads = 2;
	};

	/**
	 * @brief 创建 gzip 响应压缩中间件（同步版，SyncAfterHandler）
	 * 自动检查请求的 Accept-Encoding: gzip，在响应时压缩 body
	 * 并设置 Content-Encoding: gzip；合并 Vary: Accept-Encoding，已编码响应不重复压缩。
	 * 小 body（< streamingThreshold）整体压缩后替换 body + 更新 Content-Length
	 * 大 body（>= streamingThreshold）在 HTTP/1 使用分块编码；HTTP/2 直接输出 gzip 字节。
	 * 两种协议目前都完整缓冲压缩结果，不是边生产边发送的网络流式接口。
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
	 * 小 body（< streamingThreshold）仍在调用线程同步压缩；结果总在原执行器上写回响应。
	 * 通过 opts.asyncThreads 控制后台线程数（默认 2），设为 0 时全部同步执行且不创建池。
	 * 后台任务按值持有输入，thread_local 压缩状态只在无挂起的压缩调用内使用。
	 * 使用示例：
	 * ```cpp
	 * server.use(makeGzipCompressionMiddlewareAsync({.asyncThreads = 4}));
	 * ```
	 * @param opts 压缩选项
	 * @return MiddlewareHandler 可传入 server.use() 的异步中间件
	 */
	MiddlewareHandler makeGzipCompressionMiddlewareAsync(GzipCompressionOptions opts = {});

} // namespace hical
