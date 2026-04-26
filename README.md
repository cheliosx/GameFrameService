# GameFrameService

## 服务端启动参数

`ws_server` 支持以下启动配置：

- `--port`：WebSocket 监听端口，默认 `8888`
- `--fps`：帧率，广播间隔按 `1000/fps` 毫秒计算
- `--redis-host`：Redis 地址，默认 `127.0.0.1`
- `--redis-port`：Redis 端口，默认 `6379`
- `--redis-password`：Redis 密码，默认空

示例：

```bash
./ws_server --port 8888 --fps 2 --redis-host 127.0.0.1 --redis-port 6379 --redis-password your_password
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
