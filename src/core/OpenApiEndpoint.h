/**
 * @file OpenApiEndpoint.h
 * @brief OpenAPI 端点暴露 + Swagger UI
 * 提供 serveOpenApi() 一键注册函数，
 * 在 Router 上注册 /openapi.json 和 /docs 两个端点。
 * 用法：
 *   auto doc = std::make_shared<OpenApiDocument>(registry, config);
 *   serveOpenApi(server.router(), doc);
 *   // GET /openapi.json → 返回 OpenAPI 3.0 JSON
 *   // GET /docs → 返回 Swagger UI HTML
 */

#pragma once

#include "OpenApiDocument.h"
#include "Router.h"
#include "HttpResponse.h"
#include <memory>
#include <string>

namespace hical::meta::openapi
{

	/**
	 * @brief 一键注册 OpenAPI JSON 端点 + Swagger UI 页面
	 * @param router 路由器实例
	 * @param document OpenAPI 文档生成器（shared_ptr 保证生命周期）
	 * @param jsonPath JSON 端点路径（默认 /openapi.json）
	 * @param uiPath Swagger UI 页面路径（默认 /docs）
	 */
	inline void serveOpenApi(Router& router,
							 std::shared_ptr<OpenApiDocument> document,
							 const std::string& jsonPath = "/openapi.json",
							 const std::string& uiPath = "/docs")
	{
		// JSON 端点
		router.get(jsonPath,
				   [document](const HttpRequest&) -> HttpResponse
				   {
					   HttpResponse res;
					   res.setStatus(HttpStatusCode::hOk);
					   // 不硬编码 CORS 头，遵循用户配置的 CORS 中间件策略
					   res.setBody(document->generateString(), "application/json; charset=utf-8");
					   return res;
				   });

		// Swagger UI（通过 CDN 引用 swagger-ui-dist，零本地依赖）
		// 使用 Boost.JSON serialize 安全转义 jsonPath 防止 JS 注入
		std::string safeJsonPath = boost::json::serialize(boost::json::value(jsonPath));
		// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers)
		std::string html = "<!DOCTYPE html>\n"
						   "<html lang=\"en\">\n"
						   "<head>\n"
						   "  <meta charset=\"UTF-8\">\n"
						   "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
						   "  <title>API Documentation</title>\n"
						   "  <link rel=\"stylesheet\" "
						   "href=\"https://cdn.jsdelivr.net/npm/swagger-ui-dist@5.18.2/swagger-ui.css\" "
						   "integrity=\"sha384-rcbEi6xgdPk0iWkAQzT2F3FeBJXdG+ydrawGlfHAFIZG7wU6aKbQaRewysYpmrlW\" "
						   "crossorigin=\"anonymous\">\n"
						   "  <style>html { box-sizing: border-box; overflow-y: scroll; } "
						   "*, *::before, *::after { box-sizing: inherit; } "
						   "body { margin: 0; background: #fafafa; }</style>\n"
						   "</head>\n"
						   "<body>\n"
						   "  <div id=\"swagger-ui\"></div>\n"
						   "  <script src=\"https://cdn.jsdelivr.net/npm/swagger-ui-dist@5.18.2/swagger-ui-bundle.js\" "
						   "integrity=\"sha384-NXtFPpN61oWCuN4D42K6Zd5Rt2+uxeIT36R7kpXBuY9tLnZorzrJ4ykpqwJfgjpZ\" "
						   "crossorigin=\"anonymous\">"
						   "</script>\n"
						   "  <script>\n"
						   "    SwaggerUIBundle({\n"
						   "      url: "
						   + safeJsonPath
						   + ",\n"
							 "      dom_id: \"#swagger-ui\",\n"
							 "      presets: [\n"
							 "        SwaggerUIBundle.presets.apis,\n"
							 "        SwaggerUIBundle.SwaggerUIStandalonePreset\n"
							 "      ],\n"
							 "      layout: \"StandaloneLayout\"\n"
							 "    });\n"
							 "  </script>\n"
							 "</body>\n"
							 "</html>\n";
		// NOLINTEND(cppcoreguidelines-avoid-magic-numbers)

		router.get(uiPath,
				   [html = std::move(html)](const HttpRequest&) -> HttpResponse
				   {
					   HttpResponse res;
					   res.setStatus(HttpStatusCode::hOk);
					   res.setHeader("Cache-Control", "public, max-age=3600");
					   res.setBody(html, "text/html; charset=utf-8");
					   return res;
				   });
	}

} // namespace hical::meta::openapi
