# GameFrameService

## 服务端配置文件（redis.conf 风格）

服务端支持配置文件启动（`redis.conf` 风格）。如果**不传入配置文件**，服务端会使用内置默认值：

- `port=8888`
- `fps=1`
- `redis_host=127.0.0.1`
- `redis_port=6379`
- `redis_password=123456`

传入配置文件时，格式类似 `redis.conf`：

```conf
# 监听端口
port 8888

# 帧率，广播间隔 = 1000/fps 毫秒
fps 2

# Redis 地址配置
redis_host 127.0.0.1
redis_port 6379
redis_password 123456
```

你可以先复制示例文件：

```bash
cp server.conf.example server.conf
```

不使用配置文件（直接用默认值）：

```bash
./ws_server
```

或者指定配置文件路径：

```bash
./ws_server --config /path/to/server.conf
```

## 入房鉴权协议

1. 客户端连接成功后，服务端下发 `JoinRoomChallenge`（协议号 4），body 为 8 字节毫秒级时间戳。
2. 客户端计算 `md5(timestamp + room_secret)`。
3. 客户端发送 `JoinRoomAuth`（协议号 5），body 包含 `room_id` 和 `md5`。
4. 服务端从 Redis 读取 `room:secret:<room_id>`，校验通过后玩家正式入房。

> Redis 连接在服务启动时建立并复用，避免每次鉴权都重新建连。

## 游戏开始协议

- 协议号：`GameStart`（6）
- Room 默认 `game_started=false`，只有收到该协议后才开始按帧率广播帧循环。
- 客户端示例可输入 `start_game` 触发该协议。

## 测试房间

请先在 Redis 中准备测试房间密钥：

```bash
redis-cli SET room:secret:1 123456
```

`client` 默认按 `room_id=1`、密钥 `123456` 进行测试鉴权。
