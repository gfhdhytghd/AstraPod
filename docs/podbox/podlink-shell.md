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
