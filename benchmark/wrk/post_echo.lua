-- POST JSON Echo 测试的 wrk Lua 脚本
-- 用法: wrk -t4 -c100 -d30s -s /bench/post_echo.lua http://host:port/api/echo

wrk.method = "POST"
wrk.body   = '{"name":"Alice","age":30,"email":"alice@example.com"}'
wrk.headers["Content-Type"] = "application/json"
