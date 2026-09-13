# ESP-WATCH 无线协议 v1

传输为 TCP 长连接，UTF-8 编码，每条消息是一行 JSON，以 `\n` 结束。默认端口为 `8765`，单条消息上限为 4096 字节。第一版只允许一个已认证手表。

## 建立连接

手表主动连接电脑并发送：

```json
{"v":1,"type":"hello","device_id":"...","device_name":"...","token":"...","nonce":123}
```

电脑校验协议版本、设备 ID 和配对令牌，返回：

```json
{"v":1,"type":"hello_ack","ok":true,"session":"uuid","reason":"accepted"}
```

认证失败时电脑返回 `ok:false` 后断开。`session` 由电脑为本次连接生成；重连会得到新会话，旧会话操作不得执行。

## 心跳和断线

- 手表每 5 秒发送 `heartbeat`，电脑返回同序号的 `heartbeat_ack`。
- 电脑每 5 秒发送 `ping`，手表返回同序号的 `pong`。
- 任一端连续 15 秒没有收到数据即断开；手表等待 3 秒后重新连接。
- 断线时不保存、不补发瞬时控制指令。

## 后续业务消息约束

阶段 7、8 使用的操作请求必须包含 `v`、`type`、`session`、`op_id`、`action` 和有效期。电脑是协同状态的最终来源，并按 `session + op_id` 去重；重复请求返回第一次的处理结果。确认需区分收到、已处理和拒绝。状态快照使用任务 ID 与递增版本，旧版本不能覆盖新状态。

## 演示遥控（阶段 8）

手表进入或退出演示遥控页面时发送可恢复的页面状态；它可以在重连后重新同步：

```json
{"v":1,"type":"presentation_state","session":"...","active":true}
```

电脑端演示开关默认关闭，每次新连接都必须由用户重新开启。状态变化发送：

```json
{"v":1,"type":"presentation_status","session":"...","enabled":true}
```

一次拨轮操作发送一个瞬时请求。`op_id` 在当前手表运行期间递增，发送队列超过 `ttl_ms` 后丢弃：

```json
{"v":1,"type":"presentation_control","session":"...","op_id":12,"action":"next","ttl_ms":1500}
```

`action` 为 `previous` 或 `next`。电脑按 `session + op_id` 去重，重复请求返回首次结果但不再次模拟按键。电脑返回：

```json
{"v":1,"type":"presentation_result","session":"...","op_id":12,"outcome":"processed","reason":"next"}
```

`outcome` 为 `processed` 或 `rejected`。常见拒绝原因包括 `disabled`、`watch_inactive`、`foreground_not_slideshow` 和 `send_input_failed`。`processed` 只表示 Windows 已接受模拟按键，不代表已读取并确认幻灯片页码变化。

电脑仅在演示开关开启、手表页面活动、会话匹配，并且前台是 WPS 演示或 PowerPoint 的全屏放映窗口时执行 `Page Up` / `Page Down`。断线时瞬时请求清空，重连后不补发。
