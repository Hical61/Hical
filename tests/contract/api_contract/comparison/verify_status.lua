-- wrk 的错误计数会漏掉部分非 200 响应，这里逐条数。
local workers = {}

function setup(thread)
    workers[#workers + 1] = thread
end

function init()
    statusOk = 0
    statusOther = 0
end

function response(status)
    if status == 200 then
        statusOk = statusOk + 1
    else
        statusOther = statusOther + 1
    end
end

function done()
    local ok, other = 0, 0
    for _, thread in ipairs(workers) do
        ok = ok + thread:get("statusOk")
        other = other + thread:get("statusOther")
    end
    io.write(string.format("HTTP_200=%d HTTP_OTHER=%d\n", ok, other))
end
