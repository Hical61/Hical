/**
 * @file Multipart.h
 * @brief RFC 7578 multipart/form-data 解析
 */

#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hical
{

	/**
	 * @brief multipart/form-data 中的单个 Part
	 * 对应 RFC 7578 中的一个 body part，包含头部和数据。
	 */
	struct MultipartPart
	{
		std::unordered_map<std::string, std::string> headers; ///< Part 头部（键已转小写）
		std::string name;                                     ///< form 字段名（Content-Disposition 中的 name 参数）
		std::string filename;    ///< 上传文件名（Content-Disposition 中的 filename，无则为空）
		std::string contentType; ///< Part 的 Content-Type（无则为空）
		std::string data;        ///< Part 数据（文件内容或字段值）

		/**
		 * @brief 是否为文件上传 Part（有 filename）
		 * @return true 如果是文件上传
		 */
		bool isFile() const
		{
			return !filename.empty();
		}
	};

	/**
	 * @brief multipart/form-data 解析器
	 * 按照 RFC 7578 解析 Content-Type: multipart/form-data 请求体。
	 * 用法（通过 HttpRequest 访问）：
	 * ```cpp
	 * auto parts = hical::MultipartParser::parse(req);
	 * if (parts) {
	 *     for (const auto& part : *parts) {
	 *         if (part.isFile()) {
	 *             // 处理上传文件
	 *         } else {
	 *             // 处理表单字段
	 *         }
	 *     }
	 * }
	 * ```
	 * 也可以直接用辅助方法：
	 * ```cpp
	 * auto file  = hical::MultipartParser::getFile(req, "avatar");
	 * auto field = hical::MultipartParser::getField(req, "username");
	 * ```
	 */
	class MultipartParser
	{
	public:
		/**
		 * @brief 解析 multipart/form-data 请求体
		 * @param req HTTP 请求
		 * @return 解析成功返回 Part 列表，失败返回 nullopt
		 * 失败情况：Content-Type 不是 multipart/form-data、boundary 缺失、格式错误。
		 */
		[[nodiscard]] static std::optional<std::vector<MultipartPart>> parse(const HttpRequest& req);

		/**
		 * @brief 获取指定名称的文件上传 Part
		 * @param req HTTP 请求
		 * @param fieldName 表单字段名
		 * @return 找到返回 MultipartPart（isFile() == true），否则 nullopt
		 * @note 多次调用 getFile/getField 建议先调用 parse() 再用下方重载，避免重复解析
		 */
		[[nodiscard]] static std::optional<MultipartPart> getFile(const HttpRequest& req, const std::string& fieldName);

		/**
		 * @brief 从已解析的 parts 中查找文件 Part（避免重复解析）
		 * @param parts 已通过 parse() 获取的 Part 列表
		 * @param fieldName 表单字段名
		 * @return 找到返回 MultipartPart，否则 nullopt
		 */
		[[nodiscard]] static std::optional<MultipartPart> getFile(const std::vector<MultipartPart>& parts,
																  const std::string& fieldName);

		/**
		 * @brief 获取指定名称的表单文本字段值
		 * @param req HTTP 请求
		 * @param fieldName 表单字段名
		 * @return 找到返回字段值，否则 nullopt
		 * @note 多次调用 getFile/getField 建议先调用 parse() 再用下方重载，避免重复解析
		 */
		[[nodiscard]] static std::optional<std::string> getField(const HttpRequest& req, const std::string& fieldName);

		/**
		 * @brief 从已解析的 parts 中查找文本字段（避免重复解析）
		 * @param parts 已通过 parse() 获取的 Part 列表
		 * @param fieldName 表单字段名
		 * @return 找到返回字段值，否则 nullopt
		 */
		[[nodiscard]] static std::optional<std::string> getField(const std::vector<MultipartPart>& parts,
																 const std::string& fieldName);

	private:
		/**
		 * @brief 惰性解析并缓存 multipart parts 到 HttpRequest 中
		 * @param req HTTP 请求
		 * @return 缓存的 parts 指针，解析失败或无 multipart 数据时返回 nullptr
		 */
		static const std::vector<MultipartPart>* cachedParse(const HttpRequest& req);

		/**
		 * @brief 从 Content-Type 头中提取 boundary
		 * @param contentType Content-Type 头字符串
		 * @return boundary 字符串，失败返回空字符串
		 */
		static std::string extractBoundary(const std::string& contentType);

		/**
		 * @brief 解析单个 Part 的头部
		 * @param headerBlock 头部文本块（CRLF 分隔的多行）
		 * @param part 输出的 Part（写入 headers/name/filename/contentType）
		 */
		static void parsePartHeaders(std::string_view headerBlock, MultipartPart& part);

		/**
		 * @brief 解析 Content-Disposition 中的参数（name/filename）
		 * @param disposition Content-Disposition 字段值
		 * @param name 输出 name 参数
		 * @param filename 输出 filename 参数
		 */
		static void parseDispositionParams(std::string_view disposition, std::string& name, std::string& filename);
	};

} // namespace hical
