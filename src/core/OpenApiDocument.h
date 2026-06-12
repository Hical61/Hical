/**
 * @file OpenApiDocument.h
 * @brief OpenAPI 3.0 文档懒生成与缓存
 * 从 OpenApiRegistry 中收集路由和 schema 信息，
 * 组装为符合 OpenAPI 3.0.3 规范的完整 JSON 文档。
 * 用法：
 *   OpenApiDocument doc(registry, {.title="My API", .version="1.0.0"});
 *   std::string json = doc.generateString();
 */

#pragma once

#include "OpenApiRegistry.h"
#include <boost/json.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hical::meta::openapi
{

	/**
	 * @brief OpenAPI 文档配置
	 */
	struct OpenApiConfig
	{
		std::string title = "Hical API";
		std::string version = "1.0.0";
		std::string description;

		struct Server
		{
			std::string url;
			std::string description;
		};

		std::vector<Server> servers;
	};

	/**
	 * @brief OpenAPI 3.0 文档生成器
	 * 惰性生成并缓存，线程安全。
	 */
	class OpenApiDocument
	{
	public:
		explicit OpenApiDocument(std::shared_ptr<const OpenApiRegistry> registry, OpenApiConfig config = {});

		/**
		 * @brief 惰性生成并返回序列化后的 JSON 字符串
		 * 首次调用时生成文档并缓存，后续调用直接返回缓存
		 */
		std::string generateString();

		/**
		 * @brief 清除缓存（路由变化后需调用）
		 */
		void invalidate();

	private:
		boost::json::object buildDocument();
		boost::json::object buildInfo() const;
		boost::json::array buildServers() const;
		boost::json::object buildPaths() const;
		boost::json::object buildOperation(const RegisteredRoute& route) const;
		boost::json::object buildComponents() const;

		static std::string methodToLower(HttpMethod method);
		static std::vector<std::string> extractPathParams(std::string_view path);

		std::shared_ptr<const OpenApiRegistry> registry_;
		OpenApiConfig config_;
		std::mutex mutex_;
		bool generated_ = false;
		std::string cached_;
	};

} // namespace hical::meta::openapi
