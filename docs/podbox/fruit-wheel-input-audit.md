# Fruit 按键审计

审计基线为 `a0eaad50a5`。下表的行号以该提交为准。启用滚轮导航的
主题属性是 `style.menu_tree=stock`。Fruit Company 和 Fruit Company Ultra
都使用该属性。独立主题文件也可以指定它。HUD 的滚轮规则同时读取
`style.quick_screen=hud`。当固件未内置 Ultra 时，可以用指定上述两个属性的
独立主题验证同一条 HUD 输入路径。

物理 LEFT、RIGHT 和 PLAY 在公共动作表、自定义动作表和直接读键路径
之前进入播放控制。用户松开短按的 LEFT 或 RIGHT 时，播放器切换曲目。
超过曲目开头的重播阈值时，短按 LEFT 回到曲目开头。长按 LEFT 或 RIGHT
累加进度偏移，松开时提交。短按 PLAY 切换播放和暂停。存在内存中的队列时，
短按 PLAY 可以恢复队列。播放器保留设备驱动的长按关机路径。

| 界面 | 基线映射位置 | Fruit 操作 |
|---|---|---|
| 主菜单、列表、文件及音乐浏览 | `apps-ipod/input/keymap_ipod.c:53`、`:156`、`:169`。LEFT 返回或分页。RIGHT 进入或分页。PLAY 打开 Now Playing。SELECT+PLAY 触发快捷操作。调用方包括 `apps-ipod/widgets/menu.c:1204`、`:1386`、`:1431` 和 `apps-ipod/widgets/list.c:1115`。 | 滚轮移动。SELECT 打开项目。MENU 返回。 |
| Fruit 菜单 | `apps-ipod/screens/fruit_menu.c:469` 用标准确认动作切换设置。`:775` 控制 Now Playing 行。`:806` 提供主菜单。 | 继承公共映射。当前播放可从主菜单的 Now Playing 行打开。 |
| 多选列表 | `apps-ipod/input/keymap_ipod.c:83`。SELECT 切换标记。PLAY 打开多选操作菜单。 | SELECT 切换标记。长按 MENU 打开多选操作菜单。 |
| 设置及选项列表 | `apps-ipod/input/keymap_ipod.c:202` 使用 LEFT/RIGHT 移动设置焦点。`apps-ipod/widgets/option_select.c:574`、`:598` 接收返回和确认动作。 | 滚轮调整。SELECT 确认。MENU 返回。 |
| 日期、时间和颜色编辑 | `apps-ipod/input/keymap_ipod.c:248`。LEFT/RIGHT 切换列。PLAY 切换行。`apps-ipod/screens/system/time_set.c:350`、`apps-ipod/widgets/color_picker.c:516` 消费这些动作。 | 日期和时间用滚轮改值，SELECT 进入下一字段。颜色编辑用 SELECT 切换滚轮的作用。选中确认按钮后按 SELECT 保存。长按 SELECT 可以保存。MENU 取消。 |
| EQ 编辑 | `apps-ipod/input/keymap_ipod.c:220`、`apps-ipod/screens/settings/eq_editor.c:1247`。LEFT/RIGHT 选频段。长按 PLAY 旁路 EQ。 | 长按 SELECT 在选频段和改数值之间切换。短按 SELECT 保留参数切换。 |
| 文本输入 | `apps-ipod/input/keymap_ipod.c:297`、`apps-ipod/widgets/keyboard.c:160`。LEFT/RIGHT 移动或删除。PLAY 把焦点移到操作按钮。 | 滚轮选择字符。SELECT 提交当前字符。长按 SELECT 进入确认按钮行。长按 MENU 删除字符。 |
| Yes/No 对话框和交接页 | `apps-ipod/input/keymap_ipod.c:263`、`apps-ipod/widgets/yesno.c:255`。LEFT/RIGHT 改选项。PLAY 取消。`apps-ipod/screens/playback/handoff_sheet.c:249` 也读取 LEFT/RIGHT。 | 滚轮选项。SELECT 确认。MENU 返回。 |
| USB 连接确认 | 短按 SELECT 连接。MENU 仅充电。 | PLAY 切换播放。 |
| Control Centre | `apps-ipod/input/keymap_ipod.c:279`、`apps-ipod/screens/playback/quick_screen.c`。非导航页的全屏控制页：左栏为随机播放和重复播放，右栏为一项通高的亮度。 | 滚轮直接调节音量，并通过状态栏音量覆盖层显示。LEFT 切换随机播放，RIGHT 切换重复播放，MENU 调高亮度，PLAY 调低亮度。短按 SELECT 关闭且无闪烁，长按 SELECT 打开 Shortcuts。 |
| Cover Flow 和专辑面板 | `apps-ipod/screens/covers/carousel.c:158`、`:185`、`:188`、`:8687`、`:8727`、`:9333`、`:9477`。LEFT/RIGHT 跳页。长按 PLAY 随机转动。PLAY 可以从面板进入 Now Playing 或播放所选专辑。SELECT+PLAY 切换排序。 | 滚轮移动封面或歌曲焦点。SELECT 打开专辑或播放所选歌曲。MENU 返回。 |
| Now Playing 及歌词 | `apps-ipod/input/keymap_ipod.c:181`、`apps-ipod/screens/playback/wps.c:896`、`:947`。LEFT/RIGHT 切歌和快进快退。PLAY 切换播放。长按 PLAY 打开菜单。SELECT+PLAY 触发快捷操作或 A/B 标记。歌词继承 WPS 播放动作。 | 播放动作保持可用。长按 SELECT 打开播放菜单。短按 SELECT 打开歌词或执行歌词操作。 |
| 全局按键组合 | `apps-ipod/input/agents_shortcut.c:65`、`apps-ipod/input/action.c:89`、`:535`、`:650`。SELECT+PLAY 进入 Agents 或 Now Playing。 | SELECT+MENU 的 Cover Flow 组合仍可使用。 |
| Agents | `apps-ipod/input/keymap_ipod.c:147`、`apps-ipod/screens/system/agents_screen.c:2920`、`:3502`、`:3659`、`:3695`。LEFT/RIGHT 切换页面或标签，触发终端退格和问答。PLAY 控制终端语音。 | 滚轮移动焦点或阅读终端。长按 SELECT 打开 Usage、切换标签、打开 T3 操作或发出退格。二选一用滚轮选答案，SELECT 提交。配件仍可以发出语音动作。 |
| Clocks | `apps-ipod/screens/extras/extras_clock.c:915`、`:974`。对话框继承 LEFT/RIGHT 选择和 PLAY 取消。时钟列表继承 PLAY 打开 Now Playing。 | 用滚轮选时钟和按钮。SELECT 确认。MENU 返回。地区、城市和时钟设置沿用滚轮选择。 |
| Calendars、Contacts、Notes、Alarms | `apps-ipod/screens/extras/extras_calendars.c:516`、`:593`。LEFT/RIGHT 切月或切日。Contacts 的 `extras_contacts.c:859`、`:913`、Notes 的 `extras_notes.c:237` 和 Alarms 的 `extras_alarms.c:377`、`:708` 使用继承的标准动作。 | 日历用滚轮切日。月视图按住 SELECT 转轮可以切月。联系人和笔记用滚轮滚动，SELECT 打开。闹钟用滚轮调整字段，SELECT 前进。 |
| Screen Lock 和 Stopwatch | `apps-ipod/screens/extras/extras_screen_lock.c:426`、`:530`。LEFT/RIGHT 切换数字位或选项。`apps-ipod/screens/extras/extras_stopwatch.c:368` 用 PLAY 开始或停止。 | 锁屏用滚轮调数字或选 Lock/Reset。SELECT 前进。秒表短按 SELECT 开始或计圈。长按 SELECT 停止。 |
| 照片列表和查看器 | `apps-ipod/screens/fruit_menu.c:306` 进入照片列表。`apps-ipod/viewers/image_viewer/image_viewer_button.h:36` 和 `image_viewer.c:694` 将 LEFT/RIGHT 用于换照片和水平平移。 | 滚轮换照片。平移时短按 SELECT 切换滚轮的平移轴。长按 SELECT 切换平移模式。 |
| 文本查看器 | `apps-ipod/viewers/text_viewer/text_viewer.c:3039`、`:3059`、`:3102`、`:3717`、`:3794`。LEFT/RIGHT 翻页或跳章。横排页把 PLAY、LEFT、RIGHT 当作方向键。竖排页双按 PLAY 改方向。 | 滚轮翻页或按偏好跳到百分比。SELECT 前进或切换朗读。长按 SELECT 打开笔记。MENU 返回。长按 MENU 打开设置。横排沿用这些手势。 |
| Playback Report | `apps-ipod/viewers/playback_viewer/pv_row.c:1060`、`:1084`。LEFT/RIGHT 换栏目。PLAY 播放卡片。 | 将滚轮转到栏目边缘可以换栏目。长按 SELECT 播放卡片。 |
| 录音页和调试波形 | `apps-ipod/input/keymap_ipod.c:33` 和 `apps-ipod/screens/recording.c:1465`。LEFT/RIGHT 改电平，PLAY 控制录音或取消。`apps-ipod/audio/peak_meter.c:1085` 在调试波形中用 PLAY 清直方图。 | 录音用 SELECT 控制开始或暂停。长按 SELECT 新建录音段。按住 SELECT 转轮调电平。MENU 返回或停止。长按 MENU 打开录音设置。 |
| Videos | `apps-ipod/screens/fruit_menu.c:290` 和 `apps-ipod/screens/browse/browser_flat.c:150` 用列表映射。`apps-ipod/files/filetypes.c:356` 启动 H.264 播放器。`apps-ipod/plugins/stage-h264.py:62` 对接播放动作。 | 视频目录使用滚轮、SELECT 和 MENU。视频播放用 SELECT 切换进度条模式，滚轮跳转进度。LEFT/RIGHT 保留播放定位，PLAY 暂停或继续。MENU 离开视频。 |

Classic 和 Scrim 的菜单树仍使用方向键导航。Spike、Music Quiz、Arcade
和兼容插件保留自己的按键。配件输入保留原映射。连接电脑时，USB HID 继续
控制电脑，见 `apps-ipod/input/keymap_ipod.c:319` 至 `:396`。录音占用音频
设备时，播放键不会启动音乐。Fruit 的 Cover Flow、
Agents 和 EQ 恢复 PLAY 长按关机路径。PLAY 长按期间没有应用层菜单、语音或 EQ 动作。
设备驱动的关机条件见 `firmware/drivers/button.c:339` 及
`firmware/target/arm/s5l8702/ipod6g/button-target.h:78`。

## 原厂 2.0.5 对照

- 已验证。无窗口 QEMU 使用 2.0.5 内核、测试磁盘副本、`-icount shift=3,align=off,sleep=on` 和指定的 loader 注入。WHEELSCRIPT 记录物理按键，LCDCAP 记录画面。
- 已验证。Clocks 中，LEFT 和 RIGHT 的短按保持当前时钟。SELECT 打开 Add/Edit。滚轮把焦点从 Edit 移到 Add。SELECT 进入 Region 和 City。滚轮分别改变地区和城市焦点。记录在本机探针的 `clock-or-calendar-*.png` 和 `probe2-clock-*.png`。
- 部分完成。带三张测试图和一段测试视频的磁盘副本显示了 All Photos 的图片预览和 Movies 的片名。模拟器未得到可判读的全屏幻灯片或视频画面。记录在本机探针的 `media-*.png`、`media-input.csv` 和 `media-frames/frames.csv`。
- Apple 的手册描述了滚轮浏览照片，LEFT/RIGHT 翻照片页。播放幻灯片时，LEFT/RIGHT 切照片。Fruit 的普通照片浏览用滚轮。现有幻灯片菜单仍可用。[iPod classic User Guide，第 37–38 页](https://cdsassets.apple.com/live/6GJYWVAV/user/ma1195_ipod_classic_160gb_user_guide.pdf#page=37)
- Apple 的手册描述了视频的两种进度操作。SELECT 打开进度条后，滚轮可以定位。长按 LEFT/RIGHT 也可以快退或快进。Fruit 的 H.264 播放器用 SELECT 切换进度条模式。转轮时沿用播放定位的步长。[iPod classic User Guide，第 5 页](https://cdsassets.apple.com/live/6GJYWVAV/user/ma1195_ipod_classic_160gb_user_guide.pdf#page=5)
- Apple 的手册描述了日期、时间和锁屏码的字段操作。滚轮改当前值，SELECT 前进。原厂锁屏码也接受 LEFT/RIGHT 移位。Fruit 用滚轮和 SELECT 输入。[iPod classic User Guide，第 41、43 页](https://cdsassets.apple.com/live/6GJYWVAV/user/ma1195_ipod_classic_160gb_user_guide.pdf#page=41)
- 未验证。此轮原厂模拟没有进入游戏验证按键。AstraPod 的内置游戏和兼容插件仍由各自处理输入。
