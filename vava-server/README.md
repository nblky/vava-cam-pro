# VAVA Server

独立的 C17 管理与媒体服务。小主机/NAS 可原生运行；OpenWrt 路由器可编译 `vava-server` 包；Docker 使用仓库根目录作为构建上下文。浏览器前端和 API 属于 Server，摄像头侧工作交给 Agent。

完整直播需要编译时安装 FFmpeg 5+ 开发库及 pkg-config；运行时还需要 FFmpeg 命令行程序用于回放适配、缩略图和对讲。具体依赖与主机工具链见 [根目录构建说明](../README.md#构建与验证)。缺少开发库时管理服务仍可构建，但连续直播链路不可用。

## 原生 Linux / NAS

```sh
cmake -S . -B build-server -DVAVA_BUILD_AGENT=OFF -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build-server -j4
cmake --install build-server
# 在私有目录初始化；不会连接、重置或更改现有基站。
python3 tools/provision_server.py /private/vava-deploy --device-id HOMEWRK \
  --server-binary build-server/vava-server/vava-server
```

生成 `config/devices.keys`、`config/initial-admin-password`、`agent/server-key` 与 `data/record-keys`，权限按当前用户收紧。部署用户必须拥有配置、密钥和状态文件。初始管理员为 `admin`，密码从生成的私有文件读取；初始化数据库后这个文件不会重置账号。将 `agent/server-key` 单独部署到对应基站。

```sh
VAVA_DATA_DIR=/private/vava-deploy/data \
VAVA_DEVICE_KEYS=/private/vava-deploy/config/devices.keys \
VAVA_ADMIN_PASSWORD_FILE=/private/vava-deploy/config/initial-admin-password \
VAVA_FFMPEG=/usr/bin/ffmpeg \
  /usr/local/libexec/vava-server-run
```

默认 UDP 监听 `0.0.0.0:3731`，管理 HTTP 监听 `127.0.0.1:8080`，供本机 HTTPS 反向代理访问。直接对外提供 Web 时设置 `VAVA_WEB_BIND=0.0.0.0`、`VAVA_WEB_CERT` 和 `VAVA_WEB_KEY`，两份 TLS 文件必须可读。浏览器访问公网/局域网管理平台使用 HTTPS。

完整环境变量见 `deploy/server.env.example`。systemd 模板在 `deploy/systemd`；创建 `vava` 服务用户，将数据部署在 `/var/lib/vava-server`、配置部署在 `/etc/vava-server` 并由该用户拥有后，安装并启用服务。模板的 `ReadWritePaths` 对应默认数据位置，变更位置时同步修改。

## Docker / NAS 容器

```sh
# 在 vava-cam-pro 仓库根目录执行
python3 tools/provision_server.py /private/vava-deploy --device-id HOMEWRK
# 在 /private/vava-deploy/config/tls 放入 fullchain.pem 和 server.key。
# 私有配置、TLS 私钥和 data 必须由容器运行 UID/GID 拥有。
export VAVA_DEPLOY_ROOT=/private/vava-deploy
export VAVA_UID="$(id -u)" VAVA_GID="$(id -g)"
docker compose -f vava-server/deploy/docker/compose.yaml up -d --build
```

默认映射 UDP 3731 和 HTTPS 8443；通过 `https://服务器地址:8443` 访问。TLS 证书需覆盖访问域名并被浏览器信任。镜像不包含设备密钥、账号密码或录像；配置只读挂载，持久状态、存储密钥与云短片放在独立数据挂载。Docker 运行非 root、只读根目录。`VAVA_UID/VAVA_GID` 默认 10001；上例使用初始化文件的实际所有者。

Dockerfile 使用 Debian 官方软件包，可按构建主机架构生成 amd64/arm64 镜像；Server 镜像构建与目标 NAS 运行尚待验证，详见验证文档。

## OpenWrt 路由器 Server

```sh
python3 tools/export_openwrt.py /path/to/openwrt/package/vava-cam-pro --component all
# 在 OpenWrt SDK 内
make package/vava-server/compile V=s
```

Server 的 UCI 为 `/etc/config/vava-server`，服务为 `/etc/init.d/vava-server`，默认停用。另装 FFmpeg 才能在网页播放、抓图和转换对讲音频。Server 的 SQLite、FFmpeg 与存储需求应按路由器能力规划；安装 Agent 不会隐式安装 Server。

## 界面与存储语义

- 实时观看中的“录像”保存当前接收的音视频到观看设备。
- TF 卡视频读取基站 TF/USB 上的事件短片；回放需要按需调取，调取缓存不是云存储。
- 云存储只列出按摄像头云上传开关实际上传到 Server 的短片，基站离线时仍可查看已有副本。
- NAS 属于每台基站的私有挂载设置，与云存储分开。每摄像头 NAS 备份开关独立。

API、角色和任务语义见 [管理 API](../docs/API.md)；逐项实现与实机验收状态见 [功能状态](../docs/FEATURES.md)。
