# VAVA Agent

`vava-station` 是源码组件名，产物和 OpenWrt 软件包名均为 `vava-agent`。它负责原厂摄像头的 AP 配对、本地连接、设置下发、布防/休眠、PIR 事件短片、TF/USB 加密归档、每基站 NAS 和向自有 Server 传输数据。Agent 自身没有管理 HTTP 服务。

`luci-app-vava-agent` 是单独的 LuCI 包，使用 OpenWrt 的登录与权限机制，提供服务配置、运行状态、配对窗口及外部管理平台入口。Web 摄像头设置、画面与视频目录位于 Server。LuCI 不保存或展示设备 PSK，只配置私有密钥文件路径。

## OpenWrt 包

```sh
# 在 vava-cam-pro 仓库根目录执行；目标目录必须尚不存在。
python3 tools/export_openwrt.py /path/to/openwrt/package/vava-cam-pro --component agent
# 在对应路由器的 OpenWrt SDK / 源码树中执行：
make menuconfig
# Network -> vava-agent；LuCI -> Applications -> luci-app-vava-agent
make package/vava-agent/compile V=s
make package/luci-app-vava-agent/compile V=s
```

导出结果包含两个独立包和完整、固定依赖的 Agent 源码。选择对应板型和 OpenWrt 版本的 SDK，产物由其打包为 `.ipk` 或 `.apk`；架构不同不能混装。NFS 另选目标平台的内核 NFS 客户端和挂载工具。

安装后 `/etc/config/vava-agent` 默认 `enabled=0`。LuCI 入口为“服务 → VAVA Agent”。服务为 `/etc/init.d/vava-agent`，主程序 `/usr/bin/vava-agent`，本机维护工具 `/usr/bin/vavactl`。

LuCI 默认打开“运行与服务”配置页；“摄像头配对”标签提供即时配对操作，“运行状态”标签集中显示自动刷新的只读状态。这两个模块不会向 UCI 写入显示数据。

## 首次配置

1. 配置存储挂载点，例如 `/mnt/vava`。不要把录像直接写在路由器内部 Flash。
2. 选择支持 2.4 GHz AP 的 radio，执行 `vava-network setup radioX`。脚本只新建专用 UCI sections，拒绝覆盖已有专用网络或 Wi-Fi 密钥；保存后检查并应用网络配置。
3. 生成新的录像密钥：`vava-agent --storage-key-create /etc/vava/record-keys`。已有设备应恢复原密钥，不能重新创建替代。
4. 把 Server 初始化得到的独立 `agent/server-key` 安装为 `/etc/vava/server-key`，权限 `0600`，配置 Server 域名/IP、UDP 端口和一致的设备 ID。仅本地配对时可显式选择本地独立模式。
5. 在 LuCI 核对正式 SSID、原厂基站 SN、存储路径后启用服务。原厂摄像头要求正式 SSID 以 `VAVA-` 开头、31 字节以内；摄像头侧地址保留 `172.26.168.1/16`。
6. 在 LuCI 的“摄像头配对”标签打开 120 秒配对窗口，让摄像头进入配对状态。弹窗在临时热点就绪后显示实际倒计时及配对结果；可以在弹窗内取消配对。临时 SSID 按原厂基站 SN 后八位生成 `VAPair-XXXXXXXX`，摄像头正式回连后关闭热点。

配对成功后目前仍需在 Server 的摄像头设置中明确选择工作模式；配对流程没有自动下发完整模式。新摄像头的兼容布防策略为持续布防、中灵敏度，沿用摄像头已有模式。详见 [摄像头控制](../docs/CAMERA_CONTROL.md) 与 [功能状态](../docs/FEATURES.md)。

## 检查加密传输依赖

安装或升级后、连接 Server 前，可以执行：

```sh
/usr/bin/vava-agent --transport-check
```

成功返回 `{"ok":true,"dtls_version":"1.2","psk":true}`；失败返回稳定的错误码并以非零状态退出。此命令不启动服务、不读取密钥、不修改配对或状态目录。

Agent 使用 DTLS 1.2 / PSK。部分路由器的系统 OpenSSL 虽然导出 DTLS 函数，却在构建时裁剪了对应能力，不能仅凭库名或符号判断可用。正式 OpenWrt 包要求 `OPENSSL_WITH_DTLS` 和 `OPENSSL_WITH_PSK`；手工复制 Agent 不会自动补齐已安装的系统库。遇到 `dtls_1_2_unavailable` 等错误，应使用具备这些能力的目标 SDK 依赖，或将匹配目标 ABI 的完整 OpenSSL 静态链接进 Agent，不应直接替换路由器其他服务共用的库。部署边界见 [部署说明](../docs/DEPLOYMENT.md)。

## 升级与备份

见 [部署与运维](../docs/DEPLOYMENT.md)。多个实例不能同时占用摄像头端口；先备份状态、录像密钥和配置，停止服务，再替换匹配的程序。保留 `/etc/vava-state`、`/etc/vava`、`/mnt/vava` 路径；部署升级本身不重新配对、不重建密钥。
