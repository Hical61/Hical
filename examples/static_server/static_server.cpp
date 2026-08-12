/**
 * @file static_server.cpp
 * @brief 静态文件服务示例，集成 translate.js 多语言插件
 */

#include "core/HttpServer.h"
#include "core/StaticFiles.h"
#include <cstdlib>
#include <iostream>

using namespace hical;

int main(int argc, char* argv[])
{
	try
	{
		auto port = static_cast<uint16_t>(argc >= 2 ? std::atoi(argv[1]) : 8080);

		// public 目录路径由 CMake 在编译期写入宏定义
		// 如果命令行传了第二个参数，可以手动覆盖
		const char* publicDir = STATIC_SERVER_PUBLIC_DIR;
		if (argc >= 3)
		{
			publicDir = argv[2];
		}

		HttpServer server(port);

		// 日志中间件：打印请求方法和路径
		server.use(
			[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
			{
				std::cout << httpMethodToString(req.method()) << " " << req.path() << std::endl;
				co_return co_await next(req);
			});

		// 静态文件服务：将 public/ 目录映射到 /
		// 使用 *path 通配路由支持多层路径（如 /css/style.css）
		server.router().get("/*path", serveStatic(publicDir, "/"));

		std::cout << "Hical 静态文件服务" << std::endl;
		std::cout << "监听端口: " << port << std::endl;
		std::cout << "服务目录: " << publicDir << std::endl;
		std::cout << "访问 http://localhost:" << port << " 查看首页" << std::endl;

		server.start();
	}
	catch (const std::exception& e)
	{
		std::cerr << "异常: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}
