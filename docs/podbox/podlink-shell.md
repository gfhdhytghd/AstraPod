# PodLink Shell

iPod classic 固件默认启用 PodLink Shell。它使用 PodLink 非磁盘连接上的 USB 控制端点。主机需要 PyUSB，且必须指定 iPod 的 USB 序列号。

在 [AstraPod-Desktop](https://github.com/gfhdhytghd/AstraPod-Desktop) 仓库运行以下命令。

```sh
python3 podlink_shell.py --serial USB_SERIAL
python3 podlink.py reboot --serial USB_SERIAL
python3 podlink_shell.py --serial USB_SERIAL debug-list
python3 podlink_shell.py --serial USB_SERIAL debug "Hardware info"
python3 podlink_shell.py --serial USB_SERIAL open-bench
```

- `reboot` 执行正常关机和重启。脚本等待 USB 重新枚举。
- `debug-list` 列出当前固件实际提供的 Debug 菜单项目。`debug` 按名称在机身打开该项目，沿用机身操作和确认流程。需要交互的页面在 iPod 上操作及退出。
- `open-bench` 打开「System → Debug → PodLink」。播放器必须处于 PodLink USB 模式，且前台没有其他 PodLink 客户端。打开后可以借用串口运行 `podlink.py bench`。
- `status` 显示开机时钟、已提交及已处理的命令数，以及界面开始处理最后一条命令时的时钟。
- `play` 用 `/.rockbox/sm1-test/irq-dma44.flac` 建立单曲播放列表。测试文件由测试者放入设备。执行前备份当前播放列表。
- `seek` 将当前曲目定位到 6.237 秒。`stop` 停止播放。

一次只能执行一条命令。按键队列正忙时会拒绝提交进入该队列的命令。
`record-stop` 直接交给录音器。录音器关闭文件并更新录音状态后确认完成。
其他命令的计数表示界面处理进度。PCM 等测试结果由相应诊断功能提供。

## 播放诊断

```sh
python3 podlink_shell.py --serial USB_SERIAL play "/iPod_Control/Music/F09/libgpod243817.m4a"
python3 podlink_shell.py --serial USB_SERIAL play --start-ms 1000 "/Music/Example.m4a"
python3 podlink_shell.py --serial USB_SERIAL --json playback-status
python3 podlink_shell.py --serial USB_SERIAL open-lyrics
python3 podlink_shell.py --serial USB_SERIAL seek 6237
python3 podlink_shell.py --serial USB_SERIAL next
python3 podlink_shell.py --serial USB_SERIAL prev
python3 podlink_shell.py --serial USB_SERIAL log-fetch /.rockbox/crumbs-last.txt ./crumbs-last.txt
```

- `play PATH` 用设备上的绝对路径建立单曲播放列表。执行前备份当前播放列表。
  `--start-ms` 指定起始位置，单位为毫秒。省略路径时保留原有测试曲目命令。
- `seek MS` 定位当前曲目，单位为毫秒。位置必须在曲目时长内。省略参数时保留原有固定位置命令。
- `next` 和 `prev` 使用当前播放列表的跳曲规则。用多曲播放列表检查跳曲。
- `open-lyrics` 打开 Now Playing 的歌词页。页面保持打开时可以发送播放命令。
  使用 `screenshot` 确认设备画面。要复现歌词行选择，用机身转盘或遥控滚动和 Select。
- `playback-status` 显示播放状态、路径、进度、时长、编码、采样率、源位深和错误。
  未知位深显示为 0。`command_error` 是最后一条带参数播放命令的错误号，
  `playback_error` 是音频引擎的错误。命令返回 `processed` 表示界面收到请求。
  用 `playback-status` 观察播放或 seek 的实际结果。
- `log-fetch PATH OUTPUT` 用 MS3 借用 HerdRemote 串口并校验长度和 CRC32。
  可读取 `/.rockbox/crumbs-last.txt`、`/.rockbox/lyrics-perf.txt`、
  `/.rockbox/lyrics-perf.1.txt`、`/.rockbox/sm1/codec-trace.txt`
  和 `/.rockbox/logs/` 下的调试日志，例如 `tasks.log`。
  其他路径返回 `MS3 ERR sample path`。
  崩溃后，固件在重启时保存 `crumbs-last.txt`。先下载该文件，再重现下一次崩溃。

分析 `crumbs-last.txt` 时，先核对构建版本和崩溃时钟。用 `mp4-moov` 和
`mp4-mdat` 查看容器偏移。用 `seek`、`seek-now` 和 `seek-done`
对齐定位过程。状态行的 `seek-step`、`codec-op`、`buf-op`、
`buf-fill` 和 `lyrics` 记录各路径的最后阶段。`freeze-ctx` 的高位
记录看门狗类型，低 8 位记录中断前的 CPSR 标志。

播放控制使用请求 `0x5d`、索引 `0x4954`。OUT 值 1 为播放，
负载是 32 位起始毫秒值和 2 至 259 字节 UTF-8 路径。OUT 值 2 为定位，
负载是 32 位毫秒值。OUT 值 3、4、5 分别是下一曲、上一曲、打开歌词页，
负载为空。IN 值 0 返回 `<IIIi6Ii24s260s>`，各字段依次为版本 1、
完成计数、操作号、命令错误号、播放标志、进度毫秒、时长毫秒、编码 ID、
采样率、位深、播放错误号、编码标签、路径。整数均为小端。两个字符串以 NUL 结尾。

## LCD 实验

`lcd-pacing` 适用于任何 LCD。其他 LCD 命令只适用于 LCD type 1。
命令不打开调试菜单。设置只在本次开机期间有效。

```sh
python3 podlink_shell.py --serial USB_SERIAL lcd-timing
python3 podlink_shell.py --serial USB_SERIAL lcd-te-sync get
python3 podlink_shell.py --serial USB_SERIAL lcd-async get
python3 podlink_shell.py --serial USB_SERIAL lcd-async off
python3 podlink_shell.py --serial USB_SERIAL lcd-async on
python3 podlink_shell.py --serial USB_SERIAL lcd-pacing off
python3 podlink_shell.py --serial USB_SERIAL lcd-pacing status
python3 podlink_shell.py --serial USB_SERIAL lcd-diag
python3 podlink_shell.py --serial USB_SERIAL log-fetch /.rockbox/lcd-te-diag.txt ./lcd-te-diag.txt
python3 podlink_shell.py --serial USB_SERIAL ui-perf ./ui-perf.txt
```

- `lcd-timing` 读取固定的 `LCD_PHTIME` 值 `0x33`。
- `lcd-te-sync` 和 `lcd-pacing` 都接受 `on`、`off` 和 `get`。
  TE 同步默认开启。信号连续丢失时，驱动记录无 TE 定时的启动。
- `lcd-async on|off|get` 切换或查询 LCD type 1 的异步提交。默认开启。
  对比两种提交方式时，保持 `lcd-te-sync on`，执行相同的滑动，再分别下载日志。
- `lcd-pacing` 选择界面动画的时钟，默认开启。开启时，动画帧跟随面板刷新，
  约 66.96 Hz。一帧在一次刷新内画不完时，该动画改为每两次刷新显示一帧。
  关闭时，动画按 60 fps 运行。机身的「Debug → UI TE pacing」是同一开关。
- `lcd-pacing status` 显示帧周期，单位为微秒和 Hz。周期来源为
  `measured TE`（实测）、`nominal TE`（未测得 TE 时的标称值）或
  `authored 60 fps`（关闭时）。`TE phase` 表示是否已知刷新相位。
  其余各行描述最近一次动画：`full` 或 `half` 帧率、已显示帧数、
  错过的刷新数、TE 后的唤醒偏移、每帧平均耗时和最长 DMA 耗时。
  开机后尚无动画时显示 `last animation: none`。
  要比较两种设置，切换后在机身滑动一次列表或 Cover Flow，再读取状态。
- `lcd-diag` 等待 `/.rockbox/lcd-te-diag.txt` 写入成功。`lcd-diag survey`
  另外用一秒采样全部 GPIO 和 LCD 信号，这一秒内机身画面停止更新。
  机身预览不做这项采样。报告开头是
  `lcd-pacing status` 的内容。其后是 `LCD_PHTIME`、DMA 次数与耗时，
  以及排队、推迟和无 TE 定时启动的计数。运行前保持 LCD 唤醒。
  `debug "LCD TE diagnostics"` 仍打开机身预览，命令计数在退出预览时更新。
- `log-fetch` 下载本次诊断报告。`ui-perf OUTPUT` 下载已有的
  `/.rockbox/ui-perf.txt`。要启用帧耗时采集，开机前放置
  `/.rockbox/ui-perf.enable`。两种下载都使用 MS3。
  异步模式下，`submit_f` 记录复制耗时。`submit_overhead_f` 记录其余提交耗时。
  `last_dma_start_phase_us` 记录采样时最近一次 DMA 相对其调度 TE 边沿的启动相位。

LCD 控制使用 EP0 请求 `0x5d`、索引 `0x4955`。OUT 值 1、2、3、4、10、11
对应时序、TE 同步、动画节奏开关、诊断、动画节奏状态和异步提交开关。值 5 至 9
保留给 LCD 吞吐实验，本固件拒绝这些值。负载为小端 `<IB>`，
包含非零请求标识和一个参数字节。时序参数 0 表示读取。三个开关的参数
0、1、2 分别表示关闭、启用、读取。诊断参数 0 跳过信号采样，1 加入采样。
状态的参数选择一个字段：0 周期（µs），1 帧率除数（1 全速，2 半速，
0 尚无动画），2 已显示帧数，3 错过的刷新数，4 唤醒偏移（µs），
5 每帧平均耗时（µs），6 最长 DMA 耗时（µs），7 标志。标志位 0 表示开启，
位 1 表示周期为实测值，位 2 表示刷新相位已知。每次请求只返回一个字段。
IN 值 0 返回小端 `<4I>`，依次为版本 1、完成请求标识、固件错误号和结果。
错误号 0 表示成功。时序结果为当前值，开关结果为 0 或 1。
诊断在关闭临时文件并替换报告后才发布成功结果。

## 设置

保持 iPod classic 处于 PodLink USB 模式。HerdRemote 可以继续使用串口。

```sh
python3 podlink_shell.py --serial USB_SERIAL settings-tree
python3 podlink_shell.py --serial USB_SERIAL --json settings-tree
python3 podlink_shell.py --serial USB_SERIAL settings-get "settings mode"
python3 podlink_shell.py --serial USB_SERIAL settings-set "settings mode" everything
python3 podlink_shell.py --serial USB_SERIAL settings-action "Update Database" --yes
```

- `settings-tree` 列出菜单、子菜单、设置和动作。设置行包含配置名、当前值、类型和可用的取值限制。JSON 中的 `visible` 表示播放器当前是否显示该行。`available` 表示该行是否符合当前硬件与设置条件。
- `settings-get CFG` 用配置名读取设置。菜单名称随播放器语言变化。配置名保持不变。
- `settings-set CFG VALUE` 接受 `config.cfg` 中的值格式。用树中的配置名和取值限制填写参数。带空格的参数必须加引号。
- 对于 `writable: false` 的设置，用 `settings-action` 打开原生编辑界面。
- `settings-action PATH --yes` 接受完整菜单路径、唯一的动作名称或树中显示的动作 ID。路径的各段用 ` / ` 分隔。
- `--json` 输出机器可读的结果。树的 JSON 是按菜单顺序排列的行数组。每行包含完整路径。

用 `settings-set` 保存扫描目录后，若要立即重建数据库，请打开原生重建动作。
录音触发阈值随峰值表刻度切换。MP3 比特率在录音格式设为 MP3 后出现。

每个动作都必须加 `--yes`。播放器仍按原有流程显示界面和确认提示。
如果播放器要求确认，请在机身选择。动作打开后，等待其结束或退出界面，再发送下一条命令。

## 电池校准

```sh
python3 podlink_shell.py --serial USB_SERIAL --json settings-get battery-calibration
python3 podlink_shell.py --serial USB_SERIAL settings-set battery-calibration reset
```

- `charge` 和 `discharge` 分别显示充电和放电校准。各项包含平滑后的参考电压
  `mv`、当前满电阈值 `full_mv`、上次采样数量 `samples` 和记录时间
  `recorded_at`。电压单位为 mV。时间以设备本地时钟计算，自 1970 年起按秒计。
  参考电压为 0 表示尚未校准。时间为 0 表示设备时钟无效或尚未校准。
- `reset` 清除两项校准，从原厂曲线重新开始。用户电压表继续优先。

## 插件测试与截图

在启用 `--rock-compat` 的 iPod classic 固件上，Shell 可以启动内置安装清单中的插件。
`plugin-launch` 使用与机身浏览器相同的加载器。参数为区分大小写的 ASCII 文件名，
省略目录和 `.rock` 后缀。插件必须安装在清单指定的路径。

`plugin-exit` 相当于点按一次实体 Menu 键。Maze（`maze`）和
Rockblox 1D（`rockblox1d`）用 Menu 退出。2048 的游戏界面用长按 Select
打开暂停菜单，再选 Quit。Snake 的游戏界面用长按 Select 结束当前局，再从游戏菜单
选 Quit。菜单内的 Menu 仍按当前界面的返回规则工作。

Shell 插件运行时，`0x5b` 的 Menu、Select、Left、Right、Play 映射到
实体键。按下和松开分别用索引 1 和 0。保持按下时，按键沿用实体键的长按和重复规则。
音量加和音量减分别产生一格向后和向前滚动。音量键按下和松开分别用索引 1 和 2。
插件退出、Disk Mode 接管或 USB 重置时，固件清除测试按键的保持状态。

以下请求使用 `0x5d`、索引 `0x4951`。固件只在配置完成的非磁盘 PodLink
连接上接受请求。整数均为小端。

| 方向 | 值 | 数据 |
| --- | --- | --- |
| OUT | 9 | 1 至 31 字节的插件名，无 NUL |
| OUT | 10 | 空数据，点按一次 Menu |
| OUT | 11 | 空数据，排队截取已提交到 LCD 的画面 |
| IN | 3 | `<IIIIi32s>`，版本 1、启动代号、插件状态、退出原因、有符号返回值、NUL 结尾的插件名 |
| IN | 4 | `<IIHHHH>`，截图代号、总字节数 153600、宽 320、高 240、分块大小 256、截图状态 |
| IN | 5 | 4 字节 `audio_status()` 标志，PLAY 为 1，PAUSE 为 2 |
| IN | `0x100 + 块号` | 256 字节的 RGB565 像素。块号为 0 至 599 |

每次接受启动请求后，启动代号加一。插件状态为 0 空闲、1 已排队、2 正在运行。
退出原因为 0 无、1 正常、2 USB 接管、3 错误。固件保留最后一次结果，
直到接受新的启动请求。Disk Mode 结束并恢复 PodLink 后，可以读取结果。

截图状态为 0 空、1 已排队、2 可读。截图按从上到下、每行从左到右排列。
原生界面和直接绘制的插件都使用同一个 LCD 截图路径。状态变为 2 后读取全部块，
然后核对截图代号和状态。固件在下一次接受 OUT 11 前保留同一份像素。

## 录音

保持 HerdRemote 运行。开始和停止用 USB 控制端点。下载会向
HerdRemote 借用串口，并用 `MS3 STREAM SAMPLE` 校验偏移、长度和 CRC32。

```sh
python3 podlink_shell.py --serial USB_SERIAL record-info
python3 podlink_shell.py --serial USB_SERIAL record-start line-in --format wav
sleep 5
python3 podlink_shell.py --serial USB_SERIAL record-stop
python3 podlink_shell.py --serial USB_SERIAL record-fetch capture.wav
```

录音保存为 `/Recordings/PodLink-NNNN.wav`，采样率为 44.1 kHz。
`line-in` 用双声道。支持耳机插孔麦克风的机型可以用 `mic` 录制单声道。
`record-info` 显示硬件版本和可用输入。断开 USB 时，固件结束录音。
最后一次成功写入的路径保留到重启。单次 SAMPLE 下载上限为 128 MiB。

控制请求 `0x5d`、索引 `0x4951` 的 OUT 值 6、7、8 分别启动
line-in WAV、启动 mic WAV、结束录音。IN 值 1 返回 96 字节。
前两个小端 32 位整数为硬件版本和录音状态。余下 88 字节为以 NUL
结尾的路径。硬件版本 0 支持底座 line-in，版本 1 额外支持耳机插孔麦克风。
状态值依次为 0 空闲、1 正在录制、2 文件已写入、3 错误、4 输入不支持、
5 录音资源忙。

录音失败时，`record-info` 显示失败阶段、音频状态、PCM 错误位、
文件系统错误号和 DMA 完成块数。控制请求的 IN 值 2 返回 20 字节，
依次为阶段、音频状态、PCM 错误位、文件系统错误号、DMA 完成块数。
每个字段为小端 32 位整数。阶段值 0 至 6 依次为 `none`、`dir`、
`filename`、`init`、`start`、`capture` 和 `save`。
`pcm_error=0x80400000` 表示录音缓冲区分配失败。
