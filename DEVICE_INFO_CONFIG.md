# 阶段 1：设备信息配置说明

设备信息页面显示三个字段：拥有者、设备名称和设备编号。拥有者与设备名称保存在手表 NVS 中；设备编号由芯片的 Wi-Fi Station MAC 地址生成，首次使用后固定保存，不能通过配置命令修改。

## 当前输入限制

- `owner`：最多 32 个 ASCII 字符，可留空。
- `device_name`：最多 32 个 ASCII 字符，可留空，首次使用默认是 `ESP32-S3 Watch`。
- `device_id`：只读，格式类似 `S3-1234-5678-9ABC`。
- 空的拥有者或设备名称在页面中显示为“未设置”或 `Not set`。
- 第一版未加入完整中文字库，因此拥有者和设备名称暂不接受中文。固定界面标签支持中文。

## 通过 USB 串口设置

手表通过 USB 连接电脑后，打开串口终端并选择手表所在端口。波特率使用 `115200`，每条命令是一行 JSON，发送时需要带换行符。

设置拥有者和设备名称：

```json
{"cmd":"set_config","owner":"Alice","device_name":"Study Watch"}
```

只修改其中一个字段时，可以省略另一个字段：

```json
{"cmd":"set_config","owner":"Bob"}
```

读取当前配置和只读设备编号：

```json
{"cmd":"get_config"}
```

正常返回中会包含：

```json
{"owner":"Bob","device_name":"Study Watch","device_id":"S3-1234-5678-9ABC"}
```

设置完成后退出并重新进入“设备信息”页面即可刷新显示；重新启动或断电后数据仍会保留。向 `set_config` 发送 `device_id` 会返回 `device_id is read only`。

`clear_config` 会清空拥有者并恢复默认设备名称，同时保留设备编号。该命令也会清除原有 Wi-Fi、天气等配置并重启，不能只为了清除姓名而使用。
