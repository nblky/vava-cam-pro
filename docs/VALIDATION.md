# 测试与实机验收

当前功能的验证范围统一见 [FEATURES](FEATURES.md)。本页给出可重复命令与待验收项，不混合旧版本测试数量或用模拟结果代替硬件结论。

## 自动测试

完整构建使用 C17、严格 POSIX 特性宏及 `-Wall -Wextra -Wpedantic -Werror`。CI 运行 Ubuntu 24.04 / Clang、Python 3.12、Node 22；GitHub Actions `v6` 的运行时为 Node 24，与项目 JS 测试版本分开。

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
python3 tools/check_repository.py --history
cmake -S . -B build-test -DCMAKE_C_COMPILER=clang \
  -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build build-test -j4
ctest --test-dir build-test --output-on-failure -j2
```

必须安装根 [README](../README.md) 中的 FFmpeg CLI、开发库、Node 和 Python 依赖。正常完整配置注册 **44 个 CTest 测试组**；`web-state` 内含多项 JS 测试，不把它们混作相同计数。缺少媒体库、Node，或关闭某组件会减少项目数，不能报告为完整 44 项通过。

CI 结果以具体提交的 GitHub Actions 记录为准。本地日志在被忽略的 `artifacts/`；这些文件不作为分发源码的依赖。CI 不刷机、不更新运行中 HomeWRK、不连接原厂云。

| 测试领域 | 主要测试组 | 能证明什么 |
| --- | --- | --- |
| 摄像头设置 | `settings-state`、`desired-state`、`sdk-control-config` | 范围、持久化、revision、身份、配置协调和模拟报文 |
| 配对/删除 | `sdk-pairing`、`sdk-removal`、`platform-retry-state` | 状态机、恢复、取消、热点失败处理、录像保留逻辑 |
| 低功耗与连接 | `sdk-lowpower`、`link-recovery-state`、`sdk-integration` | 模拟连接、超时、重连、重启、任务幂等与事件确认 |
| 本地短片 | `sdk-clips`、`sdk-archive`、`sdk-archive-encryption`、`record-index-state` | 触发、检测类型、完整音视频、加密、索引、删除和恢复 |
| 云/调取 | `sdk-uploads`、`sdk-exports`、`sdk-retention`、`sdk-encryption`、`file-store-state` | 合成片段可靠上传、续传、来源隔离、留存与读取保护 |
| NAS | `sdk-nas` | 本地普通目录复制、MP4、任务、冲突和配置恢复；**不验证 NFS 挂载** |
| 实时媒体 | `sdk-live`、`sdk-live-duration`、`sdk-web-media` | 租约、音视频、无固定短时截止、权限与媒体接口 |
| 连续切档 | `h264-state`、`continuous-mux`、`sdk-continuous-live` | 六方向、参数代次、像素、音频时间轴、租约及录像门控 |
| Web / LuCI | `sdk-web`、`web-state`、`sdk-credentials` | 认证/ACL、任务、前端状态、配对弹窗、设置草稿、播放器和密钥重载 |
| 部署/维护 | `deployment`、`maintenance-tools`、`openwrt-mock` | 主机启动、源码导出、备份工具和模拟 helper；**不验证目标安装包** |

权限拒绝测试需用普通用户运行。root 可绕过 `chmod 0500`，会让模拟写失败场景失效；容器中应以非 root 执行测试。Node 18 的 WebCrypto 由测试显式导入，浏览器仍需受信任安全上下文。

## 本次 CI 修复覆盖

- Linux 严格 POSIX 环境下以 `nanosleep` 替代不再声明的 `usleep`，覆盖文件仓库测试。
- Server / Agent 改为 `sigaction` 注册持久处理器，密钥重复 `SIGHUP` 不应终止进程；密钥撤销、恢复与轮换均由 `sdk-credentials` 覆盖。
- 小控制结果不因协商批量传输而延长到大批量的 60 秒期限；异常重启后的重连由 `sdk-integration` 覆盖。
- NAS 无 Server 模式测试等待异步存储线程就绪，避免 socket 已可用却过早读取任务目录。

本次工作没有再次进行摄像头效果验收。通过哪些平台/测试及最终 CI 结论，应随提交核对运行记录，不把 macOS 单次通过代替 Linux 回归。

## 已有实机证据的范围

HomeWRK + 原摄像头已有配对、专用 AP、真实 PIR 事件加密落盘、解密校验、两级索引、既有片段回放、三档实际尺寸、镜像四方向、检测框及再次唤醒、真实 H.264 / AAC 播放、本机录制等记录。最近一次服务核验中，两台摄像头 1080P 音视频可完整解码，既有加密片段保留。

连续切档在 `VA-HS03 / 1.4.0.54` 上完成同次观看六方向、约六分钟呈现统计。它是特定硬件/固件的短程验收；功能默认关闭，不能外推为所有摄像头通用。现场录像、SN、密钥、抓包和原始日志不发布。

## 仍需实机或目标系统验收

| 优先级 | 场景 | 通过标准 |
| --- | --- | --- |
| P1 | 三种工作模式、自定义时长/间隔/灵敏度 | 逐项记录摄像头回显、真实触发间隔、短片时长、休眠与电量；不能只看保存成功 |
| P1 | 夜视、防闪烁、OSD、音量、音频门控 | 对应暗光/照明/扬声器场景实际验证，重连后再次核对 |
| P1 | 对讲 | 真实扬声器发声、停止、权限撤销及不支持浏览器的失败提示 |
| P1 | 恢复 / 删除摄像头 | 真机提示、断开/重新配对、配对登记一致，旧录像仍可访问 |
| P1 | 真实云上传 | HomeWRK 自动上传新事件，断网重试、完整校验、关闭不追溯、目标变化不误传 |
| P1 | 真实 NFS | 匹配目标内核/工具，挂载、UID/权限、TS+MP4、断网、卸载、任务恢复与不覆盖冲突 |
| P1 | 外部介质故障 | 重启、拔插、只读、满盘、断电与换卡隔离，已有录像/索引/密钥保留 |
| P2 | 连续媒体与自动策略 | 多浏览器、数小时、弱网、丢包/抖动、PIR 与切档瞬间竞争 |
| P2 | 四摄像头及计划 | 并发连接、内存/CPU、到点布防、跨午夜、时间修正与离线运行 |
| P2 | 正式交付物 | 目标 OpenWrt SDK 安装/升级包、Docker Server 镜像与实际 NAS 部署 |
| 独立工程 | 原 HS003 系统镜像 | 板级分区/校准/恢复入口、16 MiB 容量、64 MiB 内存及刷机回滚 |

## 安全检查边界

已有主机权限/解析器/异常恢复回归，以及有限的 Clang 静态分析、ASan / UBSan 与短程模糊测试记录。它们不等于覆盖全部线程、故障磁盘、第三方 CVE 或公网攻击面。当前没有完整 ThreadSanitizer、长期覆盖引导模糊测试、第三方组件全面 CVE 清单或独立安全审计结论。

摄像头本地协议仍受原固件限制；保持专用 AP 和防火墙，管理端使用 HTTPS 与独立凭据。具体边界见 [SECURITY](../SECURITY.md)。
