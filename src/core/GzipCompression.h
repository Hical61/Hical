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
	};

	/**
	 * @brief 创建 gzip 响应压缩中间件
	 * 自动检查请求的 Accept-Encoding: gzip，在响应时压缩 body
	 * 并设置 Content-Encoding: gzip。
	 * 小 body（< streamingThreshold）整体压缩后替换 body + 更新 Content-Length
	 * 大 body（>= streamingThreshold）走 chunked 流式压缩，逐块发送
	 * @param opts 压缩选项
	 * @return SyncAfterHandler 可传入 server.use() 的同步后置中间件
	 */
	SyncAfterHandler makeGzipCompressionMiddleware(GzipCompressionOptions opts = {});

} // namespace hical
