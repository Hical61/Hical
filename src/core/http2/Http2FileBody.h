/**
 * @file Http2FileBody.h
 * @brief HTTP/2 文件任务、预取限制和读取收尾。
 */

#pragma once

#ifdef HICAL_HAS_HTTP2

	#include "Http2Output.h"
	#include "core/HttpResponse.h"
	#include <memory>

namespace hical::detail
{
	/**
	 * @brief 所属 loop 串行使用的文件任务，不直接写网络，不在协议回调中读取文件。
	 * 会话须持有对象及其任务额度，直到 END_STREAM 写出或失败任务结束；run 返回前会等待读取完成。
	 */
	class Http2FileBody : public std::enable_shared_from_this<Http2FileBody>
	{
		struct Impl;

	public:
		/**
		 * @brief 先取得任务配额，再保存文件信息和尾字段；此时不打开文件。
		 * @param streamId 已接纳的响应流，用于返回逐流错误元数据
		 * @param producer 已提交初始响应头的 DATA 生产者
		 * @param file 非负且不会溢出的文件范围，按值接管路径
		 * @param options 与连接相同的预取及头部上限
		 * @param trailers 已校验的独立尾字段，调用期间复制
		 * @return 文件任务；任务配额不足返回空指针，分配失败抛出 bad_alloc。
		 */
		static std::shared_ptr<Http2FileBody> create(int32_t streamId,
													 std::shared_ptr<Http2Output::Producer> producer,
													 FileBody file,
													 const Http2Options& options,
													 std::span<const Http2Codec::Header> trailers = {});
		~Http2FileBody();
		Http2FileBody(const Http2FileBody&) = delete;
		Http2FileBody& operator=(const Http2FileBody&) = delete;
		/**
		 * @brief 调用时持有自身引用，按窗口和额度逐块读取并提交 DATA；只能运行一次。
		 * @param deadline 由应用返回时固定的响应绝对截止点，读取和额度等待不重建期限
		 * @return 成功或单流错误；取消后仍等待读取回调完成，异常不逃出任务。
		 */
		Awaitable<NetworkError> run(std::chrono::steady_clock::time_point deadline);
		/**
		 * @brief 发出取消信号，实际文件取消在退出当前协议回调后执行；重复调用无效果。
		 */
		void cancel() noexcept;

	private:
		explicit Http2FileBody(std::unique_ptr<Impl> implementation);
		static Awaitable<NetworkError> runOwned(std::shared_ptr<Http2FileBody> self,
												std::chrono::steady_clock::time_point deadline);
		static Awaitable<NetworkError> produce(std::shared_ptr<Http2FileBody> self);
		std::unique_ptr<Impl> impl_;
	};
} // namespace hical::detail

#endif
