# 开发与 Git 上传

本仓库统一维护 `vava-server`、`vava-station`、`myvava-sdk`、LuCI、Web 和测试。当前构建入口见 [README](README.md)，全部文档见 [文档导航](docs/README.md)。不要把基站 TF 短片、Server 调取缓存、NAS 备份、云存储和观看设备本机录像合并为同一种存储任务。

只维护当前 Agent / Server 系统。功能改动同步更新 [功能状态](docs/FEATURES.md)，分别记录实现、自动测试和实机验收；有菜单或匹配回显不等于硬件效果已通过。原厂录像只读导入属于现有可选功能。

## 日常修改

使用独立的 `build-*` 构建目录。开发依赖装入 `.venv`；不要修改系统 Python。协议或持久格式有变化时，同时更新双方兼容处理、对应测试和协议文档。摄像头配置的持久值与心跳、电量、在线状态分开处理，避免高频写 Flash。

测试使用临时状态和模拟摄像头，不自动连接 HomeWRK 或现有 Server。实机部署必须备份配对、设置和录像密钥，并核对目标 ABI；普通构建和自动测试不代表硬件效果或功耗已验收。

```sh
python3 tools/check_repository.py
git diff --check
cmake --build build-test -j4
ctest --test-dir build-test --output-on-failure -j2
```

首次配置 `build-test` 的依赖与命令见 README。GitHub Actions 执行主机构建和回归；不刷机、不部署设备。

## 上传内容

提交源码、测试、示例配置、文档和第三方声明。`myvava-sdk/vendor/libcoap-4.3.5b.tar.gz` 是固定 SHA-256 的离线源码依赖，必须保留；同目录的补丁也必须提交。不要只上传两个业务子目录，否则会缺少共享代码。

`run/`、`artifacts/`、`build*/`、`.venv/`、录像、抓包、数据库、真实配置和密钥保留在本机。`.gitignore` 只能防止新增，已经跟踪的文件仍需检查。文档使用示例身份，不提交私有现场标识或旧项目研究资料。

上传前检查当前文件及历史中的常见凭据模式：

```sh
python3 tools/check_repository.py --history
git status --short
git diff --stat
git diff --check
# 后续修改经过人工确认后：
git add -A
git diff --cached --stat
git diff --cached --check
git commit -m "Prepare VAVA Home source repository"
# 将你的仓库地址配置为 origin 后再 push。
```

清理后的更新请使用 `git add -A` 暂存全部变更，包含旧文件删除和新增文件。`.gitignore` 不会自动取消已跟踪文件；不要只上传新增、修改的文件而遗漏删除操作。

检查工具只报告文件位置与问题类型，不输出匹配凭据；它不是完整的秘密扫描或漏洞审计。共享故障日志前仍需人工检查。

## 导出干净源码

通常直接提交当前仓库即可。需要仅共享当前源码时，可以从工作树导出到新目录。工具包含尚未提交但未被忽略的源码，先执行仓库检查，拒绝覆盖现有目录；不复制 `.git`、运行数据、录像或私有证据。导出后的 SHA-256 清单用于检查拷贝完整性。

```sh
python3 tools/export_source.py /path/to/new/vava-home-source
cd /path/to/new/vava-home-source
python3 tools/check_repository.py --verify-manifest
# 建议按 README 在此干净副本完成一次构建和测试。
git init -b main
git add .
git commit -m "Initial VAVA Home source"
# 配置你的远端，再推送。
```

导出仅整理当前磁盘文件，不代表已提交内容或历史中的某个版本。不要用 `git archive HEAD` 代替，尚未提交的功能和修复会遗漏。`SOURCE_MANIFEST.json` 是导出快照的校验清单，已被 Git 忽略；后续修改源码使用普通检查，只有核对未修改的导出包时才加 `--verify-manifest`。

## 代码许可

项目自有代码尚未选择开源许可证。公开托管前由维护者决定授权方式；本次整理不代为指定 MIT、GPL 或其他许可。第三方的原有许可和声明必须保留，见 [libcoap / OpenSSL](myvava-sdk/THIRD_PARTY_NOTICES.txt) 与 [minimp4](third_party/minimp4/README.md)。
