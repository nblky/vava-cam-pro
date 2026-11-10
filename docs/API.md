# 管理 API

API 由 `vava-server` 提供。前端可单独部署在其他机器；它访问 Server HTTPS，不直接连接基站摄像头端口或原厂云。Agent 只有本机受文件权限保护的控制 socket 与自有 UDP 协议。精确请求校验以 [server_web.c](../vava-server/src/server_web.c)、[api.c](../shared/api.c) 为准；本页不是完整 OpenAPI schema。

## 认证与权限

`POST /api/v1/login` 接收 `username`、`password`，返回自有会话 token。后续请求发送 `Authorization: Bearer TOKEN`，POST 使用 `Content-Type: application/json`。不要把 token 放 URL。`GET /api/v1/session` 查询，`POST /api/v1/logout` 撤销；改密码为 `POST /api/v1/password` 的 `old_password` / `new_password`。

三类角色为 viewer、operator、admin，并按基站 ACL 过滤。只读用户不能修改设置或对讲，但允许读取短片及为回放创建受控 TF 调取。恢复、删除摄像头、留存清理等高权限操作由管理员执行。`/api/v1/users` 的 GET / POST 为管理员接口；其主导航入口目前未开放。

独立前端在 Server 配置精确 `--web-origin`；支持受控预检，不使用任意来源通配放行。跨机器使用受信任 HTTPS。修改密码、停用用户或改授权会撤销相关会话，不能仅靠隐藏菜单做权限控制。

## 接口入口

下表中的 `D` 为基站 ID，文件 ID 为十六位十六进制，epoch / boot 为三十二位十六进制。

| 方法与路径 | 用途 |
| --- | --- |
| `GET /api/v1/devices` | 授权基站、当前 boot、Server epoch、请求键前缀与在线状态 |
| `POST /api/v1/devices/D/commands` | 提交白名单 Agent 命令，返回任务身份 |
| `GET /api/v1/devices/D/tasks/EPOCH/ID` | 查询持久任务及结果 |
| `GET /api/v1/devices/D/lookup/REQUEST_KEY` | 响应丢失后恢复同一次提交 |
| `GET /api/v1/devices/D/history?before=ID` | 操作历史 |
| `GET /api/v1/devices/D/events?before=ID&category=detection&serial=SN` | 分页事件，可选类别/摄像头过滤 |
| `POST /api/v1/devices/D/events/read` | `through` 已读游标，按用户/基站保存 |
| `GET /api/v1/media-capabilities` | Server 连续媒体能力 |
| `POST /api/v1/devices/D/live` | 认证媒体流，含 `boot`、`slot`、`serial`、`talk`、`format` |
| `POST /api/v1/devices/D/live/LEASE/talk` | 当前会话所属租约的 AAC 下行 |
| `GET /api/v1/devices/D/files?storage=cloud&serial=SN&page=0` | 按来源/摄像头过滤后统计与分页 |
| `GET /api/v1/devices/D/files/ID/info` | 文件身份、来源、加密、时间和检测元数据 |
| `GET /api/v1/devices/D/files/ID/download` | 认证解密下载 |
| `GET /api/v1/devices/D/files/ID/playback` | 媒体适配播放 |
| `GET /api/v1/devices/D/files/ID/thumbnail` | JPEG 缩略图 |
| `POST /api/v1/devices/D/files/ID/delete` | 显式删除该 Server 对象，JSON 空对象 |
| `GET / POST /api/v1/archive-policy` | 管理员 Server 云留存策略 |

文件 `storage` 可为 `cloud`、`tf_export`、`tf_preview`、`legacy_import`。不能省略过滤后再把所有对象展示为云存储；TF 与云的播放选择不互相隐式回退。

## 任务请求

```json
{
  "boot": "<本次读取的基站 boot>",
  "epoch": "<本次读取的 Server epoch>",
  "request_key": "<当前前缀与随机后缀组成的 32 位十六进制键>",
  "command": {
    "command": "configure-many",
    "args": ["0", "mirror=1"],
    "camera_serial": "CAMERA_SN",
    "expected_revision": 12
  }
}
```

先读取 capabilities / status 和对应设置域 revision，再提交。`args` 为字符串数组；未知字段、非法参数与命令拒绝。任务受理不等于设置保存，更不等于摄像头生效。轮询任务完成后解析 `result`；网络响应丢失使用原 `request_key` lookup，不生成新键重发副作用操作。

任务可为 queued、dispatched、completed、expired、unknown。unknown 表示执行结果无法确认，必须核对实际状态，不能当成普通失败自动重放。切换页面或基站后，旧任务仍属于原基站，取消浏览器等待也不等于撤销已执行命令。

## 常用 Agent 命令

| 领域 | 命令 |
| --- | --- |
| 查询 | `status`、`capabilities`、`settings`、`policy` |
| 配对 | `pair-open`、`pair-close`；任务进度在状态中读取 |
| 名称 / 设置 | `name`、`configure-many`、`configure-pir`、`apply-settings` |
| 布防 / 计划 | `policy-config`、`plan`，使用策略域 revision |
| 摄像头维护 | `reset`、`remove`、`remove-confirm`，管理员权限及身份保护 |
| TF | `record-query`、`record-info`、`record-delete`、`record-export`、`record-preview` |
| 传输任务 | `export-list`、`upload-list`、`upload-task`、`nas-list`、`nas-task` |
| 基站 NAS | `nas-status`、`nas-config`、`nas-control` |
| 本地留存 | `storage-policy`、`storage-policy-set` |

普通配置、策略、名称的 revision 不可混用。TF 游标需同时绑定基站 boot 和 storage generation；介质变化必须重置游标。分页不是冻结快照。能力协商后再使用新命令；不要向摄像头发送原 App 的数字命令编号。

建议复用 [api-client.js](../vava-server/web/api-client.js) 的请求封装及 [management.js](../vava-server/web/management.js) 中任务、短片调取和身份保护逻辑。独立前端可用 `tools/export_web.py` 导出。

## 错误和媒体

401 表示会话无效，403 表示权限或租约不匹配，409 可能为并发/文件占用冲突，429 为登录限流，503 为暂时不可用。HTTP 200 内仍需检查业务 `ok` 和任务状态；保留具体错误码用于展示及处理。

`live` 的 `format` 接受 `ts`、`mp4`、`mse-v2`；`mse-v2` 是自有有界记录流，不是直接把网络 chunk 当 MP4 分片。连续切档通过受当前租约约束的 `live-quality` 命令，不能用普通写设置绕过权限。停止观看应取消接收以释放租约；详见 [实时观看](LIVE.md) 与 [SDK](../myvava-sdk/PROTOCOL.md)。

API 自动测试见 [sdk_web.py](../tests/sdk_web.py)、[sdk_record_api.py](../tests/sdk_record_api.py)、[sdk_web_media.py](../tests/sdk_web_media.py) 和前端测试。真实云存储、NAS及硬件效果的限制见 [功能状态](FEATURES.md)。
