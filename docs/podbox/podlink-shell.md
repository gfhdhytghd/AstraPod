# PodLink Shell

iPod classic 固件默认启用 PodLink Shell。它使用 PodLink 非磁盘连接上的 USB 控制端点。主机需要 PyUSB，且必须指定 iPod 的 USB 序列号。

在 [AstraPod-Desktop](https://github.com/gfhdhytghd/AstraPod-Desktop) 仓库运行以下命令。

```sh
python3 podlink_shell.py --serial USB_SERIAL
python3 podlink.py reboot --serial USB_SERIAL
python3 podlink_shell.py --serial USB_SERIAL debug-list
python3 podlink_shell.py --serial USB_SERIAL debug "Hardware info"
```

- `reboot` 执行正常关机和重启。脚本等待 USB 重新枚举。
- `debug-list` 列出当前固件实际提供的 Debug 菜单项目。`debug` 按名称在机身打开该项目，沿用机身操作和确认流程。需要交互的页面在 iPod 上操作及退出。
- `status` 显示开机时钟、已提交及已处理的命令数，以及界面开始处理最后一条命令时的时钟。
- `play` 用 `/.rockbox/sm1-test/irq-dma44.flac` 建立单曲播放列表。测试文件由测试者放入设备。执行前备份当前播放列表。
- `seek` 将当前曲目定位到 6.237 秒。`stop` 停止播放。

一次只能执行一条命令。按键队列正忙时会拒绝提交。命令计数表示界面处理进度。PCM 等测试结果由相应诊断功能提供。
