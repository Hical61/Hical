#include "HttpResponse.h"

namespace hical
{

HttpResponse::HttpResponse()
{
    res_.version(11);  // HTTP/1.1
    res_.result(boost::beast::http::status::ok);
}

HttpResponse::HttpResponse(BeastResponse res)
    : res_(std::move(res))
{
}

HttpStatusCode HttpResponse::statusCode() const
{
    return static_cast<HttpStatusCode>(res_.result_int());
}

void HttpResponse::setStatus(HttpStatusCode code)
{
    res_.result(static_cast<unsigned int>(code));
}

std::string HttpResponse::header(const std::string& name) const
{
    auto it = res_.find(name);
    if (it != res_.end())
    {
        return std::string(it->value());
    }
    return "";
}

void HttpResponse::setHeader(const std::string& name, const std::string& value)
{
    res_.set(name, value);
}

const std::string& HttpResponse::body() const
{
    return res_.body();
}

void HttpResponse::setBody(const std::string& body,
                           const std::string& contentType)
{
    res_.body() = body;
    res_.set(boost::beast::http::field::content_type, contentType);
    res_.prepare_payload();
}

void HttpResponse::setJsonBody(const boost::json::value& json)
{
    res_.body() = boost::json::serialize(json);
    res_.set(boost::beast::http::field::content_type, "application/json");
    res_.prepare_payload();
}

HttpResponse::BeastResponse& HttpResponse::native()
{
    return res_;
}

const HttpResponse::BeastResponse& HttpResponse::native() const
{
    return res_;
}

// ============ 快捷工厂方法 ============

HttpResponse HttpResponse::ok(const std::string& body)
{
    HttpResponse res;
    res.setStatus(HttpStatusCode::hOk);
    if (!body.empty())
    {
        res.setBody(body);
    }
    return res;
}

HttpResponse HttpResponse::json(const boost::json::value& json)
{
    HttpResponse res;
    res.setStatus(HttpStatusCode::hOk);
    res.setJsonBody(json);
    return res;
}

HttpResponse HttpResponse::notFound()
{
    HttpResponse res;
    res.setStatus(HttpStatusCode::hNotFound);
    res.setBody("Not Found");
    return res;
}

HttpResponse HttpResponse::badRequest(const std::string& message)
{
    HttpResponse res;
    res.setStatus(HttpStatusCode::hBadRequest);
    res.setBody(message);
    return res;
}

HttpResponse HttpResponse::serverError()
{
    HttpResponse res;
    res.setStatus(HttpStatusCode::hInternalServerError);
    res.setBody("Internal Server Error");
    return res;
}

}  // namespace hical
