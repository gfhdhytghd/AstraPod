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

一次只能执行一条命令。按键队列正忙时会拒绝提交。命令计数表示界面处理进度。PCM 等测试结果由相应诊断功能提供。

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
