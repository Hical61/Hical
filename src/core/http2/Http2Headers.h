/**
 * @file Http2Headers.h
 * @brief HTTP/2 头部收集、校验和存储。
 */

#pragma once

#ifdef HICAL_HAS_HTTP2

	#include "Http2Codec.h"
	#include <memory>
	#include <optional>
	#include <span>
	#include <string_view>

namespace hical::detail
{
	/**
	 * @brief 每个初始头或尾头块独立持有存储，不借用解码器或请求级 PMR。
	 * append 阶段只保存偏移；seal 成功后才产生视图，之后不可追加。
	 * 移动对象只转移存储的所有权，已有视图仍有效；存储释放后视图失效。
	 * 单线程使用；本类补充应用映射检查，不替代协议库自带的 HTTP 校验。
	 */
	class Http2Headers
	{
	public:
		using Kind = Http2Codec::HeaderKind;
		using Field = Http2Codec::Header;

		/**
		 * @brief 为一个流的头部块分配独立存储。
		 * @param kind 请求头、响应头或尾头。
		 * @param streamId 正数流 ID，用于错误分类。
		 * @param options 头字段数量及 header-list 字节预算，构造时复制相关限制。
		 */
		Http2Headers(Kind kind, int32_t streamId, const Http2Options& options = {});
		/**
		 * @brief 释放拥有存储，该对象提供的所有视图随之失效。
		 */
		~Http2Headers();
		Http2Headers(const Http2Headers&) = delete;
		Http2Headers& operator=(const Http2Headers&) = delete;
		/**
		 * @brief 转移存储所有权，不移动字符串或重建视图。
		 * @param other 转出存储后成为空对象。
		 */
		Http2Headers(Http2Headers&& other) noexcept;
		/**
		 * @brief 转移所有者，原目标对象的视图失效，源对象先前的视图保持有效。
		 * @param other 转出存储后成为空对象。
		 * @return 当前对象。
		 */
		Http2Headers& operator=(Http2Headers&& other) noexcept;

		/**
		 * @brief 检查预算和字段规则后复制输入，首次错误后不再接受该块。
		 * @param name 原始小写字段名，伪头也计入预算。
		 * @param value 原始字段值，允许普通字段值为空。
		 * @return 成功或带流 ID 的协议、资源错误；封存后追加返回配置错误。
		 */
		NetworkError append(std::string_view name, std::string_view value);
		/**
		 * @brief 检查完整伪头和 authority/Host，再生成稳定视图；重复封存不改变存储。
		 * @return 成功或该头部块的首个错误。
		 */
		NetworkError seal();
		/**
		 * @brief 查询是否已成功封存。
		 * @return 成功封存为 true；移动后的源对象返回 false。
		 */
		[[nodiscard]] bool sealed() const noexcept;
		/**
		 * @brief 获取常规字段，Cookie 已合并，Set-Cookie 仍逐项保留；伪头不在其中。
		 * @return 仅封存后有效的连续视图，最多 64 项；请求包含按 authority 规范化的 Host。
		 */
		[[nodiscard]] std::span<const Field> fields() const noexcept;
		/**
		 * @brief 查找第一个常规字段。
		 * @param name 大小写不敏感的字段名。
		 * @return 字段值；未封存或不存在时为空。
		 */
		[[nodiscard]] std::string_view find(std::string_view name) const noexcept;
		/**
		 * @brief 获取请求的原始方法，保留扩展方法及大小写。
		 * @return :method 值，非请求或未封存时为空。
		 */
		[[nodiscard]] std::string_view rawMethod() const noexcept;
		/**
		 * @brief 获取未经 URI 解码的请求目标，普通 CONNECT 使用 authority。
		 * @return :path 或 CONNECT authority，非请求或未封存时为空。
		 */
		[[nodiscard]] std::string_view target() const noexcept;
		/**
		 * @brief 获取请求 scheme。
		 * @return :scheme 值，普通 CONNECT 或非请求时为空。
		 */
		[[nodiscard]] std::string_view scheme() const noexcept;
		/**
		 * @brief 获取优先采用 :authority、其次 Host 的请求 authority。
		 * @return 稳定的原始 authority，非请求或未封存时为空。
		 */
		[[nodiscard]] std::string_view authority() const noexcept;
		/**
		 * @brief 获取三位响应状态码；1xx 头部块单独保存。
		 * @return :status 解析值，非响应或未封存时为零。
		 */
		[[nodiscard]] uint16_t status() const noexcept;
		/**
		 * @brief 获取唯一且非负的消息长度，上限为协议库支持的 INT64_MAX。
		 * @return 已封存的 Content-Length；不存在或未封存时为空。
		 */
		[[nodiscard]] std::optional<uint64_t> contentLength() const noexcept;
		/**
		 * @brief 识别语法合法的普通 CONNECT，交上层返回不支持业务的 501。
		 * @return 请求方法为 CONNECT 且成功封存时为 true。
		 */
		[[nodiscard]] bool isConnect() const noexcept;
		/**
		 * @brief 查询合并前原始字段数。
		 * @return 含伪头、分拆 Cookie 的已接收字段数。
		 */
		[[nodiscard]] size_t decodedFieldCount() const noexcept;
		/**
		 * @brief 查询头部占用的预算；合并字段不退还额度。
		 * @return 每个原始字段 name + value + 32 字节的累计值。
		 */
		[[nodiscard]] size_t decodedBytes() const noexcept;

	private:
		struct Storage;
		std::unique_ptr<Storage> storage_;
	};
} // namespace hical::detail

#endif
