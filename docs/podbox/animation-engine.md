# PodBox 全局动画引擎设计

固件内部设计。FU85 阶段 0 使用 `f3165f2240` 完成调查、离线测量和 SDL 原型。Native UI 的实现基准为 `43eb847f83`。本文用 f 表示帧时长，1 f = 1/60 s。

## 架构决策

FU85 阶段 0 建议方案 A。用户随后选择逐步迁移到方案 B，同时保留 Scrim 的静态外观。首期保留旧绘制器作为迁移适配器。新合成器拥有页面快照、位移与显示提交，接力面板和确认框复用原有几何。第二期逐步接入列表，之后迁移特殊页面。媒体时间、音频计算和协议计时仍由各自模块管理。

| 决策项 | A：现有 GUI 加动画引擎 | B：新的保留模式 GUI |
| --- | --- | --- |
| 改动规模 | 工程估算。首期约 1–2 千行 C，含调度、合成及集成点。列表追加约 1–2 千行。第三期按页面迁移，保留专用光栅器和数据源。 | 工程估算。场景树、布局、焦点、合成、命中测试、旧渲染桥接约 4–8 千行 C。全量迁移约 1–2 万行，取决于复用程度。 |
| 迁移范围 | 首期修改页面分派、菜单、接力面板和呈现接口。第二期接入普通列表及皮肤列表。第三期接入歌词、Cover Flow、播放记录卡片、Agents 和皮肤装饰动效。 | 逐批迁移首页、设置、浏览和播放列表、通用对话框、接力、Agents、歌词、Cover Flow、阅读和系统页面。保留 WPS/SBS 皮肤解释器、字体、列表数据源、设置、导航返回值和插件 API。用适配器把现有 WPS 及暂未迁移页面绘制到宿主图层。 |
| 规模依据 | `apps-ipod/root_menu.c` 有 31 个 `[GO_TO_…]` 表项。对基准运行 `git grep -l -F -e 'gui_synclist_init' -e 'gui_synclist_do_button' -e 'list_do_action(' f3165f2240 -- 'apps-ipod/*.c'` 得 11 个直接引用文件。还有通过组件进入的页面、动态菜单和插件。 | 同一组入口都必须验证。迁移期间要同时维护场景树与现有视口、活动栈、皮肤回调和嵌套对话框。两个实现可能同时持有屏幕相关资源。 |
| 运动与代码 | 共享绝对时间、缓动、重定向、帧调度、损伤合并和取消。已有代码的副作用集中在少数绘制适配器。新增页面仍须声明快照能力。 | 位置、尺寸和透明度是节点属性。场景树集中管理布局、命中、失效与遮挡。覆盖率达到后，新页面少写一套即时绘制控制。迁移中要维护皮肤和场景桥接。 |
| 60 fps | 已预绘制的小条带值得验证。整屏滑动跨越大部分像素，上限受 6G 的 SDRAM、LCD DMA、面板扫描及音频竞争共同制约。 | 保留图层有利于重用复杂列表行，细粒度损伤可以减少提交。整屏滑动仍经过同一 LCD 路径，且过多小区块会增加命令、等待和行复制。保留模式本身无法提高面板吞吐。 |
| RAM 规划 | 新常驻可写状态目标 ≤128 B。像素来自按需 `core_alloc`。调查以两张快照估算 307,200 B。当前页面合成另需一张输出图，共 460,800 B。 | 场景节点需池化。举例，96 个 48–80 B 节点为 4.5–7.5 KiB。三个全屏等效图层为 450 KiB，双路径额外一个全屏等效图层为 150 KiB。均为规划假设，实际预算必须包含字体、缓存、旧页和最坏并发。 |
| 播放、USB、dock、PodLink | 主要风险是分配触发音频缓冲收缩、在同步前台调用中占用 UI、抢占现有 LCD 所有权。直接沿用当前输入和系统事件派发。 | 除 A 的风险，还要逐页证明焦点、按键语义、系统事件顺序、USB ACK、dock 断开、活动栈和缓冲区生命周期一致。旧皮肤和新合成器同时绘制时尤其敏感。 |
| 逐步发布 | 默认关闭的全局开关接入完成的页面。每阶段保留现有瞬时绘制及支持的兼容路径。 | 独立编译开关或发布配置选择新后端。首批只覆盖首页、菜单和简单对话框，随后扩展列表、系统界面和专用页面。未覆盖页面通过旧适配器进入。每批保留旧实现及旧启动入口，直至相关场景验收和回退演练通过。 |

6G 测量说明快照合成有希望达到 30 fps。它也说明两套架构共用的 LCD 路径有明显约束。A 可以分期验证这些约束，同时沿用目前可运行的播放器、皮肤、USB 和 dock 路由。若多层遮挡、跨控件布局或频繁动态皮肤使 A 的适配代码超出第二期范围，可以用同一基准比较 B 的一页切片。B 的首个切片应复用统一调度和损伤接口，保留帧缓冲兼容层，再用相同输入和画面测量可见收益。

## 现有运动和可见刷新

表中的频率是源码节拍或标明来源的历史测量。没有测量记录的条目使用名义频率或事件触发。范围以 PodBox 的首方固件 GUI 为准。插件、游戏循环和媒体内容有独立的可见时钟及兼容边界。

| 路径和源码位置 | 时间、缓动与绘制 |
| --- | --- |
| 根导航和菜单。[`root_menu.c:2178`](../../apps-ipod/root_menu.c#L2178)、[`root_menu.c:2443`](../../apps-ipod/root_menu.c#L2443)、[`menu.c:1007`](../../apps-ipod/widgets/menu.c#L1007)、[`menu.c:1077`](../../apps-ipod/widgets/menu.c#L1077) | 根页使用返回码、来源记录和活动栈分派。嵌套菜单使用 `mstack`。菜单进入和返回立即重绘目标。首次绘制使用 `gui_synclist_draw_settled` 的两遍排版和单次最终呈现。 |
| 普通列表。[`list.c:399`](../../apps-ipod/widgets/list.c#L399)、[`list.c:946`](../../apps-ipod/widgets/list.c#L946)、[`list_render.c:176`](../../apps-ipod/widgets/list_render.c#L176)、[`list_render.c:256`](../../apps-ipod/widgets/list_render.c#L256) | 按键或轮加速步数立即更新逻辑选项与首行。选择条、行起点和滚动条按整数行更新。普通列表重画可见行，随后刷新视口和皮肤脏区。第 534 行的 2.4 f `sleep` 属于触屏分支。 |
| 皮肤列表和标题。[`list_skinned.c:428`](../../apps-ipod/widgets/list_skinned.c#L428)、[`list_skinned.c:506`](../../apps-ipod/widgets/list_skinned.c#L506)、[`list_render.c:400`](../../apps-ipod/widgets/list_render.c#L400)、[`lcd-scroll.c:155`](../../firmware/drivers/lcd-scroll.c#L155)、[`lcd-scroll.c:186`](../../firmware/drivers/lcd-scroll.c#L186) | `%Lb` 逐行或逐格重绘 `%Vl`，使用离散选中状态。底层文字横向跑马灯使用独立线程、配置步长与延时，按滚动速度线性推进，只刷新文本矩形。标题和选中长文本分别持有视口。手动横移在 `list.c:826` 附近直接调整偏移。 |
| 歌词焦点、逐词与进度。[`lyrics_motion.c:4`](../../apps-ipod/viewers/lyric_viewer/lyrics_motion.c#L4)、[`lyrics_motion.c:58`](../../apps-ipod/viewers/lyric_viewer/lyrics_motion.c#L58)、[`lyric_viewer.c:774`](../../apps-ipod/viewers/lyric_viewer/lyric_viewer.c#L774)、[`lyric_viewer.c:1303`](../../apps-ipod/viewers/lyric_viewer/lyric_viewer.c#L1303) | 用 `current_tick`、立方减速和 15.6 f 重定向焦点。歌词、逐词高亮和进度条使用播放时钟。缓存遮罩与背景，合并变化行，稳态按下一个字符/进度像素、换行或检查期限唤醒。名义 30 fps。FU65 历史 SDL 完整对照 32–33 fps。 |
| WPS 与歌词的页面切换及换曲。[`lyric_viewer.c:812`](../../apps-ipod/viewers/lyric_viewer/lyric_viewer.c#L812)、[`lyric_viewer.c:856`](../../apps-ipod/viewers/lyric_viewer/lyric_viewer.c#L856)、[`lyric_viewer.c:888`](../../apps-ipod/viewers/lyric_viewer/lyric_viewer.c#L888)、[`lyric_viewer.c:1093`](../../apps-ipod/viewers/lyric_viewer/lyric_viewer.c#L1093)、[`lyric_viewer.c:1456`](../../apps-ipod/viewers/lyric_viewer/lyric_viewer.c#L1456)、[`wps.c:948`](../../apps-ipod/screens/playback/wps.c#L948) | 13.2 f 入场用快照、24 px 位移和透明度。入场和无快照的旧曲淡出存在 `lv_sleep_frame`。换曲按 6/12 f 插值并绘像素。退出保留最后一张歌词画面，交给 WPS 恢复皮肤并提交完整目标。当前出口是交接边界。 |
| Cover Flow，含本地、远程、随机转动。[`carousel.c:447`](../../apps-ipod/screens/covers/carousel.c#L447)、[`carousel.c:2394`](../../apps-ipod/screens/covers/carousel.c#L2394)、[`carousel.c:3423`](../../apps-ipod/screens/covers/carousel.c#L3423)、[`carousel.c:3909`](../../apps-ipod/screens/covers/carousel.c#L3909)、[`carousel.c:4734`](../../apps-ipod/screens/covers/carousel.c#L4734)、[`carousel.c:4828`](../../apps-ipod/screens/covers/carousel.c#L4828)、[`carousel.c:4946`](../../apps-ipod/screens/covers/carousel.c#L4946) | 点击轮改变目标。运动用 `USEC_TIMER`、有界子步、速度曲线、摆角和定点透明度。相邻封面有立体纹理光栅和遮挡，快速姿态走平面快路。绘封面、字幕和皮肤，首帧全刷，后续按脏矩形或条带刷。6G 名义 30 fps。FU82 另一分支 SDL 五组记录 32.49–33.13 fps。载入线程 `sleep(HZ/5)` 属于预取等待。错误页的 `get_action` 和 120 f `sleep` 是提示。 |
| 接力面板，基准。[`handoff_sheet.c:96`](../../apps-ipod/screens/playback/handoff_sheet.c#L96)、[`handoff_sheet.c:225`](../../apps-ipod/screens/playback/handoff_sheet.c#L225)、[`handoff_sheet.c:253`](../../apps-ipod/screens/playback/handoff_sheet.c#L253)、[`wps.c:884`](../../apps-ipod/screens/playback/wps.c#L884) | 7.2 f 线性按钮焦点及准备态淡入。活跃轮询为 1.2 f，其他状态约 6 f。远程 WPS 用 12 条隔行带分四次揭幕，轮间睡 2.4 f。接力协议中的预同步及播放音量淡入另用业务时钟。 |
| 接力面板，FU83 只读快照。[`handoff_sheet.c:190`](../../../podbox.wt-presync/apps-ipod/screens/playback/handoff_sheet.c#L190)、[`handoff_sheet.c:232`](../../../podbox.wt-presync/apps-ipod/screens/playback/handoff_sheet.c#L232)、[`handoff_sheet.c:302`](../../../podbox.wt-presync/apps-ipod/screens/playback/handoff_sheet.c#L302) | 读取时文件对象为 `1cf3e8b0bdafe16f3d7c09283e15acd9bdec6c2a`。入场 12 f 立方减速，退场 9.6 f 立方加速。固定两张全屏位图，脏时重绘卡片，逐帧复制底部变化条带。入场按 3 tick 或剩余时间轮询 `get_action`，再调用 `advance`。稳定后，准备进度线性扫过底部两行。入场和退出临时升频。退出 313–319 行仍是睡眠循环。若转换到共享调度，系统事件和输入在退出期间也必须获得处理。 |
| WPS 动态标签和皮肤。[`skin_display.c:284`](../../apps-ipod/skin/skin_display.c#L284)、[`skin_display.c:390`](../../apps-ipod/skin/skin_display.c#L390)、[`skin_display.c:1121`](../../apps-ipod/skin/skin_display.c#L1121)、[`skin_render.c:310`](../../apps-ipod/skin/skin_render.c#L310)、[`skin_render.c:1366`](../../apps-ipod/skin/skin_render.c#L1366)、[`skin_tokens.c:607`](../../apps-ipod/skin/skin_tokens.c#L607) | 子行轮换、显示/隐藏期限和近期输入标签使用 tick。文本框按自身速度和像素步长。进度条跟随播放，峰值有保持与衰减，频谱由音频数值驱动，本地 `SPECTRUM_FPS` 为 10 Hz，远程为 25 Hz。按皮肤刷新类型重画并提交脏区。WPS 快轮询可执行 `sleep(0)` 让出至下一 tick。这些节拍由音频和皮肤状态决定。 |
| SBS、忙图标和换色。[`statusbar_skinned.c:230`](../../apps-ipod/skin/statusbar_skinned.c#L230)、[`statusbar_skinned.c:320`](../../apps-ipod/skin/statusbar_skinned.c#L320)、[`statusbar.c:176`](../../apps-ipod/skin/statusbar.c#L176)、[`cider_spinner.h:7`](../../apps-ipod/screens/playback/cider_spinner.h#L7)、[`skin_albumart_color.c:232`](../../apps-ipod/skin/skin_albumart_color.c#L232)、[`skin_albumart_color.c:2051`](../../apps-ipod/skin/skin_albumart_color.c#L2051) | SBS 节流后更新指定视口。忙图标、构建图标、文本框和旋转点用 tick 或自身相位。SBS 空闲忙图标源码注释约 7 fps。内建状态栏的充电与音量数字按约每 60 f 切换。封面取色一次切到最终配色，其后保留短期重绘期限以收敛各部件。 |
| 对话框、提示、确认、编辑。[`dialog.c:397`](../../apps-ipod/widgets/dialog.c#L397)、[`dialog.c:741`](../../apps-ipod/widgets/dialog.c#L741)、[`splash.c:213`](../../apps-ipod/widgets/splash.c#L213)、[`splash.c:234`](../../apps-ipod/widgets/splash.c#L234)、[`yesno.c:95`](../../apps-ipod/widgets/yesno.c#L95)、[`yesno.c:355`](../../apps-ipod/widgets/yesno.c#L355)、[`edit_line.c:101`](../../apps-ipod/widgets/edit_line.c#L101) | 普通对话框立即绘制，输入与脏区重绘由询问循环控制。编辑条件和光标由组件轮询，搜索有输入去抖。固定时长 splash 用 `sleep(ticks)`。加载进度按期限更新，确认倒计时按秒，结果文字停留 60 f。保留提示期限，移除必须响应系统事件的视觉睡眠。 |
| 播放记录卡片。[`card_row.c:37`](../../apps-ipod/draw/card_row.c#L37)、[`card_row.c:253`](../../apps-ipod/draw/card_row.c#L253)、[`pv_row.c:955`](../../apps-ipod/viewers/playback_viewer/pv_row.c#L955)、[`pv_row.c:1232`](../../apps-ipod/viewers/playback_viewer/pv_row.c#L1232)、[`pv_row.c:1298`](../../apps-ipod/viewers/playback_viewer/pv_row.c#L1298) | 卡片宽度 12 f 二次减速。位置每 0.6 f 追赶余距的 3/16。用 `USEC_TIMER` 算帧间隔，在约 2 f 帧预算内重画卡片条并尽量读一张图，剩余时间 `sleep` 或 `yield`。 |
| Agents。[`agents_screen.c:508`](../../apps-ipod/screens/system/agents_screen.c#L508)、[`agents_screen.c:669`](../../apps-ipod/screens/system/agents_screen.c#L669)、[`agents_screen.c:1383`](../../apps-ipod/screens/system/agents_screen.c#L1383)、[`agents_screen.c:2986`](../../apps-ipod/screens/system/agents_screen.c#L2986)、[`agents_screen.c:3294`](../../apps-ipod/screens/system/agents_screen.c#L3294) | 10 Hz 可见 dot。余弦表呼吸，故障按 36 f 亮、24 f 灭。动画重画单元格。任务文字另有推进期限。尾部睡眠属于发送/退出等待。迁移动画调度时保留 PodLink 重试和连接周期。 |
| 阅读、字幕和图片。[`text_reel.c:35`](../../apps-ipod/viewers/text_reel.c#L35)、[`text_reel.c:136`](../../apps-ipod/viewers/text_reel.c#L136)、[`text_viewer.c:2951`](../../apps-ipod/viewers/text_viewer/text_viewer.c#L2951)、[`text_viewer.c:3689`](../../apps-ipod/viewers/text_viewer/text_viewer.c#L3689)、[`image_viewer.c:214`](../../apps-ipod/viewers/image_viewer/image_viewer.c#L214)、[`image_viewer.c:474`](../../apps-ipod/viewers/image_viewer/image_viewer.c#L474)、[`image_viewer.c:583`](../../apps-ipod/viewers/image_viewer/image_viewer.c#L583)、[`image_viewer.c:1119`](../../apps-ipod/viewers/image_viewer/image_viewer.c#L1119) | Text Reel 每 1.2 f 推进一像素，交互可以暂停自动推进。阅读页有离散翻页、缩略图进度、语音及快速定位提示，条件触发重绘。图片平移/缩放按事件或解码完成，加载显示有旧睡眠。动图和幻灯片有媒体帧期限。阅读页 1340、1652、3298 行的睡眠属于提示。 |
| QuickScreen 与背光。[`quick_screen.c:481`](../../apps-ipod/screens/playback/quick_screen.c#L481)、[`quick_screen.c:533`](../../apps-ipod/screens/playback/quick_screen.c#L533)、[`backlight.c:221`](../../firmware/backlight.c#L221)、[`ipodvideo.h:131`](../../firmware/export/config/ipodvideo.h#L131) | QuickScreen 进入和退出立即呈现，名义每 12 f 轮询一次以更新状态，长标签复用文字滚动。5G 的 PWM 亮度渐变由背光驱动执行。屏幕唤醒和电源时序保有各自控制。 |
| 启动、扫描和诊断。[`main.c:403`](../../apps-ipod/main.c#L403)、[`main.c:432`](../../apps-ipod/main.c#L432)、[`sound_scan.c:429`](../../apps-ipod/screens/system/sound_scan.c#L429)、[`podlink_screen.c:207`](../../apps-ipod/screens/system/podlink_screen.c#L207)、[`probe_debug.c:61`](../../apps-ipod/screens/system/probe_debug.c#L61)、[`cider_wps.c:248`](../../apps-ipod/screens/playback/cider_wps.c#L248)、[`usb_screen.c:159`](../../apps-ipod/screens/system/usb_screen.c#L159) | 启动阶段按任务进度刷小条。扫描、调试、远程加载和 USB 有状态轮询或提示。启动睡眠、I/O `yield`、USB ACK、音频和后台线程等待属于任务进度或协议。盲目改成动画等待会改变终止和事件行为。 |
| 设置与接力的等待。[`general_settings.c:560`](../../apps-ipod/screens/settings/general_settings.c#L560)、[`handoff.c:242`](../../apps-ipod/screens/playback/handoff.c#L242) | USB DAC 设置等设备发现，每 6 f 唤醒。WPS 接力入口最多等待 60 f 协商，每 tick 检查一次。两处等待驱动协议检测和提示，保留各自的期限及取消条件。 |
| 游戏、插件与输出边界。[`quiz.c:831`](../../apps-ipod/games/quiz/quiz.c#L831)、[`arcade/vm.c:1251`](../../apps-ipod/games/arcade/vm.c#L1251)、[`spike_clock.c`](../../apps-ipod/games/spike/spike_clock.c)、[`lcd-s5l8702.c:409`](../../firmware/target/arm/s5l8702/lcd-s5l8702.c#L409) | Quiz 有答案揭晓睡眠。Arcade 和 Spike 有自己的帧时钟。外部插件、动图、视频保留内容节拍。LCD 驱动等待是传输同步。跨过这些边界时结束 UI 动画并交还 LCD。 |

调查用 `git grep` 与 `rg` 交叉查找 `current_tick`、`USEC_TIMER`、`sleep`、`get_action`、`lcd_update*`、插值、透明度、滚动、进度和忙状态。上述分类包括直接分派、线程级文字滚动、事件驱动刷新和非视觉等待。历史参考 [FU65 歌词](../../../ipod/FU65-REPORT.md) · [FU82 Cover Flow 数据](../../../ipod/evidence/fu82)。

## ARM 和 SDL 基准

- 从 `git archive f3165f2240` 建立隔离源码。以 `--target=ipod6g --type=n --appsdir=apps-ipod` 构建 6G ARM ELF。沿用 `tools/tests/arm_skin_stack.py` 的 Unicorn/ELF 执行方式，使用 ARM926 模型。执行生产 `memcpy`、`lcd_bitmap[_part]`、`lcd_update[_rect]` 和 LCD 命令及暂存复制。模拟寄存器就绪、上次 DMA 空闲、无锁竞争。用桩记录 DMA 队列入口收到的实际字节并逐字节比较。Cache 写回、DMA 描述符与物理传输、视频镜像、即将发生的总线竞争和调度不在指令计数内。
- 在隔离 SDL 镜像上执行相同操作。使用 Xvfb、SDL 软件渲染和 dummy audio。160 条合成文本，选项逐行推进。每种配置预热 24 次，记录 360 次。`gui_synclist_draw` 分开记录抑制 LCD 呈现和实际提交。行图和字形为热缓存，数据源在内存中。主机帧时长统计包括所调用的 SDL 呈现，短区域也走窗口渲染。主机结果覆盖当前列表实现与画面，物理 LCD 数据由 ARM 路径和条件模型补足。

| 操作 | ARM 指令，8 bit 命令集 | DMA 请求字节 | SDL p95 f，默认 / Scrim |
| --- | ---: | ---: | ---: |
| RGB565 320×240 `lcd_bitmap` | 27,696 | — | 0.0016 / 0.0016 |
| 320×240 快照 `memcpy` | 19,223 | — | 0.0010 / 0.0010 |
| `lcd_update()` | 19,532 | 153,600 | 0.1979 / 0.1945 |
| `lcd_update_rect(0, 0, 320, 240)` | 19,527 | 153,600 | 同一 SDL 路径 |
| 横条 320×24 | 2,247 | 15,360 | 0.2102 / 0.1988 |
| 横条 320×48 | 4,167 | 30,720 | 0.2121 / 0.1988 |
| 横条 320×160 | 13,127 | 102,400 | 0.2119 / 0.1912 |
| 竖条 24×240，x=13 | 20,704 | 11,520 | 0.2199 / 0.1967 |
| 竖条 48×240，x=13 | 25,024 | 23,040 | 0.2029 / 0.1925 |
| 两块页面合成后整屏提交，x=80/81 | 55,706 / 125,306 | 153,600 | 0.2069 / 0.2038 |
| 单次列表可见区重绘，抑制提交 | — | — | 0.0070 / 0.0094 |
| 单次列表可见区重绘，含提交 | — | 依视口与脏区 | 0.1937 / 0.1889 |

16 bit 命令集的各项提交少 35 条指令。两套命令集的 DMA 载荷一致。`lcd_update` 是满屏矩形的薄包装。6G 的 `lcd_update_rect` 先等待上次 DMA，把显示缓冲区复制到驱动暂存区，再排队 DMA。满宽条带是一段连续复制。窄条带逐行复制，奇数 x 会影响对齐。横移两张图在 x=81 的 ARM 指令约为 x=80 的 2.9 倍。新合成器可以将滑动边界量化到偶数像素，先验证尾帧和视觉效果。条带损伤优先用满宽横条，是否用窄竖条由行复制、总线与命令预算共同决定。 [ARM 数值和像素校验](../../../ipod/evidence/fu85/arm-display.json) · [SDL 数值](../../../ipod/evidence/fu85/sdl-measurements.json)。

### iPod 6G 的条件帧率

采用保守的串行预算。每帧时间近似为「ARM 指令数 × 假设每条 4 周期 ÷ CPU 频率 + 请求字节 ÷ 假设有效 DMA 带宽，所得秒数乘 60，再加 0.24 f」。0.24 f 是驱动未计部分、音频及调度的示例余量。该数和有效带宽均未在此任务实测。正常、最高频率分别是 54、216 MHz。32/16/8/4 MiB/s 可以用相同公式重算。以下为四舍五入结果，括号内是按该预算算的持续上限。

| 工作量 | 54 MHz、8 MiB/s | 216 MHz、8 MiB/s | 216 MHz、16 MiB/s | 54 MHz、4 MiB/s |
| --- | ---: | ---: | ---: | ---: |
| 整屏 push/pop，x=81，125,306 指令 | 1.90 f（32 fps） | 1.48 f（41 fps） | 0.93 f（65 fps） | 2.99 f（20 fps） |
| 预绘制列表移动，合成并提交 320×160，25,950 指令 | 1.09 f（55 fps） | 1.00 f（60 fps） | 0.64 f（94 fps） | 1.82 f（33 fps） |
| 预绘制选择条修复，合成并提交 320×48，8,030 指令 | 0.50 f（121 fps） | 0.47 f（128 fps） | 0.36 f（167 fps） | 0.71 f（84 fps） |

列表模型覆盖热缓存内的像素移动。边缘行栅格化、图像缺失、滚动条及额外脏矩形增加成本。SDL 实测的列表重绘仅用于确认实际光栅路径对比，不能乘频率比例换算成 ARM 时间。以最坏横移和 54 MHz 估算，30 fps 至少需要约 7.3 MiB/s 有效吞吐。216 MHz 下，60 fps 至少约 14.2 MiB/s。推导结果支持先验证 30 fps，并优先把小条带作为 60 fps 候选。即使计算预算够用，100 Hz 等待产生约 1.8/2.4 f 或 0.6/1.2 f 帧距。上一帧 DMA、扫描相位与撕裂、背光唤醒、缓存写回、DRAM 长尾、音频欠载、功耗和 USB/dock 并发必须用实机确认。

一次性原型基于同一 FU79 源码，在 SDL 的实际 Settings → Playback Settings 菜单上捕获两张真实画面。复用菜单动作循环，按 tick 合成。日志记录绘制提交，GIF 记录可见变化。单次进入提交 8 帧，首末间隔 13.67 f。返回提交 7 帧，首末间隔 10.89 f。两组以提交间隔计算约 30.7 和 33.1 fps。进入与返回各捕获 6 次可见变化。反向请求在第二次入场开始约 6.03 f 后到达，约 0.028 f 后开始计算新的合成帧。原型在反向时重新捕获混合画面，用于验证输入能够重选目标。生产实现保留同一对快照及当前位移，以便沿同一边界反向。 [进入](../../../ipod/evidence/fu85/push.gif) · [返回](../../../ipod/evidence/fu85/pop.gif) · [反向](../../../ipod/evidence/fu85/retarget.gif) · [时间记录](../../../ipod/evidence/fu85/slides.tsv)。

## 共享时钟、页面合成与所有权

首期使用 `animation.h` 管理帧坐标和定点立方缓动，使用 `scene.h` 管理页面合成。逻辑导航、主题、音频和系统事件由已有调用者派发。

```c
unsigned ui_clock_frame(uint32_t elapsed_clock);
uint32_t ui_frame_clock(unsigned frame);
int ui_clock_timeout(uint32_t elapsed, unsigned duration, unsigned ticks_per_second);
int ui_tween_sample(const struct ui_tween *tween, unsigned frame);

bool ui_scene_capture(struct ui_scene *scene, const void *from, const void *to,
                      bool push);
void ui_scene_start(struct ui_scene *scene);
void ui_scene_update(struct ui_scene *scene, bool target_changed);
int ui_scene_timeout(const struct ui_scene *scene, int timeout);
void ui_scene_cancel(struct ui_scene *scene);
```

`ui_clock_frame` 将无符号 `USEC_TIMER` 差值转换为 60 Hz 整数帧。晚帧直接采样当前位置。`ui_clock_timeout` 只在等待边界转换为内核 tick。调用者可以在帧间继续读取输入。`ui_scene_capture` 失败时，调用者用原绘制路径显示稳定目标。逻辑页维护其常规帧缓冲；过渡时场景从保留快照合成到独立输出。取消会归还 LCD 并提交逻辑页。后续原生控件可以使用同一时钟管理自身的可动画属性。

### 调度与输入

1. 从 `USEC_TIMER` 的无符号差值取 60 Hz 帧坐标。动作队列等待到下一帧边界，晚到时只绘制最新采样。
2. 进入为 12 f 立方减速，返回为 10 f 立方加速。全屏横向合成把像素边界对齐到 2 px。使用同一时间线测量帧间距和输入延迟。测试 32 位回绕、长时间挂起、整除与边界。
3. 在 `get_action` 和 `get_custom_action` 共享的等待边界只缩短超时，保留已有短超时、`TIMEOUT_NOBLOCK` 与无限等待语义。驱动、按键组合、音量、USB consent、handoff 和屏幕快捷键继续在原位置派发。同步弹窗或系统接管之前先取消动画并归还目标所有权。合成回调不调用 `get_action`。
4. `get_action` 返回后，页面先处理输入或系统事件，再在帧到期时绘制。把过期的零超时与有效按钮同时出现列入测试。没有待绘动画时，动画引擎保持原等待和 LCD 更新次数。旧状态栏、语音、频谱及传输唤醒依旧按各自契约。高频轮事件时合并显示工作，语义事件仍按顺序进入动作循环。
5. `root_menu.c:2178` 和主循环负责明确的向前、返回和重定向意图。保留 WPS、Cover Flow 与 Agents 现有分派。 `menu.c` 的 `mstack` 负责同一页面函数内的子菜单。只有双方支持抑制呈现、首帧完成且布局代际相同时开始动画。慢页先按现在的反馈机制显示加载态，目标未准备好时可以走原路径。清空或覆盖导航历史的命令、系统接管和跨不兼容所有者的动作直接切换。
6. 推入时新页从右侧覆盖旧页，返回时顶页向右滑出。同一页对反向共用快照，从现有 x 坐标重新定向。新去向必须以同一个正在显示的合成画面为起点。有新目标可绘制时，可以复用两张位图的旧内容位置。目标准备、分配或所有权发生变化时立即切换。选择、返回和轮输入立即作用于逻辑页，动画只改变显示。
7. 面板使用方角。入场和退场分别为 12 f 和 10 f。数据或焦点变化只重光栅化面板脏区。退出时恢复被遮挡的真实页面。冻结的底图代际落后时先恢复/重绘可靠目标，再交还显示。SELECT、MENU、主机丢失和 dock 断开从第一帧走正常事件路径。

### 内存与呈现

- 320×240 RGB565 每张 153,600 B。两张 307,200 B，使用单句柄的 ARM 账面约 307,220 B，另计对齐与调度元数据。一个 320×160 条带为 102,400 B。两张 320×24 的行图合计 30,720 B。面板可以按实际高度申请前景及底下受影响区域。覆盖全屏的 FU83 面板使用两张全屏。
- 菜单合成使用一张独立输出图。6G 的 `lcd_present_scene` 将该图复制到驱动自有的 `lcd_dblbuf` 后提交 DMA。忙时放弃中间帧，结束帧重试。接力面板输出到现有 framebuffer，用带损伤范围的 `lcd_update_rect` 提交。释放前检查驱动合约。未来若直接让 DMA 读取动画缓冲，必须保留 pin 至中断确认。像素表面和大工具表全部来自音频池 `core_alloc`。
- 用句柄表示所有权。每次绘制用 `core_get_data_pinned` 取地址及配对 `core_put_data_pinned`。任何会等待、让出或触发分配的调用都按 buflib 的 pin 规则执行。常驻指针不得指向可移动的分配。使用 `core_alloc_free_space`，仅申请现有空闲块。取消先撤回回调、释放 pin，再 `core_free`。裸 `core_get_data` 只在已证明不会切线程的范围使用。 [core 初始化与分配](../../firmware/core_alloc.c#L60) · [buflib pin 合同](../../firmware/include/buflib.h#L253)。
- 最终 FU79 ELF 的静态预算余量是 **32,160 B**。FU85 的独立 ARM 构建总分配 6,230,492 B。按同一上限 6,262,660 B 计算，余量为 32,168 B。两者是不同的 ELF，发布预算按交付基线和每阶段实际 map 复核。FU79 的封面暂存已从静态内存移至音频池，32,768 B 载荷加 20 B 块和句柄开销。 [交付 ELF 账目](../../../ipod/FU79-REPORT.md)。
- `audio_reset_buffer` 捕获近乎全部 core 池，基准已用 `core_shrink` 为后台任务及 FU79 封面留空。运行中额外收缩可使音频重缓冲。只从确实可用的连续空间申请。`core_allocatable` 会尝试移动和压紧，按当前连续可用空间估计容量。后续 `core_alloc` 在无槽位或找不到块时可以触发其他块的 shrink 回调。两个调用之间存在竞争。首期用 `core_alloc_free_space` 在一次调用中申请空闲块。失败时显示稳态图。打开动画后，下次正常重建音频池时为菜单和面板留出两者较大值。阶段 1 验证有无播放、音频池重建、内存不足、与 FU79 工作区同时活动。预留意味着从文件缓冲扣除可用容量，避免音频不能维持的永久默认配额。 [播放池分配](../../apps-ipod/audio/playback.c#L1150) · [buflib 申请](../../firmware/buflib_mempool.c#L570) · [buflib 可分配空间](../../firmware/buflib_mempool.c#L781)。
- WPS/SBS、文字滚动及特殊页各自具有绘制入口。仅抑制 flush 会留下对同一 framebuffer 的写入。因此捕获和过渡必须同时定义绘制所有权，或者让后台页绘制到自己的位图。冻结渲染时继续计算皮肤失效和业务状态。释放时重做整页主题绘制，再重新启用跑马灯。固件驱动、panic、USB 接管和插件都可以撤销视觉所有权。屏幕休眠时终止运动，唤醒时立即绘逻辑目标。

### 列表

`list_do_action` 和 `gui_synclist_do_button` 每次继续更新 `selected_item`、`start_item`、边界、网格语义、声音与导航。只有列表参与连续动画时，选择变化委托给原始逻辑之后的绘制适配器。`gui_synclist_layout` 在布局代际变化时运行。`list_render.c` 与 `list_skinned.c` 共用滚动适配器，分别光栅化普通行和皮肤行。第一版支持行高均匀且可截取的列表。网格按整行，其他自定义行继续使用原显示路径。

- 视觉状态保留当前像素偏移、最新目标、轮事件时间和可见行缓存。轮子快速连续输入使逻辑选择立即跳到目标。按最新时间与步进估速度。距离超过缓存邻域时跳到目标附近再滑动最后一小段。反向从当前偏移重定向。禁止把每一格做成动画队列。
- 松手时用最后已选的行作为对齐位置。可先短时速度衰减，但不得越过未选择的条目，且下一次按键即刻覆盖该状态。初版可采用有界追赶或立方减速。选择条独立插值，滚动条位置跟随可见偏移并夹在范围内。按下 SELECT 时以逻辑项为准。对偏移产生歧义的快照立即切至逻辑位置。
- 渲染顺序为绘干净行及前后缓存、合成内容移动、绘选择条、提交列表视口脏区。视口边缘只补新进入的一行或必要条带，渲染选择依赖的字体和皮肤属性。缓存键包括行身份、源内容、选中样式、皮肤、字体和视口代际。禁用迁移中的列表文字跑马灯可消除竞争。停止后从原有策略恢复。缓存缺失时用同步稳态绘制，磁盘与网络补图继续由现有后台机制驱动。
- `gui_synclist_forget` 和页面退出在栈对象失效前取消动画。原有 list/SBS 脏区管理必须按阶段保留。`gui_synclist_do_button` 在菜单中也由 `do_menu` 直接调用。验证两条入口。对 5G 另检窄条对齐、较低频率、32 MiB 设备和低余量降级。

### 开关与发布

设置项使用现有设置存储和语言机制。建议配置键 `animations`，中文显示 **动画**，可选开/关。首批发布默认关。关闭后使用原路径。原有歌词、Cover Flow、交接面板、媒体帧和文字滚动设置继续工作。每期发布说明开关当期覆盖的页面。已有歌词、Cover Flow 专用效果选项如果承担页面行为，迁移时保持兼容。保留原始键位、语言和配置文件读取路径。

| 阶段 | 可独立发布的范围 | 回退与检查 |
| --- | --- | --- |
| 1 | 引擎、FU83 接力面板、选择性的菜单 push/pop。 | 开关默认关，编译时可选原绘制路径。开关关时运行原路径的回归。USB、dock、主题切换、休眠唤醒、RAM、静音及 30 fps 实机门槛逐项验收。 |
| 2 | 符合条件的普通、皮肤列表。 | 保留原列表渲染及事件语义作为回退。先验证纯文本和主题列表，再验证有图列表与网格。实机验收长转、反转、滚动条、I/O 压力和音频。 |
| 3 | 分批迁移歌词、Cover Flow、卡片行、Agents、主题装饰和过渡。 | 歌词、Cover Flow 保留各自绘制器。动画调度接入一页验收一页。文字跑马灯、真实内容时钟、皮肤刷新、媒体、插件和协议仍保留各自契约。 |

主分支每个里程碑都保持旧启动路径。合入可单独回滚的小步。发布演练必须在 30 分钟内切回旧路径并产生可发布的已知良好构建。现场可以即时关动画。构建失败时用旧发布源和旧配置恢复，构建开关必须在每阶段有测试。B 若启动，也必须用并行的老/新后端切换和相同的 30 分钟门槛。阶段 0 只交付文档和证据。

## 测试与设备验收

- 确定性时钟测试用注入的内核 tick 和 `USEC_TIMER` 值。覆盖 1.8/2.4 f 与 0.6/1.2 f 的内核等待间距、跳帧、关闭时零额外唤醒、插值两端、奇像素对齐、坐标裁剪、32 位回绕、重复转向、速度保持及快轮跳行。验证到期后只用当前时间一次，精确落在终点。
- 注入内存不足、碎片、handle 移动、取消、滚动中的栈对象退出。每个分支检查 pin/handle 平衡。验证回调重入、`skin_hold_scrolling` 与 flush 计数、画面所有者代际、USB 连接前后和 backing store 释放顺序。用 ARM map 比较分配段及栈峰值。
- SDL 帧序列比较入口、退出、反向、快速三级导航、首帧 SELECT、连续 wheel、主题切换、WPS ↔ 歌词、远程快切、Cover Flow 专用返回、模拟 USB 和 dock 中断。按帧比较位置方向、单合成提交、脏区字节、最终 RGB565 像素、旧页面串帧和卡住的焦点。保持模拟 peer、临时存储及私有运行目录。
- 后续获准的实机阶段在 6G 两类命令集和 5G 记录整屏 LCD 申请、上次 DMA 等待、CPU 合成、DMA 完成、实际扫描、输入到可见帧的 p50/p95/最大值、掉帧、音频 PCM 空洞、USB ACK 延迟和功耗。覆盖高采样率 ALAC、低内存、复合视频、背光唤醒、USB storage/DAC 的角色切换及 dock/HerdRemote 突发。30 fps 是目标门槛。统一时间线固定为 60 Hz。可见帧率依实际提交能力确定。

## 原生界面迁移与后续动效

用户规定的顺序是启动字幕修复、原生界面迁移、Motion v2。迁移期保留已有动效路径。已通过像素对照的原生页面由 `native ui` 开关选用。未覆盖或不符合支持条件的页面继续交给旧绘制器。移除皮肤解析器与主题资源的前提是所有支持页面、用户设置及系统接管路径都拥有原生实现。

### 保留式视图、布局和合成

`apps-ipod/ui/retained.h` 的视图拥有父子关系、绘制回调、布局回调及焦点输入。几何相对父视图定义。合成遍历把几何转换到屏幕坐标，并合并父子裁剪。伤害范围取已改变节点新旧覆盖区域的并集。页面只绘制与伤害范围相交的节点，每次提交一个合并后的 LCD 矩形。拥有页面的调用者仍使用旧的逻辑数据、动作映射、播放和 USB 状态机。

可以直接绘制稳定控件，也可以使用 RGB565 图层节点。图层从 `core_alloc_free_space` 获取。图层拥有者必须在视图离树后释放句柄。绘制时固定句柄，绘制结束立即解除固定。普通不透明原尺寸图层按行拷贝。移动或裁剪时按照屏幕区域合成，缩放、透明度和亮度共享该路径。没有空闲块时使用仅直接绘制的稳定端点或旧页面。初期不新增常驻像素缓冲。

导航栈显式记录整页、面板和弹窗。整页覆盖下层页面，叠加页面使用当前可见下层。入栈和退栈通知合成器，此时两个页面的拥有者都有效。后续过渡可以在该通知里接管图层并安排回收，保留旧逻辑分派和系统事件优先级。输入从顶部页面的焦点开始，未消费时沿父视图回传。

### 为 Motion v2 保留的接口

位置、偏移、宽高、透明度、亮度、裁剪、滚动偏移和选择位置都使用同一种属性和曲线接口。每条活动轨道由页面控制器持有。采样使用共享 60 Hz 时间线，重新定向先计算当前属性值。晚帧只呈现当前采样。页面退出前取消轨道，并把未完成的输入交给当前拥有者。滚动视图的逐帧回调可以提供惯性、限幅和弹回。现阶段该回调与导航通知定义了接入点。Motion v2 的参数和覆盖在旧主题引擎退役后接入。
