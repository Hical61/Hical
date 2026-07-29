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
#include <chrono>
#include <fstream>
#include <filesystem>
#include <functional>
#include <list>
#include <optional>
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
		 * Static handler 据此决定用 setBody（内存读取，两次协程挂起，一次 scatter-gather 写出）
		 * 还是 setFileBody（异步流式发送，gzip 自然跳过）。
		 * @param mime 完整的 MIME 字符串（可含 charset）
		 * @return true 表示适合走内存读取路径
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

		/**
		 * @brief 每线程独立的 PathCache（LRU），无锁访问。
		 * 单线程 io_context + 协程协作式调度保证同一线程无并发访问。
		 * 单容量 64 条目，64 核总容量 ~4096，与旧全局缓存规模持平。
		 * 旧 shared_mutex 方案在高并发下（4096c+）引发跨核缓存行弹跳，
		 * 导致 static/4096 -38%、static/6800 -48% 退化。
		 */
		struct TlPathCache
		{
			static constexpr size_t kMaxEntries = 64;
			static constexpr auto kTtl = std::chrono::seconds(60);

			struct Entry
			{
				std::string key;
				std::filesystem::path canonical;
				std::chrono::steady_clock::time_point cachedAt;
			};

			std::list<Entry> lruList;
			std::unordered_map<std::string, std::list<Entry>::iterator> index;
		};

		/// @brief 返回当前线程的 PathCache
		[[nodiscard]] inline TlPathCache& getTlPathCache()
		{
			static thread_local TlPathCache cache;
			return cache;
		}

		/**
		 * @brief 每线程独立的文件内容缓存（LRU），无锁访问。
		 * 将小文件（≤512KB）的 body 和预计算 ETag 缓存在内存中，
		 * 热路径命中时跳过 open/read/close syscall，只做 memcpy + async_write。
		 * 单线程 io_context + 协程协作式调度保证同一线程无并发访问。
		 */
		struct TlContentCache
		{
			static constexpr size_t kMaxEntries = 32;
			static constexpr size_t kMaxFileSize = 512 * 1024;
			static constexpr auto kTtl = std::chrono::seconds(10);

			struct Entry
			{
				std::string key;     // canonical path，驱逐时从 index 反查
				std::string content; // 文件 body 内容（已读入内存）
				std::string etag;    // 预计算的 ETag
				int64_t mtimeNs = 0; // 文件修改时间（纳秒），TTL 过期后检测变更
				std::chrono::steady_clock::time_point cachedAt;
			};

			std::list<Entry> lruList;
			std::unordered_map<std::string, std::list<Entry>::iterator> index;
		};

		/// @brief 返回当前线程的文件内容缓存
		[[nodiscard]] inline TlContentCache& getTlContentCache()
		{
			static thread_local TlContentCache cache;
			return cache;
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

		return [root, urlPrefix, maxFileSize](const HttpRequest& req) -> Awaitable<HttpResponse>
		{
			namespace fs = std::filesystem;

			// 从请求路径中去除 URL 前缀，得到相对文件路径
			std::string reqPath(req.path());
			std::string_view relPathView = reqPath;
			if (relPathView.substr(0, urlPrefix.size()) == urlPrefix)
			{
				relPathView = relPathView.substr(urlPrefix.size());
			}
			else if (!urlPrefix.empty() && relPathView == urlPrefix.substr(0, urlPrefix.size() - 1))
			{
				relPathView = "";
			}

			// 构建目标路径并规范化（thread_local LRU 缓存，无锁，省掉每次请求的 canonical 系统调用）
			// 缓存 key = root.string() + "\x01" + relPath，不同 serveStatic handler 空间隔离
			std::string relPath(relPathView);
			std::string cacheKey = root.string();
			cacheKey += '\x01';
			cacheKey += relPath;
			fs::path target;
			bool cacheHit = false;

			{
				auto& cache = detail::getTlPathCache();
				auto it = cache.index.find(cacheKey);
				if (it != cache.index.end())
				{
					auto age = std::chrono::steady_clock::now() - it->second->cachedAt;
					if (age < detail::TlPathCache::kTtl)
					{
						target = it->second->canonical;
						cacheHit = true;
						// LRU 提升：把该条目移到链表头部（无锁，线程独占）
						cache.lruList.splice(cache.lruList.begin(), cache.lruList, it->second);
					}
				}

				if (!cacheHit)
				{
					fs::path rawTarget = root / relPath;
					std::error_code ec2;
					target = fs::canonical(rawTarget, ec2);
					if (ec2)
					{
						co_return HttpResponse::notFound();
					}

					// 插入缓存（线程独占，无需 double-check—canonical() 之间无 co_await）
					if (cache.index.size() >= detail::TlPathCache::kMaxEntries)
					{
						auto& victim = cache.lruList.back();
						cache.index.erase(victim.key);
						cache.lruList.pop_back();
					}
					cache.lruList.push_front(
						detail::TlPathCache::Entry {cacheKey, target, std::chrono::steady_clock::now()});
					cache.index.emplace(cacheKey, cache.lruList.begin());
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

			// MIME 类型提前确定——文本/二进制的分支和缓存逻辑都依赖它
			std::string ext = target.extension().string();
			std::string mime = detail::mimeType(ext);
			bool isText = detail::isTextMime(mime);

			// 文本文件 + 非 Range 请求：先查内容缓存（在 fstat 之前），
			// 命中时跳过 file_size/last_write_time 两个系统调用，直接走 memcpy + async_write。
			// 二进制文件和 Range 请求跳过缓存——二进制走 setFileBody 流式发送，
			// Range 是部分内容，缓存全量 body 后用不上。
			if (isText && req.header("Range").empty())
			{
				auto& cc = detail::getTlContentCache();
				auto it = cc.index.find(target.string());

				if (it != cc.index.end())
				{
					bool servedFromCache = false;
					auto now = std::chrono::steady_clock::now();
					auto age = now - it->second->cachedAt;

					if (age < detail::TlContentCache::kTtl)
					{
						// TTL 未过期——直接信任缓存。
						// 注：10 秒窗口内如果文件被外部修改，缓存不会感知。
						// 对生产环境的只读静态文件这不是问题，和 CDN 边缘缓存行为一致。
						servedFromCache = true;
					}
					else
					{
						// TTL 过期，fstat 检查 mtime 是否变化
						std::error_code ec3;
						auto lw = fs::last_write_time(target, ec3);
						int64_t currentNs = lw.time_since_epoch().count();
						if (!ec3 && currentNs == it->second->mtimeNs)
						{
							// 文件没变，更新 cachedAt 续期
							it->second->cachedAt = now;
							servedFromCache = true;
						}
						else
						{
							// mtime 变了或 fstat 失败，移除旧条目。
							// 先把 list 迭代器取出来——index 的 key 和 it->first 是同一个，
							// erase(index) 会把 it 指向的 hash node 干掉，it 立刻失效。
							auto listIt = it->second;
							cc.index.erase(it);
							cc.lruList.erase(listIt);
						}
					}

					if (servedFromCache)
					{
						// LRU 提升到头部
						cc.lruList.splice(cc.lruList.begin(), cc.lruList, it->second);

						// 用缓存 ETag 做 If-None-Match 比对
						auto ifnm = req.header("If-None-Match");
						if (!ifnm.empty() && ifnm == it->second->etag)
						{
							HttpResponse res;
							res.setStatus(HttpStatusCode::hNotModified);
							res.setHeader("ETag", it->second->etag);
							res.native().preparePayload();
							co_return res;
						}

						HttpResponse res;
						res.setStatus(HttpStatusCode::hOk);
						res.setBody(it->second->content, mime);
						res.setHeader("Accept-Ranges", "bytes");
						res.setHeader("ETag", it->second->etag);
						res.setHeader("X-Content-Type-Options", "nosniff");
						co_return res;
					}
				}
			}

			// 以下路径需要文件元信息：
			// - 二进制文件（需要 fileSize 给 setFileBody）
			// - Range 请求（需要 fileSize 做范围校验）
			// - 文本文件缓存未命中（需要读文件 + 写缓存）
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

			// 200 全量响应：文本文件走 setBody（两次协程挂起，一次 scatter-gather 写出），
			// 二进制文件走 setFileBody（异步流式发送）。
			//
			// 文本文件走 setBody 理由：
			// - 两次挂起（读文件 + async_write）vs FileBody 三次挂起（open + 发头 + 读写循环）
			// - 小文件 head+body 同在 FixedBuffer<512> 栈缓冲，单次 async_write 完成
			if (!isText)
			{
				HttpResponse res;
				res.setStatus(HttpStatusCode::hOk);
				res.setFileBody(target, 0, static_cast<int64_t>(fileSize), mime);
				res.setHeader("Accept-Ranges", "bytes");
				res.setHeader("ETag", etag);
				res.setHeader("X-Content-Type-Options", "nosniff");
				co_return res;
			}

			// 文本文件 miss 路径：读入内存，setBody 后 scatter-gather 写出
			std::string content(fileSize, '\0');
			size_t totalRead = 0;

#ifdef BOOST_ASIO_HAS_FILE
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

			// 小文件内容写缓存，下次请求跳过磁盘 I/O
			if (fileSize <= detail::TlContentCache::kMaxFileSize)
			{
				auto& cc = detail::getTlContentCache();
				if (cc.index.size() >= detail::TlContentCache::kMaxEntries)
				{
					auto& victim = cc.lruList.back();
					cc.index.erase(victim.key);
					cc.lruList.pop_back();
				}
				detail::TlContentCache::Entry entry;
				entry.key = target.string();
				entry.content = content;
				entry.etag = etag;
				entry.mtimeNs = lastWrite.time_since_epoch().count();
				entry.cachedAt = std::chrono::steady_clock::now();
				cc.lruList.push_front(std::move(entry));
				cc.index.emplace(target.string(), cc.lruList.begin());
			}

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
