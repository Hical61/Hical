#include "core/OpenApiRegistry.h"

namespace hical::meta::openapi
{

	void OpenApiRegistry::addRoute(HttpMethod method, std::string path, std::string handlerName, RouteApiInfo apiInfo)
	{
		std::lock_guard lock(m_mutex);
		m_routes.push_back(RegisteredRoute {method, std::move(path), std::move(handlerName), std::move(apiInfo)});
	}

	void OpenApiRegistry::addSchema(const std::string& name, boost::json::object schema)
	{
		std::lock_guard lock(m_mutex);
		m_schemas[name] = std::move(schema);
	}

	bool OpenApiRegistry::hasSchema(const std::string& name) const
	{
		std::lock_guard lock(m_mutex);
		return m_schemas.find(name) != m_schemas.end();
	}

	std::vector<RegisteredRoute> OpenApiRegistry::routes() const
	{
		std::lock_guard lock(m_mutex);
		return m_routes;
	}

	std::unordered_map<std::string, boost::json::object> OpenApiRegistry::schemas() const
	{
		std::lock_guard lock(m_mutex);
		return m_schemas;
	}

} // namespace hical::meta::openapi
