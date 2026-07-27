/**
 * @file StaticFiles.h
 * @brief 异步静态文件服务（ETag/Range/缓存）
 */

#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpTypes.h"
#include "Coroutine.h"
#ifdef BOOST_ASIO_HAS_FILE
	#include <boost/asio/random_access_file.hpp>
#endif
#include <boost/asio/use_awaitable.hpp>
#include <charconv>
#include <fstream>
#include <filesystem>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hical
{

	/**
	 * @brief 静态文件服务
	 * 提供 serveStatic() 工厂函数，将指定 URL 前缀映射到本地目录，
	 * 返回可直接注册到 Router 的 SyncRouteHandler。
	 * 用法：
	 * ```cpp
	 * // 将 /static/... 映射到 ./public 目录
	 * server.router().get("/static/{file}", hical::serveStatic("./public", "/static/"));
	 * ```
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
		[[nodiscard]] inline std::string mimeType(const std::string& ext)
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

			if (auto it = table.find(ext); it != table.end())
			{
				return it->second;
			}
			return "application/octet-stream";
		}

		/**
		 * @brief 判断 MIME 是否为可压缩文本类型
		 * 用于 Static handler 决定用 setBody（内存读取，可被 gzip 压缩）
		 * 还是 setFileBody（异步文件发送，gzip 自然跳过）。
		 * @param mime 完整的 MIME 字符串（可含 charset）
		 * @return true 表示适合走内存读取 + gzip 压缩路径
		 */
		[[nodiscard]] inline bool isTextMime(std::string_view mime)
		{
			if (mime.empty())
			{
				return false;
			}
			auto semi = mime.find(';');
			auto mediaType = (semi != std::string_view::npos) ? mime.substr(0, semi) : mime;

			if (mediaType.starts_with("text/"))
			{
				return true;
			}
			if (mediaType.starts_with("application/json"))
			{
				return true;
			}
			if (mediaType.starts_with("application/javascript"))
			{
				return true;
			}
			if (mediaType.starts_with("application/xml"))
			{
				return true;
			}
			if (mediaType.starts_with("image/svg+xml"))
			{
				return true;
			}
			if (mediaType.starts_with("application/xhtml+xml"))
			{
				return true;
			}
			return false;
		}

		/**
		 * @brief 检查路径是否试图跳出根目录（路径遍历攻击防护）
		 * @param root 根目录规范路径
		 * @param target 目标文件规范路径
		 * @return true 表示安全
		 */
		[[nodiscard]] inline bool isSafePath(const std::filesystem::path& root, const std::filesystem::path& target)
		{
			// 按路径分量逐段比，比字符串前缀靠谱（/pub vs /public）
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
		[[nodiscard]] inline std::string makeEtag(std::uintmax_t fileSize, std::filesystem::file_time_type lastWrite)
		{
			auto ns = lastWrite.time_since_epoch().count();
			return "\"" + std::to_string(fileSize) + "-" + std::to_string(ns) + "\"";
		}

		/**
		 * @brief Range 请求解析结果
		 */
		struct ByteRange
		{
			int64_t start;
			int64_t end; // inclusive
		};

		/**
		 * @brief 解析 Range 请求头
		 * 支持格式："bytes=0-499" / "bytes=500-" / "bytes=-500"
		 * @param header Range 头部值
		 * @param fileSize 文件总大小
		 * @param isMultiRange 输出参数，true 表示检测到 multi-range（含逗号）
		 * @return 解析后的字节范围，nullopt 表示无效
		 */
		[[nodiscard]] inline std::optional<ByteRange> parseByteRange(std::string_view header,
																	 int64_t fileSize,
																	 bool& isMultiRange)
		{
			isMultiRange = false;

			// 必须以 "bytes=" 开头
			if (header.size() < 7 || header.substr(0, 6) != "bytes=")
			{
				return std::nullopt;
			}

			auto rangeSpec = header.substr(6);

			// Multi-range 检测（含逗号）
			if (rangeSpec.find(',') != std::string_view::npos)
			{
				isMultiRange = true;
				return std::nullopt;
			}

			// 找 '-' 分隔符
			auto dashPos = rangeSpec.find('-');
			if (dashPos == std::string_view::npos)
			{
				return std::nullopt;
			}

			auto startStr = rangeSpec.substr(0, dashPos);
			auto endStr = rangeSpec.substr(dashPos + 1);

			int64_t start = 0;
			int64_t end = fileSize - 1;

			if (startStr.empty())
			{
				// "bytes=-500" 后缀格式：最后 N 个字节
				if (endStr.empty())
				{
					return std::nullopt;
				}
				int64_t suffixLen = 0;
				auto [ptr, ec] = std::from_chars(endStr.data(), endStr.data() + endStr.size(), suffixLen);
				if (ec != std::errc {} || suffixLen <= 0)
				{
					return std::nullopt;
				}
				if (suffixLen >= fileSize)
				{
					start = 0;
				}
				else
				{
					start = fileSize - suffixLen;
				}
				end = fileSize - 1;
			}
			else
			{
				// "bytes=500-" 或 "bytes=0-499"
				auto [ptr1, ec1] = std::from_chars(startStr.data(), startStr.data() + startStr.size(), start);
				if (ec1 != std::errc {})
				{
					return std::nullopt;
				}

				if (!endStr.empty())
				{
					auto [ptr2, ec2] = std::from_chars(endStr.data(), endStr.data() + endStr.size(), end);
					if (ec2 != std::errc {})
					{
						return std::nullopt;
					}
				}
				// else: open-end "bytes=500-", end 保持 fileSize - 1
			}

			// Clamp end
			if (end >= fileSize)
			{
				end = fileSize - 1;
			}

			// 校验
			if (start < 0 || start > end || start >= fileSize)
			{
				return std::nullopt;
			}

			return ByteRange {start, end};
		}

	} // namespace detail

	/**
	 * @brief 创建静态文件服务处理器
	 * @param rootDir     本地目录路径（如 "./public"）
	 * @param urlPrefix   URL 前缀（如 "/static/"），用于从请求路径中去除前缀得到相对路径
	 * @param maxFileSize 单文件最大字节数（默认 64MB），超出返回 413
	 * @return SyncRouteHandler 可直接注册到 Router 的处理器
	 * 示例：
	 * ```cpp
	 * // 注册通配路由：/static/{path} -> ./public/{path}
	 * server.router().get("/static/{path}", hical::serveStatic("./public", "/static/"));
	 * ```
	 * 支持功能：
	 * - MIME 自动推断
	 * - 目录默认文件（index.html）
	 * - ETag 缓存验证（304 Not Modified）
	 * - 路径遍历攻击防护
	 * - 大文件限制（防止 bad_alloc 崩溃）
	 */
	[[nodiscard]] inline std::function<Awaitable<HttpResponse>(const HttpRequest&)> serveStatic(
		const std::string& rootDir,
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
			return [rootDir](const HttpRequest&) -> Awaitable<HttpResponse>
			{
				co_return HttpResponse::notFound();
			};
		}

		// canonical 缓存（LRU），省掉每次请求的 syscall
		struct CacheEntry
		{
			std::string key;
			fs::path canonical;
			std::chrono::steady_clock::time_point cachedAt;
		};

		struct PathCache
		{
			mutable std::shared_mutex mutex;
			std::list<CacheEntry> lruList;                                          // 头部 = 最近使用
			std::unordered_map<std::string, std::list<CacheEntry>::iterator> index; // key → 链表迭代器
		};

		static constexpr size_t kMaxPathCacheEntries = 4096;
		static constexpr auto kCacheTtl = std::chrono::seconds(60);
		auto pathCache = std::make_shared<PathCache>();

		return [root, urlPrefix, maxFileSize, pathCache](const HttpRequest& req) -> Awaitable<HttpResponse>
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
				relPath = "";
			}

			// 构建目标路径并规范化（使用 LRU 缓存避免每次请求都调用 canonical 系统调用）
			std::string relPathStr(relPath);
			fs::path target;
			bool cacheHit = false;

			// 读路径：shared_lock
			{
				std::shared_lock<std::shared_mutex> readLock(pathCache->mutex);
				auto indexIt = pathCache->index.find(relPathStr);
				if (indexIt != pathCache->index.end())
				{
					auto age = std::chrono::steady_clock::now() - indexIt->second->cachedAt;
					if (age < kCacheTtl)
					{
						target = indexIt->second->canonical;
						cacheHit = true;
					}
				}
			}

			// 命中但需要提升 LRU 位置（写锁，O(1) splice）
			if (cacheHit)
			{
				std::unique_lock<std::shared_mutex> writeLock(pathCache->mutex);
				auto indexIt = pathCache->index.find(relPathStr);
				if (indexIt != pathCache->index.end())
				{
					pathCache->lruList.splice(pathCache->lruList.begin(), pathCache->lruList, indexIt->second);
				}
			}

			if (!cacheHit)
			{
				fs::path rawTarget = root / relPathStr;
				std::error_code ec2;
				target = fs::canonical(rawTarget, ec2);
				if (ec2)
				{
					co_return HttpResponse::notFound();
				}

				// 插入缓存（写锁）
				{
					std::unique_lock<std::shared_mutex> writeLock(pathCache->mutex);

					// Double-check：其他线程可能已在我们等待写锁期间插入了同一 key
					auto existIt = pathCache->index.find(relPathStr);
					if (existIt != pathCache->index.end())
					{
						// 已存在：更新并提升到头部
						existIt->second->canonical = target;
						existIt->second->cachedAt = std::chrono::steady_clock::now();
						pathCache->lruList.splice(pathCache->lruList.begin(), pathCache->lruList, existIt->second);
					}
					else
					{
						// 驱逐：缓存已满时弹出尾部（最久未使用），O(1)
						if (pathCache->index.size() >= kMaxPathCacheEntries)
						{
							auto& victim = pathCache->lruList.back();
							pathCache->index.erase(victim.key);
							pathCache->lruList.pop_back();
						}

						// 插入头部
						pathCache->lruList.push_front(
							CacheEntry {relPathStr, target, std::chrono::steady_clock::now()});
						pathCache->index.emplace(relPathStr, pathCache->lruList.begin());
					}
				}
			}

			// 路径遍历防护
			if (!detail::isSafePath(root, target))
			{
				HttpResponse res;
				res.setStatus(HttpStatusCode::hForbidden);
				res.setBody("403 Forbidden", "text/plain");
				co_return res;
			}

			// 目录处理：尝试 index.html
			std::error_code ec2;
			if (fs::is_directory(target, ec2))
			{
				fs::path indexTarget = target / "index.html";
				indexTarget = fs::canonical(indexTarget, ec2);
				if (ec2 || !detail::isSafePath(root, indexTarget))
				{
					co_return HttpResponse::notFound();
				}
				target = indexTarget;
			}

			// 文件存在性检查
			if (!fs::is_regular_file(target, ec2))
			{
				co_return HttpResponse::notFound();
			}

			// 获取文件元信息
			auto fileSize = fs::file_size(target, ec2);
			if (ec2)
			{
				co_return HttpResponse::serverError();
			}

			// 大文件限制
			if (fileSize > maxFileSize)
			{
				HttpResponse res;
				res.setStatus(HttpStatusCode::hPayloadTooLarge);
				res.setBody("413 File Too Large", "text/plain");
				co_return res;
			}

			auto lastWrite = fs::last_write_time(target, ec2);
			if (ec2)
			{
				co_return HttpResponse::serverError();
			}

			// ETag 缓存验证
			std::string etag = detail::makeEtag(fileSize, lastWrite);
			auto ifNoneMatch = req.header("If-None-Match");
			if (!ifNoneMatch.empty() && ifNoneMatch == etag)
			{
				HttpResponse res;
				res.setStatus(HttpStatusCode::hNotModified);
				res.setHeader("ETag", etag);
				res.native().preparePayload();
				co_return res;
			}

			// Range 请求处理
			std::string ext = target.extension().string();
			std::string mime = detail::mimeType(ext);
			auto rangeHeader = req.header("Range");
			if (!rangeHeader.empty())
			{
				// If-Range 条件检查：ETag 不匹配时忽略 Range，返回 200 全量
				auto ifRange = req.header("If-Range");
				bool rangeValid = ifRange.empty() || ifRange == etag;

				if (rangeValid)
				{
					bool isMultiRange = false;
					auto range = detail::parseByteRange(rangeHeader, static_cast<int64_t>(fileSize), isMultiRange);

					if (!range.has_value() && !isMultiRange)
					{
						// 无效 Range → 416
						co_return HttpResponse::rangeNotSatisfiable(fileSize);
					}

					if (range.has_value())
					{
						int64_t rangeLen = range->end - range->start + 1;

						// Content-Range 头
						std::string contentRange = "bytes " + std::to_string(range->start) + "-"
												   + std::to_string(range->end) + "/" + std::to_string(fileSize);

						// 小范围（≤ 4MB）：内联读入内存
						static constexpr int64_t kInlineThreshold = 4 * 1024 * 1024;
						if (rangeLen <= kInlineThreshold)
						{
							std::string content(static_cast<size_t>(rangeLen), '\0');
							size_t totalRead = 0;

#ifdef BOOST_ASIO_HAS_FILE
							auto executor = co_await boost::asio::this_coro::executor;
							boost::asio::random_access_file file(executor,
																 target.string(),
																 boost::asio::random_access_file::read_only);
							while (totalRead < static_cast<size_t>(rangeLen))
							{
								auto bytesRead = co_await file.async_read_some_at(
									static_cast<uint64_t>(range->start) + totalRead,
									boost::asio::buffer(content.data() + totalRead,
														static_cast<size_t>(rangeLen) - totalRead),
									boost::asio::use_awaitable);
								if (bytesRead == 0)
								{
									break;
								}
								totalRead += bytesRead;
							}
#else
							{
								std::ifstream ifs(target, std::ios::binary);
								if (ifs)
								{
									ifs.seekg(range->start);
									ifs.read(content.data(), rangeLen);
									totalRead = static_cast<size_t>(ifs.gcount());
								}
							}
#endif
							if (totalRead < static_cast<size_t>(rangeLen))
							{
								content.resize(totalRead);
							}

							HttpResponse res;
							res.setStatus(HttpStatusCode::hPartialContent);
							res.setBody(std::move(content), mime);
							res.setHeader("Content-Range", contentRange);
							res.setHeader("Accept-Ranges", "bytes");
							res.setHeader("ETag", etag);
							res.setHeader("X-Content-Type-Options", "nosniff");
							co_return res;
						}
						else
						{
							// 大范围：FileBody 延迟发送
							HttpResponse res;
							res.setStatus(HttpStatusCode::hPartialContent);
							res.setFileBody(target, range->start, rangeLen, mime);
							res.setHeader("Content-Range", contentRange);
							res.setHeader("Accept-Ranges", "bytes");
							res.setHeader("ETag", etag);
							res.setHeader("X-Content-Type-Options", "nosniff");
							co_return res;
						}
					}
					// isMultiRange && !range → fall through 到 200 全量
				}
				// If-Range 不匹配 → fall through 到 200 全量
			}

			// 200 全量响应：二进制文件走 FileBody 异步发送，文本文件走内存读取
			// Gzip 中间件通过 isCompressible() MIME 过滤 + !hasFileBody() 双重保护，无需按大小切分

			if (!detail::isTextMime(mime))
			{
				// 二进制文件（webp/woff2/mp4 等）：FileBody 异步发送，gzip 自然跳过
				HttpResponse res;
				res.setStatus(HttpStatusCode::hOk);
				res.setFileBody(target, 0, static_cast<int64_t>(fileSize), mime);
				res.setHeader("Accept-Ranges", "bytes");
				res.setHeader("ETag", etag);
				res.setHeader("X-Content-Type-Options", "nosniff");
				co_return res;
			}

			// 文本文件：读入内存，Gzip 中间件可以压缩
			std::string content(fileSize, '\0');
			size_t totalRead = 0;

#ifdef BOOST_ASIO_HAS_FILE
			// 异步读取（不阻塞 io_context 线程）
			auto executor = co_await boost::asio::this_coro::executor;
			boost::asio::random_access_file file(executor, target.string(), boost::asio::random_access_file::read_only);

			while (totalRead < fileSize)
			{
				auto bytesRead = co_await file.async_read_some_at(
					totalRead,
					boost::asio::buffer(content.data() + totalRead, fileSize - totalRead),
					boost::asio::use_awaitable);
				if (bytesRead == 0)
				{
					break;
				}
				totalRead += bytesRead;
			}
#else
			// 同步 ifstream 回退（macOS 等不支持 BOOST_ASIO_HAS_FILE 的平台）
			{
				std::ifstream ifs(target, std::ios::binary);
				if (!ifs)
				{
					co_return HttpResponse::serverError();
				}
				ifs.read(content.data(), static_cast<std::streamsize>(fileSize));
				totalRead = static_cast<size_t>(ifs.gcount());
			}
#endif

			if (totalRead == 0)
			{
				co_return HttpResponse::serverError();
			}
			if (totalRead < fileSize)
			{
				content.resize(totalRead);
			}

			// 构建响应
			HttpResponse res;
			res.setStatus(HttpStatusCode::hOk);
			res.setBody(std::move(content), mime);
			res.setHeader("Accept-Ranges", "bytes");
			res.setHeader("ETag", etag);
			res.setHeader("X-Content-Type-Options", "nosniff");
			co_return res;
		};
	}

} // namespace hical
