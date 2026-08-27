# remarkx — 在 reMarkable 2 上看 X（Twitter）

在 reMarkable 2 墨水屏上舒适地浏览 X 首页关注时间线：
**整页位图阅读**，像翻报纸一样翻页，随时用笔做批注。

```
┌──────────────────────────┐      LAN (HTTP)      ┌──────────────────────────┐
│  reMarkable 2            │ ─GET /page?p=N&force→│  家中转站 (Ubuntu PC)     │
│  /home/root/xreader/xr   │ ◄────PNG 1404x1872───│  relay/relay.py          │
│  Qt6 Quick + epaper QPA  │                      │  ├ 按需抓取(过期才抓)     │
│  手势翻页 · 笔迹层        │                      │  ├ SQLite 缓存           │
└──────────────────────────┘                      │  ├ PIL 整页排版渲染       │
                                                  └───────────┬──────────────┘
                                                              │ 小号登录态(twikit)
                                                              ▼
                                                         X (Twitter)
```

## 设备端 `xreader-app/`

Qt 6.8 交叉编译的单一可执行文件 `xr`，运行于系统自带 epaper QPA/QSG 插件：

- **内容全屏**：PIL 渲染的 PNG 占满整屏；右下角显示「第 N 页 · 共 M 页」
- **手势**：手指左右滑翻页、顶部下滑退出、底部上滑刷新（边缘手势任何时候可用）
- **笔迹**：笔即书写，压感线宽；笔迹逐页保存（`.draw.png`），翻页自动保存恢复；
  笔点底部区域可点击 UI（合成鼠标事件）
- **电源键**：短按 = 屏显提示后挂起休眠（进度已存），再按唤醒回到原处
- **容错**：连接失败显示错误页，重试按钮可用，且退出/刷新手势始终有效
- 启动即强刷最新一页；触屏 Y 轴经 `QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=inverty` 校正

构建需 Remarkable 官方 SDK（Qt 6.8 sysroot）：

```bash
cd xreader-app
cmake -B build && cmake --build build   # 在 SDK 环境中
scp build/xr root@<rm2>:/home/root/xreader/xr
```

### 启动机制（`device/launcher/`）

设备主用途是读电子书，因此不做常驻阅读器，采用**原生观感的确认菜单**方案：

```
长按顶部中央(≥0.7s, 位移≤40px)
  → hello-hotkey(常驻小进程, 只听手指触摸, 笔迹免疫)
  → run-reader.sh: 先弹确认菜单 [启动阅读器] [取消]（10s 自动取消）
  → 确认后进入阅读器；退出/取消均自动拉回原生 xochitl
```

误触率归零： accidental 碰撞需要"长按住 + 再点一次按钮"两个刻意动作。
安装：

```bash
${CC} -O2 -o hello-hotkey device/launcher/hello-hotkey.c   # SDK 交叉编译
scp device/launcher/{hello-hotkey,run-reader.sh} ...
# hello-hotkey 放 /home/root/hello-launch/，服务 hello-hotkey.service 开机自启
```

## 中转站 `relay/`

Python 3 + aiohttp + httpx + twikit + PIL：

- **按需抓取**：仅当设备请求首页且数据过期（`poll_seconds`）才抓一次 X；
  `force=1` 强制刷新。一次阅读会话通常只触发一次上游请求
- **流式双栏排版**：卡片按原子拆分（头部/图片不可拆、正文逐行可拆），
  跨栏跨页自动接续（续排块带「┆续」标记），单页填满不半截
- **防越界**：任何文本元素超宽一律省略号截断；链接隐藏、emoji 剥离
- **绝对时间戳**：`MM-DD HH:MM`（跨年含年份）
- **书页簿记**：每页 PNG 按 `YYYYMMDDNNN` 编号落盘 `book/`，
  `book.json` 记录 版本/页号/帖子索引，支持断点续读与历史翻页缓存

```bash
python -m venv .venv && .venv/bin/pip install -r relay/requirements.txt
cp relay/config.example.json relay/config.json   # 填账号与代理等

./relayctl login        # 导入浏览器 Cookie 登录（见 cookies_browser.py）
systemctl enable --now remarkx-relay    # 开机自启（deploy/remarkx-relay.service）
curl http://<pc>:8788/api/status
```

## 仓库结构

```
relay/            中转站（aiohttp 服务 + 抓取器 + 渲染器 + 可选翻译管线）
xreader-app/      设备端 Qt6 应用源码（阅读器 + 启动确认菜单 --menu 模式）
device/launcher/  启动机制：hello-hotkey 长按守护 + run-reader.sh + 服务单元
deploy/           systemd 单元等部署资产（remarkx-relay.service）
```

> 历史遗留：早期 rM1（rmkit/simple）与 rm2fb 方案的脚本已删除，
> 如需追溯见 git 历史。

## 安全说明

- X 账号凭据与会话只存在中转站本地 `relay/data/`（已 gitignore），不上传任何第三方
- 使用小号 + 浏览器同源 Cookie 方式，行为等同网页浏览；非官方 API，风险自担
