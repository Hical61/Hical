/**
 * @file positive.cpp
 * @brief 用 Router 的公开接口验证三种同步签名和对应的文档
 */

#include "Endpoint.h"

#include "core/Log.h"
#include "core/OpenApiDocument.h"
#include <cstdlib>
#include <memory>

namespace contract_test
{
	struct NamedPayload
	{
		std::int32_t count = 0;
		HICAL_JSON(NamedPayload, count)
	};
} // namespace contract_test

HICAL_SCHEMA_NAME(contract_test::NamedPayload, "NamedPayload")

namespace
{
	using namespace hical;
	using namespace hical::example::contract;
	namespace openapi = hical::meta::openapi;

	struct State
	{
		std::int32_t count = 0;
	};

	struct Payload
	{
		std::int32_t count;
		HICAL_JSON(Payload, count)
	};

	Payload readCount(State& state)
	{
		return {state.count};
	}

	Payload setCount(State& state, const Payload& body)
	{
		state.count = body.count;
		return {state.count};
	}

	std::optional<Payload> findCount(State& state, RoomId id)
	{
		if (id.value != state.count)
		{
			return std::nullopt;
		}
		return Payload {state.count};
	}

	bool preservesNamedSchema()
	{
		using contract_test::NamedPayload;
		State state;
		Router router;
		openapi::OpenApiRegistry registry;
		const auto output = openapi::outputSchema<NamedPayload>();
		if (output == openapi::inputSchema<NamedPayload>())
		{
			return false;
		}
		registerEndpoint(
			router,
			registry,
			{HttpMethod::hGet, "/named", "readNamed", HttpStatusCode::hOk},
			state,
			+[](State&)
			{
				return NamedPayload {};
			});
		try
		{
			registerEndpoint(
				router,
				registry,
				{HttpMethod::hPost, "/named", "writeNamed", HttpStatusCode::hOk},
				state,
				+[](State&, const NamedPayload& input)
				{
					return input;
				});
		}
		catch (const std::logic_error&)
		{
			return registry.schemas().at("NamedPayload") == output && registry.routes().size() == 1;
		}
		return false;
	}

	struct SyncCase
	{
		HttpMethod method;
		const char* path;
		HttpStatusCode status;
		const char* body = "";
		std::string_view expected;
	};

	bool checkSync(Router& router, const SyncCase& test)
	{
		HttpRequest request;
		request.setMethod(test.method);
		request.setTarget(test.path);
		request.setBody(test.body);
		const auto resolved = router.resolveRoute(request);
		auto response = router.dispatchSyncResolved(request, resolved);
		if (!resolved.isMatch() || !response || response->statusCode() != test.status)
		{
			HICAL_LOG_ERROR("同步分派的响应与预期不符，路径为 {}", test.path);
			return false;
		}
		const auto json = boost::json::parse(response->body());
		if (test.expected.empty())
		{
			return json.as_object().at("message").is_string();
		}
		return json == boost::json::parse(test.expected);
	}
} // namespace

int main()
{
	State state;
	Router router;
	auto registry = std::make_shared<openapi::OpenApiRegistry>();
	registerEndpoint(router,
					 *registry,
					 {HttpMethod::hPost, "/counts", "setCount", HttpStatusCode::hCreated},
					 state,
					 setCount);
	registerEndpoint(router,
					 *registry,
					 {HttpMethod::hGet, "/counts", "readCount", HttpStatusCode::hOk},
					 state,
					 readCount);
	registerEndpoint(router,
					 *registry,
					 {HttpMethod::hGet, "/counts/{id}", "findCount", HttpStatusCode::hOk},
					 state,
					 findCount);

	const bool requestsPassed =
		checkSync(router, {HttpMethod::hPost, "/counts", HttpStatusCode::hCreated, R"({"count":3})", R"({"count":3})"})
		&& checkSync(router, {HttpMethod::hPost, "/counts", HttpStatusCode::hBadRequest, R"({"count":"x"})"})
		&& checkSync(router, {HttpMethod::hGet, "/counts", HttpStatusCode::hOk, "", R"({"count":3})"})
		&& checkSync(router, {HttpMethod::hGet, "/counts/3", HttpStatusCode::hOk, "", R"({"count":3})"})
		&& checkSync(router, {HttpMethod::hGet, "/counts/3x", HttpStatusCode::hBadRequest})
		&& checkSync(router, {HttpMethod::hGet, "/counts/4", HttpStatusCode::hNotFound});

	openapi::OpenApiDocument document(registry);
	const auto json = boost::json::parse(document.generateString());
	const auto& paths = json.as_object().at("paths").as_object();
	const auto& post = paths.at("/counts").as_object().at("post").as_object();
	const auto& get = paths.at("/counts/{id}").as_object().at("get").as_object();
	const auto& parameter = get.at("parameters").as_array().at(0).as_object();
	const bool docsPassed = post.at("operationId") == "setCount" && post.at("responses").as_object().contains("201")
							&& post.at("requestBody").as_object().at("required") == true && parameter.at("name") == "id"
							&& parameter.at("schema").as_object().at("type") == "integer"
							&& get.at("responses").as_object().contains("404");
	if (!requestsPassed || !docsPassed || !preservesNamedSchema())
	{
		HICAL_LOG_ERROR("同步分派或 OpenAPI 文档检查失败");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
