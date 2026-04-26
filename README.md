# GameFrameService

## 服务端配置文件（redis.conf 风格）

服务端通过配置文件启动，默认读取项目根目录 `server.conf`，格式类似 `redis.conf`：

```conf
# 监听端口
port 8888

# 帧率，广播间隔 = 1000/fps 毫秒
fps 2

# Redis 地址配置
redis_host 127.0.0.1
redis_port 6379
redis_password your_password
```

你可以先复制示例文件：

```bash
cp server.conf.example server.conf
```

启动服务端：

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

## 测试房间

请先在 Redis 中准备测试房间密钥：

```bash
redis-cli SET room:secret:1 123456
```

`client` 默认按 `room_id=1`、密钥 `123456` 进行测试鉴权。
