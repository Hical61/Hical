#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpTypes.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hical
{

	/**
 * @brief 静态文件服务
 *
 * 提供 serveStatic() 工厂函数，将指定 URL 前缀映射到本地目录，
 * 返回可直接注册到 Router 的 SyncRouteHandler。
 *
 * 用法：
 * ```cpp
 * // 将 /static/... 映射到 ./public 目录
 * server.router().get("/static/{file}", hical::serveStatic("./public", "/static/"));
 * ```
 *
 * 安全：内置路径遍历攻击防护（阻止 "../" 跳出根目录）。
 * MIME：根据文件扩展名自动推断 Content-Type。
 * 缓存：支持 ETag / Last-Modified / 304 Not Modified。
 */

	namespace detail
	{

		/**
 * @brief 根据文件扩展名返回 MIME 类型
 * @param ext 文件扩展名（含 "."，如 ".html"）
 * @return MIME 类型字符串
 */
		inline std::string mimeType(const std::string& ext)
		{
			static const std::unordered_map<std::string, std::string> table = {
				{".html", "text/html; charset=utf-8"},
				{".htm", "text/html; charset=utf-8"},
				{".css", "text/css; charset=utf-8"},
				{".js", "application/javascript; charset=utf-8"},
				{".mjs", "application/javascript; charset=utf-8"},
				{".json", "application/json; charset=utf-8"},
				{".xml", "application/xml; charset=utf-8"},
				{".txt", "text/plain; charset=utf-8"},
				{".md", "text/markdown; charset=utf-8"},
				{".svg", "image/svg+xml"},
				{".png", "image/png"},
				{".jpg", "image/jpeg"},
				{".jpeg", "image/jpeg"},
				{".gif", "image/gif"},
				{".webp", "image/webp"},
				{".ico", "image/x-icon"},
				{".woff", "font/woff"},
				{".woff2", "font/woff2"},
				{".ttf", "font/ttf"},
				{".otf", "font/otf"},
				{".pdf", "application/pdf"},
				{".zip", "application/zip"},
				{".gz", "application/gzip"},
				{".mp4", "video/mp4"},
				{".webm", "video/webm"},
				{".mp3", "audio/mpeg"},
				{".wav", "audio/wav"},
			};

			auto it = table.find(ext);
			return (it != table.end()) ? it->second : "application/octet-stream";
		}

		/**
 * @brief 检查路径是否试图跳出根目录（路径遍历攻击防护）
 * @param root 根目录规范路径
 * @param target 目标文件规范路径
 * @return true 表示安全
 */
		inline bool isSafePath(const std::filesystem::path& root, const std::filesystem::path& target)
		{
			// 逐段迭代器比较：root 的每个路径分量必须是 target 的前缀
			// 比字符串前缀比对更可靠，不受 /pub vs /public 等 edge case 影响
			auto rootIt = root.begin();
			auto targetIt = target.begin();
			for (; rootIt != root.end(); ++rootIt, ++targetIt)
			{
				if (targetIt == target.end() || *rootIt != *targetIt)
				{
					return false;
				}
			}
			return true;
		}

		/**
 * @brief 生成简单的 ETag（基于文件大小 + 最后修改时间）
 * @param fileSize 文件大小（字节）
 * @param lastWrite 最后修改时间
 * @return ETag 字符串（带引号，符合 RFC 7232）
 */
		inline std::string makeEtag(std::uintmax_t fileSize, std::filesystem::file_time_type lastWrite)
		{
			auto ns = lastWrite.time_since_epoch().count();
			return "\"" + std::to_string(fileSize) + "-" + std::to_string(ns) + "\"";
		}

	} // namespace detail

	/**
 * @brief 创建静态文件服务处理器
 *
 * @param rootDir     本地目录路径（如 "./public"）
 * @param urlPrefix   URL 前缀（如 "/static/"），用于从请求路径中去除前缀得到相对路径
 * @param maxFileSize 单文件最大字节数（默认 64MB），超出返回 413
 * @return SyncRouteHandler 可直接注册到 Router 的处理器
 *
 * 示例：
 * ```cpp
 * // 注册通配路由：/static/{path} -> ./public/{path}
 * server.router().get("/static/{path}", hical::serveStatic("./public", "/static/"));
 * ```
 *
 * 支持功能：
 * - MIME 自动推断
 * - 目录默认文件（index.html）
 * - ETag 缓存验证（304 Not Modified）
 * - 路径遍历攻击防护
 * - 大文件限制（防止 bad_alloc 崩溃）
 */
	inline std::function<HttpResponse(const HttpRequest&)> serveStatic(const std::string& rootDir,
																	   const std::string& urlPrefix,
																	   std::uintmax_t maxFileSize = 64ULL * 1024 * 1024)
	{
		namespace fs = std::filesystem;

		// 提前规范化根目录路径（只做一次）
		std::error_code ec;
		fs::path root = fs::canonical(rootDir, ec);
		if (ec)
		{
			// 根目录不存在时，每次请求都返回 404
			return [rootDir](const HttpRequest&) -> HttpResponse
			{
				return HttpResponse::notFound();
			};
		}

		return [root, urlPrefix, maxFileSize](const HttpRequest& req) -> HttpResponse
		{
			namespace fs = std::filesystem;

			// 从请求路径中去除 URL 前缀，得到相对文件路径
			std::string reqPath(req.path());
			std::string_view relPath = reqPath;
			if (relPath.substr(0, urlPrefix.size()) == urlPrefix)
			{
				relPath = relPath.substr(urlPrefix.size());
			}
			else if (!urlPrefix.empty() && relPath == urlPrefix.substr(0, urlPrefix.size() - 1))
			{
				// 访问 /static（无尾部斜杠）
				relPath = "";
			}

			// 构建目标路径并规范化（使用 canonical 完全解析符号链接，防止路径遍历）
			fs::path target = root / std::string(relPath);
			std::error_code ec2;
			target = fs::canonical(target, ec2);
			if (ec2)
			{
				return HttpResponse::notFound();
			}

			// 路径遍历防护
			if (!detail::isSafePath(root, target))
			{
				HttpResponse res;
				res.setStatus(HttpStatusCode::hForbidden);
				res.setBody("403 Forbidden");
				return res;
			}

			// 目录处理：尝试 index.html
			if (fs::is_directory(target, ec2))
			{
				target /= "index.html";
				// 追加后重新 canonical 解析（防止 index.html 是指向 root 外的符号链接）
				target = fs::canonical(target, ec2);
				if (ec2 || !detail::isSafePath(root, target))
				{
					return HttpResponse::notFound();
				}
			}

			// 文件存在性检查
			if (!fs::is_regular_file(target, ec2))
			{
				return HttpResponse::notFound();
			}

			// 获取文件元信息
			auto fileSize = fs::file_size(target, ec2);
			if (ec2)
			{
				return HttpResponse::serverError();
			}

			// 大文件限制：防止 bad_alloc 崩溃
			if (fileSize > maxFileSize)
			{
				HttpResponse res;
				res.setStatus(HttpStatusCode::hPayloadTooLarge);
				res.setBody("413 File Too Large");
				return res;
			}

			auto lastWrite = fs::last_write_time(target, ec2);
			if (ec2)
			{
				return HttpResponse::serverError();
			}

			// ETag 缓存验证
			std::string etag = detail::makeEtag(fileSize, lastWrite);
			std::string ifNoneMatch = req.header("If-None-Match");
			if (!ifNoneMatch.empty() && ifNoneMatch == etag)
			{
				HttpResponse res;
				res.setStatus(HttpStatusCode::hNotModified);
				res.setHeader("ETag", etag);
				// 304 无 body，需手动 prepare_payload 使 Content-Length 为 0
				res.native().prepare_payload();
				return res;
			}

			// 读取文件内容
			std::ifstream ifs(target, std::ios::binary);
			if (!ifs)
			{
				return HttpResponse::serverError();
			}
			std::string content(fileSize, '\0');
			ifs.read(content.data(), static_cast<std::streamsize>(fileSize));
			auto bytesRead = ifs.gcount();
			// 文件在 stat 和 read 之间被截短时，截断到实际读取长度，拒绝返回零填充内容
			if (bytesRead <= 0)
			{
				return HttpResponse::serverError();
			}
			if (static_cast<std::uintmax_t>(bytesRead) < fileSize)
			{
				content.resize(static_cast<std::size_t>(bytesRead));
			}

			// 构建响应
			std::string ext = target.extension().string();
			HttpResponse res;
			res.setStatus(HttpStatusCode::hOk);
			res.setBody(content, detail::mimeType(ext));
			res.setHeader("ETag", etag);
			return res;
		};
	}

} // namespace hical
