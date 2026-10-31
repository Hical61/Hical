/**
 * @file Http2Response.h
 * @brief 将 HttpResponse 转为 HTTP/2 响应头、DATA 和尾字段。
 */

#pragma once

#ifdef HICAL_HAS_HTTP2

	#include "Http2Output.h"
	#include "Http2FileBody.h"
	#include "core/HttpResponse.h"
	#include <utility>

namespace hical::detail
{
	/**
	 * @brief 所属 loop 串行调用的响应映射；不执行 I/O，不使用 HTTP/1 序列化。
	 */
	class Http2Response
	{
	public:
		/**
		 * @brief 响应提交结果；文件任务由会话持有并运行。
		 */
		struct Result : Http2Output::OpenResult
		{
			Result() = default;

			/**
			 * @brief 接管通用输出结果。
			 * @param output 头或 DATA 生产者提交结果
			 */
			Result(Http2Output::OpenResult output) : Http2Output::OpenResult(std::move(output))
			{
			}

			std::shared_ptr<Http2FileBody> file;
		};

		/**
		 * @brief 校验最终响应，提交复制的头字段，再接管 body 并结束生产。
		 * 普通响应及 HEAD 的长度按当前原始 body 或 chunks 合计重算；HEAD 不发送 DATA；204 不带长度或 body。
		 * 304 只保留已有合法 Content-Length，由调用者保证它描述选定表示，不从残留 body 生成。
		 * ChunkedBody 是已 end 的完整收集结果，不是流式生产者；原始 chunks 经 Output 先计费再有界聚合。
		 * 成功转交初始头、payload 与尾字段后释放原 ChunkedBody，等待物理完成时不再持有双份载荷。
		 * NativeResponse 与 ChunkedBody 的尾字段合成独立尾头，按独立预算严格校验；无正文响应不发送尾头。
		 * 文件响应按声明的范围设置长度，并返回文件任务；HEAD/204/304 和零长度不打开文件。
		 * 单独返回的 1xx 转为独立 500；未 end 的 ChunkedBody 返回明确错误，不等待后续生产。
		 * @param output 所属连接的唯一输出调度
		 * @param streamId 已接纳的请求流 ID
		 * @param requestMethod 原请求方法，用于 HEAD 的无正文语义
		 * @param response 转交的响应；调用前必须结束绑定该响应的 JsonWriter
		 * @param options 与连接一致的头部和资源配置
		 * @return 普通 producer 已 finish；文件需运行 file 并等 END_STREAM 写出；失败时会话须发送 RST 并唤醒 writer
		 */
		[[nodiscard]] static Result submit(Http2Output& output,
										   int32_t streamId,
										   HttpMethod requestMethod,
										   HttpResponse&& response,
										   const Http2Options& options = {});
	};
} // namespace hical::detail

#endif
