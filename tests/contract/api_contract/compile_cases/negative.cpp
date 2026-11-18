/**
 * @file negative.cpp
 * @brief 检查哪些函数签名会在编译时被接口适配器拒绝
 */

#include "Endpoint.h"

namespace
{
	using namespace hical;
	using namespace hical::example::contract;

	struct State
	{
	};

	struct Body
	{
		std::int32_t count;
		HICAL_JSON(Body, count)
	};

	template <typename Handler>
	concept CanRegister =
		requires(Router& router, meta::openapi::OpenApiRegistry& registry, State& state, Handler handler) {
			registerEndpoint(router, registry, EndpointSpec {}, state, handler);
		};

	static_assert(!CanRegister<Awaitable<Body> (*)(State&)>);
	static_assert(!CanRegister<HttpResponse (*)(State&)>);
	static_assert(!CanRegister<Body (*)(State&, Body&)>);
	static_assert(!CanRegister<Body (*)(State&, RoomId)>);
	static_assert(!CanRegister<Body (*)(State&, const Body&, std::int32_t)>);
} // namespace
