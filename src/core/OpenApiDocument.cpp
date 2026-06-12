/**
 * @file OpenApiDocument.cpp
 * @brief OpenAPI 文档生成与缓存实现
 */

#include "core/OpenApiDocument.h"
#include <algorithm>

namespace hical::meta::openapi
{

	OpenApiDocument::OpenApiDocument(std::shared_ptr<const OpenApiRegistry> registry, OpenApiConfig config)
		: registry_(std::move(registry)), config_(std::move(config))
	{
	}

	std::string OpenApiDocument::generateString()
	{
		std::lock_guard lock(mutex_);
		if (!generated_)
		{
			auto doc = buildDocument();
			cached_ = boost::json::serialize(doc);
			generated_ = true;
		}
		return cached_;
	}

	void OpenApiDocument::invalidate()
	{
		std::lock_guard lock(mutex_);
		generated_ = false;
		cached_.clear();
	}

	boost::json::object OpenApiDocument::buildDocument()
	{
		boost::json::object doc;
		doc["openapi"] = "3.0.3";
		doc["info"] = buildInfo();

		auto servers = buildServers();
		if (!servers.empty())
		{
			doc["servers"] = std::move(servers);
		}

		doc["paths"] = buildPaths();

		auto components = buildComponents();
		if (!components.empty())
		{
			doc["components"] = std::move(components);
		}

		return doc;
	}

	boost::json::object OpenApiDocument::buildInfo() const
	{
		boost::json::object info;
		info["title"] = config_.title;
		info["version"] = config_.version;
		if (!config_.description.empty())
		{
			info["description"] = config_.description;
		}
		return info;
	}

	boost::json::array OpenApiDocument::buildServers() const
	{
		boost::json::array servers;
		for (const auto& s : config_.servers)
		{
			boost::json::object server;
			server["url"] = s.url;
			if (!s.description.empty())
			{
				server["description"] = s.description;
			}
			servers.push_back(std::move(server));
		}
		return servers;
	}

	boost::json::object OpenApiDocument::buildPaths() const
	{
		// 获取路由快照
		auto allRoutes = registry_->routes();

		// 按 path 分组，同路径不同 method 合并到一个 Path Item Object
		// 用索引不用指针，省得 vector 重新分配后指针变野
		std::unordered_map<std::string, std::vector<size_t>> pathGroups;
		for (size_t i = 0; i < allRoutes.size(); ++i)
		{
			pathGroups[allRoutes[i].path].push_back(i);
		}

		boost::json::object paths;
		for (const auto& [path, indices] : pathGroups)
		{
			boost::json::object pathItem;
			for (size_t idx : indices)
			{
				const auto& route = allRoutes[idx];
				std::string method = methodToLower(route.method);
				pathItem[method] = buildOperation(route);
			}
			paths[path] = std::move(pathItem);
		}
		return paths;
	}

	boost::json::object OpenApiDocument::buildOperation(const RegisteredRoute& route) const
	{
		boost::json::object op;
		const auto& info = route.apiInfo;

		// operationId
		if (!info.operationId.empty())
		{
			op["operationId"] = info.operationId;
		}
		else
		{
			op["operationId"] = route.handlerName;
		}

		// summary / description
		if (!info.summary.empty())
		{
			op["summary"] = info.summary;
		}
		if (!info.description.empty())
		{
			op["description"] = info.description;
		}

		// tags
		if (!info.tags.empty())
		{
			boost::json::array tagsArr;
			for (const auto& tag : info.tags)
			{
				tagsArr.push_back(boost::json::value(tag));
			}
			op["tags"] = std::move(tagsArr);
		}

		// parameters（路径参数自动提取 + 用户覆盖）
		auto pathParams = extractPathParams(route.path);
		if (!pathParams.empty())
		{
			boost::json::array params;
			for (const auto& paramName : pathParams)
			{
				boost::json::object param;
				param["name"] = paramName;
				param["in"] = "path";
				param["required"] = true;

				// 查找用户是否提供了类型覆盖
				std::string schemaType = "string";
				std::string paramDesc;
				for (const auto& userParam : info.parameters)
				{
					if (userParam.name == paramName)
					{
						schemaType = userParam.schemaType;
						paramDesc = userParam.description;
						break;
					}
				}

				boost::json::object schema;
				schema["type"] = schemaType;
				param["schema"] = std::move(schema);

				if (!paramDesc.empty())
				{
					param["description"] = paramDesc;
				}

				params.push_back(std::move(param));
			}
			op["parameters"] = std::move(params);
		}

		// requestBody
		if (info.requestBodySchema)
		{
			boost::json::object reqBody;
			if (!info.requestBodyDescription.empty())
			{
				reqBody["description"] = info.requestBodyDescription;
			}
			reqBody["required"] = info.requestBodyRequired;

			boost::json::object content;
			boost::json::object jsonMedia;
			jsonMedia["schema"] = info.requestBodySchema();
			content["application/json"] = std::move(jsonMedia);
			reqBody["content"] = std::move(content);

			op["requestBody"] = std::move(reqBody);
		}

		// responses
		boost::json::object responses;
		if (!info.responses.empty())
		{
			for (const auto& [code, respInfo] : info.responses)
			{
				boost::json::object resp;
				resp["description"] = respInfo.description;
				if (respInfo.schema)
				{
					boost::json::object content;
					boost::json::object jsonMedia;
					jsonMedia["schema"] = respInfo.schema();
					content["application/json"] = std::move(jsonMedia);
					resp["content"] = std::move(content);
				}
				responses[std::to_string(code)] = std::move(resp);
			}
		}
		else
		{
			// 无标注时生成默认 200 响应
			boost::json::object defaultResp;
			defaultResp["description"] = "OK";
			responses["200"] = std::move(defaultResp);
		}
		op["responses"] = std::move(responses);

		return op;
	}

	boost::json::object OpenApiDocument::buildComponents() const
	{
		const auto& schemas = registry_->schemas();
		if (schemas.empty())
		{
			return {};
		}

		boost::json::object components;
		boost::json::object schemasObj;
		for (const auto& [name, schema] : schemas)
		{
			schemasObj[name] = schema;
		}
		components["schemas"] = std::move(schemasObj);
		return components;
	}

	std::string OpenApiDocument::methodToLower(HttpMethod method)
	{
		switch (method)
		{
			case HttpMethod::hGet:
				return "get";
			case HttpMethod::hPost:
				return "post";
			case HttpMethod::hPut:
				return "put";
			case HttpMethod::hDelete:
				return "delete";
			case HttpMethod::hPatch:
				return "patch";
			case HttpMethod::hHead:
				return "head";
			case HttpMethod::hOptions:
				return "options";
			default:
				return "unknown";
		}
	}

	std::vector<std::string> OpenApiDocument::extractPathParams(std::string_view path)
	{
		std::vector<std::string> params;
		size_t pos = 0;
		while (pos < path.size())
		{
			auto start = path.find('{', pos);
			if (start == std::string_view::npos)
			{
				break;
			}
			auto end = path.find('}', start + 1);
			if (end == std::string_view::npos)
			{
				break;
			}
			params.emplace_back(path.substr(start + 1, end - start - 1));
			pos = end + 1;
		}
		return params;
	}

} // namespace hical::meta::openapi
