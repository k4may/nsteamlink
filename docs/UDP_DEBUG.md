# UDP 调试

本页仅适用于 `-DNSL_DIAGNOSTICS=ON` 构建；默认 Release 不包含这些入口或日志。
诊断应用和 `switch-stream-selftest.nro` 的诊断端口为 UDP `28772`。
调试 Switch 地址为 `10.10.10.17`；命令由 `app/platforms/common/runtime.c` 实现。

```bash
tools/switch-debugctl.py 10.10.10.17 state
tools/switch-debugctl.py 10.10.10.17 cloud
tools/switch-debugctl.py 10.10.10.17 stats
tools/switch-debugctl.py 10.10.10.17 audio
tools/switch-debugctl.py 10.10.10.17 hid
tools/switch-debugctl.py 10.10.10.17 hidlog
tools/switch-debugctl.py 10.10.10.17 diag current
tools/switch-debugctl.py 10.10.10.17 diag prev
```

| 命令 | 内容 |
|---|---|
| `ping` / `state` / `stats` / `perf` | 会话是否存在、实际呈现帧数、fps、HID 队列、最大 ACK 等待 |
| `cloud` | 云端目录/TLS/Upgrade/WebSocket 阶段、会话是否就绪、候选位置、重试等待、可订阅及在线主机数 |
| `audio` | 实际采样率／声道、队列字节数、解码帧数、丢弃及错误计数 |
| `hid` | 提交／发送／确认计数、待发与在途数、重试次数、最大 ACK 等待 |
| `hidlog` / `hid-history` | 有界的 SDL／原始输入及已提交报告历史 |
| `haptics`（Switch） | 普通/原生震动命令、解码/提交样本、延迟、拒绝/丢失、IPC 及源队列计数 |
| `haptic-trace <serial>`（Switch） | serial 之后最多六条原包/输出记录；只读分页，无记录返回 `{}` |
| `diag current` | 当前诊断日志的内存尾部，最多 3500 字节 |
| `diag prev` | 上次启动日志的缓存尾部，最多 3500 字节 |

该产品入口的 UDP 接口只读。连接、配对、安全码、停流和退出通过同一套屏幕／手柄操作完成。
手动地址位于选项 → 设置 → 手动地址。图形、IHS 初始化及发现不再由调试命令分步驱动。
独立 `switch-discover` 工具的 `hosts/select/pair/code/exit` 等命令属于其工具接口，
不适用于正式应用。

震动 trace 包含原始 `0x01/0x0d` 命令、接收/到期/提交/完成单调时间以及两侧
low/high 幅频参数。环形缓冲上限 512，覆盖递增计数；序号缺口不能当成连续完整
保真记录。`commands` 等应用计数从应用初始化累计，`source_*` 计数只统计当前
会话仍打开的 SDL 设备；关闭设备后不能用它恢复已关闭设备的历史。
`scheduled_samples` 是已选中的原生帧，`emitted_samples` 是系统调用成功提交的
原生帧；失败或停止时两者不一定相等。`canceled_outputs` 包括发送前被更新命令
或停止取消的任务。`max_lateness_ns` 观察实际提交相对到期时间的迟到。
诊断查询只复制内存，不在输出或渲染线程打印或写文件。

```sh
tools/switch-debugctl.py 10.10.10.17 haptics
tools/switch-debugctl.py 10.10.10.17 haptic-trace 0
python3 tools/capture-switch-haptics.py --seconds 30 --output build/game-haptics.jsonl
```

采集器只读取已运行的诊断应用；不启动串流或制造震动。单次运行内按 serial
去重并记录缺口，跨应用重启应另开采集文件。`native_commands=0` 仅表示客户端
尚未收到原生命令，不能据此认定手柄不支持 HD。包/IPC 记录均不证明实际马达
频谱；分层验证、自动部署夹具及物理测量边界见 `OFFICIAL_INPUT_RE.md` §22。

若原生样本提交与参数对照通过，但系统读回不同，诊断构建的
`nsl-haptics-calibration.nro` 可在同一次运行比较 libnx 单侧 IPC 201、双侧 IPC
206 和生产输出线程的 IPC 206。程序包含全部刺激，无需 SD 文件；约两秒测量，
随后自动停止、join 并收集日志。只在用户确认 netloader 就绪后部署一次：

```sh
python3 tools/switch-haptics-calibration.py --address <本次确认的Switch地址> \
  --nro build/switch/app/nsl-haptics-calibration.nro \
  --log build/haptic-calibration.log --output build/haptic-calibration.json
# 仅重新分析已有日志，不部署：
python3 tools/switch-haptics-calibration.py --log build/haptic-calibration.log \
  --output build/haptic-calibration-reanalysis.json
```

对照完成只表示诊断记录完整。长保持值一致不能替代原生夹具的严格 5ms 验收，
也不证明马达波形或返回 hbmenu；具体刺激与判定边界见协议参考 §22.9。

若需核对 Windows Steam 主机的实际版本、配置与脉冲开关，可在主机运行
`tools/collect-steam-haptics-host.ps1`（Windows PowerShell 5.1+）。默认查找注册表
中的 Steam 安装路径，在桌面生成 `steam-haptics-host-日期时间.zip`。包含当前
steamclient DLL 的版本/SHA-256、相关 controller/streaming 日志、指定游戏
（默认 AppID 1049320）的 VDF，以及全局/本地配置中匹配 haptic/rumble/vibration
的行。缺少的可选文件列入 manifest；不停止 Steam，也不改变 Steam 配置。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\collect-steam-haptics-host.ps1
# Steam 未注册或另选游戏时可明确指定：
powershell -NoProfile -ExecutionPolicy Bypass -File .\collect-steam-haptics-host.ps1 -SteamRoot 'D:\Steam' -AppId 1049320
```

受控主机源使用 `tools/steam-haptics-probe-win.c`，默认通过公开 Steam Input
接口请求效果，不需要特定游戏提供震动。先运行带 native capability 查询的
诊断 NRO，并正常连接这台 Steam 主机；采集端运行
`capture-switch-haptics.py --address <当前设备 IP> --seconds 60 --output <文件>`。
Windows 测试工具和 `run-steam-haptics-probe.ps1` 放在同一目录后执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\run-steam-haptics-probe.ps1
```

脚本默认 AppID 1049320，仅作为测试进程上下文；不安装/启动该游戏，不编辑
Steam 布局/偏好。工具只对唯一 Switch Pro handle 发普通对照和六次 legacy
pulse 请求，并在结束请求归零。桌面生成 ZIP，含调用记录、DLL/工具散列与
两种主机日志末尾。`--list` 可仅列出 handle/type/remote session ID。Windows
脚本须以实际执行结果验证；Wine mock 工具通过不表示真实 Steam 可用。

公开手柄枚举按进程 AppID 过滤，工具 Init 成功可能仍列不出桌面串流设备。
修正版在已核验 DLL SHA/adapter/controller vtable/查询函数 RVA 一致时，另行
只读查询桌面/UI 的上下文并去重，再通过公开 API 请求效果；不修改关联、布局
或触觉偏好。未知 DLL/布局即拒绝，不猜控制器索引。`public_controller_count`、
`context_controller_count` 和布局拒绝阶段可区分这些情况。原始过滤函数的离线
对照与 Wine 守卫测试不替代真实主机结果。复核工具：

```sh
python3 tools/test-steam-haptics-probe.py --output build/haptics-probe-mock \
  --wine-prefix build/haptics-probe-wine
```

该测试依赖 MinGW/Wine，mock DLL 与允许 mock hash 的测试 EXE 不得放入主机工具包。

隔离公开进程转发与主机输出时，可显式运行内部 RPC 对照：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\run-steam-haptics-probe.ps1 -SwitchAddress 10.10.10.111 -CompareHostRpc
```

地址必须是用户确认的当前设备地址。该模式在发送前查询 UDP 28772，不接触
netloader；诊断无响应即拒绝。它固定当前 DLL SHA、私有 map vtable/方法 RVA，
从主机 ActualControllerDetails 精确匹配远程 Switch handle/type，禁止猜 slot。
分别经公开 API 和主机索引 RPC 请求左右各三次脉冲，均使用 flags=0，不修改
偏好、布局、能力或游戏上下文。每次前后读取 trace，并保存原始 UDP 响应与
主机发送/接收时间；两台设备时钟不直接相减。新建 map 在释放用户/pipe 前销毁。
该入口仅为版本固定的诊断工具，不能加入通用客户端；RPC 路径通过也不能证明
公开游戏路径通过。协议依据见 §22.15。

主机 ZIP 解压后可离线分析；旧版无逐请求 bracket 时只能统计，不能关联认证：

```sh
python3 tools/analyze-steam-haptics-probe.py --requests requests.jsonl \
  --manifest manifest.json --output analysis.json
# 可追加 --capture <设备 capture.jsonl> 合并独立采集。
```

`software_correlation_verified` 要求该路由的六次请求各自有完整 serial 区间、
目标一致的非零原包及三条成功提交的正确样本；超时、缺号、参数/目标错误和
源失败不能通过。这不认证机械振幅、系统读回、游戏支持或 loader 返回。

已确认主机 caps 后仍无非零效果时，可在上述命令追加 `-IsolateGates`：省去
非零普通刺激、每次停止后等待 800ms；增加 `host_rpc_flags1` 诊断正对照并
尝试读取固定布局的 Steam 主机门禁字段。该结果与 flags=0 分开，正对照成功
不等于正常游戏路径修好。读取仅针对 Steam 目标手柄；不支持的 32 位主机、
布局/权限错误会报告 unavailable，不写内存、不改偏好。ZIP 另含磁盘配置的
触觉相关匹配行；这些行不是当前运行时状态证明。机制和边界见 §22.16。

关联器按请求前同 slot 的最近原包恢复两侧缓存；Steam 的单侧 pulse 会保留
另一侧 word，零时长 API 请求不保证该 word 已清除。因而双侧原包不自动意味着
串侧，另一侧也不能被任意放行。缺失/未知基线、错误另一侧和短于 5ms 保持
仍拒绝。可用固定 DLL 的原始编码器另行复核每次完整原包：

```sh
python3 tools/steam-haptics-routing-audit.py --host /path/to/steamclient64.dll \
  --requests requests.jsonl --output original-packet-oracle.json
```

该参数 oracle 使用观测到的前包作为缓存输入、合成的允许条件与时钟，证明
编码一致性，不能取代真实 gate/cache 读回或证明前次失败的具体原因。主机状态
result=0 是未找到目标对象，不是触觉开关关闭。详见协议参考 §22.17。

验收需在主机调用时间段看到接受的非零 `0x0d` Nintendo 原包及对应输出；
普通震动、静音包、主机工具成功退出均不满足该条件。`haptics` 的
`native_capability_mask` 只表示本地后端支持快照，不是主机已采用该能力的 ACK。
若主机未枚举到目标或无非零包，保留错误和配置证据，不归因于游戏。

该归档用于匹配真实主机证据；本身不证明游戏支持原生效果。门禁及旧版本原始
指令对照见协议参考 §22.10。配置收集不到时仍需以实际生效的游戏布局为准。

## 自包含真实主机会话测试

诊断构建单独生成 `nsl-live-media-probe.nro`；普通发布构建不包含此测试入口。
测试包直接读取 Switch 现有 `profile.bin`、按主机密钥和串流设置，不需复制 SD 文件。
它只选择与指定地址的实际发现回复匹配、且已保存为配对的主机；身份缺失、损坏、
需要授权码/安全码或连接失败时记录失败并退出，不猜测 PIN，也不新建测试身份。
正常串流过程中的密钥轮换、配置和测网统计保存仍由生产 runtime 执行。

测试执行一次被动播放，不发送用户控制输入。若普通开流先进入正式测网，工具使用
生产结果确认命令执行“继续但不接受推荐”，保留已有设置，再进入播放。
测网 session 的流量不纳入普通媒体计数。此自动确认只属于独立测试程序；
正式应用的推荐确认、FEC 保存/初始请求策略仍见协议参考 §32、§116。

```sh
# 先完成 clean 提交及对应诊断构建；默认仅归档，不启动设备。
python3 tools/live-media-probe.py --steam 10.10.10.166 --switch 10.10.10.25 \
  --seconds 30 --output build/live-media-ready
# 用户准备好 netloader 后，另选一个新归档目录，明确 --run，只部署一次。
python3 tools/live-media-probe.py --steam 10.10.10.166 --switch 10.10.10.25 \
  --seconds 30 --output build/live-media-run --run
```

工具归档实际 NRO/SHA256、提交身份、graphics manifest、命令、nxlink 原始日志与
机器结果；不探测 netloader TCP，也不自动重复启动。示例地址只是明确的测试参数，
设备或主机变化时应使用实际确认地址。离线复核可用 `--analyze-log`、
`--expected-build`、相同 `--steam/--seconds` 和一个输出 JSON 路径。

`LIVE_WIRE` 是通过连接入口的普通 type3/4、type11 包数；`parity_enabled` 仅表示
包头声明 m>0，`parity_shards` 表示到达的校验片，`fec_complete` 是生产 FEC 接收器
实际交付次数。收到 type11（可为 m=0）或 FEC 交付，都不单独证明发生了 RS 修复。
`LIVE_FLOW` 的 complete 是完整媒体回调次数，recovery 是 DataLost 入口调用次数；
feedback_sent/failed 是普通会话实际 Socket 发送 type10 的结果，不把每份反馈都当作
请求缺片，更不等于主机收到/执行了请求。计数覆盖普通会话的所有媒体 channel，
不是只覆盖视频。观察器原样转发生产调用，只做有界原子记账，不逐包输出或等待。
读取元数据前先检查数据指针和长度；空/短 FEC 包仍交给原接收器拒绝，
不会为诊断提前请求空缓冲的指针。空/短出站缓冲也保留原发送结果，不读取其包头。

观察器格式 2 在 `LIVE_BEGIN` 中声明 `observer=2`，另按实际 Video channel 类型识别
视频角色，不预设 channel ID=4。每个计数周期与最终计数组还包含：

| 记录 | 直接观察的边界 |
|---|---|
| `VCHANNEL` | 当前视频 channel 的普通/type11 入站计数；retry 仅计普通包重发字节非零，不等于唯一丢片或主机已处理反馈 |
| `VRECV` | 送往视频完整媒体回调的次数；key/finish 是其中明文视频头的原始标志，在 FIFO 准入/退休之前 |
| `VWORK` | decoder worker 取得完整媒体的次数；key/finish 在消费者偏移缓冲之前读取，不表示关键帧已完成组装/解码 |
| `VSUBMIT` | 应用视频提交入口、返回、关键帧参数、致命返回计数；不把入口/返回当作产生图像 |
| `VDECODE` | 解码反馈调用与实际布尔返回；reset/lost/disable 是提供的有效标志，accepted 不证明异步重建已完成 |
| `VREQUEST` | 实际 Video 类型 endpoint 的 DataLost 入口，以及普通会话同 channel 的 DataLost datagram Socket 成功/失败；不证明主机收到或响应 |

`VRECV/VWORK.last` 是一次原始头观察的打包值：bit40 为存在位、低16位为媒体
frame ID、bit16–31 为视频片段 sequence、bit32–39 为原始 flags；工具在 JSON 中展开。
`VSUBMIT.last` 的 bit48 为存在位、低16位为提交 frame ID、bit16–47 为应用提交 flags。
这些 ID/flags 与原子计数不是跨字段事务快照，也不是完整帧像素身份；关键帧标志依据
协议参考 §24/59，不能直接当作成功恢复。入站在角色确定前也按 channel 记账，未保留
跨线程的 channel 指针；不同视频 epoch/channel 的计数不能误作同一连续帧序列。
网络/decoder 观察器只做固定上限的原子操作，不逐包输出；主线程每秒采样，单行保持
在平台 240B 日志上限内。丢失/截断记录按原规则拒绝通过。

格式 2 的最终六组视频计数均为验收必需项，并要求提交已返回；播放失败原因与
清理结果分别保留，不能因为 `result=FAIL` 就把已记录的 join/destroy 称作失败。
旧格式归档仍保留其原提交工具生成的报告，不补造缺失的视频计数；新工具拒绝把
旧格式当作本版本验收。

`LIVE_RESULT` 保留实际解码/首次呈现/音频解码计数、硬解事实、首帧后的播放时间、
主循环观察的最大呈现计数推进间隔 gap_ms 和结束前无推进时间 tail_ms。它不是逐帧
GPU 时间戳；epoch 内计数在两次采样间多次增加时，只能观察到最后一次主循环采样。
测试至少要求两次实际呈现，且结束前两秒仍有推进，拒绝“首帧后冻结仍通过”；
两秒仅是 smoke 验收阈值，不进入生产重发、丢帧或重连决策。历史 gap_ms 仍单独保留。
PASS 只表示已有配对身份完成真实主机视频播放 smoke 与应用资源清理；音频计数
需单独判断，未注入丢包，不证明异常网络恢复、长期同步或全部线程交错。
GPU/显示 API 的呈现计数也不证明物理扫描完成；FINAL 在关闭日志 socket 之前输出，
不证明 socketExit、返回 hbmenu 或第二次启动。缺少当前 BEGIN/完整终态、
计数或清理记录、播放时间不足、超时及 nxlink 非零退出均不能通过。

## 屏幕诊断

串流中同时长按 **− 和 + 0.8 秒**打开本地游玩菜单；菜单内单独长按 **X 一秒**切换只读 Debug 浮层，
随后自动返回游戏。再次进入菜单长按 X 关闭。默认关闭，断开后复位。

浮层的“本机视频”从客户端提交完整帧计时到 present 返回，不是网络延迟或物理屏幕扫描完成时间。
最大 ACK 等待是会话累计传输确认最大值，不证明游戏已应用输入。重复绘制同一视频纹理不增加视频帧率。
没有样本显示“—”；超过三秒没有更新标记数据已过期。

## 日志与清理

独立 joinable 日志线程接收有界队列；生产者 trylock，队列满时丢弃日志，不阻塞渲染或输入。
每秒记录实际媒体与可靠传输摘要。每次启动在 `sdmc:/switch/nsteamlink/logs/`
创建独立的 `session-<时间>-<纳秒>-<分卷>.log`；单卷达到 4 MiB 后续写新卷，
不会因总写入超过 4 MiB 停止，也不自动覆盖或删除历史文件。磁盘写入失败由下载索引的
`storage_errno` 报告。升级前的三份 `stream_diag*.log` 原样保留，也可通过下载接口读取。
UDP 的 `diag` 仍只是兼容用的内存尾部，完整历史应从 HTTP 接口下载。
配对码、安全码及密钥不进入该日志接口。

调试版运行期间，原日志线程提供 TCP `28773` 的只读 HTTP 接口；停流返回首页后仍可使用。
无需重进 hbmenu 或部署回收 NRO。切回可达的局域网后，在电脑运行：

```bash
python3 tools/pull-switch-logs.py --address 10.10.10.17 --output build/device-logs
```

`GET /logs` 返回版本、commit、存储错误、诊断队列丢弃数及日志索引；超过一页可按
`next_after` 查询。`GET /logs/<文件名>` 支持有界 HTTP Range。工具以 32 KiB 分段下载，
超时重试并保留 `.part` 以续传，核对响应范围和长度，在 `capture.json` 保存本地 SHA-256。
当前文件按索引时的大小取快照，下次拉取补齐其后追加部分。工具不修改远端文件，
也不读取配对资料。退出应用后 HTTP 服务随日志线程关闭；重新启动调试版即可读取保留下来的历史。
文件与网络操作均由后台日志线程承担，协议收包、渲染和输入线程不执行下载文件 I/O。

日志首行包含应用版本、完整 commit 和设备 Unix 时间，回收后须先核对是否属于目标会话。
`connection_event` 记录实际选用 LAN UDP 或云端 SNS、请求响应及 IHS 配置/连接/断开阶段；
每秒 `connection` 的 `route` 区分 `lan_udp`、`sns_direct`、`sns_relay` 和未就绪/已关闭，
同时记录 SDK state、relay 标志、终态来源和 SNS datagram 累计数。它不把云端发现等同中继。
`control_tx` 每秒记录握手state、完整地址查找/空地址查找、CM本机入队、被拒、交给WebSocket
发送队列、交付失败、队列清理/旧会话丢弃及当前待发送数量。reason为最近入队拒绝原因：1云端未就绪、
2没有目的账号记录、3本地队列/大小上限、4分配失败。forwarded仅证明CM发送队列接收，
不证明Steam服务器或主机收到；direct_lookup也只证明有完整缓存地址，不证明UDP送达。
`control_rx`记录云端控制回调累计数、最后的消息type、开流request_id和结果码；它位于
IHS准入之前，收到不等于请求编号匹配或票据被接受。结果不可读取时为-1，received=0时
最后字段无含义。计数从进程启动累计，包含发现产生的控制发送，按请求阶段比较增量。
`control_identity`只记录目的账号是否存在/为零、是否匹配已配对卡片、key文件加载结果
（0读到、1不存在、负值错误），不记录账号ID、密钥或token；不得仅凭这些布尔值判定授权成功。
`pipeline` 即使尚未显示第一帧也输出提交、解码、显示、音频及帧停滞时间；`reliable` 输出
ACK/NACK/重试/失败、待确认数量与最老项年龄。退出前及 IHS join 后再保存一次阶段快照。
`log_drop` 是有界队列或 trylock 丢弃的诊断记录数，不能把它当作网络丢包数。
这些记录只在诊断包启用，不包含 packet payload、配对材料或 SDK 原始身份文本。

`cloud_net` 在目录请求、候选连接、失败/重试时记录具体阶段、libcurl result、HTTP 状态、
系统网络错误和证书校验结果；`cloud_time` 的 DNS/TCP/TLS 数值是 curl 的阶段完成时间，
均以请求开始为起点，不能将它们相加。`curl=-1` 表示该事件没有对应的 curl result，
不是成功；`curl=0` 只说明那次 curl 操作完成，HTTP、Upgrade、protobuf 仍可能失败。
常见 result：6=DNS、7=连接、28=超时、35=TLS、60=证书校验。`cloud_addr` 记录连接实际
本地/远端 IP；`cloud_update stall_ms` 记录 owner 内一次云端更新超过 250 ms（最多每秒一条），
用于核对是否阻塞了同线程的其他工作，不能仅凭使用 multi 就断言所有 DNS 路径均不阻塞。
`cloud_try` 从 curl 的连接前事件记录每个尝试的数字 IP/端口，即使 TCP 超时、
`cloud_addr` 尚无值也能留证。每个请求最多 16 条；该地址不是 DNS 响应全集，也不代表连接成功。
只接受 `Trying` 文本中经数字地址校验的 IP/端口；其他文本、头部及数据全部忽略。
Switch统一解析策略用`dns_query event=begin`记录主机名和当前本机/实际查询DNS数字地址。
D-A097替代D-A095的单DNS实验：向Google `8.8.8.8`、Cloudflare `1.1.1.1`、
Quad9 `9.9.9.9`、AliDNS `223.5.5.5`、DNSPod `119.29.29.29` 并行查询，所有
提供器每家最多4个A地址，合并最多20个。D-A102替代D-A101整批FIFO：进程最多5个
wire查询，逐提供器FIFO、初始启动相隔25ms；每家3秒wire预算不含排队，排队/收集另有
51秒上限。`begin`的queue_ms是首次开始前的等待，不是DNS网络耗时。
`available`记录每个有效提供器的实际到达时间，HTTP首个地址即可连接，后续结果补入候选。
3秒不是开始TLS前必须等待的时间。DNS UDP单独请求8KiB收发缓冲，
不改媒体socket配置。HTTP owner持有可取消/join的DNS job，防止收集暂停同线程LAN发现
和SDK更新；封面/更新器阻塞worker也以临时DNS job继续收集迟到结果。`begin`的started/os记录本机启动结果；`answer`的starts/start_os
记录实际启动次数及最后启动错误。status=2表示到期时仍在排队，0表示仍在等待响应，
1表示取得地址，-1表示失败。临时本机资源错误以50ms间隔在原3秒预算内重试。IP去重后按`/24`分组交错，
先尝试每组首个地址，再尝试各组第二个。`/24`仅用于排序，不说明真实路由相同，也不
封禁失败地址的整个网段。每次读取当前接口，无跨Wi-Fi结果缓存，不回退DHCP/系统DNS。
`event=answer`逐个记录提供器结果，`ready|failed|cancelled`记录合并列表和耗时；ready
只代表取得DNS地址，不代表HTTPS成功。`https_route`记录实际请求候选的`try/failed`、
主机名、IP及序号；目录、CM TLS、两处SDR配置、封面与更新器共用此策略。候选连接
含TLS最多4秒，候选轮换预算90秒，收到HTTP响应/正文后不重放请求；已有下载的正文
超时规则继续适用。重定向目标的DNS同样使用多提供器，但已收到HTTP重定向后失败不
从头重放整条请求。数字地址不参加DNS轮换，普通桌面平台保持本机原有连接行为。
成功使用原域名/SNI/证书校验建立连接后沿用该连接。这是针对当前环境的兼容方案，
不代表Valve官方策略，也未证明DNS或MTU是根因。旧4e810d5的cloud_dns仅覆盖目录，不能据此判断SDR配置或SDK解析已恢复；
该单点补丁已被D-A075统一策略替代。`sns_config`另外记录GetSDRConfig HTTP/curl结果、
信任导入、证书安装与P2P创建。须继续对应后续SDK状态、IHS和媒体进度，不把发现成功当作
传输成功。清理时DNS scope/socket结果在日志关闭前保存。

持久日志的`hid_transport`按连接采样周期（通常每秒）记录输入报告submitted/sent、
RemoteHID传输包ACK及pending/flight。ACK包含设备生命周期报文或其分片，不能把它与
报告提交数一一对应，也不能据此证明主机应用执行了按键。
`hid_capture`记录本地提供器数量、session接入状态attached（不是线程创建成功证明）、
UI输入门控gate、协议输入开关input、采集/尝试提交计数及最近本地报告seq/buttons。
这些值由不同已有owner采样，不构成一个跨线程原子事务；seq/buttons不是主机反馈。
另保留HID枚举、打开/关闭、StartInputReports和RequestFullReport的低频记录，
每秒最多8条；不开放逐报告、feature正文或设备序列号日志。
`sns_start failed_stage=... detail=...` 在SDK尚未返回有效连接句柄时记录拒绝类别：
`option` 的detail是配置项ID；`udp_socket/nonblock/send_buffer/recv_buffer/bind/local_address`
的detail是底层socket错误码；`crypto` 的detail=1表示尚无可用签名证书。
同时保留SDK认证可用性和本地证书剩余秒数（无证书为INT_MIN）。只有同步创建连接期间
临时启用SDK错误回调，最多接纳8条并转换成固定分类/数字，不保存SDK原文或身份；
回调内不输出日志，调用返回后恢复禁用，正常收流不增加SDK日志。
`cloud_watch` 记录保存主机中有多少具备配对/账户/设备信息；`cloud_subscription queued`
仅代表订阅进入发送队列，`cloud_host event=online` 才代表收到并接纳主机状态。

调试主应用另外每秒记录 `udp_rx` 与 `media_rx`。前者按本次会话的 IPv4 主机地址/端口
统计 native UDP 返回的 datagram；后者按 channel 分开记录原始媒体报文头、通过 IHS
校验后到达媒体通道入口的报文及已排队的恢复请求。入口计数不代表窗口接纳或完整组帧。
原始计数尚未通过 CRC/连接身份检查，
不能视为有效媒体；计数差也不能直接当作无线丢包率。协议线程只更新固定原子计数，
不逐包输出、分配、复制 payload 或重新校验 CRC。SNS 的底层收发仍看既有 `connection`
计数；测试驱动有自己的观察器，不能把其 `observer=0` 误读为没有收到包。

清理日志包括 HID worker、session interrupt/join/destroy、IHS client、IHS_Quit、SDL_Quit。
退出后 nxlink 结束仅是 PC 侧证据，Switch 返回 hbmenu 及再次启动结果仍以屏幕观察为准。

## 视频直显性能统计

诊断构建每秒由 runtime worker 输出 `vp1` 至 `vp5`，同一 `id` 表示一份一致快照。
Switch 继续使用现有非阻塞日志发送；不重试阻塞发送，不逐帧写日志。渲染线程仅进行固定大小
计数/快照、额外三个单调时钟读取和短锁内复制，无格式化、文件 I/O 或新增线程。
新增采样以 `NSL_DIAGNOSTICS` 编译开关隔离；关闭诊断时不执行这些渲染采样。
实际设备上的诊断开销仍应通过 ON/OFF 同场景对照确认，不能宣称为零。

- `vp1`：session/epoch、后端、硬件解码标志、尺寸、解码/呈现/替换/拒绝累计数。
- `vp2`：新呈现帧的样本数 `n`；CPU 视频准备 `prep`；解码准入至 present 返回 `age`；
  解码输出至准备开始 `wait`；全部解码输出的驻留累计 `decode`。时间单位均为微秒。
  解码驻留包含重排/缓冲，不能直接视为 NVDEC 硬件执行时间；`age` 不包含此前网络接收，
  也不是输入至屏幕发光延迟。`prep` 不包含 UI、acquire、present 或 GPU fence 等待。
- `vp3`：包括旧帧重画的绘制次数 `draws`、重画次数 `redraw`，及 begin（可能包含 acquire 等待）、
  视频调用结束至 present 前的 UI/记账阶段、present API 的累计墙钟耗时。
  两个后端等待垂直同步的位置可以不同，不能只比较 present 耗时。
- `vp4`：仅视频调用造成的上传次数/有效像素字节数、硬件帧下载次数，以及 renderer 生命周期累计导入次数。
  UI 字形上传不混入视频上传统计。旧帧缓存重画不增加新帧准备样本或实际上传数。
- `vp5`：renderer 管理的图像/导入字节数、map/pool 数量和在途 batch 数。
  它不覆盖 decoder 全部内部内存或进程总内存，也不能将 mapped+image 直接当成唯一物理内存占用。

分析一份日志，默认按每个 session/epoch/尺寸组合排除前 5 秒：

```sh
python3 scripts/analyze-video-perf.py deko.log --output deko-perf.json
python3 scripts/analyze-video-perf.py deko.log --baseline sdl.log --output comparison.json
```

脚本只使用完整的五行组，并对累计量取差、按对应帧数加权；丢失中间日志仍可跨采样计算。
计数倒退、epoch 切换不会产生负耗时；活跃期间无新帧的窗口仍计入 FPS 分母。
比较输出每个新帧节省的 CPU 准备微秒和百分比。基准必须采用相同游戏场景、分辨率、码率、
刷新率、设备模式、频率策略与诊断配置；脚本仅能检查其中的后端/解码/尺寸元数据。
旧版只有 `stats` 且耗时为 `—` 的日志无法反推出这些指标。每秒快照不能推导逐帧 p95。

deko 诊断构建另外每秒输出 `vq1/vq2`，采用同一快照 `id`，不影响旧的五行分析器：

- `vq1`：`attempts` 是 begin 尝试次数（不是完整主循环次数）；`nobatch` 是没有空闲
  GPU batch 的返回次数；`acquire` 和 `acquire_us` 是输出缓冲 acquire 的次数和 CPU
  累计耗时。均为 renderer 生命周期累计量，应取差分析。
- `vq2`：`polls/timeout` 是现有非阻塞完成 fence 查询次数/超时次数；一次 batch
  可以被查询多次，不能将超时次数当作丢帧数。`done` 是观察到完成的 batch 数，
  `residence_us/done` 的差分比值是提交命令前至 CPU 观察完成的平均驻留时间。
  它包括 GPU 队列等待、输出图像 release fence 等待及 CPU 回收采样延迟，**不是 GPU
  绘制耗时或物理屏幕延迟**。阶段边界的在途 batch 会混入后一段，应排除过渡窗口。

`vp3.draws` 只记录成功的视频绘制，不包含 BUSY 返回；`vp3.begin` 同样只覆盖成功路径。
即使持续 BUSY，主线程仍至多每秒复制一次资源/队列计数，避免诊断数据随成功绘制一起停更。
新增计数复用现有 fence 查询，仅在 acquire 和提交/回收边界读取 ARM 计数器，不增加 GPU
等待、逐帧日志或新线程；关闭诊断时不执行新增队列计数和计时。

扩展节奏诊断：

- `vq3`：每 16 个 batch 采一次 GPU 时间戳。三个位置为输出 fence 等待之前、之后、
  绘制末尾；前两项使用 pipeline-top，末项使用 timestamp report。`wait_ns` 是前两点
  的累计差，`work_ns` 是后两点的累计差，`n/bad` 是有效/无效组数，`wmax/xmax` 是
  renderer 生命周期最大值。工作区间包含屏障、清屏、视频和 UI，不是纯 shader 时间；
  不包含第一个时间戳之前的队列等待。报告格式/时钟换算遵循 deko3d 0.5.0 `Primer.md`
  Counters 和 `dk_variable.cpp`。报告可能扰动管线，稀疏采样不能保证捕获所有尖峰。
  每个 batch 额外 4 KiB CPU/GPU uncached 报告内存，仅原完成 fence 成功后读，退出在
  queue idle 后销毁。不开启诊断时不分配、不记录。无额外 waitIdle 或 CPU 等待。
- `vq4`：同一 epoch 的解码发布间隔 `<8 ms` / `>25 ms` 次数和最大间隔；两次 take
  之间发布数量为零/一/多次的次数，以及最大 take 间隔（微秒）。在现有 mailbox 锁内
  更新，不逐帧输出。固定阈值用于当前 60 fps 实验，不是通用掉帧判据。
- `vq5`：进入 decoder 后、send_packet 前的提交次数、压缩字节数、间隔 `>25 ms`
  次数及最大间隔。这不是网卡到包时间，包含上游组帧和 decoder 串行化影响，不能独立
  区分主机、网络和解码锁。`vp2.decode` 提供提交到解码输出的驻留时间作为交叉证据。
- `vq6`：主循环次数和 control/media/tail/sleep 累计墙钟微秒。control 包括 UI 状态、
  输入和 runtime 事件；media 包括 present 调用；collect 是 media 内部事件处理/资源
  回收的子区间，不应再次加进总耗时。main 每秒发布，和视频快照的采样边界不完全一致。
- `vq7`：主线程 CPU 执行 ticks、采样墙钟 ticks 和 svcGetInfo 返回码。仅 `rc=0` 且
  两次计数单调时，`Δcpu/Δwall` 可表达主线程占用一个 CPU 核的比例；不是全进程 CPU
  使用率，更不覆盖 NVDEC/GPU。接口失败保留错误码，不用墙钟耗时冒充 CPU 使用率。

### 产品自适应选帧诊断

Deko 产品调度由 `NSL_ADAPTIVE_PACING` 控制，默认 ON；算法输入在 diagnostics OFF 中也保留。
诊断版 `--no-adaptive-pacing` 可关闭本次运行的调度，供基线比较。
`vq8` 的 `a/p` 是同 epoch 累计发布/首次成功提交数，`ca/cp` 是已关闭 cohort 的计数，
`unknown` 非零表示元数据观测缺失，此时不把累计 cohort 当完整样本；`defer` 为主动延后重复绘制次数，
`tv/ts` 是在线估计的输入/输出周期，`active` 表示已启用且满足预测条件。重复绘制不增加 p。
不要把任意时间窗的 p 增量除以 a 增量当作严格 cohort 利用率，也不要以 fence 等待是否缩短判断收益。
这些日志由既有诊断 worker 输出，release 编译排除。

`vq9` 的 `ready_us` 是控制器具备预测条件的累计时间，`wait_us` 是主动等待的累计时间，
`start_us` 是首次发布至首次具备预测条件的间隔，`hold` 为输入观测中断时保留时钟的次数。
时间跨度过长的主循环暂停不计入状态占比；首次具备条件不等于利用率已经收敛。
该行由诊断 worker 输出，release 排除；控制器不使用这些计数决策。

### 正式客户端云端媒体自包含测试

`nsl-runtime-probe` 复用正式桌面应用，通过现有 UI 自动启动、显示视频、核对音频队列、
合成 SDL 手柄的报告内容及 ACK，再从菜单断开并退出。需 `NSL_BUILD_SNS=ON`、
`NSL_BUILD_TOOLS=ON` 和 `NSL_DIAGNOSTICS=ON`；外部服务验收不加入普通 CTest。
传入独立的已配对 profile 目录和 Steam 主机 LAN IPv4：

```sh
build/gns-mbed/app/nsl-runtime-probe /absolute/private-profile 10.10.10.166 --sdr-only
```

省略 `--sdr-only` 则保留自动 ICE/SDR。两种模式都仅在 fixture 进程内过滤指定主机
及广播 UDP，使正式 runtime 通过云端发现请求 SNS；不会修改电脑/路由器的防火墙。
指定 IP 必须是实际 Steam 主机，不能把该隔离称为不同出口 NAT 验收。

Switch 对应入口复用私有 RomFS 交付和加密回读，用户只准备 hbmenu Netloader：

```sh
python3 tools/switch-sns-runtime-probe.py --address 10.10.10.25 \
  --lan-host 10.10.10.166 --profile /absolute/private-profile --mode sdr --prepare-only
python3 tools/switch-sns-runtime-probe.py --address 10.10.10.25 \
  --lan-host 10.10.10.166 --profile /absolute/private-profile --mode sdr --run-only
```

`--mode auto` 保留自动选路。私有 NRO、profile 与诊断只保留在 ignored build 目录，
不要分发为发布包；同一身份不能同时运行于 PC 和 Switch。结果保存在私有 build
目录的 `runtime-result.json`，日志含阶段、错误、身份回读 ACK 与临时文件清理结果。
按下/释放均要求新的报告序号和按钮位、发送/ACK 进展以及空待确认队列；初始化全量
报告不能代替输入验收。合成事件不证明物理按键，音频队列不证明实际扬声器声音，
`nxlink` 正常退出也不证明 LCD 已返回 hbmenu。契约见 D-A071。

桌面 runner 另有 `--cloud-reconnect`（替代 `--sdr-only`）：保留首页并注入一次 CM
读取错误，将该 provider 保持停止更新 20 秒，核对 UI 离线、新匿名连接和重订阅后
恢复在线，再正常退出。不启动媒体；其余网络继续正常工作。这是客户端进程故障环境，
不代表真实服务器 Offline、退订或不同 NAT 出口的验收。

`nsl-remote-lifecycle-live` 是不启动媒体、不创建 LAN discovery socket 的桌面云端
生命周期验收工具。手动运行，不能放进无人值守 CTest：

```sh
build/desktop/app/nsl-remote-lifecycle-live --pair-cycles
build/desktop/app/nsl-remote-lifecycle-live --subscriptions /absolute/private-profile
build/desktop/app/nsl-remote-lifecycle-live --offline /absolute/private-profile
```

`--pair-cycles` 使用临时随机安装身份，申请的 PIN 不输出、不要求主机输入，随后自动取消。
它在接收入口暂存真实 StartPairing 回复，核对取消后迟到 PIN 的清理、发布后取消，以及
丢弃一次回复后真实等待 RPC 超时、重建 session、重新申请和取消；不修改产品协议。
`--subscriptions` 使用两个只读发现会话，以其中一个为持续订阅对照，另一个退订后排空
两秒再观察 30 秒；同时要求原始接收层的退订侧 status 为零、对照侧收到 status，随后
重订阅恢复。若对照期没有通知，只能报告 INCONCLUSIVE，不能以本地不再分发 callback
代替服务端退订证明。`--offline` 打印 READY 后各等待最多三分钟，需先让主机完整退出
Steam，再重新打开；要求真实服务 Offline、匹配目标的回调和在线恢复，期间 CM 没有断线。
后两种模式独占私有 profile，不得同时启动其他使用此身份的测试或客户端。

`nsl-remote-tests` 的密钥中断测试采用合成主机输入、真实密码算法和生产 profile 存储：
在保存新 secret 后、Proof 入队前，以及入队后直接退出子进程；父进程重新加载身份，
验证请求 token 和后续 session key 使用正确密钥。保存失败另验证不发送 Proof、旧身份
可读。这证明本地持久化与重启契约，不证明主机端回滚规则或设备断电时文件系统耐久性。

`--cloud-initial-failure` 保留首页，将真实目录首轮 CM 候选的 TLS 完成结果替换为
连接失败，检查逐个轮换、耗尽后的 10–30 秒退避，以及下一轮真实匿名连接/主机发现。
`--cloud-initial-bridge` 对同一 provider 显式设置 bridge_mode，核对耗尽后立即重试的
配置分支；不启动串流，也不代替完整媒体 bridge 重连验收。两个模式总期限为 180 秒，
均只作用于当前桌面测试进程，日志只输出计数、数值错误和延迟。

`--sdr-loss-soak` 使用正式 SDR 客户端，输入内容与 ACK 验收后，对测试进程的公网
IPv4 UDP 双向注入约 1% 固定种子伪随机丢包，持续 180 秒，核对显示帧进展和新鲜度，
再经菜单正常断开/退出；总期限为 240 秒。此模式会启动桌面串流。每五秒记录收发/丢弃
计数与媒体计数，不输出地址或 payload，不改全局网络。它不覆盖突发断路、jitter、
跨 NAT 回切或数小时稳定性。详细验收门槛见 D-A071。

`--sdr-abrupt` 在正式客户端完成显示与输入内容/ACK 验收后，仅在进程内双向屏蔽当时
primary 的一个 UDP endpoint，保留其他地址；不追随新 primary 改写屏蔽目标。要求不同
POP 的有效 E2E 与至少 30 个新显示帧，保持五秒且末尾一秒仍有 E2E/新帧，再正常菜单
断开；等待恢复最多 35 秒。记录 SDK 终态及 type20/type21/CM 关闭元数据，失败走正式
清理路径。这是媒体故障实验，会启动桌面串流；与独立 CLI 的编码帧 fixture 分开记录。

`end_game_status`记录本地结束请求期间LAN/cloud状态的present/running/timestamp和
累计empty；同client/instance且两条递增无游戏状态才完成，ACK不等于游戏已退出。
`update_notice available/modal_open/modal_closed`用于区分版本请求完成与弹窗遮挡，
不表示它暂停了runtime。三类记录均只在诊断构建，成功会话仍以connection route和
pipeline进度确认；新版连接/结束路径是否改善需真实复测，不以fixture作验收。


## 分层反馈矩阵与来源诊断

`nsl-feedback-probe.nro` 内嵌14条反馈命令，自动覆盖普通批量短震、body/trigger
独立停止、Sony模拟震动/模式切换与Nintendo原生样本。只需准备hbmenu netloader；
无需SD夹具或PC安装组件。部署前构建程序与验证器，然后显式指定用户确认的设备IP：

```sh
cmake --build build/switch --target nsl-feedback-probe_nro
python3 tools/switch-feedback-probe.py --ip 10.10.10.111 \
  --nro build/switch/app/nsl-feedback-probe.nro --output build/feedback-device.json
```

工具仅使用nxlink，不探测28280；保存JSON结果、原始log、命令/文件散列元数据，
部署期限最多30秒。nxlink退出正常不认证Switch回到hbmenu。
已有日志可独立复核，不接触网络：

```sh
python3 tools/check-feedback-matrix.py --log build/feedback-device.log \
  --output build/feedback-recheck.json
python3 tools/check-feedback-matrix.py --probe build/desktop/app/nsl-feedback-probe-host
```

第二条是同一夹具的host-simulation，真实HID/loader为终端替身，所有真机IPC、
机械及屏幕认证保持false。验证器要求固定刺激、来源、输出、间隔、计数及停止/
join/归零，拒绝旧Nintendo-only夹具冒充新矩阵。普通/trigger输入可为21B外壳，
SDK规范化为9B语义命令；matrix不认证被忽略的padding。Nintendo与Sony容器仍
按完整69B核对。参数读回与机械效果分别报告。

矩阵首组在第一条实际输出完成后才让第二条owner回调继续，强制覆盖队列
暂空的线程交错；`owner_worker_interleave_verified` 从输出完成/第二条apply
时间记录计算。旧日志不满足该顺序时不得认证为这项回归通过。普通的
`ordinary_played_interval_ns` 按submit-to-submit与接收间隔核对；另列
`ordinary_after_completion_interval_ns`，不能把IPC耗时误加为普通间隔的
要求。Nintendo的5ms最低保持仍按上一提交完成时刻核对。

串流客户端的 `haptics` 追加 ordinary_commands/trigger_commands/sony_commands、
适配/不可还原、替换/丢失/过期/失步、PCM本地帧等计数；
`pcm_steam_source_ready=false` 表示没有声明Steam专用PCM来源。
`haptic-trace <serial>` 的 `source` 为0普通、1Nintendo、2trigger、3Sony、
4到期、5本地归零、6本地专用PCM。kind仍为1命令、2输出、3清理；kind4是
显式typed PCM本地输入，没有伪造wire。unsupported保留不可还原原因位。
Sony 47B控制包不是触觉PCM；不能把所有0d都按Nintendo解码。旧归档验证器
不认识新来源时应拒绝，而不是把它认证为已验证的游戏高级触觉。

### 本地原生震动体验

`nsl-haptics-experience.nro` 先运行上述14命令矩阵，再显示并自动播放七个
各四秒的本地场景：普通持续震动、数值幅度渐变、80/160/320/640Hz分段、
左右交替、40ms点击与600ms保持、双载波升调、双频组合交替。方向键上重播、
左右切换并播放、下从头播放全部，`+`立即停止并退出；整个交互最多五分钟。
无需Steam、游戏或SD资源。两侧定位要求两只震动器；数值幅度相同不代表
不同频率的主观强度相同，线性数值渐变也不代表感知线性。
第三组的80/160Hz来自低频参数、320/640Hz来自高频参数，幅度编码均为64
（约0.25547）。此示例同时改变频率和频带，不能据此隔离频率响应或认证四档
机械效果；要定位高频弱的原因，须另行固定频率比较两频带，并逐级比较幅度，
同时采集活动参数读回。不得仅凭相同数值幅度就推断用户应感到同样明显。

```sh
cmake --build build/switch --target nsl-haptics-experience_nro
nxlink -a 10.10.10.111 -r 1 -s build/switch/app/nsl-haptics-experience.nro
ctest --test-dir build/desktop -R '^nsl-haptics-experience$' --output-on-failure
```

这些刺激通过真实`DeviceWrite -> SDK owner -> 原生worker -> HID IPC`路径，
而不是绕过客户端直接调用HID。七个场景用Generic01与Nintendo10B命令
独立构造；不模拟Sony自适应扳机，不将合成声音冒充游戏专用触觉PCM。
前置矩阵的日志仍可由`check-feedback-matrix.py`单独检验。
场景逐次stop/join/归零后输出`demo-scene`、`demo-stopped`、`demo-send`，
实际播放和输入循环不写网络日志；场景之间重建worker以完整保存有界trace。
最后的`demo-end`与原矩阵`end`都须采集，不能仅凭nxlink传输成功认证完成。
每组join后最多100ms、间隔5ms读取停止参数，完整保留`demo-stop-readback`
序列；不要求成功提交后第一次读取就已变成零，也不将该期限解释为机械停止
保证。桌面终端额外模拟40ms的停止读回延迟，防止立即读回的假设漏测。
桌面终端替身检查幅度级数、频率集合、空间分侧、点击/保持时长、升调及归零。
体验能区分效果只证明本地输出具备相应表达能力；既不证明Steam已发送该源，
也不能据此判断某个游戏没有支持。软件提交、系统参数读回、机械响应与用户
主观辨别仍分别记录。
