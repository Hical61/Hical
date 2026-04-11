#include "core/HttpRequest.h"
#include "core/Multipart.h"
#include <gtest/gtest.h>
#include <string>

using namespace hical;

// ============ 测试辅助：构建 multipart body ============

static HttpRequest makeMultipartRequest(const std::string& boundary, const std::string& body)
{
	HttpRequest req;
	req.setMethod(HttpMethod::hPost);
	req.setTarget("/upload");
	req.setHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
	req.setBody(body);
	return req;
}

static std::string buildMultipart(
	const std::string& boundary,
	const std::vector<std::pair<std::string, std::string>>& textFields,
	const std::vector<std::tuple<std::string, std::string, std::string, std::string>>& files)
{
	// files: (fieldName, filename, contentType, data)
	std::string body;

	for (const auto& [name, value] : textFields)
	{
		body += "--" + boundary + "\r\n";
		body += "Content-Disposition: form-data; name=\"" + name + "\"\r\n";
		body += "\r\n";
		body += value + "\r\n";
	}

	for (const auto& [name, filename, ct, data] : files)
	{
		body += "--" + boundary + "\r\n";
		body += "Content-Disposition: form-data; name=\"" + name + "\"; filename=\"" + filename + "\"\r\n";
		body += "Content-Type: " + ct + "\r\n";
		body += "\r\n";
		body += data + "\r\n";
	}

	body += "--" + boundary + "--\r\n";
	return body;
}

// ============ 基础解析测试 ============

TEST(MultipartTest, ParseTextFieldOnly)
{
	std::string boundary = "TestBoundary";
	std::string body = buildMultipart(boundary, {{"username", "alice"}, {"age", "30"}}, {});
	auto req = makeMultipartRequest(boundary, body);

	auto parts = MultipartParser::parse(req);
	ASSERT_TRUE(parts.has_value());
	EXPECT_EQ(parts->size(), 2u);

	EXPECT_EQ((*parts)[0].name, "username");
	EXPECT_EQ((*parts)[0].data, "alice");
	EXPECT_FALSE((*parts)[0].isFile());

	EXPECT_EQ((*parts)[1].name, "age");
	EXPECT_EQ((*parts)[1].data, "30");
}

TEST(MultipartTest, ParseFileUpload)
{
	std::string boundary = "----WebKitBoundary";
	std::string body = buildMultipart(boundary, {}, {{"avatar", "photo.png", "image/png", "\x89PNG\r\nfakedata"}});
	auto req = makeMultipartRequest(boundary, body);

	auto parts = MultipartParser::parse(req);
	ASSERT_TRUE(parts.has_value());
	ASSERT_EQ(parts->size(), 1u);

	EXPECT_TRUE((*parts)[0].isFile());
	EXPECT_EQ((*parts)[0].name, "avatar");
	EXPECT_EQ((*parts)[0].filename, "photo.png");
	EXPECT_EQ((*parts)[0].contentType, "image/png");
	EXPECT_EQ((*parts)[0].data, "\x89PNG\r\nfakedata");
}

TEST(MultipartTest, ParseMixedFieldsAndFiles)
{
	std::string boundary = "MixedBoundary";
	std::string body = buildMultipart(boundary,
									  {{"title", "Hello World"}},
									  {{"document", "report.txt", "text/plain", "Report content here"}});
	auto req = makeMultipartRequest(boundary, body);

	auto parts = MultipartParser::parse(req);
	ASSERT_TRUE(parts.has_value());
	EXPECT_EQ(parts->size(), 2u);

	// 文本字段
	EXPECT_FALSE((*parts)[0].isFile());
	EXPECT_EQ((*parts)[0].name, "title");
	EXPECT_EQ((*parts)[0].data, "Hello World");

	// 文件
	EXPECT_TRUE((*parts)[1].isFile());
	EXPECT_EQ((*parts)[1].filename, "report.txt");
}

TEST(MultipartTest, WrongContentTypeReturnsNullopt)
{
	HttpRequest req;
	req.setHeader("Content-Type", "application/json");
	req.setBody("{\"key\": \"value\"}");

	auto result = MultipartParser::parse(req);
	EXPECT_FALSE(result.has_value());
}

TEST(MultipartTest, MissingBoundaryReturnsNullopt)
{
	HttpRequest req;
	req.setHeader("Content-Type", "multipart/form-data");
	req.setBody("some body");

	auto result = MultipartParser::parse(req);
	EXPECT_FALSE(result.has_value());
}

TEST(MultipartTest, EmptyBodyReturnsNullopt)
{
	auto req = makeMultipartRequest("boundary", "");
	auto result = MultipartParser::parse(req);
	EXPECT_FALSE(result.has_value());
}

// ============ 辅助方法测试 ============

TEST(MultipartTest, GetFieldHelper)
{
	std::string boundary = "HelpBoundary";
	std::string body = buildMultipart(boundary, {{"email", "test@example.com"}, {"name", "Bob"}}, {});
	auto req = makeMultipartRequest(boundary, body);

	auto email = MultipartParser::getField(req, "email");
	ASSERT_TRUE(email.has_value());
	EXPECT_EQ(*email, "test@example.com");

	auto missing = MultipartParser::getField(req, "nonexistent");
	EXPECT_FALSE(missing.has_value());
}

TEST(MultipartTest, GetFileHelper)
{
	std::string boundary = "FileBoundary";
	std::string body =
		buildMultipart(boundary, {{"desc", "a photo"}}, {{"photo", "img.jpg", "image/jpeg", "JFIF binary data"}});
	auto req = makeMultipartRequest(boundary, body);

	auto file = MultipartParser::getFile(req, "photo");
	ASSERT_TRUE(file.has_value());
	EXPECT_EQ(file->filename, "img.jpg");
	EXPECT_EQ(file->data, "JFIF binary data");

	// getFile 不返回文本字段
	auto notFile = MultipartParser::getFile(req, "desc");
	EXPECT_FALSE(notFile.has_value());
}

TEST(MultipartTest, MultipleFiles)
{
	std::string boundary = "MultiBound";
	std::string body =
		buildMultipart(boundary,
					   {},
					   {{"file1", "a.txt", "text/plain", "content a"}, {"file2", "b.txt", "text/plain", "content b"}});
	auto req = makeMultipartRequest(boundary, body);

	auto parts = MultipartParser::parse(req);
	ASSERT_TRUE(parts.has_value());
	EXPECT_EQ(parts->size(), 2u);

	EXPECT_EQ((*parts)[0].filename, "a.txt");
	EXPECT_EQ((*parts)[1].filename, "b.txt");
}

TEST(MultipartTest, HeadersAreStoredLowercase)
{
	std::string boundary = "CaseBoundary";
	std::string body = buildMultipart(boundary, {}, {{"upload", "file.dat", "application/octet-stream", "binary"}});
	auto req = makeMultipartRequest(boundary, body);

	auto parts = MultipartParser::parse(req);
	ASSERT_TRUE(parts.has_value());
	ASSERT_EQ(parts->size(), 1u);

	// 头部键应为小写
	EXPECT_NE((*parts)[0].headers.find("content-disposition"), (*parts)[0].headers.end());
	EXPECT_NE((*parts)[0].headers.find("content-type"), (*parts)[0].headers.end());
}

// ============ P2: Part 数量上限 DoS 防护测试 ============

TEST(MultipartTest, ExceedMaxPartsReturnsNullopt)
{
	// 构造超过 256 个 part，应触发上限保护返回 nullopt
	std::string boundary = "DosBoundary";
	std::string body;
	for (int i = 0; i < 257; ++i)
	{
		body += "--" + boundary + "\r\n";
		body += "Content-Disposition: form-data; name=\"f" + std::to_string(i) + "\"\r\n";
		body += "\r\n";
		body += "v\r\n";
	}
	body += "--" + boundary + "--\r\n";

	auto req = makeMultipartRequest(boundary, body);
	auto result = MultipartParser::parse(req);
	EXPECT_FALSE(result.has_value());
}
