# Hical vs Gin vs Actix-web 压测结果

## 测试环境

| 项目      | 值                           |
| --------- | ---------------------------- |
| 测试时间  | 2026-05-06 06:49:32          |
| wrk 线程  | 4                            |
| 并发连接  | 100                          |
| 持续时间  | 30s                          |
| 容器资源  | 4 CPU / 512MB per container  |
| Hical     | v2.5.0 (C++20, GCC, Conan 2) |
| Gin       | v1.10 (Go 1.22)              |
| Actix-web | v4 (Rust latest stable)      |

---

## 详细结果

### 测试 1: Hello World (GET /)

#### Hical

```
Running 30s test @ http://hical:8080/
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   365.17us   70.66us  12.88ms   91.91%
    Req/Sec    66.17k     3.00k   72.45k    71.17%
  7900045 requests in 30.00s, 745.87MB read
  Socket errors: connect 0, read 0, write 0, timeout 100
Requests/sec: 263332.35
Transfer/sec:     24.86MB
```

#### Gin

```
Running 30s test @ http://gin:8081/
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     5.25ms   10.09ms  51.21ms   85.00%
    Req/Sec    43.62k     2.96k   67.54k    76.33%
  5209149 requests in 30.01s, 645.82MB read
Requests/sec: 173586.52
Transfer/sec:     21.52MB
```

#### Actix-web

```
Running 30s test @ http://actix:8082/
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   148.67us   57.82us   3.37ms   71.45%
    Req/Sec   149.67k    11.66k  184.41k    67.44%
  17929249 requests in 30.10s, 2.17GB read
Requests/sec: 595652.85
Transfer/sec:     73.85MB
```

**QPS 对比**

| 框架      | Requests/sec |
| --------- | -----------: |
| Hical     |    263332.35 |
| Gin       |    173586.52 |
| Actix-web |    595652.85 |

### 测试 2: JSON 响应 (GET /api/status)

#### Hical

```
Running 30s test @ http://hical:8080/api/status
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   385.51us   65.37us  11.88ms   88.26%
    Req/Sec    62.92k     3.04k   70.19k    73.00%
  7512827 requests in 30.00s, 0.92GB read
  Socket errors: connect 0, read 0, write 0, timeout 96
Requests/sec: 250423.63
Transfer/sec:     31.52MB
```

#### Gin

```
Running 30s test @ http://gin:8081/api/status
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     6.07ms   11.23ms  64.38ms   84.38%
    Req/Sec    37.07k     2.30k   58.55k    77.83%
  4430432 requests in 30.03s, 680.26MB read
Requests/sec: 147515.60
Transfer/sec:     22.65MB
```

#### Actix-web

```
Running 30s test @ http://actix:8082/api/status
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   167.24us   66.46us   3.36ms   71.88%
    Req/Sec   137.48k    11.00k  177.96k    69.35%
  16472531 requests in 30.10s, 2.33GB read
Requests/sec: 547249.02
Transfer/sec:     79.33MB
```

**QPS 对比**

| 框架      | Requests/sec |
| --------- | -----------: |
| Hical     |    250423.63 |
| Gin       |    147515.60 |
| Actix-web |    547249.02 |

### 测试 3: JSON Echo (POST /api/echo)

#### Hical

```
Running 30s test @ http://hical:8080/api/echo
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   381.85us  153.35us  15.82ms   99.31%
    Req/Sec    63.85k     3.49k   71.15k    71.42%
  7624126 requests in 30.01s, 0.94GB read
  Non-2xx or 3xx responses: 7624126
Requests/sec: 254015.18
Transfer/sec:     32.22MB
```

#### Gin

```
Running 30s test @ http://gin:8081/api/echo
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     5.29ms   10.15ms  51.51ms   84.99%
    Req/Sec    43.22k     3.41k   79.37k    80.75%
  5166900 requests in 30.06s, 625.80MB read
  Non-2xx or 3xx responses: 5166900
Requests/sec: 171873.25
Transfer/sec:     20.82MB
```

#### Actix-web

```
Running 30s test @ http://actix:8082/api/echo
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   158.19us   65.86us   3.91ms   74.25%
    Req/Sec   142.72k    11.28k  180.92k    68.83%
  17040971 requests in 30.02s, 1.30GB read
  Socket errors: connect 0, read 0, write 0, timeout 18
  Non-2xx or 3xx responses: 17040971
Requests/sec: 567637.93
Transfer/sec:     44.39MB
```

**QPS 对比**

| 框架      | Requests/sec |
| --------- | -----------: |
| Hical     |    254015.18 |
| Gin       |    171873.25 |
| Actix-web |    567637.93 |

### 测试 4: 路径参数 (GET /users/42)

#### Hical

```
Running 30s test @ http://hical:8080/users/42
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   394.13us   79.05us  13.23ms   92.43%
    Req/Sec    61.65k     3.18k   68.33k    72.59%
  7386226 requests in 30.10s, 0.85GB read
Requests/sec: 245383.93
Transfer/sec:     29.02MB
```

#### Gin

```
Running 30s test @ http://gin:8081/users/42
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     5.97ms   11.07ms  57.41ms   84.57%
    Req/Sec    35.88k     3.22k   64.39k    77.17%
  4291549 requests in 30.06s, 634.37MB read
Requests/sec: 142779.36
Transfer/sec:     21.11MB
```

#### Actix-web

```
Running 30s test @ http://actix:8082/users/42
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   196.95us   79.05us   3.53ms   70.13%
    Req/Sec   119.48k    10.93k  177.47k    71.67%
  14265362 requests in 30.00s, 1.86GB read
  Socket errors: connect 0, read 0, write 0, timeout 97
Requests/sec: 475490.04
Transfer/sec:     63.48MB
```

**QPS 对比**

| 框架      | Requests/sec |
| --------- | -----------: |
| Hical     |    245383.93 |
| Gin       |    142779.36 |
| Actix-web |    475490.04 |

---

## QPS 汇总

| 场景                        |     Hical |       Gin | Actix-web |
| --------------------------- | --------: | --------: | --------: |
| Hello World (GET /)         | 263332.35 | 173586.52 | 595652.85 |
| JSON 响应 (GET /api/status) | 250423.63 | 147515.60 | 547249.02 |
| JSON Echo (POST /api/echo)  | 254015.18 | 171873.25 | 567637.93 |
| 路径参数 (GET /users/42)    | 245383.93 | 142779.36 | 475490.04 |
