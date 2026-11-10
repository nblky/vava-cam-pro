# 部署与运维

当前交付物是程序与软件包源码，不是可刷入原厂基站的 OpenWrt 系统镜像。推荐分别部署 Agent 与 Server；两者也可在性能足够的同一设备上运行，但存储、端口及状态目录必须明确。

## 部署入口与验证范围

| 目标 | 入口 | 当前验证范围 |
| --- | --- | --- |
| HomeWRK Agent | [Agent / LuCI 说明](../vava-station/README.md) | ARM64 / musl 手工部署、配对、事件与媒体已有实测 |
| 其他 OpenWrt Agent | `tools/export_openwrt.py --component agent` | 源码导出、主机构建和 helper 模拟；目标 SDK 编译/安装待验 |
| Linux / NAS 原生 Server | [Server 说明](../vava-server/README.md) | 主机构建、启动与权限回归；具体 NAS 系统待验 |
| Docker Server | `vava-server/deploy/docker` | Dockerfile / Compose 已提供；Server 镜像构建和 NAS 容器运行未验收 |
| OpenWrt Server | `tools/export_openwrt.py --component server` | 包源码及模板已提供；目标 SDK 与性能待验 |
| 独立 Web | `tools/export_web.py` | 源码导出与客户端回归；跨域需配置精确 origin 与 HTTPS |

Docker 曾用于 Linux 构建测试，不能据此声称 Server 的产品镜像已验收。原 HS003 的板级、分区、驱动、恢复串口与容量适配均未完成，不宣称可装入 16 MiB Flash / 64 MiB RAM。

## 首次部署顺序

1. 按根 [README](../README.md) 安装 C17 / CMake / OpenSSL / SQLite 依赖。完整浏览器直播还需 FFmpeg 5+ 开发库、pkg-config 和运行时 ffmpeg；只安装 CLI 不会启用持续媒体库模块。
2. 在新的私有部署目录运行 `tools/provision_server.py`，生成管理员初始密码、每基站 PSK 与录像密钥。不要向已有目录重复初始化以替代升级。
3. Server 以部署用户运行，配置、数据库和密钥归该用户所有；默认 HTTP 只监听回环，远程管理使用受信任 HTTPS。
4. Agent 配置基站 ID、Server 域名/IP、UDP 端口和匹配的私有 PSK 文件。安装后先执行 `vava-agent --transport-check`，确认目标 OpenSSL 具有 DTLS 1.2 / PSK 能力。
5. 在路由器配置 TF/USB 的持久挂载及录像密钥，准备支持 2.4 GHz 的专用 AP 与防火墙区域；摄像头固定网段不能与现有网络冲突。
6. 在 LuCI 启用 Agent，核对时钟、存储和 Server 连接。首次配对后在 Server 摄像头设置中明确配置工作模式。

Agent 的安装包默认停用，不能假定安装后已接管网络。录像写外部介质，不写路由器内部 Flash。只做本地配对可显式选择独立模式，云与远程管理需要 Server 配置。

OpenSSL 可能导出 DTLS 函数但裁掉所需协议/PSK。正式包要求目标依赖支持相应能力；不要替换路由器其他系统服务共用的 libssl 来掩盖不兼容。

## 启动与检查

OpenWrt 使用 `/etc/init.d/vava-agent` / procd，日志通过系统日志查看；`vavactl` 仅对本机控制 socket 操作。LuCI“运行状态”显示服务、摄像头、时钟、存储及配对状态；这些值不写回 UCI。

Server 可用原生启动器、systemd 或 Compose。配置示例在 [server.env.example](../vava-server/deploy/server.env.example)；完整参数以对应组件说明和可执行程序 `--help` 为准。云上传和 NAS 开关默认关闭，必须分别配置并完成真实存储端验收。

## 备份与升级

升级前备份 Agent 的 UCI 配置、`/etc/vava` 私有密钥、`/etc/vava-state` 持久状态以及外部存储；Server 备份设备登记、用户/任务/事件数据库、存储密钥与文件。通过 [维护工具](../tools) 执行一致性备份/恢复，勿只复制仍在写入的 SQLite 主文件而遗漏 WAL。

新旧实例不能同时占用摄像头端口、同一状态目录或归档目录。停止服务后替换匹配目标 ABI 的程序，保留状态与密钥；重启后核对原配对、设置、片段数、时间和存储。升级不自动重新配对、格式化磁盘或生成新密钥。

模板、主机模拟和手工部署不替代正式安装包验收。待验清单见 [VALIDATION](VALIDATION.md)，产品能力与限制见 [FEATURES](FEATURES.md)。
