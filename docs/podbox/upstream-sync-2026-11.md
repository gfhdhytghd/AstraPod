# AstraPod 上游同步记录

Rockbox 基点为 `51ea28ffd2`。合并提交为 `8fecf1cc5f`，第二父提交为 GitHub 镜像的
`79b2ad3967`。这次合并包含 307 个提交。未启用的机型和未编译的上游应用树保留在合并历史中。

PodBox 基点为 `f0850c4647`，检查的 `origin/master` 为 `7a89c33d0e`。缺少的 424 个提交中，
218 个与 Rockbox 共用提交 ID。表中逐项记录剩余 206 个提交的处置。

Rockbox 的插件 API 从 286 升到 287，增加 `tagtree_request_reload()`。
本分支保留上游声明，未修改独立分支上的 rock-compat。

## Rockbox 应用层

范围来自 `sh tools/upstream-apps-port.sh --since 51ea28ffd2 --to 79b2ad3967`。
下表的处置仅指编译的 AstraPod 应用层。所有对应的 stock 路径随真实合并进入镜像。

| 处置 | 提交 |
| --- | --- |
| 不适用。专属其他机型，或仅服务目标不启用的硬件功能 | `2476968a20` `3b226dd23e` `c8e9ea1746` `38e2ff86b9` `f9fe698319` `53f6669c63` `8e4a103eb0` `2c70d2454b` `31a046b4a8` |
| 移植至原生应用层 | `ba96040021` `dacd53c781` `decc6181c3` `963444a591` `91e1b24441` `3716177b5d` `4db426cca6` `11156e99a9` `790d282746` `de3dab73b7` `b174c74001` `d9dd1f8d9d` `51e9934dcb` `1bc3fe0434` `d52bc0f2d7` `127dff1016` `d4c0eeb956` `35fe08dfb9` `5bc75fa315` `782dff7c87` `f96282992b` `654e69f22d` `08c892267a` `a332000a74` `0e450f00b0` |
| 共享路径已经合入。原生路径保留自己的调试菜单与应用目录 | `32a2076078` `0669b96d5c` `2ed4d36c17` `15adc15dad` |
| 仅更新镜像。应用层使用原生菜单、书签和本地语言表 | `ae83f59f23` `2c427b055a` `65a5b08e0d` `9836fc8c16` |
| 共享解析器随合并接收。原生章节界面继续使用原生章节读取器 | `bb370f9504` `c2ade1451f` `f85b43398e` |
| 已有本地实现。保留现有队列锁、数据库刷新、滚轮和分隔线行为 | `9b1f1e9e65` `11c4f68656` `8a1aee5c76` `cc41dd21c1` `ab8859915b` `b486769b23` `86a3bb0678` `204c49b796` `5d33e09f7c` `3dc105a54a` |
| 保留原生 Cover Flow 的渲染和滑动算法 | `73f4f069b5` `58583e3467` `4a821b37f3` `2203d38f38` `4fb20b03d4` `4a66a0ac85` `6256979302` |
| 保留原生 iAP 的边界检查、队列索引、身份验证或控制逻辑 | `0c52484714` `863f15494e` `5325cfad0e` `0d4207b9de` `47dbd6a393` `5d8d485e61` `66dbfd533c` `3309747e5d` `4485c6960f` `04094ee2dc` `42c52f8f6a` `0c12fb0abc` `b54e29c821` `4ce5cea231` `7cdc5ec7c2` `78b77c2943` `4de8e07f96` `0e390ff86b` `4b7bc2d32e` `4c78914c8f` `923be48eff` `9eca823739` `042b9419d6` `f209e70291` `7163a9d373` `d1febdc3cf` `97eb61cc79` `2b6bf3275c` `55f21af1db` `be2bfa7022` `baa2b4564b` `6a2f2c77ad` `9b078cdb3b` `59c0bff61b` `edb4f34f93` `a587ee311c` `f852fe5aee` `4d89096045` `943a5af196` `68e0953167` `eeedeca4bf` `d5df66a7ca` `e56ef48f7c` |
| 保留 AstraPod 的队列容量策略。上游的低内存配置不适用于 5G、6G | `2ae4d6cb42` `47379b4475` `f25395e6e5` `a958507036` |
| 保留原生启动和队列恢复。跳过 stock UI 队列消息 | `c6d3012d7b` |
| 已有本地 JPEG 防护 | `6b6642fcbe` `162241602a` `3be791bc1d` `473b827ea1` |
| 仅更新镜像。依赖 Rockbox 插件加载器或插件入口 | `51cd988e61` `c4fdfae7a5` `01bcba89b7` `c1a706228c` `ec11d6ba77` `f3ffe789ef` `b9002ac186` `06881ac6a9` `cd9ba1ff77` `47bb59dedb` `683b5fa76b` `230b3624aa` |

共享文件的合并保留 AstraPod 的 USB 配置、PodLink、iAP2 和设备日志。USB 存储使用运算前的
LBA 边界检查，并拒绝无效 LUN 或范围外的 WRITE。6G 在 flush 或 standby 失败后保持驱动器供电。
5G 保留已有的 ARC 硬件计时。FLAC 接收坏帧限制、重同步和 seek 修复，并保留本地 LPC 路径。
ALAC 保留本地低字节合并、Rice 检查和 ARM 汇编。USB iAP 接收随机认证挑战和重识别。
保留本地延迟认证兼容和失败发送清理。图片解析继续保留原生封面和艺术家图的独立选择。

## PodBox 独有提交

| 处置 | 提交 |
| --- | --- |
| 跳过 PodBox 发布记录或治理文档。使用 AstraPod 自有文档和发布脚本 | `51e020e4db` `846dfc7ab7` `604b14db37` `e38d7fc977` `aae88cade8` `803544fa47` `ed1e80dd04` `13d518015f` |
| 跳过 PodBox 的 Arcade 运行时、脚本游戏、IDE、模板或相关发布链。保留原生 Arcade 和 C 游戏 | `05b6236e98` `2a6684e9d8` `aebaa4e7f5` `b260782ee6` `2a9b40c6e3` `25e7c05b15` `8734613a76` `817b11671b` `fc7e53f3c5` `11693f7d3a` `8237682852` `a0ffff204c` `df4de2435c` `b8f290d027` `d78478a443` `b6ed46e2a8` `ba74f5e91a` `29aadf6233` `906b97044f` `f8ff785d4d` `437c3c0d15` `766fd3afb4` `7ed9ba439b` `bcb9f77e44` `4560877848` `74b6ee7f9f` `a34d4a3f82` `d1d84b1d47` `15523516ea` `20e3d8fd34` `4cb8e5f533` `92db41dec3` `c63174f9db` `bf852be345` `6d93fc4369` `cd55b6cd50` `b67f1ce762` `0a907d1745` `1ab8cc8472` `b2fc048d10` `832dce749e` `43f0f33e02` `4ef94896dc` `1ff90c4bc3` `edabf98f09` `2d56e3f2ae` `889d4b4964` `8f6d75058c` `632cd3c6de` `99ed7d7289` `02a3cace96` `d9a50cc375` `774e665de3` `d6a8e24431` `c8e48e7bd8` `9cd207fa5a` `df93eb871f` `8f2fabfb67` `8728662bd3` `b3996a7729` `b24e9a80a7` `95a01b26c9` `cccdeb263a` `027ad09bb5` `61fc9dfbe3` `d4f88c424d` `62c24dc895` `ac58c939d8` `691a372e66` `b54477524e` `dc80edd6b8` `fe093db1cb` `82a6e16a97` `66be3eee11` `c1d940f66e` `168f382f45` `0c3bf45016` `018bbae532` `88c60bfbfd` `23bcf333ad` `31298bd98a` `4dd99eb5eb` `7dc2132532` `2ff6aa547a` `1926b67d61` `7bed5df640` `4dbed67c26` `3953367da5` `4f9e8ef203` `2ad3367b4e` `993f3412b5` `3f95becee2` `5cdcc90084` `ced8a039a6` `72074d1a35` `a2a1324a56` `d1650e016a` `2e6c7902f8` `c312826dec` `4c79139ca6` `00b6758313` `e8dcf0c654` `455bff7634` `70fbf6a324` `04833693d9` `db89735c2c` `23aa03d2e1` `e012c0d86c` `328c89ab6e` `9a98afe4a6` `c4f0222934` `6110bd4d0d` `a3d5074bd4` `5e841a182f` `6cca89f816` `e1520785f0` `fb6d490cbd` `a821b983a4` `924198e007` `3a0edcfce9` `8e4f125b96` `c52307e8c9` `7165b150a5` `502204896c` `93932a5290` `d09abcea7e` `889e36781f` `f7b8117082` `3cc4d72966` `fbbaf99d33` `fa923de9df` `c0709b75a9` `becdb33f20` `a1cee2565c` `6075c6f856` `126ebba20e` `ab4595f18c` `680f452224` `b01d3ab924` `76bb13a17a` `bbc85ded68` `7a125def9a` `4ad0866ff0` `fa3144edb5` `fb8de91d41` `7a89c33d0e` |
| 移植 USB Sound Card、ROLO 或 USB 描述符。保留原生 Arcade 的按键处理 | `01dfd91719` `5943566d41` `4fb656db4f` `c4b22859c9` `123df29be9` `8ab27918d3` `60eaab69c7` `c326895ad0` `82b216d97f` |
| 合并记录。218 个共同提交已经随 Rockbox 合并 | `d65129f47e` |
| 已接收同一 Rockbox 修复或其本地适配。保留本地 ALAC 汇编 | `78a147c620` `dde2de3cdc` `80efccc3b7` `2ed31412e1` |
| 移植语音构建、artcache 工具或发布演练 | `1254da7ab3` `657460d021` `b73e78aa3b` `9ebd902ee0` |
| 移植至原生队列、播客、播放快捷键或封面缓存 | `25f50e2e1c` `4dd4bfe7a6` `2d1320aab0` `c0b3d511b6` `097bf3e1b5` `84fcaf020c` `7a5dcc0e97` |
| 跳过对应主题或按键设置。保留原生主题、返回栈、长按 Play 和 Party Mode | `2eacc2c633` `5f7b525f8e` `29262cdbc9` `11c7c4c5d8` `d0684672c0` `6b2285f853` `fdfc938af9` `26fe87c605` `42238c3471` `20bb4b7295` `eda69f5ea1` |
| 移植共享存储、解码、图像或混音修复。AAC 帧初始化采用 Rockbox 实现 | `c61746fe83` `39aad5a33d` `a0576e81f1` `38c0060303` `e0416743e2` `e18326beab` |
| 已有本地实现。保留 EXIF、ID3 图片回收和封面发布代际逻辑 | `9b7fcfba43` `40cdc819aa` `43d8ca291a` |
| 保留 AstraPod 的封面失败重试、默认配置和原生 Arcade | `726eef1631` |
| 移植双写、日志失败恢复和统计兼容 | `c8bc366cb0` `8da393ce4d` `8aea84fda0` `d53352dc5f` `a053baaf74` `e2b52c2eac` |

## master 后续合并

合并 `626da24e7d`，保留 `6290e652b1` 的 PodLink 传输改动和 6G 录音。
将 Rockbox `15207b1fa4` 的录音缓冲区容量检查移植到原生录音模块。保留 6G 的 8 MiB 录音分配。
