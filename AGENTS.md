# AGENTS.md — remarkx 开发指南

本文件是给 AI 编码 Agent 的长期项目指南：技术栈、构建/部署方法、代码约定、
关键技术决策与修改代码时的坑。事实以代码为准；README 与代码不一致时以代码为准
（已知一处：README"仓库结构"把字体写成根目录 `fonts/`，实际在 `xreader-app/fonts/`）。

## 项目概览

**remarkx** 是 reMarkable 2 墨水屏平板上的 X（Twitter）阅读器，约 7500 行代码：

- X 关注时间线渲染成 **1404×1872 整页双栏位图**，像翻报纸一样翻页
- **写字即收藏**：手写笔在帖子上批注 → 笔迹像素命中帖子块 → 渲染"帖子+笔迹"
  合成图存本地（`book/`），可后台推送 Telegram（说明文字 = 帖子链接，成功即删本地图）
- **设备端完全独立**：抓取、渲染、推送全部在设备上完成，不依赖 PC 中转

## 技术栈

| 层 | 技术 |
| --- | --- |
| 设备端应用 | C++（Qt 6.8：Quick / Network / Gui / GuiPrivate）+ QML，CMake ≥ 3.16 |
| 交叉编译 | reMarkable 官方 SDK（Qt 6.8 sysroot），目标 **ARM 32 位**（arm-remarkable-linux-gnueabi） |
| 设备 | reMarkable 2，Linux + BusyBox，墨水屏经 Qt **epaper QPA** 平台插件 |
| 启动守护 | 纯 C（直接轮询 `/dev/input`），systemd 服务 |
| 部署 | bash（SSH/SCP + md5 增量）+ Python 3（yt-dlp 提取浏览器 Cookie） |

## 目录结构

```
remarkx/
├── xreader-app/            设备端 Qt6 应用源码
│   ├── main.cpp            入口：崩溃处理器、QML 引擎装配、各组件接线
│   ├── xclient.*           X Web GraphQL 抓取 + 媒体/头像懒加载
│   ├── renderer.*          整页渲染：双栏流式排版、全文页、收藏帖图 renderFavorite
│   ├── pagestore.*         页面状态机：翻页、收藏索引 favs.json、Telegram 联动
│   ├── telegram.*          后台队列：pending.json 持久化、指数退避、重启补发
│   ├── inkitem.*           压感笔迹层（逐页存/恢复 .draw.png、橡皮擦）
│   ├── stylus.*            电磁笔事件 + 校准（calib.json）
│   ├── powerkey.*          电源键（短按挂起休眠）
│   ├── touchguard.*        手掌/误触过滤（笔使用中吞掉一切手指触摸）
│   ├── crashctx.h          全局崩溃上下文（remarkxSetCtx）
│   ├── Main.qml            主阅读界面（手势、全屏看图/全文、分页信息）
│   ├── Menu.qml            启动确认菜单（--menu 加载；当前部署未使用，见"注意事项"）
│   └── fonts/remarkx-cjk.ttf  渲染字体（方正书宋；同名覆盖即换字体）
├── device/launcher/        启动机制
│   ├── hello-hotkey.c      长按守护：顶部中央 ≥2s（位移≤40px，仅手指）
│   ├── run-reader.sh       停 xochitl → 起 xr → 退出恢复 xochitl（实际入口）
│   ├── hello-hotkey.service  systemd 单元（Restart=always，开机自启）
│   └── hello-launch.sh     死代码（被 run-reader.sh 取代，但安装脚本仍会拷贝）
├── deploy/
│   ├── install-remarkable.sh   一键部署：编译 + Cookie 导入 + 增量 scp + 注册服务
│   ├── import-cookies.py       借 yt-dlp 从 PC 浏览器提取 X Cookie（支持 v11）
│   ├── make-shot.py            生成 README/Release 封面图（设备外壳合成）
│   └── build/                本地构建产物（gitignore，勿手改）
├── .venv/                  Python 环境（yt-dlp 等；无依赖清单，见"未知信息"）
└── README.md               完整文档（中文，与代码基本同步）
```

## 构建

**前提**：必须在 reMarkable SDK 交叉编译环境中（本机 SDK 位于
`/home/wwq/remarkable-sdk`，可用环境变量 `RM_SDK` 覆盖）。本机编译器编出的
`hello-hotkey` 会 203/EXEC 跑不起来。

```bash
cd xreader-app
cmake -B build -DREMARKX_ASAN=OFF    # 必须显式 OFF（原因见下）
cmake --build build
# 输出 build/xr：ARM 32 位单文件，QML 已内嵌（qt_add_qml_module, URI=xreader）
```

- **ASan 默认 OFF**：运行时开销大，且设备端没有 `libasan.so.8`——按 ON 构建
  部署后一启动就崩。`-DREMARKX_ASAN=OFF` 必须显式传：旧 CMake 缓存可能残留 ON。
  安装脚本会用 readelf 校验产物不含 libasan 依赖，不通过则中止。
- 调试崩溃时才 `-DREMARKX_ASAN=ON` 重编；崩溃信息写入设备端 `crash.log`。
- `hello-hotkey` 由安装脚本自动交叉编译（SDK gcc，`-mcpu=cortex-a7 -mfpu=neon
  -mfloat-abi=hard`），不需要手动构建。
- 链接选项 `-rdynamic`：让崩溃日志能解析主程序符号，勿删。

## 部署与运行

**部署纪律：未经用户明确指示，Agent 不得部署**——不运行
`install-remarkable.sh`、不 SSH/SCP 连设备、不改动设备端任何文件。
部署会停服务并覆盖正在运行的阅读器，且设备 IP 未记录在仓库中
（需向用户确认）。构建、改码、本地验证、git 提交均不受此限。
（2026-09 一次修完 bug 后 Agent 自行扫局域网找设备 IP 准备部署，
被用户制止，故记入此条。）

```bash
./deploy/install-remarkable.sh <设备IP> \
    [--proxy http://PC:7890] \
    [--cookie-file F | --browser brave,chromium] \
    [--timezone Asia/Shanghai] \
    [--telegram-bot TOKEN] [--telegram-chat ID]
```

流程：导入/校验 Cookie → 交叉编译 xr 与 hello-hotkey → 与设备端 md5 逐文件比对 →
**有变化才**停服务（`systemctl stop hello-hotkey`、`killall -9 xr`）→ scp 更新文件 →
安装并启用 systemd 服务。全量未变化时不动运行中的阅读器。

**设备端布局**（`/home/root/xreader/`）：

```
xr                     阅读器可执行文件
run-reader.sh          启动/退出脚本
config.json            代理 / Cookie 路径 / 时区 / Telegram
cookies.json           X 登录态 Cookie
fonts/remarkx-cjk.ttf  渲染字体
book/                  收藏帖图 + 笔迹层
favs.json              收藏索引（含 "sent"：已推送成功的帖子 mid 去重集合）
pending.json           Telegram 待发队列
```

**启动链**：

1. `hello-hotkey` 守护（systemd，开机自启）轮询 `/dev/input` 的 `pt_mt` 设备；
   长按顶部中央（屏幕坐标 X 552–852、Y 0–200）≥2s 且位移 ≤40px 才触发；
   **仅当 xochitl 在运行且 xr 未运行**时才启动，防误触/防重复启动。
2. `run-reader.sh`：`killall -9 xr`（清残留）→ `systemctl stop xochitl` →
   删 `/tmp/epframebuffer.lock /tmp/epd.lock` → 设
   `QT_QUICK_BACKEND=epaper`、`QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=inverty`
   （触屏 Y 轴翻转）→ 运行 `xr -platform epaper` → 退出后恢复 xochitl。

## 测试与验证

- **没有单元测试、没有测试框架、没有 CI**（仓库中未找到任何测试代码/配置）。
- 常规验证 = 部署到真机手工测试。
- 渲染可在 PC 上用本机 Qt 本地验证：`Renderer::renderPage` 可直接把页面位图
  渲染成 PNG（README 称配合 fake feed 数据使用；仓库中未找到专门的测试入口，
  辅助代码是否存在未知）。
- 崩溃调试：ASan 构建 + 内置崩溃处理器（发布版也保留），见"关键技术决策"。

## 配置

设备端 `/home/root/xreader/config.json`（由安装脚本生成，勿手工编辑设备文件
而不经过脚本）：

| 字段 | 说明 | 解析位置 | 缺省/兜底 |
| --- | --- | --- | --- |
| `proxy` | X 直连代理（无协议头自动补 `http://`，支持 socks5） | `xclient.cpp`、`telegram.cpp`（推送复用） | 空 = 直连 |
| `cookies` | Cookie 文件路径 | `xclient.cpp` | 固定 `/home/root/xreader/cookies.json` |
| `timezone` | 帖子时间显示：IANA 名或 ±HH:mm | `renderer.cpp` | 设备本地时区（设备常被设成 UTC，会慢 8 小时） |
| `telegram_bot` | Bot Token | `telegram.cpp` | 不配置 = 纯本地收藏 |
| `telegram_chat` | 数字 chat id / 群组 `-100…` / `@频道名` | `telegram.cpp` | 不配置 = 不推送 |

注意：三个组件（xclient/telegram/renderer）**各自独立读一遍 config.json**，
没有统一配置层；字段拼错会静默走缺省值。

`cookies.json` 格式：`{cookie名: 值}`，至少需 `auth_token` 与 `ct0`
（`twid`/`guest_id` 更佳）。

## 代码规范

- **注释与提交信息均为中文**。提交格式 `xr: <改动>——<根因/细节解释>`，
  习惯把 bug 根因和取舍写清楚（参考 git log）。
- C++ 风格：头文件 `#pragma once`；成员变量 `m_` 前缀；Qt 信号/槽 +
  `Q_PROPERTY`/`Q_INVOKABLE` 暴露给 QML。
- **日志策略**：不使用 `qInfo` 调试日志（`87657d8` 已整体移除，勿加回）；
  错误用 `qWarning`；保留崩溃处理器，并在关键操作处调 `remarkxSetCtx("...")`
  记录崩溃上下文。
- QML 模块名 `xreader`（`qt_add_qml_module`，文件内嵌进单二进制）；
  主程序名 `xr`（`OUTPUT_NAME`）。
- 凭据与运行数据（`cookies.json`、`config.json`、`favs.json`、`pending.json`、
  `state.json`、`calib.json`、`*.log`）一律 gitignore，**绝不入库**。

## 关键技术决策（含原因）

1. **设备端直连 X Web GraphQL**（带 Cookie 会话）而非 PC 中转：设备完全独立，
   无需常开 PC。代价是依赖非官方接口，有账号风控风险（README 已声明）。
2. **整页位图渲染**（QPainter 双栏流式排版 → `QQuickImageProvider` 内存出图）：
   墨水屏不擅长增量刷新，整页出图 + 翻页零编码最稳。
3. **图片懒加载**：先出文字页，图片占位框异步下载后原位贴图；头像到位自动重绘。
4. **写字即收藏**：笔迹像素命中当前页帖子块（一笔可命中多帖，各收一条）；
   `renderFavorite` 把跨页/跨栏拆分的帖子重新排成单栏单卡，按块精确叠加笔迹。
5. **Telegram 后台队列**：待发先持久化 `pending.json`，成功才出队；失败按
   10s→20s→40s…（上限 1h）指数退避；进程重启 `flush()` 立即补发；
   成功即删本地帖图省空间。
6. **按帖子 mid 全局去重**（`favs.json` 的 `sent`）：同一帖子无论翻页/刷新/
   重排再出现都不再收藏、不再推送。
7. **启动用长按热键守护**（直接轮询 `/dev/input`）：曾尝试"书架特殊书点开启动"
   （监视 xochitl 文档 lastOpened），实测不触发，已放弃（见 git 历史）。
8. **内置崩溃处理器**（SIGSEGV/ABRT/BUS/ILL → `crash.log`）：记录信号、
   PC 偏移、栈回溯、`/proc/self/statm` 内存用量与崩溃上下文。基址在启动期用
   `dladdr` 记录（信号处理器内调 dladdr 可能因链接器锁死锁，故只做纯算术）。
   配合 `-rdynamic` 离线 addr2line。发布版也保留此处理器。
9. **QML 内嵌**（`qt_add_qml_module`）：设备端只有一个 `xr` 文件，部署简单。
10. **页面位图 LRU 缓存**（PageStore `m_pageCache`）：翻页回看直接复用，
    不重新排版渲染。
11. **手势分工**（防误触为核心）：笔只写字（无点按）；手指 tap 有 1 秒确认窗口
    （期间任何新触摸/笔触碰即取消）；边缘滑动 = 翻页/顶滑退出/底滑刷新；
    笔使用中 TouchGuard 吞掉一切手指触摸。
12. **部署两阶段拷贝**（先 md5 比对收集变更 → 停服务 → 再 scp）：
    Linux 不允许覆盖正在执行的二进制（ETXTBSY）。
13. **进程检测直接读 `/proc/<pid>/comm`**：BusyBox 的 `pgrep -x` 不可靠，
    `-f` 会误匹配 shell 自身。
14. **Cookie 导入借 yt-dlp**：Chrome v11 Cookie 需 2024-11 起的新版 yt-dlp
    解密，故部署优先使用项目 `.venv` 的 python。
15. **时区可配置**：设备默认 UTC 导致帖子时间慢 8 小时，故 config.json 加
    `timezone` 字段而非改设备时区。

## 修改代码时特别注意

- **32 位整数溢出**：毫秒 epoch（`Date.now()` ≈ 1.79e12）**装不进 int32**。
  QML 里存时间戳必须 `property real`；C++ 用 `qint64`/`long long`。
  历史真实 bug：`pressMs` 用 `property int` 被截断，导致每个 tap 都判成
  手掌托屏被拒（见提交 `494a281`）。QML 属性 `int`↔`real` 的类型变更
  会改变运行行为，改动时逐处核对。
- **QNetworkReply 必须设超时**：代理挂起时不设 `setTransferTimeout` 则
  `finished` 永不触发（历史 bug 导致 `m_fetching` 永久卡死、刷新静默失效）。
  现有约定：API 请求 30s、媒体下载 60s。
- **硬编码路径**：设备根目录 `/home/root/xreader`（`main.cpp` 的 `baseDir`）、
  崩溃日志路径均写死。本机 SDK 路径 `/home/wwq/remarkable-sdk`（`RM_SDK`
  可覆盖）。改动这些路径需全局同步（含安装脚本）。
- **BusyBox 限制**：无 `pkill`（用 `killall`）；`pgrep -x` 不可靠。
  设备端 shell 脚本遵循现有写法。
- **删除文件需同步部署链**：`hello-launch.sh` 是死代码但安装脚本仍拷贝；
  `Menu.qml` 当前部署未使用（`--menu` 参数仅 `main.cpp` 引用，
  `run-reader.sh` 不带该参数）——若要删除，需同步改 `CMakeLists.txt` 的
  `QML_FILES` 与 `main.cpp`。
- **不要加 qInfo 调试日志、不要提交任何凭据/运行数据**（见代码规范）。
- **换字体**：同名覆盖 `xreader-app/fonts/remarkx-cjk.ttf` 后重新部署即可，
  文件名是渲染端约定，不要改。

## 未知信息（勿猜测，需先核实）

- reMarkable 2 的具体系统版本 / xochitl 版本：仓库未记载。
- 是否兼容其他 reMarkable 型号（如 reMarkable 3）：未知。
- `.venv` 的 Python 依赖**没有清单文件**（无 requirements.txt / pyproject.toml）；
  实际用到 yt-dlp、Pillow 等，具体版本集合未知。
- 其他开发机上的 SDK 安装路径：未知（本机为 `/home/wwq/remarkable-sdk`）。
- README 提到的"PC 本地渲染验证"（renderPage + fake feed）：仓库中未找到
  专门的测试入口，是否存在配套辅助代码未知。
