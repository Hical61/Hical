/**
 * @file OpenApiRegistry.cpp
 * @brief 路由元数据注册实现
 */

#include "core/OpenApiRegistry.h"

namespace hical::meta::openapi
{

	void OpenApiRegistry::addRoute(HttpMethod method, std::string path, std::string handlerName, RouteApiInfo apiInfo)
	{
		std::lock_guard lock(mutex_);
		routes_.push_back(RegisteredRoute {method, std::move(path), std::move(handlerName), std::move(apiInfo)});
	}

	void OpenApiRegistry::addSchema(const std::string& name, boost::json::object schema)
	{
		std::lock_guard lock(mutex_);
		schemas_[name] = std::move(schema);
	}

	bool OpenApiRegistry::hasSchema(const std::string& name) const
	{
		std::lock_guard lock(mutex_);
		return schemas_.find(name) != schemas_.end();
	}

	std::vector<RegisteredRoute> OpenApiRegistry::routes() const
	{
		std::lock_guard lock(mutex_);
		return routes_;
	}

	std::unordered_map<std::string, boost::json::object> OpenApiRegistry::schemas() const
	{
		std::lock_guard lock(mutex_);
		return schemas_;
	}

} // namespace hical::meta::openapi
