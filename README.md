# remarkx — 在 reMarkable 2 上看 X（Twitter）

在 reMarkable 2 墨水屏上舒适地浏览 X 首页关注时间线：
**整页位图阅读**，像翻报纸一样翻页，随时用笔做批注。

```
┌────────────────────────────┐   HTTPS (经代理)   ┌──────────────┐
│  reMarkable 2              │ ──── X (Twitter) ─→│  X 网页端     │
│  /home/root/xreader/xr     │                     │  GraphQL     │
│  Qt6 Quick + epaper QPA    │ ◄── 关注时间线 JSON ─│  (Grok译文)  │
│  ├ 直连抓取 · 设备端渲染    │                     └──────────────┘
│  ├ 手势翻页 · 笔迹层        │
│  ├ 图片懒加载 · 全屏看图     │
└────────────────────────────┘
```

设备完全独立：抓取、排版渲染都在设备端完成，不再依赖家中 PC 中转站。
打开即实时抓取，翻到书尾实时续抓更早内容（无后台定时抓取）。

## 设备端 `xreader-app/`

Qt 6.8 交叉编译的单一可执行文件 `xr`，运行于系统自带 epaper QPA/QSG 插件：

- **直连抓取**：带 Cookie 的 GraphQL 请求拉 For You + Following 时间线
  （1:1 交错合并去重）；X 网页端内嵌的 Grok 译文优先，无译文显示原文
- **设备端渲染**：QPainter 双栏流式排版整页位图（转推/引用卡片、互动数、
  绝对时间戳、超宽省略号截断），QQuickImageProvider 内存出图，翻页零编码
- **图片懒加载**：先出文字页（图片位置为占位框），图片异步下载成功后在
  原槽位贴图，失败保持"加载失败"占位；头像下载到位自动重绘
- **图片全屏**：手指点按任意图片 → 全屏居中最大化显示，左右滑在
  该推文的多张图间切换，点按/顶部下滑关闭
- **手势**：左右滑翻页、顶部下滑退出、底部上滑刷新（边缘手势任何时候可用）
- **笔迹**：笔即书写，压感线宽；笔迹逐页保存（`.draw.png`），翻页自动保存恢复；
  笔点底部区域可点击 UI（合成鼠标事件）
- **收藏**：写字即收藏——按笔迹起始位置锁定对应帖子，单独渲染"帖+笔迹"图
  （帖子跨页/跨栏拆分时各块自上而下拼合，笔迹保持相对帖子的位置），连同原始
  帖子链接存入 `book/`（`favs.json` 索引），**不在 UI 展示**；若安装脚本配置了
  Telegram bot，则把图片连同原始帖子 id 作为说明推送到目标 chat
- **电源键**：短按 = 屏显提示后挂起休眠（进度已存），再按唤醒回到原处；
  5 分钟无操作自动休眠
- **容错**：无 Cookie/网络失败显示错误页，重试按钮可用；触屏 Y 轴经
  `QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=inverty` 校正

构建需 Remarkable 官方 SDK（Qt 6.8 sysroot）：

```bash
cd xreader-app
cmake -B build && cmake --build build   # 在 SDK 环境中
```

### 启动机制（`device/launcher/`）

设备主用途是读电子书，因此不做常驻阅读器。启动入口为**手势**：

```
长按顶部中央(≥3s, 位移≤40px, 仅手指触摸/笔迹免疫)
  → hello-hotkey(常驻小进程, 开机自启)
  → run-reader.sh: 停 xochitl，直接进入阅读器
  → 退出阅读器(顶部下滑)：自动拉回原生 xochitl
```

> 曾试验"书架特殊书点开启动"（监视文档 lastOpened），实测点开只正常打开
> PDF、守护进程无法可靠触发，已放弃（详见 git 历史）。

## 配置与部署（`deploy/`）

设备直连 x.com 需要：**代理**（家中 PC 的 Clash/V2Ray，须能被设备访问）
+ **X 账号 Cookie**（浏览器登录 x.com 后由脚本自动提取）。

```bash
./deploy/install-remarkable.sh <设备IP> \
    --proxy http://192.168.1.100:7890 \
    --browser brave        # 或 --cookie-file /path/cookies.json
# 可选：笔迹收藏推送到 Telegram
./deploy/install-remarkable.sh <设备IP> \
    --proxy http://192.168.1.100:7890 \
    --browser brave \
    --telegram-bot 123456789:AAF... \
    --telegram-chat 987654321   # 或 @频道名
```

脚本会：交叉编译 `xr` + `hello-hotkey` → 从 PC 浏览器导入 X Cookie →
部署 `/home/root/xreader/{xr,config.json,cookies.json,fonts/remarkx-cjk.ttf}`
→ 注册 hello-hotkey 开机自启。

### 设备配置 `/home/root/xreader/config.json`

```json
{
  "proxy": "http://192.168.1.100:7890",
  "cookies": "/home/root/xreader/cookies.json",
  "telegram_bot": "123456789:AAF...",
  "telegram_chat": "987654321"
}
```

- `proxy` / `cookies`：X 直连所需（见安装脚本）
- `telegram_bot` / `telegram_chat`（可选）：写字收藏后把"帖+笔迹"图推送到该
  Telegram 聊天，图片说明为该帖的完整链接；不配置则只本地保存。发送全程后台，
  直接走 HTTP POST（multipart）到 `api.telegram.org`，并复用 `proxy`（若已配置）；
  待发消息先写入 `pending.json`（`book/` 同目录），发送成功才出队，失败按
  指数退避自动重试，程序异常退出/重启后也会自动补发未完成的消息；**推送成功即
  删除本地帖图以节约空间**（笔迹层 `.draw.png` 保留，供翻页恢复）

### 字体

渲染字体为 `xreader-app/fonts/remarkx-cjk.ttf`（方正书宋，GBK 全覆盖）。
更换字体只需**同名覆盖**该文件再运行安装脚本即可。

## 仓库结构

```
xreader-app/      设备端 Qt6 应用源码（直连抓取 + 渲染 + 阅读器）
device/launcher/  启动机制：hello-hotkey 长按守护 + run-reader.sh + 服务单元
deploy/           安装脚本 + Cookie 导入工具 + 服务单元
```

## 安全说明

- X 账号凭据（Cookie）只存设备本地 `/home/root/xreader/cookies.json`
  （gitignore，不入库），不上传任何第三方
- 使用小号 + 浏览器同源 Cookie 方式，行为等同网页浏览；非官方 API，风险自担
