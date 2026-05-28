/**
 * @file LogAdmin.cpp
 * @brief 动态日志级别管理端点实现
 */

#include "LogAdmin.h"

#include "Log.h"
#include "LogChannel.h"
#include "Router.h"

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

namespace hical
{

	namespace
	{
		// 字符串转日志级别
		std::optional<LogLevel> parseLogLevel(std::string_view str)
		{
			if (str == "TRACE" || str == "trace")
			{
				return LogLevel::hTrace;
			}
			if (str == "DEBUG" || str == "debug")
			{
				return LogLevel::hDebug;
			}
			if (str == "INFO" || str == "info")
			{
				return LogLevel::hInfo;
			}
			if (str == "WARN" || str == "warn")
			{
				return LogLevel::hWarn;
			}
			if (str == "ERROR" || str == "error")
			{
				return LogLevel::hError;
			}
			if (str == "FATAL" || str == "fatal")
			{
				return LogLevel::hFatal;
			}
			return std::nullopt;
		}
	} // namespace

	void registerLogAdminEndpoints(Router& router, const std::string& prefix, AdminAuthCheck authCheck)
	{
		// GET /admin/log-level
		router.get(prefix + "/log-level",
				   [authCheck](const HttpRequest& req) -> HttpResponse
				   {
					   // 认证检查
					   if (authCheck)
					   {
						   if (auto denied = authCheck(req))
						   {
							   return *denied;
						   }
					   }

					   boost::json::object result;
					   result["default"] = logLevelToString(Logger::instance().level());

					   // 列出所有命名通道
					   auto channelList = Logger::instance().channels().listChannels();
					   if (!channelList.empty())
					   {
						   boost::json::object channelsObj;
						   for (const auto& [name, lvl] : channelList)
						   {
							   channelsObj[name] = logLevelToString(lvl);
						   }
						   result["channels"] = std::move(channelsObj);
					   }

					   return HttpResponse::json(result);
				   });

		// PUT /admin/log-level
		router.put(prefix + "/log-level",
				   [authCheck](const HttpRequest& req) -> HttpResponse
				   {
					   // 安全默认值：PUT 修改操作在无认证回调时默认拒绝
					   // 防止"审计致盲"攻击（攻击者将级别设为 Fatal 后发起其他攻击）
					   if (!authCheck)
					   {
						   boost::json::object err;
						   err["error"] = "admin endpoint requires authentication";
						   auto res = HttpResponse::json(err);
						   res.setStatus(HttpStatusCode::hForbidden);
						   return res;
					   }
					   if (auto denied = authCheck(req))
					   {
						   return *denied;
					   }

					   boost::system::error_code ec;
					   auto parsed = boost::json::parse(req.body(), ec);
					   if (ec || !parsed.is_object())
					   {
						   boost::json::object err;
						   err["error"] = "invalid JSON object";
						   auto res = HttpResponse::json(err);
						   res.setStatus(HttpStatusCode::hBadRequest);
						   return res;
					   }

					   auto& obj = parsed.as_object();

					   // 获取级别字段
					   auto levelIt = obj.find("level");
					   if (levelIt == obj.end() || !levelIt->value().is_string())
					   {
						   boost::json::object err;
						   err["error"] = "missing or invalid 'level' field";
						   auto res = HttpResponse::json(err);
						   res.setStatus(HttpStatusCode::hBadRequest);
						   return res;
					   }

					   auto levelOpt = parseLogLevel(levelIt->value().as_string());
					   if (!levelOpt)
					   {
						   boost::json::object err;
						   err["error"] = "unknown level, valid: TRACE/DEBUG/INFO/WARN/ERROR/FATAL";
						   auto res = HttpResponse::json(err);
						   res.setStatus(HttpStatusCode::hBadRequest);
						   return res;
					   }

					   // 检查是否指定了通道
					   auto channelIt = obj.find("channel");
					   if (channelIt != obj.end() && channelIt->value().is_string())
					   {
						   auto channelName = std::string(channelIt->value().as_string());
						   auto ch = Logger::instance().channels().get(channelName);
						   if (!ch)
						   {
							   boost::json::object err;
							   err["error"] = "channel not found";
							   auto res = HttpResponse::json(err);
							   res.setStatus(HttpStatusCode::hNotFound);
							   return res;
						   }
						   ch->setLevel(*levelOpt);

						   HICAL_LOG_WARN("LogAdmin: channel '{}' level changed to {}",
										  channelName,
										  logLevelToString(*levelOpt));

						   boost::json::object ok;
						   ok["channel"] = channelName;
						   ok["level"] = logLevelToString(*levelOpt);
						   return HttpResponse::json(ok);
					   }

					   // 默认通道
					   Logger::instance().setLevel(*levelOpt);

					   HICAL_LOG_WARN("LogAdmin: default level changed to {}", logLevelToString(*levelOpt));

					   boost::json::object ok;
					   ok["level"] = logLevelToString(*levelOpt);
					   return HttpResponse::json(ok);
				   });
	}

} // namespace hical
