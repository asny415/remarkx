# remarkx — 在 reMarkable 上看 X（Twitter）

在 reMarkable 1 墨水屏上舒适地浏览 X 首页时间线（关注的内容）：
**只看文本 + 图片**，没有视频、没有动图、没有无限滚动的诱惑。

```
┌─────────────────────┐         LAN (HTTP)          ┌──────────────────────────┐
│  reMarkable 1       │  ──GET /page?p=N──►        │  家中转站 (Ubuntu)        │
│  xreader.sh         │  ◄──PNG 1404x1872──        │  relay.py                 │
│  └ simple (SAS)     │                            │  ├ 轮询: twikit 拉首页    │
│    显示整页 PNG      │                            │  ├ 缓存: SQLite           │
│    4 个翻页按钮      │                            │  ├ 图片: data/media/      │
└─────────────────────┘                            │  └ 渲染: PIL 整页 PNG     │
                                                   └────────────┬───────────────┘
                                                                │ 小号登录态
                                                                ▼
                                                          X (Twitter)
```

**为什么这样设计**

- 设备端**零依赖**：只要系统自带的 `wget` + 一个 `simple` 可执行文件（rmkit 构建的
  SAS 解释器），不需要在墨水屏上跑 Python/浏览器，也不装 Toltec（你的 OS 3.27 已超出
  Toltec 支持范围，装它会软变砖；Vellum 也没有 python 包）。
- 中转站负责一切重活：登录、抓取、缓存、排版渲染。渲染成**整页位图**，墨水屏只显示
  一张 PNG + 几个按钮，刷新体验干净利落。
- 不付官方 API 费用：用 `twikit` 以你的**小号**会话抓取（等同网页行为，非官方，
  请低频轮询、只用小号）。

## 目录

```
relay/
  relay.py           # 入口：login / run / render / mockseed
  fetcher.py         # twikit 抓取 + 媒体下载
  store.py           # SQLite 缓存
  render.py          # PIL 页面渲染（1404x1872）
  requirements.txt
  config.example.json
device/
  xreader.sh         # 设备端阅读脚本（纯 shell）
  install_device.sh  # 设备端安装脚本
```

---

## 一、部署中转站（Ubuntu 电脑）

### 1. 安装依赖

```bash
cd remarkx
python3 -m venv venv
./venv/bin/pip install -r relay/requirements.txt
# 中文字体（渲染需要，二选一）：
sudo apt install fonts-noto-cjk    # 或 fonts-wqy-microhei
```

### 2. 配置

```bash
cp relay/config.example.json relay/config.json
```

| 字段 | 说明 |
|---|---|
| `port` | 服务端口（设备端要访问到它） |
| `proxy` | 访问 X 的代理，如 `http://127.0.0.1:7890`；国内网络基本必填（`x.com` 与 `pbs.twimg.com` 都走它） |
| `poll_seconds` | 轮询间隔（秒），默认 300；别太频繁 |
| `poll_count` | 每次拉取条数 |
| `page_size` | 每页候选条数（实际按版面裁剪，大图会占满整页） |
| `title` | 页面左上角标题 |
| `font` | 留空=自动找中文字体 |

### 3. 登录小号（首次）

```bash
./venv/bin/python relay/relay.py login
# 按提示输入：账号(用户名/邮箱) -> 密码 -> 2FA secret(可选)
# 出现验证码提示时输入 6 位 2FA 码
# 成功后会话保存到 relay/data/session.json，以后不用重复登录
```

### 4. 启动

```bash
./venv/bin/python relay/relay.py run
# 浏览器访问 http://<电脑IP>:8788 查看状态页
# http://<电脑IP>:8788/page?p=0  直接看渲染出的第一页 PNG
```

**先调试？没有账号也没关系**，用模拟数据：

```bash
./venv/bin/python relay/relay.py run --mock
```

### 5. systemd 开机自启（可选）

`/etc/systemd/system/remarkx.service`：

```ini
[Unit]
Description=remarkx relay
After=network-online.target
Wants=network-online.target

[Service]
WorkingDirectory=/home/你/remarkx
ExecStart=/home/你/remarkx/venv/bin/python relay/relay.py run
Restart=on-failure
RestartSec=10

[Install]
WantedBy=default.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now remarkx
```

> 安全提醒：`relay/data/session.json` 里是小号的登录 Cookie，
> 等同账号密码，**不要提交到 git**（.gitignore 已排除）、不要分享给他人。

---

## 二、部署设备端（reMarkable 1）

前提：设备连在**同一局域网**，电脑 IP 稳定（建议路由器里给电脑设静态 IP/主机名绑定）。

### 1. 安装（SSH 到设备执行）

```bash
ssh root@<设备IP>
```

把 `device/` 目录拷到设备（在电脑上执行）：

```bash
scp device/xreader.sh device/install_device.sh root@<设备IP>:/tmp/
ssh root@<设备IP> 'sh /tmp/install_device.sh http://<电脑IP>:8788'
```

安装内容（都不碰系统文件，可删）：
- `/opt/bin/simple` — rmkit 构建的 SAS 解释器（armhf 二进制，来自 build.rmkit.dev）
- `/opt/rmx/xreader.sh` + `/opt/rmx/config` — 阅读脚本与中转站地址

> 不需要、也不建议安装 Toltec：Toltec 仅支持 OS 2.6.1.71 ~ 3.3.2.1666，
> 你的 3.27.3.0 超出范围，安装会导致系统损坏（软变砖）。

### 2. 阅读

SSH 到设备执行：

```bash
sh /opt/rmx/xreader.sh
```

屏幕显示第一页（最新内容），底部四个按钮：

| 按钮 | 作用 |
|---|---|
| 上一页 | 回翻（已到最新时不动） |
| 下一页 | 看更旧的内容 |
| 刷新 | 回到最新 |
| 退出 | 退出（恢复原屏幕） |

### 3. 常用：SSH 别名

`~/.ssh/config`（电脑上）：

```
Host rmx
    HostName 192.168.1.50
    User root
```

以后 `ssh rmx 'sh /opt/rmx/xreader.sh'` 一条命令开读。

> 注意：`simple` 会一直占着这个 SSH 会话直到你点"退出"。
> 想关屏省电：阅读时直接合盖/息屏即可，SSH 断开后阅读会话结束。

---

## 三、排错

| 现象 | 处理 |
|---|---|
| 设备显示"无法连接中转站" | 电脑开机了吗？`curl http://<电脑IP>:8788/healthz` 通吗？防火墙放行了 8788 吗？ |
| 状态页显示"尚未登录" | 在电脑跑 `./venv/bin/python relay/relay.py login` |
| 状态页显示"登录态失效" | X 会周期性让会话过期，重新 `login` 即可（数据保留） |
| 中文是方块 | 中转站没装中文字体：`sudo apt install fonts-noto-cjk` |
| 图片不显示 | 看 relay 日志里的"媒体下载失败"；多半是代理没配好 |
| 抓取报 429/限流 | 把 `poll_seconds` 调大（600+） |
| 页面空白"还没有数据" | 正常（还没抓到）；或登录失败，看状态页 last error |

调试渲染排版（不改数据）：

```bash
./venv/bin/python relay/relay.py render /tmp/page0.png
```

---

## 四、已知边界

- **非官方**：twikit 模拟网页会话，X 改版可能坏（坏时日志有明确错误，修 fetcher 即可）。
- **频率**：轮询太频繁有被限流/风控风险，默认 5 分钟一次，够用就好。
- **首页时间线**：抓的是 Home → Following 页签（你关注的人）；For You（推荐）
  含推广内容，未启用。
- 视频/GIF 按需求**直接省略**（媒体列表里只保留静态图片）。
- 翻页是"页"粒度（一页 0~2 条，视图片多少而定），不是逐条。
