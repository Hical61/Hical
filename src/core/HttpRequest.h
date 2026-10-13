/**
 * @file HttpRequest.h
 * @brief 零拷贝 HTTP 请求解析与访问
 */

#pragma once

#include "HeaderMap.h"
#include "HttpTypes.h"
#include "InetAddress.h"
#include <any>
#include <array>
#include <boost/json.hpp>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hical
{

	class MultipartParser; // 前向声明，用于友元

	namespace detail
	{
		struct RequestDispatchContext; // 前向声明：请求分发上下文（见 RequestDispatch.h）

		/**
		 * @brief 判断逗号分隔的 Connection 头值里是否含 upgrade token
		 * 逐 token 切分，只裁 ASCII 空格不裁制表符，大小写不敏感。
		 * NativeRequest 预扫和 isUpgrade 回退两条路都走这里，保证判定完全一致。
		 * @param connValue Connection 头的原始值
		 * @return 含 upgrade token 返回 true
		 */
		inline bool connectionHasUpgradeToken(std::string_view connValue) noexcept
		{
			std::string_view sv = connValue;
			while (!sv.empty())
			{
				auto comma = sv.find(',');
				auto token = (comma != std::string_view::npos) ? sv.substr(0, comma) : sv;
				sv = (comma != std::string_view::npos) ? sv.substr(comma + 1) : std::string_view {};
				while (!token.empty() && token.front() == ' ')
				{
					token.remove_prefix(1);
				}
				while (!token.empty() && token.back() == ' ')
				{
					token.remove_suffix(1);
				}
				if (HeaderMap::iequals(token, "upgrade"))
				{
					return true;
				}
			}
			return false;
		}
	} // namespace detail

	/**
	 * @brief 零拷贝请求头部容器
	 * 存储 string_view 对，引用外部缓冲区（如连接级 readBuf）。
	 * 栈分配，无堆分配。最多 64 个头部（HTTP 请求极少超过此数量）。
	 */
	class RequestHeaders
	{
	public:
		struct Entry
		{
			std::string_view name;
			std::string_view value;
		};

		static constexpr size_t kMaxHeaders = 64;

		// 不能用 = default：entries_ 在匿名 union 里，是非平凡默认构造的变体成员，
		// 会让 union（从而本类）的默认构造被 delete。手写个空体，压根不碰 entries_，
		// size_ 走它自己的默认成员初始化器置 0。
		RequestHeaders() noexcept
		{
		}

		// 默认的 move 会把整块 2048B 的 entries_ 全搬一遍，与真实 header 数量无关
		// （典型 6-8 条其实只有 200 来字节有效）。这里只搬 size_ 个有效元素。
		// string_view 的"移动"本就是拷指针+长度，所以沿用旧语义：源原样不动，
		// 调用方 move 之后仍能读源对象。
		RequestHeaders(RequestHeaders&& other) noexcept
		{
			copyValidEntries(other);
		}

		RequestHeaders& operator=(RequestHeaders&& other) noexcept
		{
			if (this != &other)
			{
				copyValidEntries(other);
			}
			return *this;
		}

		void add(std::string_view name, std::string_view value) noexcept
		{
			if (size_ < kMaxHeaders)
			{
				entries_[size_++] = {name, value};
			}
		}

		std::string_view find(std::string_view name) const noexcept
		{
			for (size_t i = 0; i < size_; ++i)
			{
				if (HeaderMap::iequals(entries_[i].name, name))
				{
					return entries_[i].value;
				}
			}
			return {};
		}

		bool contains(std::string_view name) const noexcept
		{
			for (size_t i = 0; i < size_; ++i)
			{
				if (HeaderMap::iequals(entries_[i].name, name))
				{
					return true;
				}
			}
			return false;
		}

		size_t size() const noexcept
		{
			return size_;
		}

		const Entry& operator[](size_t i) const noexcept
		{
			return entries_[i];
		}

		const Entry* begin() const noexcept
		{
			return entries_.data();
		}

		const Entry* end() const noexcept
		{
			return entries_.data() + size_;
		}

		void clear() noexcept
		{
			size_ = 0;
		}

	private:
		// 把 other 的前 size_ 个有效元素拷过来，other 保持不变
		void copyValidEntries(const RequestHeaders& other) noexcept
		{
			size_ = other.size_;
			if (size_ > 0)
			{
				std::memcpy(entries_.data(), other.entries_.data(), size_ * sizeof(Entry));
			}
		}

		// 匿名 union 把 entries_ 变成变体成员，默认构造不再碰它。否则每构造一个
		// RequestHeaders 都要把 2048B 的 string_view 数组清零（64 次 string_view
		// 构造），而实际请求通常只有几条 header。Entry 是 string_view 聚合，
		// trivial 析构，union 不需要自定义析构。
		// 读安全：find/contains/operator[]/begin/end 都只在 size_ > 0 时读 entries_，
		// 而 size_ > 0 意味着 add() 至少写过一次，不会读到未构造的对象。
		union
		{
			std::array<Entry, kMaxHeaders> entries_;
		};

		size_t size_ = 0;
	};

	/**
	 * @brief HTTP 请求的原生内部表示（零拷贝）
	 * target 和 headers 中的 string_view 引用连接级 readBuf，
	 * 生命周期由 handleSession 的 for 循环保证。
	 * body 拥有独立内存（从 socket 读取时直接写入）。
	 */
	struct NativeRequest
	{
		HttpMethod method = HttpMethod::hUnknown;
		std::string_view target; // 零拷贝，引用 readBuf
		int httpVersionMajor = 1;
		int httpVersionMinor = 1;
		RequestHeaders headers; // 零拷贝，引用 readBuf
		std::string body;       // 拥有所有权（从 socket 读取）
		bool keepAlive = true;
		bool expectContinue = false; // 请求携带了 Expect: 100-continue

		// 解析阶段预扫 Connection 头得到的 upgrade 判定结果。
		// nullopt：没预扫过（手工构造的 NativeRequest 就是这样），isUpgrade() 回退线性扫描；
		// 有值：预扫过，直接采纳（Upgrade 头那一步仍需另查）。
		std::optional<bool> upgradePreScanned;

		/**
		 * @brief 是否为 WebSocket 升级请求
		 */
		bool isUpgrade() const
		{
			// Connection 侧：预扫过就读标志，省掉再扫一遍头部；没预扫才现算
			bool hasUpgrade = upgradePreScanned.has_value()
								  ? *upgradePreScanned
								  : detail::connectionHasUpgradeToken(headers.find("Connection"));
			if (!hasUpgrade)
			{
				return false;
			}
			return HeaderMap::iequals(headers.find("Upgrade"), "websocket");
		}
	};

	/**
	 * @brief HTTP 请求封装
	 * 对自研 NativeRequest 的 hical 风格封装。
	 * 提供简洁的接口访问请求方法、路径、头部和消息体。
	 */
	class HttpRequest
	{
		friend class MultipartParser;
		friend struct detail::RequestDispatchContext;

	public:
		HttpRequest();
		HttpRequest(const HttpRequest&) = delete;
		HttpRequest& operator=(const HttpRequest&) = delete;
		HttpRequest(HttpRequest&& other) noexcept;
		HttpRequest& operator=(HttpRequest&& other) noexcept;

		/**
		 * @brief 从已解析的 NativeRequest 构造（内部使用，跳过 CRLF 检查）
		 * @param req 已解析的原生请求
		 * @return HttpRequest
		 */
		[[nodiscard]] static HttpRequest fromParsed(NativeRequest&& req);

		[[nodiscard]] HttpMethod method() const;
		[[nodiscard]] std::string_view path() const;
		[[nodiscard]] std::string_view target() const;
		[[nodiscard]] std::string_view query() const;
		[[nodiscard]] std::string_view header(std::string_view name) const;
		[[nodiscard]] const std::string& body() const;
		[[nodiscard]] const boost::json::value& jsonBody() const;

		/**
		 * @brief 反序列化请求体 JSON 为指定类型（需要 HICAL_JSON 标注）
		 */
		template <typename T>
		[[nodiscard]] T readJson() const;

		/**
		 * @brief 获取 Content-Type 头部值
		 */
		[[nodiscard]] std::string_view contentType() const;

		/**
		 * @brief 获取底层 NativeRequest 引用（可修改）
		 */
		[[nodiscard]] NativeRequest& native();
		/**
		 * @brief 获取底层 NativeRequest 常量引用
		 */
		[[nodiscard]] const NativeRequest& native() const;

		void setMethod(HttpMethod method);
		void setTarget(const std::string& target);
		void setHeader(const std::string& name, const std::string& value);
		void setBody(const std::string& body);

		/**
		 * @brief 获取对端网络地址
		 * @return 对端地址引用（未设置时为默认构造的无效地址）
		 */
		[[nodiscard]] const InetAddress& peerAddr() const;

		/**
		 * @brief 设置对端网络地址
		 * @param addr 对端地址
		 */
		void setPeerAddr(const InetAddress& addr);

		// ============ 路径参数 ============

		/**
		 * @brief 获取路径参数值（如 /users/{id} 中的 id），不存在则返回空字符串引用
		 */
		[[nodiscard]] const std::string& param(std::string_view name) const;
		void setParam(const std::string& name, const std::string& value);
		[[nodiscard]] bool hasParam(std::string_view name) const;

		// ============ Cookie ============

		/**
		 * @brief 获取指定名称的 Cookie 值，不存在则返回空字符串引用
		 */
		[[nodiscard]] const std::string& cookie(std::string_view name) const;
		/**
		 * @brief 获取所有 Cookie（惰性解析，首次调用时解析 Cookie 头）
		 */
		[[nodiscard]] const std::unordered_map<std::string, std::string, StringHash, StringEqual>& cookies() const;
		[[nodiscard]] bool hasCookie(std::string_view name) const;

		// ============ 查询参数 ============

		/**
		 * @brief 获取指定名称的查询参数值，不存在则返回 nullopt
		 */
		[[nodiscard]] std::optional<std::string> queryParam(std::string_view name) const;
		/**
		 * @brief 获取所有查询参数（惰性解析，支持同名多值）
		 */
		[[nodiscard]] const std::unordered_multimap<std::string, std::string, StringHash, StringEqual>& queryParams()
			const;
		[[nodiscard]] bool hasQueryParam(std::string_view name) const;

		// ============ 表单参数 ============

		/**
		 * @brief 获取指定名称的表单参数值（application/x-www-form-urlencoded），不存在则返回 nullopt
		 */
		[[nodiscard]] std::optional<std::string> formParam(std::string_view name) const;
		/**
		 * @brief 获取所有表单参数（惰性解析，支持同名多值）
		 */
		[[nodiscard]] const std::unordered_multimap<std::string, std::string, StringHash, StringEqual>& formParams()
			const;
		[[nodiscard]] bool hasFormParam(std::string_view name) const;

		// ============ 请求级属性 ============

		/**
		 * @brief 设置请求级属性（中间件间数据传递，如 trace_id、db connection 等）
		 */
		void setAttribute(std::string_view key, std::any value);
		/**
		 * @brief 获取请求级属性，不存在则返回 nullopt
		 */
		[[nodiscard]] std::optional<std::any> getAttribute(std::string_view key) const;

		template <typename T>
		[[nodiscard]] std::optional<T> getAttribute(const std::string& key) const
		{
			if (!attributes_)
			{
				return std::nullopt;
			}
			auto it = attributes_->find(key);
			if (it == attributes_->end())
			{
				return std::nullopt;
			}
			if (it->second.type() != typeid(T))
			{
				return std::nullopt;
			}
			return std::any_cast<T>(it->second);
		}

	private:
		// 直接 move 构造 req_，省掉"先默认构造（清 2KB headers）+ 再 move 赋值"的白工。
		// 仅供 fromParsed 使用。
		explicit HttpRequest(NativeRequest&& req) noexcept : req_(std::move(req))
		{
		}

		void parseCookies() const;
		static void parseUrlEncoded(std::string_view input,
									std::unordered_multimap<std::string, std::string, StringHash, StringEqual>& out);
		void parseQueryParams() const;
		void parseFormParams() const;

		NativeRequest req_;
		InetAddress peerAddr_;
		// setter 专用拥有存储（测试/构建请求场景，不在热路径）
		std::string ownedTarget_;
		HeaderMap ownedHeaders_;
		std::vector<std::pair<std::string, std::string>> pathParams_;
		mutable std::optional<std::unordered_map<std::string, std::string, StringHash, StringEqual>> cookies_;
		mutable std::optional<boost::json::value> cachedJsonBody_;
		mutable std::optional<std::unordered_multimap<std::string, std::string, StringHash, StringEqual>> queryParams_;
		mutable std::optional<std::unordered_multimap<std::string, std::string, StringHash, StringEqual>> formParams_;
		mutable std::any cachedMultipartParts_;
		std::unique_ptr<std::unordered_map<std::string, std::any, StringHash, StringEqual>> attributes_;

		// 框架内部请求级上下文槽（detail 命名空间专用），零堆分配。
		// 无中间件路径完全不读写；有中间件时由分发骨架在每请求写入，
		// 沿洋葱链跨 co_await 引用栈上的 dispatch 上下文。
		// 移动构造/赋值必须把源置 nullptr，防止被 move 走的旧对象残留脏指针。
		void* internalSlot_ = nullptr;
	};

} // namespace hical
