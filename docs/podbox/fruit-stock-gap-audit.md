# Fruit Company 与 iPod Classic 2.0.5 差距审计

## 基线与证据

- 原版指 iPod Classic 2.0.5。本审计用指定的 QEMU iPod-Classic、`osos-38.2.0.5.body` 和 `test.38.img` 的独立副本。启动参数含 `-icount shift=3,align=off,sleep=on`、`-device loader,addr=0x2203fe64,data=0x00130300,data-len=4` 和 `-snapshot -display none`。各区域的额外测试数据见对应章节。
- AstraPod 指 `t3/device-combined-14` 的 `6c84523b09e7539386560f7d2a71d81004973b80`。编译目标为 `ipod6g`，应用目录为 `apps-ipod`。模拟器使用 SDL dummy 视频和音频驱动。截图来自模拟 LCD 帧环。原版和 AstraPod 的媒体库是独立测试数据。歌曲数、设备容量、日期、电池状态和封面内容不可直接比较。
- 原版 QEMU 的半屏背景偶尔有封面叠到分类图标下，或出现分段绘制。这些帧可以核对文字与左栏位置。它们不能单独证明实机的右栏合成错误。原版时间用 QEMU 虚拟时钟，AstraPod 时间用主机单调时钟。
- 工作范围包括默认入口、条件入口、能打开的页面和未触发的状态。相对截图与 CSV 路径以 `/tmp/fruit-gap-audit/` 为根目录。每处标明证据级别。源码锚点只说明指定提交的分支、配置或绘制逻辑。模拟环境未到达的页面和设备状态列为未验证。工作量按人日估算，含该条目的开发与回归，子项互有重叠。
- 截图、ROM、磁盘、帧、测试媒体和运行日志留在本机。本文只记录路径、观察和源码引用。未连接真实 iPod。

## 按用户影响排序的 15 项

按日常操作频率、功能是否可达、误操作或丢失设置的后果排序。已归属其他任务的条目保留在表内。

| 排名 | 差距 | 类型 | 工作包 |
|---:|---|---|---|
| 1 | Now Playing 的短 SELECT 打开歌词。原版用它进入进度定位、评分和随机选择。 | 行为、外观 | P1 |
| 2 | Songs 缺艺人副行。Artists 用大图行。Albums 缺原版的艺人副行。 | 外观、浏览 | P2 |
| 3 | Search 用通用对话框。原版有底部字母条、实时计数和双行结果。 | 行为、外观 | P3 |
| 4 | 缺独立的 On-The-Go 队列和内置保存、清除流程。 | 功能缺失 | P4 |
| 5 | 长 SELECT 进入 AstraPod 通用上下文。原版优先显示 On-The-Go、浏览专辑和艺人。 | 行为、外观 | P4 |
| 6 | Videos 只有 Movies 接到文件索引。分类、Video Settings 和播放控制有差距。 | 功能缺失、行为、外观 | P9，已有任务 |
| 7 | Photos 缺原版相簿、缩略图网格与幻灯片设置流程。 | 功能缺失、行为、外观 | P9，已有任务 |
| 8 | Compilations 固定为空。存在合辑时此行仍无法打开。 | 功能缺失 | P2 |
| 9 | 播客和有声书用 AstraPod 的分类、进度和统计树。缺 iTunes U。 | 功能缺失、行为 | P2 |
| 10 | Music 和 Playlists 缺 Genius 入口。可用推荐的生成与数据契约仍待验证。 | 功能缺失 | P5 |
| 11 | Volume Limit 缺原版滑条和组合码锁。 | 功能缺失、外观 | P6 |
| 12 | Date 与 Time 共用有秒字段的编辑器，Time Zone 无功能。闹钟与日期设置需联验。 | 功能缺失、行为 | P6、P7 |
| 13 | Games 进入 AstraPod 插件目录。原版三项中只有 Quiz 有类似入口。 | 功能缺失、外观 | P8 |
| 14 | 充电待机、USB 同步和断开状态与原版有差距。实体电源与 USB 时序未验证。 | 行为、外观 | P10 |
| 15 | About 的容量、媒体分区统计和 Games、Contacts、iTunes U 计数不完整。 | 功能缺失、外观 | P11 |

## 主菜单与页面栈

原版主菜单的默认七行见 `/tmp/fruit-gap-audit/stock/nav/03-before-g.png`。AstraPod 的对应截图在 `/tmp/fruit-gap-audit/sim/`。左栏默认行序一致。源码文件头仍写 1.1.2，审计按 2.0.5 实拍核对。源码入口见 `apps-ipod/screens/fruit_menu.c:678–808`，菜单掩码默认值见 `apps-ipod/settings/settings_list.c:2015–2020`。

| 入口或状态 | 原版 2.0.5 | AstraPod | 差距与估算 |
|---|---|---|---|
| 标题栏 | 左上角 iPod。参见原版主菜单截图。 | AstraPod，保留自身品牌。见 `home-music.png`、`apps-ipod/screens/fruit_menu.c:804`。 | 外观差异，品牌决策。若只评估标题排版，0.25 人日。 |
| Music | 默认第 1 行。测试库有图时右侧轮播封面。进入子菜单。见 `stock/frame-at-450.png`。 | 第 1 行，右侧未找到可用封面时显示 Music 图标与标签。见 `home-music.png`、`apps-ipod/screens/fruit_menu.c:902–908`、`apps-ipod/ui/menu_pane.c:219–245`。 | 入口一致。媒体条件相同的轮播启停、切换和回退未验证。子菜单差距见音乐章节。 |
| Videos | 默认第 2 行。空库右侧写 No Videos。Movies 的空预览写 No Movies。见 `stock-home/frame-at-3950.png`、`stock-home/frame-at-3320.png`。 | 同行与分类图标。右侧固定用分类文字，见 `home-videos.png`、`videos-top.png`、`apps-ipod/screens/fruit_menu.c:895–916`。 | 外观及空状态差距，0.5–1 人日。属 `t3/photos-videos`。 |
| Photos | 默认第 3 行。照片类型和空状态决定预览。空库的原版入口显示 No Photos。见 `stock-home/frame-at-5900.png`。 | 第 3 行，分类预览无内容判断。见 `home-photos.png`、`photos-top.png`、`apps-ipod/screens/fruit_menu.c:897–918`。 | 外观及空状态差距，0.5–1 人日。属 `t3/photos-videos`。 |
| Podcasts | 默认第 4 行。原版右栏为播客预览，空库的 SELECT 保持选中行。见音乐章节的媒体条件。 | 第 4 行，固定右栏 Podcasts。SELECT 打开索引页。见 `home-podcasts.png`、`podcasts.png`、`apps-ipod/screens/fruit_menu.c:123`、`apps-ipod/screens/fruit_menu.c:919`。 | 行为差异，1–2 人日，计入播客包。 |
| Extras | 第 5 行。地图、指针钟和日期。见 `stock-home/frame-at-9000.png`。 | 第 5 行及相同形式的预览。见 `home-extras.png`、`apps-ipod/screens/fruit_menu.c:826–843`。 | 入口形态一致。应用列表、应用行为见 Extras 章节。 |
| Settings | 第 6 行。右侧 iPod、品牌标识、容量与剩余空间。见 `stock-home/frame-at-10000.png`。 | 第 6 行，AstraPod 品牌与容量。见 `home-settings.png`、`apps-ipod/ui/menu_pane.c:600–625`。 | 外观差异，品牌决策。设置页见系统章节。 |
| Shuffle Songs | 第 7 行。右侧歌曲数。加载后播放随机一首，进入 Now Playing。见 `stock-home/frame-at-11300.png` 和音乐章节。 | 第 7 行。建立整库队列后随机播放。队列无条目时留在菜单。见 `sim/home-shuffle.png`、`sim/shuffle-playing.png`、`apps-ipod/screens/fruit_menu.c:158–189`。 | 索引未就绪走忙提示。音频、口述内容混排与空库反馈见音乐章节。 |
| Now Playing | 无播放项目时隐藏。有项目时出现在底部。见 `/tmp/fruit-gap-music/stock/40-home.png`。 | 以 `audio_status()` 控制行显示。见 `sim/home-now-playing.png`、`apps-ipod/screens/fruit_menu.c:773–780` 和播放章节。 | 默认显隐原理一致。暂停、恢复、结束和快捷途径分别验收。 |
| 自选的音乐快捷入口 | Main Menu 清单可显示 Cover Flow、Genius Mixes、Playlists、Artists、Albums、Compilations、Songs、Radio、Genres、Composers、Audiobooks、Search。无线电功能取决于附件。见系统章节。 | 同类快捷行按掩码显示。缺 Genius Mixes。Radio 有清单位，运行时表没有该行。见 `apps-ipod/screens/fruit_menu.c:691–707,721–767`。 | 功能缺失，分别计入音乐与配件包。Radio 实机附件未验证。 |
| 自选的媒体快捷入口 | Main Menu 清单含 Photos、Podcasts、iTunes U、Videos、Video Playlists、Movies、TV Shows、Music Videos、Rentals。iTunes U 在测试库的 Home 未显示。 | 行掩码没有 iTunes U。其余入口按对应子行路由。四个视频分类留在菜单。见 `apps-ipod/screens/fruit_menu.c:692–705,731–736,750–760`。 | 功能缺失。iTunes U 计入口述媒体包。视频行属 `t3/photos-videos`。 |
| 自选的 Extras 快捷入口 | Clocks、Alarms、Calendars、Contacts、Games、Notes、Screen Lock、Stopwatch、Sleep。见原版清单证据。 | 对应各应用。另有隐藏的 Playback Report。见 `apps-ipod/screens/fruit_menu.c:699–707,737–767,798–801`。 | 默认 Home 不受新增项影响。配置清单有额外一行，0.25 人日。 |
| Home 长按 MENU、方向键与状态 | 原版轮盘滚动、SELECT 确认、MENU 返回。已覆盖基本轮盘、SELECT、MENU。长按从各页面返回与播放键组合未逐项验证。 | 某些页面接受 Left/Right。通用菜单仍有长按快捷界面入口，Home Play 在已有播放时切换暂停。见 `apps-ipod/widgets/menu.c:1181–1218`。 | 行为和按键矩阵分属 wheel-only nav 与 Control Centre。已有任务，0.5–1 人日做差异验证。 |

## 转场样本

左栏以 (x=0–153, y=25–215) 的行像素为判据。首个可见位移帧至首个稳定帧的差以毫秒记录。原版采样与 LCD 写入的间隔不固定，部分帧间隔约 34 ms。AstraPod 的 CSV 是主机采样时刻。小于一个原版采样间隔的差值只列为观察值。

| 路径 | 原版 QEMU | AstraPod headless sim | 判断和工作量 |
|---|---|---|---|
| Home → Music | `stock/frames/frames.csv` #12–20，730.187 → 968.882，238.7 ms。参见 `stock/frame-at-750.png`。 | `sim/select-home-music.csv` #2–18，153.91 → 420.88，267.0 ms。 | 两侧均从右往左，右栏同类封面可留在半屏。观察值差 28.3 ms。真实耗时差未确认。 |
| Home → Videos | `stock-home/frames/frames.csv` #35–43，2333.241 → 2605.923，272.7 ms。 | `sim/select-home-videos.csv` #2–17，147.3 → 397.8，250.5 ms。 | 观察值差 22.2 ms。两侧均有横向滑动。 |
| Videos → Home | 同一原版 CSV #50–58，3601.169 → 3873.950，272.8 ms。 | `sim/menu-videos-home.csv` #0–18，约 113.5 → 408.1，294.6 ms。模拟返回前选中 Settings，原版选中 Movies。 | 观察值差 21.8 ms。起始高亮场景有差异。 |
| Home → Photos | 同一原版 CSV #65–76，5533.3 → 5804.0，约 270.7 ms。 | `sim/select-home-photos.csv` #2–19，136.6 → 418.4，281.8 ms。 | 观察值差 11.1 ms。两侧均保留右侧分类区。 |
| Music → Cover Flow | `stock/frame-at-1750.png` → `stock/frame-at-2250.png`。SELECT 1,400 ms 释放，1,844 ms 可见封面横切，2,240 ms 已出现文字和标题栏边缘。 | 该入口在基线已合并 split-pane 代码，见 `apps-ipod/screens/covers/carousel.c` 和 `apps-ipod/ui/scene.c:277–359`。本次未对相同封面冷缓存测量。 | 错误时序未确认。已有 split-pane 工作，0.5–1.5 人日补测冷、热、反向和打断。 |

AstraPod 的普通进退场设置为 18 帧，二次缓出。见 `apps-ipod/ui/native_theme.c:291–297`。另有右栏预览 270 ms 独立计时，见 `apps-ipod/ui/menu_pane.c:63,373–380,926`。已合并的 split-pane 几何见 `4f9e0455ba`。两侧还缺 WPS 子页、模式切换、设置弹层、全屏播放器和 USB 的可比时序。

## 音乐浏览、播客与播放

本节的 stock 新截图来自 2.0.5 客体。目录 **M** 为 `/tmp/fruit-gap-music/stock/`。使用指定 kernel、4K 磁盘副本、`-icount shift=3,align=off,sleep=on` 和指定 loader 参数。目录 **Q** 为 `/tmp/claude-1000/stockfw/overlay-test.V1Lnce/`，只引用该目录的 2.0.5 运行。目录 **S** 为 `/tmp/fruit-gap-audit/sim/`。AstraPod 路径行号对应 `6c84523b09`。工时为独立任务人日估计，含回归。图旁未列出 S 文件的行只有源码或 stock 端验证。

| 项目 | 原版 2.0.5 与证据 | Fruit Company 与证据 | 差距类型 / 工时 |
| --- | --- | --- | --- |
| Music 行与默认顺序 | Cover Flow → Genius Mixes → Playlists → Artists → Albums → Compilations → Songs → Genres → Composers → Audiobooks → Search。M `06-next.png`、`57-playlists-preview.png`。Radio 未连接时无入口。 | 余下 10 行顺序相同。Genius Mixes 完全缺席。Radio 只有 checklist 位。无 Genius 对应的主菜单快捷位。S `music-top.png`。 `apps-ipod/screens/fruit_menu.c:226`、`:244`、`:277`、`:790`。 | 缺少功能 / 入口 0.5–1。Genius 内容见下 |
| Cover Flow | 中心封面翻为专辑面板，标题为专辑，下面是艺人和曲目。M `flip-contact.png`、`flip-1350-ms.png`。本轮客体虚拟时间：SELECT 松开 370.003 ms，首个变化帧 495.365 ms，末个面板变化帧 1029.726 ms。MENU 松开 1920.003 ms，首个明显变化帧 2053.482 ms，正面像素完全恢复 2928.000 ms。原始证据 `/tmp/fruit-gap-music/flip-input.csv` 与 `/tmp/fruit-gap-music/flip-frames/frames.csv`。2.0.5 固件静态定时值分别为 500、1000、进入播放 500 ms，见 `/tmp/claude-1000/stockfw/report.md` §6.4。 | 已有 stock 透视、翻面和过渡路径。 `apps-ipod/screens/covers/carousel.c:8222`、`:8590`。S `music-cover-flow.png`、`cover-tracks.png`、`cover-unflip.png`。`select-cover-flip.csv` 记录主机时间 155.88 至 628.08 ms，观察跨度 472.20 ms。原版使用客体虚拟时间，两项跨度不可直接相减。返回帧序列未录到恢复，静态恢复画面已捕获。 | 时间与像素差待测 / 0.5–1 |
| Playlists | 上方 Genius，On-The-Go 带曲数副标题和专用图标。M `58-playlists.png`、`62-otg-selected.png`。 | 文件目录型播放列表。S `music-playlists.png` 有 Search、Fixture Mix、夜曲选集。没有自动出现的 Genius 与 On-The-Go。 `apps-ipod/root_menu.c:1270`、`apps-ipod/playlist/catalog.c:145`。 | 缺少功能、外观不同 / 入口与行 1–2 |
| Artists | 约 25 px 单行白底，首行 All Albums，以下为艺人。Q `albums205/nav/04-end.png`。 | 路由按 canonicalartist。S `music-artists.png` 显示 `Artist` 单数标题、55 px 带图行和 feat. 项，首屏无 All Albums。 `apps-ipod/tagnavi.config:154`、`apps-ipod/screens/browse/browser.c:1008`。 | 外观、聚合行为 / 1–2 |
| Artists → 艺人/All Albums | 分支按艺人及其专辑组织。All Albums 在顶端。只有一张专辑的 Artist 01 直接打开曲目页，标题为 Artist 01。M `92-artists.png`、`94-artist01.png`。 | 已有艺人→专辑→按 disc/track 的曲目路由。S `music-artist-aurora.png` 出现 [Random]、[Shuffle Albums] 与专辑行。 `apps-ipod/tagnavi.config:25`、`:154`。 | 行与导航 / 1–2 |
| Albums | 55 px 双行。封面或浅色占位图在左，专辑名及艺人在右。2.0.5 排序夹具首项为 All Songs，并显示曲数。证据 `/tmp/claude-1000/stockfw/sort-test.m8M54t/cn205-final/albums-00.png`。Q `albumszh3/nav/04-end.png` 及 M `70-albums-quickscroll-first.png`。 | 使用通用数据库列表，封面与行高取决于 db_albumart、db_art_row_height。S `music-albums.png` 显示 55 px 单行专辑名，无艺人副行。 `apps-ipod/screens/browse/browser.c:990`、`:1008`、`:1034`。 | 专辑行外观 / 1–2 |
| Compilations | 行存在。无合辑时，右侧显示 No Compilations。M `09-next.png`。独立副本写入 compilation 标志后显示 All Songs / 1 Song、合辑标题、艺人副行及占位封面。M `pop-compilation-list.png`。 | Music 及主菜单复制行都调用 `empty()`。非空库也没有合辑查询。S `music-compilations.png` 显示通用封面预览，SELECT 留在原处。 `apps-ipod/screens/fruit_menu.c:74`、`:248`、`:725`。 | 缺少功能 / 2–4 |
| Songs | 44 px 双行，曲名加艺人。Q `songszh2/nav/04-end.png`。 | 进入 Tracks by → Title，默认配置只取 title。 `apps-ipod/screens/fruit_menu.c:117`、`apps-ipod/tagnavi.config:121`。S `music-songs.png` 显示标题 `Title` 和 25 px 单行。播放标记对比待验证。 | 外观 / 1–2 |
| Genres → 艺人 → 专辑 → 曲目 | Genres 是双行带计数，首项 All Artists。打开后可选 All Albums，再看到 All Songs 及专辑。专辑进入单行曲目列表。M `85-genres.png`、`86-genre-all-artists.png`、`87-genre-all-albums.png`、`88-genre-album.png`。 | Genre → albumartist → album → title。S `music-genres.png` 显示单行 `[All Tracks]`、`[Random]` 和类别，无计数。 `apps-ipod/tagnavi.config:155`。 | 外观、导航、计数 / 1–2 |
| Composers | 行与空库的 No Composers 已验证。M `12-next.png`。非空副本的作曲家首页为 All Albums 和 Fixture Composer。打开 All Albums 可查看全库专辑与 All Songs。此夹具的 Fixture Composer 直接进入有艺人副行的歌曲列表。M `pop-composers-list.png`、`pop-composers-sublist.png`、`pop-composer-open.png`。 | 已有 composer → album → title。S `music-composers.png` 显示单数标题和 `[All Tracks]`、`[Random]`。 `apps-ipod/screens/fruit_menu.c:115`、`apps-ipod/tagnavi.config:157`。 | 空态与分组待确认 / 0.5–1.5 |
| Audiobooks | 无内容的预览显示 No Audiobooks。M `13-composers.png`、`24-music-back.png`。2.0.5 非空行、书签、速度选项从设置区另证。 | 独立的 Search、Finished、In Progress、Not Started、By Title、By Author、By Series、Listening Stats。图书识别取 genre。S `music-audiobooks.png`。 `apps-ipod/screens/browse/browser_db.c:1857`、`:1880`。 | 浏览树和分类行为 / 2–4 |
| 排序与索引 | 2.0.5 专辑快速滑动显示 74×70 深色字母板，隐藏选中条。M `70-albums-quickscroll-first.png`、`71-songs-quickscroll-first.png`。Q `albumszh3/probe.csv` 在 5132.067 ms 设置字母，`frames/frames.csv` 在 5186.733 ms 首次显示，相差 54.666 ms 客体虚拟时间。 | 有高速分组与松手清除。兼容排序重建数字/字母/其他组，组内按 `strncasecmp`，相同再按 seek。Genre 路径给无索引键。 `apps-ipod/screens/browse/browser_db.c:2730`、`:2766`、`:7060`。`apps-ipod/widgets/list_quickscroll.c:267`、`:284`。原版保留同步库的显示名与排序名差异，例如 `Axiom` 按 `7 Sort` 排在 `4 Skies` 与 `9 Lives` 之间，`2 Sort Test` 按 `Carrot` 排在 `Café` 后。证据 `/tmp/claude-1000/stockfw/sort-test.m8M54t/cn205-final/songs-12.png`、`/tmp/claude-1000/stockfw/sort-test.m8M54t/cn205-final/songs-20.png` 及该夹具 `/tmp/claude-1000/stockfw/sort-test.m8M54t/index/order_title.csv`。AstraPod 从显示名推导索引键，无独立排序标签。运行动作时间待按同夹具对比。 | 排序行为，时间待验证 / 1–3 |
| Search | 全屏 55 px 双行结果。顶部查询回显和带数字的标题。底部黑色水平字母条，SELECT 添加字符。空查询显示 No Search Results。`a` 显示 200 条结果。`aa` 无行。M `19-search-ab.png`、`21-search-typed.png`、`22-search-typed2.png`。 | S `music-search.png` 显示居中主题对话框，无 stock 字母条。S `music-search-results-down.png` 和 `music-search-selected.png` 验证 `A` 的单行图标结果。轮盘编辑字符，PLAY 或 SELECT 进入结果。按 title、album、albumartist 查找，受最少字符数、结果上限、字段顺序控制。 `apps-ipod/widgets/search_dialog.c:116`、`:237`、`:280`。`apps-ipod/screens/system/db_search.c:61`、`:229`。 | 外观、导航、结果呈现 / 3–5 |
| Podcasts | 空库首页显示 No Podcasts，SELECT 留在首页。M `54-podcasts.png`。另一个副本注入 2 个播客后，预览显示 2 Podcasts。SELECT 仍停在首页，节目页未通过合成同步夹具打开。M `pop-podcasts-enter.png`。非空节目、未听点、续播与视频播客未核实。 | 可进入独立列表。S `podcasts.png` 有 Search、Not Started、All Episodes、By Show、Listening Stats。另两种进度行按夹具出现。按 genre/album 分类。生成新到和最近播放顺序。 `apps-ipod/screens/browse/browser_db.c:1867`、`:1900`。`apps-ipod/root_menu.c:429`。 | 空态与浏览行为 / 2–4 |
| Shuffle Songs | 启动全夹具队列，截图显示 `1 of 1016`。播放中的首页预览显示 shuffle 图及 `1016 Songs`。M `39-podcast-select.png`、`40-home.png`。该 stock 仿真在 SELECT 后延迟才出播放页，未测解除延迟的原因。 | 标签数据库可用时遍历 tag_filename，组单后打乱并启动。入口未按 media type 或 spoken 筛选。无数据时直接返回。数据库不可用显示 Tagcache Busy。 `apps-ipod/screens/fruit_menu.c:158`。S `home-shuffle.png`、`shuffle-playing.png`。原版对播客/有声书/跳过随机标志的筛选未验证。 | 空态、入队筛选待确认 / 1–2 |
| Now Playing 主页面 | 左侧倾斜封面加反射，右侧标题、艺人、专辑、队列位置，底部播放时长，右上 shuffle。暂停换状态栏图标。M `39-podcast-select.png`、`72-np-paused.png`。 | stock 风格已走 native_wps，包含对应进度条和封面位置。S `now-playing.png` 的左上封面、三行元数据、队列位置和底部进度几何与原版接近。 `apps-ipod/ui/native_wps.c:52`、`:706`、`:1472`。`apps-ipod/screens/playback/wps.c:827`。 | 两端静态截图差及配置影响待确认 / 1–2 |
| Now Playing 跳转与音量 | SELECT 打开带蓝色游标的 scrubber。轮盘修改目标时间。常规轮盘显示音量。M `np-cycle-01-ret.png`、`74-np-scrub-wheel.png`、`76-np-rating-wheel.png`。 | S `now-playing-select1.png` 确认短 SELECT 打开歌词幕。长按 PREV/NEXT 快退快进。常规轮盘改音量。 `apps-ipod/input/keymap_ipod.c:193`、`apps-ipod/screens/playback/wps.c:860`、`:954`、`:971`。 | 缺少操作、行为不同 / 2–4 |
| Now Playing 评分 | 同一屏下方显示五个点，通过 SELECT 循环抵达。M `np-cycle-02-ret.png`。持久化验证待补。 | 长 SELECT 的菜单里 Rating 打开评分对话框。 `apps-ipod/screens/context_menu.c:1328`、`:2499`，渲染 `apps-ipod/widgets/rating_dialog.c`。 | 行为、外观 / 1–2 |
| Now Playing 随机选择器 | SELECT 循环显示 Off / Songs / Albums 三段控制，选中值蓝字。M `np-cycle-03-ret.png`。 | Shuffle、Repeat 在长 SELECT 菜单的设置行。 `apps-ipod/screens/context_menu.c:279`、`:2499`。 | 缺少操作、外观 / 1–2 |
| Now Playing 歌词/详情 | 夹具无嵌入歌词。此曲的快速 SELECT 循环未显示歌词或通用元数据页。M `73-np-select1.png` 到 `80-np-select6.png`。合成曲 `0 Fixture Lyrics` 写入 ID3 USLT 与库歌词标志。其 SELECT 探针仍仅显示跳转、评分和随机。M `pop-lyrics-cycle-02-ret.png` 到 `pop-lyrics-cycle-04-ret.png`。原版识别到歌词后的分支、播客说明、章节及详情未验证。 | SELECT 无条件打开 lyric_viewer，可显示 No lyrics。上下文提供 Track Info 与其它扩展项。 `apps-ipod/screens/playback/wps.c:860`、`apps-ipod/viewers/lyric_viewer/lyric_viewer.c:1408`、`apps-ipod/screens/context_menu.c:2278`。 | 行为、外观 / 2–3 |
| Now Playing 浏览专辑及艺人 | 长 SELECT 有 Browse Album、Browse Artist。M `38-podcast-highlight.png`。与 Cover Flow 的封面翻面是两个入口。 | Go to Album/Artist 已存在于上下文。封面墙另有翻面，普通 SELECT 到歌词。 `apps-ipod/screens/context_menu.c:2499`、`apps-ipod/screens/playback/wps.c:860`、`:901`。 | 上下文长相已不同。从播放页的原版目标与过渡以补拍为准 / 1–2 |
| Genius / Genius Mixes | Music 显示 Genius Mixes，Playlists 显示 Genius。Playlists 的 Genius 有引导页。引导页写明播放页或长按歌曲启动。M `06-next.png`、`58-playlists.png`、`59-otg-empty.png`（实际是 Genius 引导）。当前库没有可用 Genius 推荐数据，但浏览器中长按歌曲仍有 Start Genius。选择后出现「Connect to iTunes to activate Genius.」。M `89-song-hold.png`、`97-genius-result.png`。 | 主题菜单、音乐浏览及播放上下文都无 Genius 入口。 `apps-ipod/screens/fruit_menu.c:244`、`:277`。`apps-ipod/screens/context_menu.c:2499`、`:2516`。 | 缺少功能 / 入口与离线空态 1–2。可用推荐兼容单独评估 |
| 长 SELECT 上下文 | 当前歌曲的菜单是白底全宽短列表。夹具显示 Add to On-The-Go、Browse Album、Browse Artist、Cancel。M `38-podcast-highlight.png`。浏览器里的歌曲以曲名为标题，另有 Start Genius，参见 M `89-song-hold.png`。其它媒体及推荐可用时的条目待验证。 | 当前唱片上下文含 Shuffle、Repeat、Rating、Track Info、Go To、Lyrics、Sleep、Current Playlist、More。数据库局部菜单还含 Play/Add/Play Next/Play Last、播放列表、属性与管理。S `now-playing-hold-select.png`、`song-hold-select.png`。 `apps-ipod/screens/context_menu.c:2499`、`:2516`。 | 行为、外观 / 2–4 |
| On-The-Go 编辑 | 长 SELECT 选择 Add to On-The-Go 后，Playlists 的 On-The-Go 从零条变成 1 Song。列表有 Save Playlist / New Playlist 1、Clear Playlist / Remove 1 Song、曲目＋艺人。M `38-podcast-highlight.png`、`62-otg-selected.png`、`63-otg-list.png`。 | 现有播放队列可移动、删除、保存。文件播放列表具有加入现有或新列表。与当前播放队列独立、内置且无需命名的 On-The-Go 契约缺席。 `apps-ipod/screens/context_menu.c:1119`、`:2511`。`apps-ipod/playlist/viewer.c:710`、`:1041`、`:1070`。 | 缺少功能 / 3–6 |
| 空状态与出错 | 2.0.5 以预览区空态表达 No Podcasts、No Compilations、No Composers、No Audiobooks。搜索空态在页面首部。M `54-podcasts.png`、`09-next.png`、`12-next.png`、`13-composers.png`、`19-search-ab.png`。 | Compilations 固定为空。数据库、播放列表、搜索与音频错误复用各自的提示/对话框和空列表。数据库失效时 Shuffle 会显示忙提示。 `apps-ipod/screens/fruit_menu.c:74`、`:164`。`apps-ipod/playlist/catalog.c:89`。`apps-ipod/screens/system/db_search.c:242`。 | 空态视觉、行为 / 1–2 |

## Extras、游戏和工具

本节原版截图目录为 `/tmp/fruit-gap-extras.Gy3P5d/`。Fruit Company 截图目录为 `/tmp/fruit-gap-audit/sim/`。下表使用这两个目录中的文件名。原版使用指定的 2.0.5 内核、QEMU 参数和磁盘副本。原版画面的日期和电池状态与 sim 不同。

| ID / 原版页面 | 原版 2.0.5 | Fruit Company 6c84523b09 | 差异与工时 |
|---|---|---|---|
| E01 · Extras | 顺序是 Clocks、Calendars、Contacts、Alarms、Games、Notes、Screen Lock、Stopwatch。顶部和底部均可见于 `extras.png`。 | 前 8 行顺序相同。随后是 Other Apps、Demos、Files、System。进入后第一屏已露出第 9 行。见 `extras-top.png` 及 `apps-ipod/screens/fruit_menu.c:335–358`。 | 外观、行为。是否把扩展入口放进单独层级属于产品决定。0.5–1 天。 |
| E02 · Clocks | 初始单行 California，黑色表盘和世界地图。SELECT 弹出 Add / Edit。见 `clocks.png`、`clock_first.png`。 | 原生世界时钟页面和 Add / Edit 路径。见 `extras-clocks.png`、`apps-ipod/screens/extras/extras_clock.c`。默认时区由 fixture 的时钟设置影响。 | 首屏结构已对照。新增、编辑、删除及各城市未验证，归时钟包。 |
| E03 · Calendars / All Calendars | All Calendars、To Do's、Alarms。首行整月网格。SELECT 进入日程页，空日显示「No events for this day」。见 `calendar.png`、`calendar_all.png`、`calendar_day.png`。 | 有同名菜单、月页和日页。数据源为 Rockbox 插件目录下的 `.memo`，按 Rockbox 记录匹配日期。见 `extras-calendars.png`、`apps-ipod/screens/extras/extras_calendars.c:45`、同文件 `:235–254`、`:510–607`。 | 缺功能。没有原版日历同步数据的输入路径。3–6 天，与 E04 共用导入。 |
| E04 · Calendars / To Do's | 无待办时显示 iTunes 同步说明。见 `calendar_todo.png`。 | SELECT 始终打开说明页。同文件 `:638–659`、`:733–738`。sim 入口见 `extras-calendars.png`。 | 缺功能。没有待办列表和待办记录状态。2–4 天，依赖 E03 数据入口。 |
| E05 · Calendars / Alarms | SELECT 在 Beep、None、Off 间循环。见 `calendar_alarm_none.png`、`calendar_alarm_off.png`、`calendar_alarm_beep.png`。事件触发和重启后的值未测。 | 值可循环，只保存在静态变量。源码明确不发出事件闹钟，同文件 `:667–688`、`:733–738`。 | 缺功能。事件提醒未接入。2–4 天，依赖 E03。持久化规则须补测原版。 |
| E06 · Contacts | 空目录时，右侧显示通讯录预览和「No Contacts」。SELECT 留在 Extras。合成 vCard 的列表显示电话。详情显示职位和 URL，滚动可见备注。见 `contacts_preview.png`、`contacts_empty.png`、`last_contacts.png`、`last_contact_detail.png`、`last_contact_bottom.png`。 | 空目录行为相同。非空时扫描 `/Contacts/*.vcf`，有姓／名排序及详情。详情代码枚举电话、邮件、地址、备注。见 `extras-contacts.png`、`apps-ipod/screens/extras/extras_contacts.c:428–465`、`:656–725`、`:942–969`。 | 缺功能。相同 vCard 的职位和 URL 在现有详情解析分支中没有对应字段。0.5–2 天。其他 vCard 字段兼容性待核对。 |
| E07 · Alarms | Alarms 下面有 Sleep Timer、Create Alarm。Sleep Timer 的 15、30、60、90、120 分钟选项见 `games_entry.png`（截图文件名为导航探针名）。 | 原生 Alarms。见 `extras-alarms.png`、`apps-ipod/screens/extras/extras_alarms.c`。 | 由时钟/电源包登记功能、触发和计时差异，本节不重复估算。 |
| E08 · Games 列表 | 三行 iPod Quiz、Klondike、Vortex。每行约 54 px，含图标、游戏名称、上次游玩状态「Never Played」。Extras 预览显示「3 Games」。见 `games_preview.png`、`games.png`。 | 统计安装的 `.rock` 与内置 Music Quiz。按本地化名称排序。用专用图标、标题和表项箭头。sim 没有游玩状态文字。见 `extras-games.png`、`apps-ipod/screens/browse/app_list.c:238–267`、`:451–503`。 | 外观、缺功能。缺少游玩时间和对应次行。分开处理图标与游玩状态。1–3 天。 |
| E09 · iPod Quiz | 有此入口。基础盘上 SELECT 后仍显示 Games，未取得游戏内页。见 `game_quiz.png`。 | 原生 Music Quiz 有出题、答案、结算、历史最佳分。见 `apps-ipod/screens/browse/app_list.c:228`、`:276–288`、`apps-ipod/games/quiz/quiz.c:838`、`:909–988`、`:1053–1107`。 | 名称不同。玩法差异未确认。0.5–1 天补测入口、无曲库状态和游戏会话。 |
| E10 · Klondike | SELECT 进入深色 Error 页，提示游戏无法启动，连接 iTunes 重装。见 `game_klondike.png`。该盘未验证实际游戏。 | 目录有独立 Solitaire 模块，名称和入口不同，无 Klondike 行。见 `apps-ipod/screens/browse/app_list.c:144`、`:238–245`、`:276–288`。 | 缺原版入口与专用恢复提示。游戏内差异未确认。0.5–1 天登记包缺失与启动错误行为。 |
| E11 · Vortex | SELECT 得到同类 Error 页。见 `game_vortex.png`。该盘未验证实际游戏。 | 无 Vortex 行。BrickMania 是不同的游戏入口。见 `apps-ipod/screens/browse/app_list.c:67–154`。 | 缺功能。只有目录和错误状态有运行证据。0.5–1 天验证入口与错误流。若新增游戏，单独估算原创方案。 |
| E12 · Notes / Instructions | 空目录时首屏只有 Instructions。SELECT 显示磁盘使用和 Notes 路径说明。填库后同时列出 `.txt` 和无扩展名的纯文本 `NoExtension`。原版会将 `<BR>` 和锚标记排成换行与蓝色链接。见 `notes.png`、`notes_instructions.png`、`last_notes.png`、`notes_audit_body.png`。 | 空目录同样显示 Instructions。只扫描 `/Notes` 的顶层、非隐藏 `.txt`。短文件作为纯文本显示，超过 4 KB 调用文本阅读器。见 `extras-notes.png`、`apps-ipod/screens/extras/extras_notes.c:79–109`、`:261–295`。 | 行为、外观。无扩展名的文本被过滤。短文本的标记排版缺失。1–3 天。链接跳转、子目录和长文本上限待核对。 |
| E13 · Screen Lock | 首次显示四位 New Combination，输入后显示 Confirm Combination。确认后提供 Lock / Reset。锁定状态显示整页锁，MENU 留在锁定页。见 `lock_choose.png`、`lock_confirm.png`、`lock_saved.png`、`lock_active.png`、`lock_unlock.png`。 | 原生四位选择、确认、错误反馈、Lock / Reset 和输入解锁。见 `extras-lock.png`、`apps-ipod/screens/extras/extras_screen_lock.c:411–493`、`:500–544`。 | 首次输入与锁定结构已对照。解除后的路由、错误反馈时长、断电和同步后的锁状态未确认。0.5–1 天验证，按差异再估算。 |
| E14 · Stopwatch | 首次进入表盘。SELECT 开始，再按 SELECT 记圈，PLAY 暂停并返回列表。列表有 Resume、New Timer、Clear Logs、Current Log 和日期日志。一次圈按键后，日志显示一条 Lap 1，Shortest、Longest、Average 均为 0。见 `sw_start.png`、`sw_running.png`、`sw_lap.png`、`sw_stop.png`、`sw_log.png`。 | 原生表盘、开始/暂停、圈时，菜单有 Resume、New Timer、Clear Logs、Current Log 和历史日志。日志内有总计、最短、最长、平均和逐圈。10 个日志及每次 64 圈上限。见 `extras-stopwatch.png`、`apps-ipod/screens/extras/extras_stopwatch.c:40–42`、`:87–100`、`:421–490`、`:534–560`、`:692–735`。 | 行为。AstraPod 日志把计圈后剩余的尾段算作第二圈，并用两段计算统计。同文件 `:139–149`、`:452–490`。0.5–1.5 天。历史上限、重启和多圈统计仍待核对。 |
| E15 · Other Apps / Demos / Files / System | 8 行 Extras 中无这四行。见 `extras.png`。 | Other Apps、Demos 来自安装表。Files 路由到文件浏览器。System 路由到 AstraPod 系统菜单。见 `extras-other-apps.png`、`extras-demos.png`、`extras-files.png`、`extras-system.png`、`apps-ipod/screens/fruit_menu.c:138–141`、`:343–355`。 | 行为、外观。扩展入口的放置和 E01 一并决定及估算。 |

### Extras 应用清单

原版列表只提供 E01 的 8 行和 E08 的 3 个游戏入口。下列 AstraPod 入口共用 E08、E15 的布局及额外入口差异。列表根据固定提交的注册表，安装模块才显示。每个模块的内部画面和运行状态未逐个验证。英文名称以固定提交语言资源为准。

- **Games（89 个注册入口）** — `2048`、`Amaze`、`Another World`、`Black Box`、`Blackjack`、`Blocks`、`Blocks 1D`、`Brickmania`、`Bridges`、`Bubbles`、`Chess`、`Chopper`、`Clix`、`Codebuster`、`Cube`、`Dice`、`Dominosa`、`Doom`、`Duke Nukem 3D`、`Fifteen`、`Filling`、`Flip`、`Flip It`、`Flood`、`Galaxies`、`Game Boy`、`Go`、`Group`、`Guess`、`Inertia`、`Invaders`、`Jackpot`、`Jewels`、`Keen`、`Light Up`、`Loopy`、`Magnets`、`Map`、`Maze`、`MazezaM`、`Mines`、`Minesweeper`、`MochiPod`、`Mosaic`、`Music Quiz`、`Net`、`Netslide`、`Pac-Box`、`Palisade`、`Pattern`、`Pearl`、`Pegbox`、`Pegs`、`Pong`、`Quake`、`Random Sokoban`、`Range`、`Rectangles`、`Reversi`、`Rick Dangerous`、`robotfindskitten`、`Same Game`、`Signpost`、`Singles`、`Sixteen`、`Slant`、`Slide`、`Sliding Puzzle`、`Snake`、`Snake 2`、`Sokoban`、`Solitaire`、`Solo`、`Space Rocks`、`Star`、`Sudoku`、`Super Domination`、`Tents`、`Towers`、`Tracks`、`Twiddle`、`Undead`、`Unequal`、`Unruly`、`Untangle`、`Wolfenstein 3D`、`Wormlet`、`Xobox`、`ZX Spectrum`。
- **Other Apps（48 个注册入口）** — `Add Shortcut`、`Announce Status`、`Calculator`、`Chess Clock`、`Cue Playlist`、`Dart Scorer`、`Database Folders`、`Database Menu Editor`、`Dictionary`、`Disk Tidy`、`EQ Optimizer`、`File Stats`、`Image Viewer`、`Image Viewer (Classic)`、`Key Remap`、`Keybox`、`Lamp`、`Last.fm Scrobbler`、`Lyrics Player`、`Main Menu Setup`、`MD5 Sum`、`Metronome`、`MIDI Player`、`Module Player`、`MPEG Player`、`One-Time Passwords`、`Open With`、`Paint`、`Periodic Table`、`Pitch`、`Playing Time`、`Properties`、`Pure Data`、`Random Folder`、`Remote Control`、`Resistor Codes`、`Scrobble Log`、`Settings Dump`、`Shortcuts`、`Sort Lines`、`Speed Reader`、`System Info`、`Text Editor`、`Text Viewer`、`Theme Remover`、`Timer`、`Tuner`、`Video Player`。
- **Demos（22 个注册入口）** — `App Tray Demo`、`Battery Bench`、`Bounce`、`Credits`、`Cube`、`Demystify`、`Fire`、`Fireworks`、`Fractals`、`Keyboard Test`、`Life`、`Logo`、`Matrix`、`Mosaique`、`Oscilloscope`、`PictureFlow`、`Plasma`、`PodLink Echo`、`Snow`、`Spectrum`、`Starfield`、`VU Meter`。

## Videos 和 Photos

本节各项均由 **`t3/photos-videos` 覆盖**。工时为基线审计的粗估，互相依赖的项不得直接相加。合成视频和照片用本地库工具写入另一份磁盘镜像。未提交原版资源。

| ID / 原版页面 | 原版 2.0.5 | Fruit Company 6c84523b09 | 差异与工时 |
|---|---|---|---|
| V01 · Videos | 顺序为 Movies、Rentals、TV Shows、Music Videos、Video Playlists、Settings。空 Movies 显示「No Movies」，空播放列表显示「No Playlists」。见 `videos.png`、`videos_rentals.png`、`videos_tv.png`、`videos_music.png`。 | 六行顺序相同。各行经同一影片图预览分支。空 Movies 仍显示「Movies」。见 `videos-top.png`、`apps-ipod/screens/fruit_menu.c:290–302`、`:895–916`。 | 外观、行为。类别和空态专用图与文案。1–2 天。 |
| V02 · Movies | 空库 SELECT 留在本菜单。基础盘见 `videos.png`，本地媒体首轮见 `media_video_menu.png`、`media_movies.png`。非空列表见 `m1movies.png`。 | 用影片文件索引。按文件名自然排序，简表以文件夹外的文件集合呈现，另有 Search／By Type。空索引弹提示。见 `videos-movies.png`、`apps-ipod/root_menu.c:1074–1077`、`apps-ipod/screens/browse/browser_flat.c:86–98`、`:378–470`。 | 行为、外观。文件索引和标题／封面／播放状态列表的差异。3–5 天，与 V03–V06 共用索引。 |
| V03 · Rentals | 具独立菜单行。空库见 `videos_rentals.png`。租期、授权和到期错误未验证。 | SELECT 直接返回 0，没有租赁目录或租期。见 `apps-ipod/screens/fruit_menu.c:74–77`、`:291`。 | 缺功能。2–4 天做无 DRM 信息列表及明确提示。原版授权机制未估算。 |
| V04 · TV Shows | 有独立菜单行。空库见 `videos_tv.png`。非空季、集分组见补测范围。 | SELECT 直接返回 0。见同文件 `:292`。 | 缺功能。2–4 天，与 V02 共用视频元数据。 |
| V05 · Music Videos | 有独立菜单行。空库见 `videos_music.png`。 | SELECT 直接返回 0。见同文件 `:293`。 | 缺功能。1–2 天，依赖 V02。 |
| V06 · Video Playlists | 有独立菜单行。空集显示文档音符和「No Playlists」，见 `v_playlists_empty.png` 的过渡帧。 | SELECT 直接返回 0。同文件 `:294`。 | 缺功能、外观。1–3 天，依赖播放列表媒体类型。 |
| V07 · Settings | 7 个值直接用 SELECT 循环。TV Out：Off、Ask、On。TV Signal：PAL、NTSC。TV Screen：Standard、Widescreen。Fit to Screen：On、Off。Alternate Audio、Captions、Subtitles：Off、On。见 `videos_settings.png`、`v_tvout_ask.png`、`v_tvout_on.png`、`v_ntsc.png`、`v_wide.png`、`v_fit_off.png`、`v_alt_on.png`、`v_captions_on.png`、`v_subtitles_on.png`。 | Settings 只弹出 2 秒「Not available on AstraPod」。见 `videos-settings.png`、`apps-ipod/screens/fruit_menu.c:66–69`、`:295`。 | 缺功能。1–2 天做菜单和值。每个输出、轨道和字幕能力另估。 |
| V08 · 视频播放、恢复和错误 | 合成片对照范围见下方补测。字幕／多音轨需要另配 fixture。 | 视频类型通过 .m4v 或带视频轨 .mp4 路由至硬件 H.264 插件，先停音乐。移植脚本去掉原上游的续播记录和播放菜单，MENU 直接退出。sim 无硬件解码时显示专用错误。见 `videos-play-attempt.png`、`apps-ipod/files/filetypes.c:321–363`、`apps-ipod/plugins/stage-h264.py:23–39`、`:49–79`、`:121`。 | 缺功能、外观、行为。3–7 天做控制层、续播与错误。轨道/字幕能力另估。sim 无法验证硬件解码。 |
| P01 · Photos | 基础盘有 All Photos、Settings，右栏显示「No Photos」，SELECT 空 All Photos 无动作。导入后增加命名相册，见 `photos.png`、`photos_empty.png`、`media_photos.png`。 | 两行固定，无导入相册行。空预览仍写「All Photos」，打开走通用索引。见 `photos-top.png`、`photos-all.png`、`apps-ipod/screens/fruit_menu.c:306–313`、`:897–918`、`apps-ipod/root_menu.c:1068–1072`、`apps-ipod/screens/browse/browser_flat.c:451–463`。 | 缺功能、外观、行为。相册来源、列表、空态。3–5 天。 |
| P02 · Photos / Settings | Time Per Slide、Music、Repeat、Shuffle Photos、Transitions、TV Out、TV Signal。Repeat、Shuffle Photos 在 Off／On 间切换。TV Out 有 Off／Ask／On，TV Signal 有 PAL／NTSC。见 `photos_settings.png`、`p_repeat_on.png`、`p_shuffle_on.png`、`p_tvout_ask.png`、`p_tvout_on.png`、`p_ntsc.png`。 | Settings 只弹「Not available on AstraPod」。见 `photos-settings.png` 及 `apps-ipod/screens/fruit_menu.c:307`。 | 缺功能。1–2 天做菜单与本地状态，应用值计入 P03–P06。 |
| P03 · Time Per Slide | 选择页标题 Next Slide。Manual、2、3、5、10、20 Seconds，默认选中 3 Seconds。见 `p_time.png`。 | 查看器的自有菜单有 Slideshow Mode 与整数秒数。入口在 Image Viewer。见 `photos-view-context.png`、`apps-ipod/viewers/image_viewer/image_viewer.c:1250–1291`。 | 行为、外观。0.5–1 天。 |
| P04 · Slideshow Music | 选择页标题 Slideshow Music。Now Playing、Off、On-The-Go，选择显示当前值。见 `p_music.png`。 | 查看器菜单无音乐选择。见同文件 `:1264–1268`。 | 缺功能。1–3 天，包含播放会话保留与恢复。 |
| P05 · Transitions | None、Random、Cross Fade、Fade To Black、Zoom Out、Wipe Across、Wipe Center，默认选中 Random。见 `p_transitions.png`。 | 查看器菜单无幻灯片转场选择。计时到点切到下一文件。见同文件 `:624–648`、`:760–778`、`:1264–1268`。 | 缺功能、外观。3–5 天。转场毫秒值待有效照片帧测量。 |
| P06 · 网格、全屏和幻灯片 | 合成照片库被识别为命名相册。播放时可观测相册标题、播放图标、亮度滑条及「2 of 2 Photos」覆盖层。QEMU 的照片区域仍有旧菜单和混合图层，网格画面／缩放比例、完整转场未确认。见 `media_photo_album_grid.png`、`media_photo_before_contacts.png`、`media_photo_settled.png`。 | 文件简表直接进入原生图像查看器。看图支持缩放、平移、按同目录换图和查看器的幻灯片选项。见 `photos-view.png`、`photos-view-context.png`、`apps-ipod/screens/browse/browser_flat.c:167–203`、`apps-ipod/viewers/image_viewer/image_viewer.c:6–16`、`:624–805`。 | 缺功能、行为、外观。4–7 天做网格、相册浏览模型及叠层。照片解码及电视输出单独验收。 |

### 合成媒体补测及证据边界

- 初次重写 iTunesDB 使库头的 hash scheme 变为 0，原版显示「No Music」。将私有盘的库头恢复至基线的值 1 后，原版识别曲库和 4 个视频。实际条目不是从推测写出。见 `media_home.png`、`m1home.png`、`m1videos.png`。
- Movies 识别了类型为 Movie 的合成片。列表为约 54 px 的双行，显示片名、时长、影片占位图及 TV 图标。见 `m1movies.png`。SELECT 进入暗色播放叠层，顶部有标题和播放／暂停图标，底部有进度、经过和剩余时间。见 `m1movie_select.png`、`m1movie_playpause.png`。
- 播放叠层仍显示底下的 Movies 列表，6 秒观察窗内仍为 0:00。见 `m1movie_after6.png`。实际视频帧、续播、快进快退、TV 输出、音轨和字幕切换未确认。可以用本地合成媒体和支持相应硬件接口的模拟运行重新确认。
- 相册测试保留在 P01、P06。测试盘识别照片条目数量。实际图像帧与切换计时缺少观测证据。
- AstraPod sim 对照覆盖菜单和设置弹窗。视频解码只依据固定提交的调度和移植脚本。从 sim 上看到入口并不能验证硬件解码。

## Settings、时钟与系统状态

本节使用两组本地路径。下表的 `S/文件名.png` 指向 `/tmp/fruit-gap-audit-stock-system.3qha8E/文件名.png`。`A/文件名.png` 指向 `/tmp/fruit-gap-audit/sim/文件名.png`。Fruit Company 源码行号以 `6c84523b09` 为准。缺少截图时，结论按源码推导标注。

原版会话直接加载 `osos-38.2.0.5.body`。每次启动用 `test.38.img` 的任务副本和 `-snapshot`。指定的 `-icount` 和 loader 参数均保留。About 第 3 页显示 `2.0.5 PC`。原版磁盘中有 1,016 首测试歌曲。模拟器中的曲数、容量、时钟值和身份字段受各自测试数据影响。

### Settings

| 页面或动作 | 原版 2.0.5 与证据 | Fruit Company 与证据 | 差异和估时 |
| --- | --- | --- | --- |
| Settings 的行和面板 | About、Shuffle、Repeat、Main Menu、Music Menu、Volume Limit、Backlight、Brightness、Audiobooks、EQ、Sound Check、Clicker、Date & Time、Sort By、Language、Legal、Reset Settings。右侧图片下有用途或当前值说明。见 `S/settings-00.png`、`S/volume-limit-row.png`、`S/settings-bottom.png`。 | 顺序对应。末尾增加 Advanced Settings。部分行复用通用图和通用标题或值。About 使用设备预览。见 `apps-ipod/screens/fruit_menu.c:543`、`apps-ipod/widgets/menu.c:415`、`apps-ipod/widgets/menu.c:466`。模拟器 `A/settings-top.png`、`A/settings-bottom.png`。 | 外观差异。逐行说明、缺失图和复用图约 2–4 人日。高级设置是产品扩展，是否隐藏由产品决策。 |
| Shuffle | SELECT 原地循环 Off、Songs、Albums。Songs 说明为 Shuffle Entire Music Library。见 `S/20-shuffle-off.png`、`S/21-shuffle-songs.png`。 | 循环三个状态。Albums 没有专用预览图。见 `apps-ipod/screens/fruit_menu.c:594`、`apps-ipod/widgets/menu.c:368`。模拟器 `A/setting-shuffle.png`、`A/setting-shuffle-songs.png`、`A/setting-shuffle-albums.png`。 | 外观差异，0.5–1 人日。原地切换已进入指定提交。 |
| Repeat | SELECT 原地循环 Off、One、All。右侧分别说明播放当前列表一次、单曲循环、当前列表循环。见 `S/settings-repeat-1.png` 至 `S/settings-repeat-3.png`。 | 原地循环三个状态。高级设置启用 Shuffle 或 A-B 后，Fruit 行可以显示原版没有的状态，并且下一次 SELECT 转到 Off。见 `apps-ipod/screens/fruit_menu.c:606`、`apps-ipod/widgets/menu.c:378`。模拟器 `A/setting-repeat.png`、`A/setting-repeat-one.png`、`A/setting-repeat-all.png`。 | 行为与外观差异，0.5–1 人日。原地切换本身已进入指定提交。 |
| Main Menu | 清单顺序为 Music → 缩进 Cover Flow、Genius Mixes、Playlists、Artists、Albums、Compilations、Songs、Radio、Genres、Composers、Audiobooks、Search → Photos、Podcasts、iTunes U、Videos → 缩进 Video Playlists、Movies、TV Shows、Music Videos、Rentals → Extras → 缩进 Clocks、Alarms、Calendars、Contacts、Games、Notes、Screen Lock、Stopwatch → Shuffle Songs、Sleep、Reset Menu。Radio 与 iTunes U 在清单默认勾选。当前无配件或内容的主菜单不展示这两项。见 `S/main-menu-top.png`、`S/main-menu-mid2.png`、`S/main-menu-mid3.png`、`S/main-menu-bottom.png`。 | 省略 Genius Mixes、iTunes U，增加 Playback Report。默认 Radio 位打开。Radio 清单项无可进入的对应入口。清单自定义行数据，未接入原版右侧预览。见 `apps-ipod/screens/fruit_menu.c:438`、`apps-ipod/screens/fruit_menu.c:486`、`apps-ipod/screens/fruit_menu.c:690`、`apps-ipod/screens/fruit_menu.c:761`、`apps-ipod/settings/settings_list.c:2015`。模拟器 `A/setting-main-menu.png`。 | 功能缺失和外观差异，1–2 人日。配件支持和媒体内容另计。 |
| Music Menu | Cover Flow、Genius Mixes、Playlists、Artists、Albums、Compilations、Songs、Radio、Genres、Composers、Audiobooks、Search、Reset Menu。Genius Mixes 默认不勾选，其他媒体项勾选。见 `S/music-menu-top.png`、`S/music-menu-bottom.png`。 | 同序清单省略 Genius Mixes。所有 11 位置默认打开。Radio 无 Music 菜单入口。见 `apps-ipod/screens/fruit_menu.c:226`、`apps-ipod/screens/fruit_menu.c:255`、`apps-ipod/settings/settings_list.c:2018`。模拟器 `A/setting-music-menu.png`。 | 功能缺失，0.5–1 人日。Genius 内容来源另计。 |
| 两个清单的 Reset Menu | 白底独立页面。标题 Reset Menu，正文说明恢复该清单原始状态，底部 Reset、Cancel，首次焦点为 Cancel。见 `S/main-menu-reset.png`、`S/music-menu-reset.png`。 | 通用确认框。接受后恢复各自位掩码。见 `apps-ipod/screens/fruit_menu.c:466`。 | 外观和交互差异，1–2 人日。 |
| Volume Limit | 专用全屏音量滑条。Play 打开 Enter Combination，正文 Enter New Combination。SELECT 确认音量并退出。见 `S/volume-limit-open.png`、`S/volume-lock-enter.png`。锁定后的解锁、重设和错误码未验证。 | 通用音量限制值列表，退出后调用 setvol。没有在此设置锁定组合码。见 `apps-ipod/screens/fruit_menu.c:368`、`apps-ipod/widgets/option_select.c:482`。模拟器 `A/setting-volume-limit.png`。 | 功能缺失和新界面，3–5 人日。 |
| Backlight | 双栏时长列表，右侧图随选择变化。值为 Off、2、5、10、20、30 秒、Always On。默认 10 秒。见 `S/backlight-default.png`、`S/backlight-bottom.png`。 | 通用列表用前 23 个超时值，可接受配置文件中的任意值。设备默认 15 s，模拟器默认常亮。预览图将多个时长映射到同一张图。见 `apps-ipod/settings/settings_list.c:229`、`apps-ipod/settings/settings_list.c:246`、`apps-ipod/settings/settings_list.c:894`、`apps-ipod/widgets/menu.c:346`。模拟器 `A/setting-backlight.png`。 | 值域和外观差异，1–2 人日。 |
| Brightness | 专用全屏亮度滑条，使用百分比。测试值 50%。见 `S/brightness-open.png`、`S/brightness-turn.png`。 | 通用选项列表，iPod 6G 范围 1–63，步长 1，默认 32。见 `apps-ipod/screens/fruit_menu.c:381`、`apps-ipod/settings/settings_list.c:1091`、`firmware/export/config/ipod6g.h:238`。模拟器 `A/setting-brightness.png`。 | 外观和控制差异，1–2 人日。原版级数未测。 |
| Audiobooks | 白底对话页。底部按键 Slower、Normal、Faster。首选 Normal。说明明确表示语音音调保持。见 `S/audiobooks-default.png`、`S/audiobooks-begin.png`、`S/audiobooks-end.png`。 | 50%–250%，步长 5%，默认 100%。原版三个名称及映射缺失。见 `apps-ipod/screens/fruit_menu.c:386`、`apps-ipod/settings/settings_list.c:1915`。模拟器 `A/setting-audiobooks.png`。 | 行为和外观差异，0.5–1 人日。两侧实际播放速率未计量。 |
| EQ | 从 Off 开始。顺序为 Off、Acoustic、Bass Booster、Bass Reducer、Classical、Dance、Deep、Electronic、Flat、Hip Hop、Jazz、Latin、Loudness、Lounge、Piano、Pop、R&B、Rock、Small Speakers、Spoken Word、Treble Booster、Treble Reducer、Vocal Booster。右侧显示所选预设的频段图。见 `S/eq-top.png`、`S/eq-mid.png`、`S/eq-mid2.png`、`S/eq-bottom.png`。 | Equalizer Presets，9 项内置 Flat、Neutral、Warm、Vivid、Bright、Bass Boost、Sub Bass、Late Night、Spoken，并追加磁盘预设文件。停留时试音。见 `apps-ipod/screens/settings/eq_editor.c:72`、`apps-ipod/screens/settings/eq_editor.c:237`。模拟器 `A/setting-eq.png`。 | 功能与外观差异，2–4 人日。预设具体响应曲线未测。 |
| Sound Check | On/Off 原地切换。见 `S/sound-check-off.png`、`S/sound-check-on.png`。 | 将 On 映射到 ReplayGain Shuffle，或显示 Advanced Settings 已设的 Track/Album Gain 为 On。默认是 ReplayGain Shuffle。见 `apps-ipod/screens/fruit_menu.c:621`、`apps-ipod/settings/settings_list.c:1458`。 | 行为差异，2–4 人日估算。原版归一化增益与相同音频的输出需独立测量。 |
| Clicker | On/Off 原地切换，右侧有状态图和说明。见 `S/clicker-on.png`、`S/clicker-off.png`。On 说明通过耳机和扬声器播放。Off 说明滚动无声。声音未录。 | 原地开关硬件压电片。独立软件 clicker 由高级设置控制。软件声默认关闭。输入重复、松开和滚轮事件各有过滤条件。见 `apps-ipod/screens/fruit_menu.c:634`、`apps-ipod/audio/sound_feedback.c:76`、`apps-ipod/settings/settings_list.c:2190`。 | 行为差异。默认开关不连动软件音频。点击波形、响度、每格时机未验证。1–3 人日。 |
| Date & Time | Date、Time、Time Zone、24-Hour Clock、Time in Title。Date 和 Time 行直接显示日期与时间。原版各有专用编辑页。日期编辑显示日历和月、日、年字段。时间编辑显示模拟时钟及小时、分钟、AM/PM。时区编辑显示世界地图，轮转切换城市，界面显示时差和当地时间。选定 Manchester 后，设备时间从 1:22 PM 变为 8:22 AM。24 Hour Clock 和 Time in Title 原地切换。见 `S/date-time-top.png`、`S/date-open.png`、`S/time-open.png`、`S/time-zone-open.png`、`S/time-zone-next.png`、`S/time-zone-confirm.png`、`S/clock-24-on.png`、`S/time-in-title-idle.png`。 | Date 和 Time 行无当前值。两行打开同一套 Time & Date Settings。选 Set Time/Date 后才能编辑。编辑器把 24 小时、分钟、秒和日期合在一起。Time Zone 打开 2 秒 Not Available 提示。24-Hour Clock、Time in Title 原地切换。见 `apps-ipod/screens/fruit_menu.c:65`、`apps-ipod/screens/fruit_menu.c:393`、`apps-ipod/screens/fruit_menu.c:533`、`apps-ipod/screens/system/time_set.c:115`、`apps-ipod/screens/settings/time_settings.c:62`、`apps-ipod/screens/settings/time_settings.c:168`。模拟器 `A/setting-date-time.png`、`A/setting-date-edit.png`、`A/setting-time-zone.png`。 | 时区功能缺失。日期时间编辑字段和确认行为不同，3–6 人日。 |
| Sort By | First、Last 原地切换，控制 Contacts。见 `S/sort-by-first.png`、`S/sort-by-last.png`。 | 原地切换 contacts_sort。见 `apps-ipod/screens/fruit_menu.c:653`、`apps-ipod/settings/settings_list.c:2023`。 | 核心行为源码对应。预览、姓名缺失时的排序和同名顺序未验证，0.5–1 人日。 |
| Language | 内置语言菜单，右侧为多语言 Welcome 串。测试的顺序为 English、日本語、Čeština、Dansk、Deutsch、Español、Français、Ελληνικά、Italiano、Magyar、Nederlands、Norsk、Português、Русский、Polski、Suomi、Svenska、Türkçe、한글、简体中文、繁體中文。见 `S/language-top.png`、`S/language-bottom.png`，首次开机页面另见 `S/31-lang.png`。 | 浏览 LANG_DIR 的语言包。文件可用性决定列表。见 `apps-ipod/screens/fruit_menu.c:421`。模拟器 `A/setting-language.png`。 | 界面、内置范围和应用流程差异，1–3 人日。 |
| Legal | 白底滚动声明页。见 `S/legal-open.png`。 | 文本查看器打开项目 GPL 声明 `COPYING.txt`。见 `apps-ipod/screens/main_menu.c:122`。模拟器 `A/setting-legal.png`。 | 文字和界面不同。项目自己的法律文本属于产品差异。界面约 0.5 人日。 |
| Reset Settings | 独立确认页面，默认选取消。正文说明重置全部设备设置，保留同步内容。见 `S/reset-open.png`。没有执行确认。 | 通用 Yes/No 对话框，默认选择 Yes。接受后重置 AstraPod 全部设置，保存并再次应用。见 `apps-ipod/screens/fruit_menu.c:416`、`apps-ipod/screens/main_menu.c:58`。模拟器 `A/setting-reset.png`。 | 外观差异。默认确认焦点不同。原版确认后的语言重选流程未验证，1–2 人日。 |

### About 的三页及按键

| 页面或动作 | 原版 2.0.5 与证据 | Fruit Company 与证据 | 差异和估时 |
| --- | --- | --- | --- |
| 标题、容量、图例 | 黑色 iPod 状态栏与品牌图标。Used 和 Free 下为 Audio、Video、Photos、Other 图例。见 `S/14-about-1.png`。 | 浅色状态栏与左对齐 AstraPod 标题。使用 AstraPod 品牌图标。Audio 以码率和长度估算。条形图分 Audio 和 Other，图例仍含 Video 和 Photos。见 `apps-ipod/screens/system/fruit_about.c:84`、`apps-ipod/screens/system/fruit_about.c:198`、`apps-ipod/screens/system/fruit_about.c:265`、`apps-ipod/screens/system/fruit_about.c:330`。模拟器 `A/about-1.png`。 | 品牌是产品差异。状态栏外观和容量字段的行为存在差异，1–3 人日。图片和视频的字节统计与 `t3/photos-videos` 协调。 |
| 计数页 | 左列 Songs、Videos、Podcasts、iTunes U。右列 Photos、Games、Contacts。测试磁盘显示 Games 3。见 `S/15-about-2.png`。 | 不显示 iTunes U。Games、Contacts 值没有填充。Songs 统计非播客音频，Photos/Videos 从文件索引统计。见 `apps-ipod/screens/system/fruit_about.c:86`、`apps-ipod/screens/system/fruit_about.c:140`、`apps-ipod/screens/system/fruit_about.c:286`。模拟器 `A/about-2.png`。 | 功能和外观差异，1–2 人日。iTunes U 导入另计。 |
| 身份页 | Serial Number、Model、Version。模拟测试显示空串号、Model 为短横线、Version 为 `2.0.5 PC`。见 `S/16-about-3.png`。 | 设备从 SysCfg 读取串号与型号。模拟器用空串号及短横线。显示 AstraPod 版本和 Windows 格式后缀。见 `apps-ipod/screens/system/fruit_about.c:118`、`apps-ipod/screens/system/fruit_about.c:150`、`apps-ipod/screens/system/fruit_about.c:311`。模拟器 `A/about-3.png`。 | 品牌和固件版本为产品差异。有效 SysCfg 和非 Windows 格式未验证。 |
| 翻页 | SELECT 与 Next 向前，Previous 向后，首尾循环。MENU 返回 Settings。见 `S/14-about-1.png` 至 `S/19-settings-return.png`。 | 相同按键语义见源码。无翻页时序测量。见 `apps-ipod/screens/system/fruit_about.c:447`。 | 行为源码对应。页面翻转视觉和耗时未确认。 |

### Extras > Clocks、Alarms 与 Sleep

| 页面或动作 | 原版 2.0.5 与证据 | Fruit Company 与证据 | 差异和估时 |
| --- | --- | --- | --- |
| Clocks 与城市选择 | 当前城市一行，显示模拟时钟与当地时间。SELECT 打开 Add/Edit。Add 进入 Region，再到 City。区域列表有 Africa、Asia、Atlantic、Australia、Europe、North America、Pacific、South America。城市行随滚动预览当地时间。见 `/tmp/fruit-stock205.YNLNri/clock-or-calendar-select.png`、`/tmp/fruit-stock205.YNLNri/probe2-clock-region.png`、`/tmp/fruit-stock205.YNLNri/probe2-clock-city.png`。 | 默认 California，支持 Add、Edit、Delete，最多 8 个城市。首个城市定义 RTC 本地时间的换算基准。显示城市表与时差，设置从 `clocks.txt` 读写。Fruit Settings 中的 Time Zone 没有独立可用的入口。见 `A/extras-clocks.png`、`apps-ipod/screens/extras/extras_clock.c:284`、`apps-ipod/screens/extras/extras_clock.c:311`、`apps-ipod/screens/extras/extras_clock.c:739`、`apps-ipod/screens/extras/extras_clock.c:848`。 | Clocks 主页和选择流程有对应。缺少系统时区入口和储存联动。各地区全量城市、夏令时边界和多城市滚动待验证。1–2 人日，与 Date & Time 共用状态。 |
| Alarms 空页与新闹钟 | Create Alarm / New Alarm、Sleep Timer / Inactive。右侧有红色闹钟。见 `S/alarms-empty.png`。 | 相同入口及两行菜单。创建时默认打开当前分钟的单次闹钟。最多 8 个。见 `A/extras-alarms.png`、`apps-ipod/screens/extras/extras_alarms.c:117`、`apps-ipod/screens/extras/extras_alarm_ring.h:14`。 | 空页核心外观对应。原版容量上限与跨重启待确认。 |
| Alarms 编辑、重复、声音和标签 | 创建后显示 Alarm 开关、Date、Time、Repeat、Sound、Label、Delete。重复项依次为 Once、Every Day、Weekdays、Weekends、Every Week、Every Month、Every Year。Sounds 的 Tones 为 None、Beep。空资料库的 Playlists 仅有 None。Label 有 23 项，首项 Wake up，末项 Party。见 `S/alarm-edit.png`、`S/alarm-repeat-top.png`、`S/alarm-sounds.png`、`S/alarm-tones.png`、`S/alarm-playlists.png`、`S/alarm-label-top.png`、`S/alarm-label-mid.png`、`S/alarm-label-bottom.png`。 | Alarm On/Off、Date、Time、Repeat、Sounds、Label、Delete。Repeat 为 Once、Every Day、Weekdays、Weekends、Every Week、Every Month、Every Year。Tones 为 None、Beep。Playlists 始终只有 None。Label 的 23 个值依次为 Wake up、Work、Class、Alarm、Appointment、Phone Call、Breakfast、Lunch、Dinner、Important、Prescription、Take Medicine、Return Call、Delivery、Pick-Up、Meeting、Reminder、Date、Anniversary、Birthday、Holiday、Go Home、Party。见 `apps-ipod/screens/extras/extras_alarm_ring.c:71–84`。见 `apps-ipod/screens/extras/extras_alarms.c:90`、`apps-ipod/screens/extras/extras_alarms.c:474`、`apps-ipod/screens/extras/extras_alarms.c:763`、`apps-ipod/screens/extras/extras_alarm_ring.h:14`。 | 编辑页、空 Playlists 和 Tones 对应。Fruit 无法绑定可用播放列表。原版带有非空播放列表时的行为未验证，2–4 人日估算。 |
| 闹钟的日期和时间编辑 | 日期编辑展示日历。时间编辑显示模拟钟、小时、分钟和 AM/PM。系统设为 24 Hour Clock On 后，该编辑器仍显示三项，左下说明显示 24 小时值。见 `S/clock-24-on.png`、`S/alarm-date.png`、`S/alarm-time.png`。 | 编辑器使用相同的日期与模拟钟布局。24 小时模式将选择字段从三个减为两个。见 `apps-ipod/screens/extras/extras_alarms.c:577`、`apps-ipod/screens/extras/extras_alarms.c:682`。 | 行为和外观差异，0.5–1 人日。 |
| 响铃、贪睡、休眠唤醒 | 原版 QEMU 设置 09:32 闹钟。采样跨过 09:31 至 09:32，仍显示 Settings，未捕获响铃提示。见 `S/alarm-main-scheduled.png`、`S/crossing-7.png`、`S/crossing-8.png`。当前 PMU 模型未连接唤醒中断。响铃、贪睡、休眠唤醒和音色未验证。 | UI 线程轮询闹钟，某些全屏程序结束后才提示。播放器关机时无唤醒。响铃暂停音频，关闭提示后恢复。Dismiss/Snooze，贪睡 10 分钟。Beep 2 kHz，每次 120 ms，每秒两次。见 `apps-ipod/screens/extras/extras_alarm_ring.c:4`、`apps-ipod/screens/extras/extras_alarm_ring.c:48`、`apps-ipod/screens/extras/extras_alarm_ring.c:444`。 | 行为待确认。登记休眠唤醒和阻塞程序用例。若证实原版支持唤醒，预计 3–6 人日，依赖 PMU/RTC。 |
| Sleep Timer | 在 Alarms 内选 Off、15、30、60、90、120 分钟。设 15 分钟后，Sleep Timer 副标题显示 15 Min Remaining，右侧显示时长。见 `S/sleep-timer-top.png`、`S/sleep-timer-bottom.png`、`S/sleep-timer-active.png`。 | Extras > Alarms 有相同六项，设完回 Alarms。Power 定时器以当前 tick 计算结束时刻。见 `apps-ipod/screens/extras/extras_alarms.c:90`、`apps-ipod/screens/extras/extras_alarms.c:808`、`firmware/powermgmt.c:1182`。 | 菜单和值域对应。倒计时舍入、到期停播与唤醒未验证。 |
| Home > Sleep | Main Menu 清单有 Sleep，默认不勾选。见 `S/main-menu-bottom.png`。勾选后在 Home 增加月亮与星星预览。SELECT 后的画面保留原帧，未出现 Sleep Timer 设置。见 `S/home-sleep/sleep-checked.png`、`S/home-sleep/home-sleep-row.png`、`S/home-sleep/home-sleep-result.png`。QEMU 的背光或待机电源未确认。 | 显示 Sleep 时，SELECT 打开 AstraPod 四项睡眠定时设置。包括启动或取消计时、计时时长、开机启动计时、按键重计时。见 `apps-ipod/screens/fruit_menu.c:144`、`apps-ipod/screens/fruit_menu.c:746`、`apps-ipod/screens/settings/general_settings.c:859`。 | 行为差异，0.5–1 人日。验证进入休眠后返回的菜单位置。 |

### Hold、电池、USB 和开关机

| 状态或动作 | 原版 2.0.5 与证据 | Fruit Company 与证据 | 差异和估时 |
| --- | --- | --- | --- |
| Hold 开关 | 阻塞。此 QEMU 的 PMU 将 GPIOSTAT 的 Hold 状态固定为 OPEN。缺少原版打开 Hold 后的图标、输入抑制、背光和退出截图。见 `/tmp/claude-1000/stockfw/qemu-ipod-classic/hw/arm/ipod_classic/ipod_classic_pmu.c:111`。Extras > Screen Lock 属于另一条路径。 | 模拟开关锁住后转轮未移动选中行，未显示 Hold 图标。见 `A/sim-hold.png`、`A/sim-hold-wheel.png`、`A/sim-hold-released.png`。6G 设备读取 PMU 锁状态，模拟器独立模拟开关。见 `firmware/target/arm/ipod/button-clickwheel.c:475`、`firmware/target/hosted/sdl/button-sdl.c:576`、`firmware/target/hosted/sdl/button-sdl.c:681`。 | 先补原版 Hold 取证，再定图标与时机。1–2 人日。 |
| 电池状态栏 | 原版普通绿电池、供电时带闪电。见 `S/04-probe.png`、`S/charger-wake.png`。ADC 低值探针没有正常进入界面。未确认电量阈值及充满、充电中和低电的分界。 | 有 23 张电池电量帧，映射经 6G 设备的校准或模拟器电量比例。充电时绘制闪电，插电但未充电绘制插头。见 `apps-ipod/ui/theme_art.c:13`、`apps-ipod/ui/native_chrome.c:588`、`A/settings-top.png`。 | 已有主要图形状态。阈值、动画和实际 PMU 行为未验证。1–3 人日。 |
| 接适配器时的待机 | QEMU 用 `PMUCHARGER=adapter`。长按 Play/Pause 2.5 秒后观察到全屏 Charging。黑色状态栏，居中大电池、闪电和倒影。SELECT 唤回菜单。见 `S/charger-standby.png`、`S/charger-wake.png`。2.5 秒为输入长度，非触发阈值。 | 6G 允许充电中关闭电源，关机进入 PMU standby。通用 Charging 文本提示在部分路径中存在。普通 6G 关机路径不进入该提示。见 `firmware/export/config/ipod6g.h:198`、`firmware/target/arm/s5l8702/ipod6g/power-6g.c:35`、`apps-ipod/system/app_util.c:326`。 | 专用充电待机画面和状态转换缺失。新界面与行为预计 3–5 人日。 |
| 低电提示 | `PMUBATT=250` 会话 31 秒采样均为黑屏。见 `S/power/boot-poll-00.png` 至 `S/power/boot-poll-14.png`。当前样本没有原版低电警告截图。 | 低电和电耗尽分支显示文本并关闭电源。见 `apps-ipod/system/shutdown.c:99`。 | 原版文案、警告触发和外观待确认。1–2 人日估算。 |
| USB Connected、Synchronizing、Eject、Disk Mode | 阻塞。此 QEMU 的 USB OTG、host 和 function 寄存器仅占位。未取得原版 USB 枚举、数据同步、安全弹出或 bootloader Disk Mode 的 2.0.5 截图。见 `/tmp/claude-1000/stockfw/qemu-ipod-classic/hw/arm/ipod_classic/ipod_classic.c:390`。电源注入与 USB 数据连接分开。 | 模拟 USB 先弹出用途提示，连接后显示橙色 dock 图标与 Connected / Eject before disconnecting.。拔线返回界面。渲染器明确以 1.1.2 Connected 为参考，未区分不同同步阶段。见 `A/sim-usb-prompt.png`、`A/sim-usb.png`、`A/sim-usb-unplug.png`、`apps-ipod/screens/system/usb_stock.c:4`、`apps-ipod/screens/system/usb_stock.c:82`、`apps-ipod/screens/system/usb_screen.c:161`。 | 目前只能确认 AstraPod 的单一连接页。同步、弹出和独立磁盘模式尚需比较。3–6 人日估算，依赖原版数据链路取证。 |
| 开机、关闭电源、唤醒 | 直接加载 OSOS 跳过 boot ROM 和 bootloader。黑屏后出现 Welcome/Language，再进入主菜单。见 `S/00-boot.png`、`S/31-lang.png`、`S/32-home.png`。冷启动标志、系统关机时机和持久恢复未验证。 | 使用 AstraPod 开机标志、阶段提示和进度条。见 `A/boot-00.png`、`A/boot-05.png`、`A/boot-10.png`。关闭电源时依设置和安全电量显示文字，再关闭硬件。空库模拟中保持 Play 3 秒，未捕获目标关机画面。见 `A/sim-shutdown.png`。见 `apps-ipod/main.c:431`、`apps-ipod/system/shutdown.c:90`。 | 开机品牌由产品决定。关机流程与恢复位置待确认。2–4 人日估算。 |

## 空库与错误提示

AstraPod 的空夹具只安装固件资源，没有用户媒体。截图记录首次建立索引前的分支。原版空媒体分类用本节前方的 2.0.5 测试盘观察。

| 场景 | 原版 2.0.5 | AstraPod | 类型与工作量 |
|---|---|---|---|
| 索引未就绪或没有音乐 | 当前原版测试盘有歌曲。空整库的 Shuffle Songs 行、拔线后建库和坏数据库，未验证。 | Shuffle Songs 弹出 Database is not ready。随后右栏出现 1 Songs。Songs 与 Podcasts 弹出 Please reboot to enable。见 `sim/empty-shuffle.png`、`sim/empty-shuffle-after.png`、`sim/empty-music-songs.png`、`sim/empty-podcasts.png`，以及 `apps-ipod/screens/fruit_menu.c:158–170`、`apps-ipod/ui/menu_pane.c:676–684`。 | 行为、外观。冷索引和空库文案计入 P2。计数来源未确认，0.5–1 日做空索引与有效空库对照。 |
| 无照片或视频 | 右侧解释无内容，SELECT 留在分类行。原版画面见 `stock-home/frame-at-3320.png`、`stock-home/frame-at-5900.png` 和媒体章节。 | 通用索引提示 Nothing found, or no scan yet。无法从文案区分首次扫描和空库。见 `sim/empty-videos.png`、`sim/empty-photos.png`、`apps-ipod/screens/browse/browser_flat.c:437–463`。 | 行为、外观，0.5–1 日。P9 已覆盖。 |
| 文件无法解码 | 此次 stock 视频样本提供了播放叠层。解码失败弹层未验证。 | 合成 H.264 在 sim 明确报告依赖 iPod 视频解码器。测试库的空 MP3 与截断 FLAC 被数据库扫描器拒绝。见 `sim/videos-play-attempt.png`、`sim/fixture-first.log`。 | 运行环境状态已验证。设备错误页面与失败后续播未验证，1–2 日补测。 |

## 证据缺口与复验顺序

下表列出尚未对照完的场景。每项已分配到对应工作包。列表现状和源码推导不能证明未到达时的运行行为。

| 状态 | 未验证的场景 | 复验条件 |
|---|---|---|
| 未验证 | 原版 iPod Quiz 的答题与历史，Klondike 和 Vortex 游戏内页。 | 在本地 QEMU 提供可启动的合法游戏数据。记录 2.0.5 的空曲库、首次启动、报错和可运行页面。P8。 |
| 未验证 | Genius 推荐成功、iTunes U 非空、Radio 附件、租赁授权和到期。 | 分别准备带对应索引或附件状态的本地夹具。先测入口条件，再测操作。P2、P5、P9、P10。 |
| 未验证 | 带歌词与章节的播放 SELECT 循环、同步评分、暂停恢复、同源缩略图。 | 两侧加载相同的合成音频、嵌入歌词、带章节的口述媒体和同一张自制封面。P1、P2、P4。 |
| 未验证 | 原版视频实际解码帧、字幕、多音轨、快进快退、挂起续播、TV 输出，以及原版照片全分辨率帧。 | 本地合成媒体先验收文件导入。再用支持这些目标能力的环境抓帧计时。P9。 |
| 未验证 | 物理 Hold、充电触发与低电保护，硬件的 Clicker，完整 USB 主机同步状态。 | 在软件模型能发出等价状态时先补模拟证据。硬件测量另立授权测试。P10。 |
| 未验证 | 不可挂载、损坏库、破损媒体和未知文件的逐屏对照。 | 只在独立镜像上使用合成错误，避免混淆 H.264 模拟器限制和设备错误。按错误归 P2、P9、P10。 |
| 未验证 | 完整的压力转场矩阵。包括早按、逆向取消、冷封面、播放启动、弹层来回、USB 抢占。 | 同夹具、同手势、同起止判据。每路径采集多次，保留首个变化帧、稳定帧和采样误差。P12。 |

### 文件浏览器边界

原版 Extras 的八行没有通用 Files。Notes、Contacts、Photos、Videos 各自提供分类入口。AstraPod 额外提供 Extras → Files，可浏览本地文件树。顶层包括 Search 和普通文件夹。见 `sim/extras-files.png`、`apps-ipod/screens/fruit_menu.c:141,345`、`apps-ipod/root_menu.c:504–531`。同类对照以各媒体专用入口为准。AstraPod 的独立文件浏览器保留为产品扩展。该入口的改名、移除和可见性由 P0 决策，估算含在 P0。

## 可并行工作包

每包只修改其独占文件范围。各包可以先提交独立页面和接口。P0 单独合并共用菜单、资源注册和入口。P1 与 P4 通过上下文入口交接。P2 与 P5 分开浏览数据和推荐的数据来源。P9 向 P11 提供媒体统计，P11 自行绘制 About。

| 包 | 交付范围 | 新视觉 / 恢复行为 | 独占范围 | 人日与依赖 |
|---|---|---|---|---|
| P0 主菜单与公共接线 | 隐藏规则、清单入口、选项的右栏说明、公共标签和资源注册。 | 清单、说明图。显隐与入口 | `screens/fruit_menu.c`、`root_menu.c`、`widgets/menu.c`、`settings/settings_list.c`、`lang/` 和公共 stock art 注册。 | 3–5。等各独立接口定稿。 |
| P1 Now Playing | 短 SELECT 状态机、定位、评分、随机选择、条件歌词和状态图标。 | 定位条、评分与选择控件。SELECT 状态机 | `ui/native_wps.c`、`screens/playback/wps.c`、`viewers/lyric_viewer/`。Cover Flow 入口联验。 | 5–9。不编辑上下文和全局按键表。 |
| P2 音乐与口述媒体浏览 | Artists、Albums、Songs、Genres、Composers、Compilations。排序与索引。播客、有声书及 iTunes U。 | 双行、封面与索引提示。分类与排序 | `screens/browse/browser{,_db}.*`、`tagnavi.config`、`database/`、`widgets/list_quickscroll.c`。 | 10–17。有内容的口述与合辑夹具先验收。 |
| P3 Search | 按 2.0.5 路径测字符编辑、实时结果、分组、无结果和返回。 | 字符条与双行结果。键入与焦点切换 | `screens/system/db_search.*`、`widgets/search_dialog.*` 与新界面私有文件。 | 3–5。P2 通过查询接口供数据。 |
| P4 上下文与 On-The-Go | 条件菜单、独立队列、添加、清除、保存和启动播放。 | 上下文与队列页面。独立保存、清除流程 | `screens/context_menu.c`、`playlist/`、专用上下文模态。 | 5–9。P1 只调用菜单入口，P5 只提供推荐状态。 |
| P5 Genius | 可见性、空态、推荐入口。单独核实推荐数据契约。 | 不可用页与推荐页。数据契约 | 新的独立推荐数据模块和页面。由 P0 与 P4 接入。 | 1–2 完成缺数据状态。可用推荐估时待接口调查。 |
| P6 设置编辑器 | 音量限制与组合码、背光、亮度、EQ、语言、日期时间、时区和确认页。 | 滑条、地图、确认页。值域、组合码、时区 | `screens/settings/`、`screens/system/time_set.*`、`widgets/option_select.c`。P0 登记入口与值。 | 8–14。与 P7 约定共享时区及组合码接口。 |
| P7 Extras 实用工具 | 时钟、闹钟及睡眠、日历、联系人、便笺、屏幕锁与秒表。 | 日历与工具页。同步、提醒、状态留存 | `screens/extras/`。保留与 P6 的时区、睡眠选项及 P10 的唤醒接口。 | 12–22。逐项按 Extras 与系统表验收。 |
| P8 Games 和扩展入口 | 游戏目录的封面、上次游玩、内容不可用状态。Quiz 入口与已有游戏区。 | 图标与副行。上次游玩和失败状态 | `screens/browse/app_list.*` 与游戏目录私有页面。 | 2–5。原版游戏内容和实际可运行性单独列为证据缺口。 |
| P9 Photos / Videos | 导入和分类、同步清单、播放、相簿、图像、幻灯片及设置。 | 网格、叠层及设置页。媒体索引、播放和幻灯片 | 由 `t3/photos-videos` 所有。对公共文件的改动由 P0 合并。 | 已有任务。本文各条只估相应差距，不计入新派工。 |
| P10 启动、连接与电源 | Boot、Hold、关机、低电、充电、USB/Disk Mode/sync 与 Clicker。 | 充电与同步状态页。关机、休眠、连接和点击声 | `screens/system/usb_{screen,stock}.*`、`system/shutdown.c`、启动屏、电源及 clicker 专用模块。 | 8–16。实体时序需要授权后的独立验收。 |
| P11 About | 字节分类、完整计数、设备信息、翻页。 | 容量图与计数排版。统计与翻页 | `screens/system/fruit_about.*`。 | 2–4。消费 P2、P7、P8、P9 的统计接口。 |
| P12 已有交互工作 | split-pane、设置 SELECT 原地切换、Scrim 行图标、Control Centre、wheel-only nav。 | 验证、已有修正 | `ui/scene.c`、`ui/menu_pane.c`、`input/keymap_ipod.c` 及现有任务各自文件。 | 2–4 补测。split-pane、SELECT 原地切换、Scrim 图标和 Control Centre 相关提交已合入基线。wheel-only 分支未合入。 |

文件范围均以 `apps-ipod/` 为前缀，固件电源模块与启动模块按实际目录。P0 只负责公共接线，不包揽各页面重写。P12 有公共渲染文件时，P0 和 P12 分开提交并顺序合并。
