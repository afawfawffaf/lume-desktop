> **非官方修改版**
> 本仓库是第三方修改版，与 Lume 原作者无关，不是官方发布。
> 依 AGPL-3.0 分发；"Lume" 名称、标志与图标不在授权范围内。改动明细见下文「本仓库的改动」一节。

# Lume 桌面客户端源码

这是 Lume Windows 桌面客户端（壁纸、桌面组件、任务栏、Dock、灵动岛、音乐等）的完整源码，
对应同版本号的官方安装包 `LumeSetup-<版本>.exe`。本次导出对应的版本和源码提交写在
[`SOURCE-REVISION.txt`](SOURCE-REVISION.txt)。

## 本仓库的改动

基于上游 Lume 1.0.8 源码（提交 `0d0678ec`）修改。以下全部改动均为 AGPL-3.0 第 5 条意义上的
修改内容说明。

### 1. 桌面音乐大卡片（新增功能）

双击桌面上音乐组件的封面，弹出一块透明玻璃的歌词面板，位于桌面层而非主窗口内。

**交互**

| 操作 | 行为 |
|---|---|
| 双击组件封面 | 打开卡片；卡片已打开时双击卡片则收起 |
| 按住卡片拖动 | 移动，位置写入 `music-page.json` |
| 拖四边/四角 | 缩放（最小 480×360，最大 1600×1200，默认 900×620），大小写入 `music-page.json` |
| 右键 | 设置菜单 |
| 右键 → 关闭卡片 | 收起卡片 |

卡片没有可见按钮：关闭按钮已按要求移除，所有操作由手势和右键菜单承担。

**右键菜单**

- 歌词样式：13 种（云阶、浮名、商籁、画境、3D、绘光、心象、倾诉、群唱、镜台、时计、流光、凝彩）
- 字体设置：默认 / 楷体 / 宋体 / 雅黑
- 歌词颜色：`lyricsFontColor`、`lyricsKaraokeFillColor`、`sonnetColors.*`（5 个取色器）
- 特效：关闭 / 雨窗 / 雪天 / 星河
- 动画强度：平静 / 标准 / 强烈
- 背景设置：自选图片 / 清除
- 打开音乐设置、关闭卡片

改动即时生效，无需暂停播放。卡片与主窗口共享同一份偏好记录，任一侧修改另一侧同步。

**实现要点**

- 卡片是**第二个视图**，不是第二个播放器：壳层把与组件相同的快照推给卡片，
  卡片的命令进入已有的音乐命令队列（`apps/shell/music_page_session.*`）
- 布局参数复用主窗口的沉浸式歌词舞台（`mountMusicVisualizer`），因此表现手法与主窗口一致
- 偏好经 `scope:"music-preferences"` 通道提交，带 `revision` 做乐观并发控制；
  壳层把回复同时转发给卡片，否则卡片会一直引用旧 revision 导致除首次外的保存全部冲突
- 背景复用主窗口的 `WallpaperCompositor`，媒体以解析后的 URL 传给卡片，文件路径不出壳层

### 2. LX 歌单同步（新增功能）

上游版本没有这个功能。在本地音乐页新增「同步 LX 歌单」按钮，把 LX Music 的歌单拉取到本地目录。

**行为**

- 按钮位于本地音乐页的导入操作区，与「导入本地音乐」「导入文件夹」并列
- 同步期间按钮禁用，并显示进度 `正在同步 LX 歌单 <已完成>/<总数>`
- 完成后提示「LX 歌单同步完成」；失败提示「无法同步 LX 歌单」
- 同步是独立进程，不阻塞播放和界面

**组成**

```
native-rebuild/tools/lxsync.py                             拉取脚本（Python）
native-rebuild/platform/windows/lx_sync_runner.{h,cpp}     进程启动、进度读取
apps/shell/desktop_application.cpp                         StartLxSync / DrainLxSync
ui/music-library.js                                        syncLx() 命令通道
ui/music-view.js                                           「同步 LX 歌单」按钮与提示
ui/music-entry.js                                          进度与完成通知
```

脚本路径默认取程序目录下的 `lxsync.py`，输出目录默认为 `D:\Music\LX music`，
两者都在 `StartLxSync()` 附近，可按需修改。

### 3. 桌面音乐组件

- 移除右上角的关闭按钮（`ports.showCloseButton = false`）：该按钮绘制在封面附近，
  伸手点封面时容易误触，导致组件直接隐藏
- 封面纳入交互面（`interactiveAt`），用于识别双击手势，且不再被误判为缩放边带

### 4. 灵动岛

收窄悬停触发范围：只有指针真正进入屏幕顶部 10 DIP 带才触发展开，
在胶囊附近移动不再误触发。已展开的岛屿保持原有矩形可点击，否则面板按钮点不到。

### 改动的文件

新增：

```
platform/windows/music/music_page_window.{h,cpp}   卡片窗口（WebView2 承载）
apps/shell/music_page_session.{h,cpp}              卡片会话（几何持久化、消息分发）
ui/music-page.html                                 卡片页面
ui/music-page.css                                  卡片样式
ui/music-page.js                                   卡片渲染与交互
ui/music-page-menu.js                              右键菜单
ui/music-page-settings.js                          设置页中的卡片尺寸控件
platform/windows/lx_sync_runner.{h,cpp}            LX 歌单同步进程管理
native-rebuild/tools/lxsync.py                     LX 歌单拉取脚本
```

修改：

```
platform/windows/music_desktop_controls.{h,cpp}    新增 ExpandHandler；移除关闭按钮
apps/shell/music_desktop_session.{h,cpp}           把展开手势转给壳层
apps/shell/desktop_application.{h,cpp}             拥有卡片会话、推送状态、分发消息；
                                                   LX 同步的启动与进度排空
platform/windows/dynamic_island/island_window.cpp  收窄悬停判定
platform/windows/private_messages.txt              登记卡片窗口私有消息 0x6e6
cmake/LayerSessions.cmake                          新增 lume-music-page-session 目标
ui/app.js  ui/bridge.js  ui/music.js
ui/music-view.js  ui/music-entry.js                卡片状态与设置页接线；
                                                   LX 同步按钮、提示与进度
ui/music-library.js                                LX 同步命令通道
```

## 许可

Copyright (C) 2026 Lume 作者

本程序（包括 Lume 作者自己编写的全部代码）按 **GNU Affero General Public License 第 3 版（AGPL-3.0）**
发布，全文见 [`LICENSE`](LICENSE)。第三方组件保留其原有许可，见下文。

你可以按 AGPL-3.0 的条款使用、研究、修改和再分发本程序。本程序不提供任何担保。
AGPL-3.0 第 7 条允许保留商标条款：本授权不包括使用 “Lume” 名称、标志和图标的权利。
分发修改版时，请使用你自己的名称和图标。

程序中包含的第三方组件按各自的许可使用，清单和对应义务见 [`THIRD-PARTY.md`](THIRD-PARTY.md)，
许可原文随代码放在对应目录里（`licenses/`、`*/licenses/`、`*NOTICE*`、`*SOURCES*`）。主要包括：

- Folia Major（AGPL-3.0）：音乐界面的可视化、歌词特效、背景与材质，以及在线音乐协议。
- Mineradio（GPL-3.0）：星河粒子特效（经 Folia 移植）。
- TranslucentTB 2026.1（GPL-3.0）：Windows 11 任务栏核心，源码与 Lume 补丁见
  `native-rebuild/vendor/taskbar-runtime/2026.1/`。
- 以及 Three.js、PixiJS、React、Tiptap、KaTeX、lunar-javascript、OpenCC、nlohmann/json 等宽松许可的库。

不在本仓库里的部分：

- **服务端**（账号、壁纸目录与下载、更新信息）不随客户端分发，不在本源码范围内。客户端连接这些
  服务的地址写在源码里，你可以改成自己的服务。
- **壁纸播放器 mpv**：安装包里的 `wallpaper-runtime/mpv.exe` 是第三方预编译程序（独立进程运行），
  见 `THIRD-PARTY.md` 的 mpv 一节；Lume 自己写的 mpv 插件 `lume-mpv-stream.dll` 的源码在
  `native-rebuild/platform/windows/wallpaper/lume_mpv_stream.cpp`。

## 构建

见 [`BUILDING.md`](BUILDING.md)。
