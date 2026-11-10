# VAVA Home 文档

本文档描述 `vava-cam-pro` 仓库中的 `vava-agent`、`vava-server`、管理 Web 和 LuCI。功能是否可用、经过何种验证，统一以 [功能状态](FEATURES.md) 为准；界面上出现按钮不等于完成实机验收。

| 文档 | 解决的问题 |
| --- | --- |
| [功能状态](FEATURES.md) | 已实现什么、自动测试覆盖什么、哪些实机效果尚待验证 |
| [架构](ARCHITECTURE.md) | 摄像头、基站、Server 与浏览器各自负责什么 |
| [部署与运维](DEPLOYMENT.md) | 依赖、部署入口、首次配置、升级和备份 |
| [摄像头控制](CAMERA_CONTROL.md) | 配对、配置、工作模式、计划、恢复与删除 |
| [实时观看](LIVE.md) | 画质、自动策略、连续切档、对讲与本机录像 |
| [存储](STORAGE.md) | 基站事件短片、索引、云存储、NAS、封面与删除边界 |
| [管理 API](API.md) | 独立前端、认证、任务、媒体与文件接口 |
| [界面使用](UI.md) | 当前菜单、状态含义与主要操作流程 |
| [验证方法与待验收项](VALIDATION.md) | 测试命令、证据范围、目标平台和实机验收清单 |

组件的具体命令见 [Agent / LuCI](../vava-station/README.md)、[Server](../vava-server/README.md)；设备间二进制协议见 [SDK 协议](../myvava-sdk/PROTOCOL.md)；贡献流程见 [开发说明](../CONTRIBUTING.md)，安全边界见 [SECURITY](../SECURITY.md)。

原厂固件反编译、App 研究、旧平台设计、迁移日志及现场调试流水已归档到仓库外。当前文档不依赖这些私有文件；固件、抓包、现场录像、凭据与测试产物不随源码分发。协议固定地址属于兼容要求，文档里的部署身份和服务器地址均为示例。
