# 官方 Steam Link 输入路径逆向调研

> **2026-09-07 证据更正**：本文包含相互矛盾的历史结论。
> 解密递增时机、NACK 与帧统计的独立复核见
> [协议参考 §14](STEAMLINK_PROTOCOL_RE.md#14-独立汇编复核与协议反例2026-09-07)。
> §18 不能用作“已排除客户端协议问题／NVENC 是卡死根因”的证明。

> **2026-09-04 更正与拆分**：本文档的协议层结论已由系统再逆向复核，
> 新权威参照为 `docs/STEAMLINK_PROTOCOL_RE.md`。对本文的具体更正：
> ① §5 对 host 日志 `CLIENT:` 前缀的解读有误——官方客户端二进制中不存在 SetQoS(87)/
>    SetTargetBitrate(94)/VideoEncoderInfo(90)/SetTitle(81)/SetActivity(98)/TouchConfig 下行族
>    (110/112/114/116) 的任何发送代码路径（SendControlMessage 全部 35 个调用点已枚举），
>    这些消息全部有 On* 接收 handler，故 `CLIENT:` 前缀 = host→client 方向，
>    287 条记录不能作为"官方客户端发送控制对话"的证据；
> ② §8 候选路线 B（客户端补发 SetQoS/SetTargetBitrate）前提作废；
> ③ §15/§16 `CStreamFrame::Send(conn, bool)` 的 bool 映射写反：true=可靠 Send，
>    false=SendUnreliable；且 SendControlMessage 两路均硬编码 reliable=true——
>    "官方输入可能默认走不可靠通道"的猜测不成立；
> ④ §6 【未知】项部分落地：官方把 host InitMsg 的 reliable_data 回显进自己的
>    NegotiationSetConfig.config.reliable_data（无条件字节拷贝）；
> ⑤ §13.2 "时间戳单位 bug 已修（ms）"方向反了：官方 GetStreamTimestamp 是
>    **16.16 定点秒**；旧文进一步解释为“减连接起点”是错误的，已撤回。
>    实际为首事件加时钟偏移，后续事件间 delta，见协议参考 §14.4、§14.10。
> 输入链的完整报告、active_input、禁用门控也须按 §14.10 的调用链解释；
> §14 "不合并批次"表述由 STEAMLINK_PROTOCOL_RE.md §9b.2 更正（官方单消息多设备合批）。

> 目的：排查串流中偶发 host/game 输入无响应约 20 秒。旧文“客户端可观测手段已用尽”
> 没有成立；传输确认不等于 host 应用确认。本文逆向 Android 客户端（v1.3.32）的输入路径，
> 与 ihslib 实现逐项对照，为后续对齐提供证据基础。
> 证据纪律：每条标注【已验证】（工具/地址可复核）或【推断】（由证据支持但未直接观测）。
> 日期：2026-08-29。

## 1. 材料与方法

| 材料 | 来源 |
|---|---|
| `Steam Link_1.3.32_apkcombo.com.xapk`（含 base + 4 ABI split） | 用户提供 |
| `lib/arm64-v8a/libmain.so`（15.9MB，含流媒体核心，24922 个动态符号，C++ 符号未剥离） | xapk 解包 |
| `lib/arm64-v8a/libshell_arm64-v8a.so`（23.1MB，Qt 外壳） | xapk 解包 |
| 工具 | radare2、devkitPro aarch64 objdump/nm、c++filt、strings |
| 对比 pcap | `test.pcapng.gz`（我们的会话，已删除，数据以本 issue #1 评论记录为准）；`good.pcapng.gz`（官方 Android 平板会话，4m40s，1.1M 包） |
| host 日志 | kxn-pc `Steam/logs/` 的 `streaming_log.txt`、`remote_connections.txt`（用户提供） |

## 2. 官方客户端架构（符号级还原）

【已验证】动态符号表枚举（nm -D + c++filt，libmain.so）：

- 流媒体会话核心：`CStreamClient`（连接/协商/控制包处理）、`CStreamPlayer`（媒体与输入回放）。
- 输入子系统：
  - `CStreamPlayer::CHIDDeviceReportThread::Run` —— 专用报告线程；
  - `CStreamPlayer::CHIDDeviceReportGenerator` —— 报告生成器，按设备类型多态：
    `BInjectGamepadStateGenericGamepad / SteamController / MobileTouch / TritonController`；
  - `EncodeDelta`、`AllocateBuffer / SendBuffer / ReleaseBuffer` —— delta 编码与缓冲池；
  - `CreateInputMark / FinishInputMark / StartPlayingInput` —— 输入标记与回放（latency test 用）。
- 输入数据模型：`CRecordedInput`（单条输入事件，带 `type`（EStreamControlMessage 值）与
  `timestamp`，oneof 含 touch/mouse/key/**hid**）与 `CRecordedInputStream`（repeated entries，
  时间戳事件批量流）。proto 定义见 ihslib `protobuf/remoteplay.proto`（Valve 同步版）。
- 传输层多实现：`CStreamTransportUDP`、`CStreamTransportSteamNetworkingSocketsBase`、
  `CStreamTransportSDR`、`CStreamTransportClient`。
- 音视频：FEC 队列（`CFECIncomingQueue/CFECOutgoingQueue`）、Android 硬解
  （`CAndroidVideoDecoder`）、SDL3。

## 3. 官方输入发送路径（SendBuffer 调用链）

【已验证】`CHIDDeviceReportGenerator::SendBuffer(CUtlBuffer*)` @ `0x7d00f0`，
objdump（devkitPro aarch64-none-elf-objdump -dC）解析调用：

- `CUtlQueueBaseImpl::ImplAddToTail(unsigned char**)` —— 编码后的报告**入队**（消费者为
  `CHIDDeviceReportThread::Run`，与生成器解耦）；
- 构造 `CHIDMessageFromRemote` → `DeviceInputReports` → `CHIDDeviceInputReport`（与 ihslib
  同一 protobuf 消息结构）；
- **`CHIDDeviceInputReport::set_delta_report(void const*, unsigned long)`** —— 设置的是
  **delta 报告**。

【已验证】`CHIDDeviceInputReportGenerator` 侧存在 `EncodeDelta`；配套 proto 字段
（`hiddevices.proto`）：

```proto
message CHIDDeviceInputReport {
    optional bytes full_report = 1;
    optional bytes delta_report = 2;
    optional uint32 delta_report_size = 3;
    optional uint32 delta_report_crc = 4;
}
```

【已验证·全量反汇编交叉引用】（main.disasm 1,694,052 行，2026-08-29 补充分析）：

- **`set_full_report` 的 PLT（0xc10020）在全二进制中零调用**——官方 Android 客户端的
  输入报告**全部为 delta_report，从不发送 full_report**。初始状态同步依赖 delta 的
  "全掩码"能力（ComputeDelta 掩码覆盖全部字节时等价全量）与 host 侧中性初始化；
- 完整发送链：`ReportGenerator`（EncodeDelta 编码入队）→
  `BCollectReports(CHIDMessageFromRemote_DeviceInputReports*, bool*)`
  （收集全部生成器的报告，@ `0x7cf884`）→ `SendBuffer`（打包）→
  `ImplAddToTail`（报告线程队列）→ 报告线程发送；
- 产生节奏：pcap 实测 31/s 批量，与 30fps 游戏的逐帧收集一致。

【推断】delta 丢失的自愈路径：delta 带 size+CRC32，host 端 CRC 校验失败或基线状态缺失时，
通过 `CHIDMessageToRemote.DeviceRequestFullReport`（proto 字段存在，ihslib 已实现
客户端侧响应）请求客户端重发——客户端以"全掩码 delta"响应（set_full_report 无调用者，
全量只能以 delta 形态发出）。该路径未被动态观测证实。

## 4. 官方会话线上数据（pcap 实测）

【已验证】官方 Android 平板（LENOVO TB375FC，Android 16）会话 4m40s，
host 日志（streaming_log.txt 2557 行起）记录协商结果：k_EStreamQualityBeautiful、
50 Mbit/s（burst 250M）、Maximum capture 2944x1840@60、
`CLIENT: Sending HID device 0000/11fb/-1 Mobile Touch Control at touch://0`。

上行（平板→host）122/s 总量：

| len | 包数 | 速率 | 判定 |
|---|---|---|---|
| 82 | 24,228 | ~86/s | 视频/音频 ACK（混合） |
| **98** | **8,685** | **~31/s** | **输入批量**【推断：尺寸与 delta 批量特征一致，内容加密不可见】 |
| 21 | 583 | ~2/s | 控制/keepalive 类 |

下行（host→平板）：1465B 视频 ×1,014,774（60fps）、185B 音频 ×15,936（57/s）、
21B ACK ×27,292（97/s）。

【已验证】官方会话 pcap 窗口内（含用户报告的唯一一次卡死前后）上行/下行均无中断
（下行最大间隔 30ms）。

## 5. host 日志对比（kxn-pc streaming_log.txt）

【已验证】同一 host 上两次会话的 host 侧控制消息记录量：

- 官方平板会话：`CLIENT:` 前缀控制消息 **287 条**（SetQoS ×2、SetTargetBitrate ×2、
  ControllerConfigMsg ×18、TouchConfig 族 ×17、VideoEncoderInfo ×7、SetTitle/SetCursor/
  SetActivity 等）；
- 我们的会话（nsteamlink-switch，19:23 段）：**0 条**。

【已验证·代码】ihslib 客户端模式实际发送的控制消息全集（grep src 逻辑代码，排除
protobuf 枚举表）：RemoteHID、ClientHandshake、AuthenticationRequest、KeepAlive（5s）、
StopRequest、NegotiationSetConfig/Complete、InputTouch*/KeyDown/Up/Text、
StartVideoData/StopVideoData、StartAudioData/StopAudioData、VideoDecoderInfo、
cursor 族、EnableHighResCapture。
**SetQoS / SetTargetBitrate / ControllerConfigMsg / TouchConfig 族 / VideoEncoderInfo
（client 方向）从不发送**——与 host 日志的 0/287 差异互相印证。

【已验证】host 日志中我们的会话（含卡死会话）全程 `Slow framerate: ... (network)`，
network 计时值异常巨大（58518652ms 量级）——我们的帧反馈时间戳与 host 时钟不同步，
host 的网络瓶颈判定基于失真数据。官方会话同位置数值正常（未在摘录段出现异常值）。
【推断】此失真可能影响 host 的码率/节奏控制决策质量，与输入 apply 的关系未证实。

## 6. 协商差异

【已验证·代码】ihslib 客户端协商（ch_control_negotiation.c）：

- `OnNegotiationInit` **忽略** host 在 `CNegotiationInitMsg` 中宣告的
  `reliable_data / supports_remote_hid / supports_touch_input`；
- 发送的 `CNegotiationSetConfigMsg` 中硬编码 `reliable_data = false`。

【已验证·逆向】官方 `CStreamClient::OnNegotiationInit`（@ `0x7ad698`）**读取** host
InitMsg 的三个 bool 宣告（`ldrb [InitMsg+0x68/0x69/0x6a]` → 存入
`CStreamClient+0x158/0x159/0x15a`，并置 has-bit）——官方将 host 的宣告纳入客户端状态。

【未知】官方实际**发送/生效**的 reliable_data 值（SetConfig 的构造未定位到；host 日志
无 "reliable" 字样）。host 端对该字段的处理逻辑未知。

## 7. 20 秒卡死机制的当前结论（截至本文件）

【已验证】卡死会话（D-039/D-040 修复后）：丢一个可靠包（21020）→ 25+ 次重传零 ACK →
3s 放弃触发即恢复传输（giveups 计数、out 归零、新包立即确认）——**传输层自愈正常**；
同期 host ACK 延迟全程 ≤99ms——**host 传输层一直在正常收包确认**；
但用户输入无响应持续 ≈20s。

【结论】卡死的停留点在 **host 对控制流洞后消息的 apply 停滞**：host 的有序窗口在洞后
 hold 输入 apply，直至其内部计时器（估 ~20s）滑动窗口放弃洞并恢复——时长由 host 决定，
客户端的放弃/重传策略只影响自己的队列，不影响 host 的 hold 时长。

【未知】为什么 host 对我们的流会进入这种 hold（官方同环境未复现）：候选因素按当前证据
强度排序——(a) 输入批量/尺寸/速率模式差异（第 4 节）；(b) 控制对话缺失（第 5 节）；
(c) reliable_data 协商差异（第 6 节）。**目前没有任何一条被证实为因果。**

## 8. ihslib 现状与对齐候选

【已验证·代码】ihslib 的 delta 基础设施完整且休眠：`IHS_HIDReportHolderAddDelta`
（report.c:129）实现 `ComputeDelta`（changed-byte 掩码 + 变化字节）+ `IHS_CRC32` +
`delta_report/size/crc` 三字段——与官方 `set_delta_report` 的字段集完全一致。
当前 fork 的 SDL provider 事件处理器已不调用 AddDelta（D-034 时代移除），常态只发
`ReplaceWithFullForced`（full_report）。host 侧 `DeviceRequestFullReport` 的处理
（回全量）在 ihslib 与本项目中均已实现。

### 候选对齐路线（每条独立可证伪，均需真机验证）

| # | 路线 | 依据 | 验证判据 | 风险/成本 |
|---|---|---|---|---|
| A | 恢复 delta 编码：常态 AddDelta（带 CRC），100ms 心跳发 full | 官方 set_delta_report 实证；带宽降 ~70% | 输入包尺寸/速率对齐第 4 节；卡死是否变化 | **推翻 D-034"线上只发完整状态"的假设**，需新决策记录；delta 链在 host hold 场景下的行为需分析 |
| B | 补控制对话：SetQoS / SetTargetBitrate / ControllerConfigMsg（游戏切换时请求配置）| host 日志 287 vs 0；官方会话含完整协商 | host 日志出现对应记录；卡死是否变化 | 中；部分消息语义需进一步逆向 |
| C | 帧反馈时间戳校准 | host 的 network 计时失真（第 5 节）| SessionStats 的 network 值正常化 | 低 |
| D | reliable_data 对齐 | 见第 6 节 | 待 host 宣告值采集后评估 | **用户已否决无证据实验**；官方值逆向未完成 |

### 路线 A 的补充事实（2026-08-29 全量反汇编补充）

- 官方 `set_full_report` 零调用：**官方从不发 full_report**，初始/恢复态一律以"全掩码
  delta"表达；我们若对齐，心跳的 full 也应以全掩码 delta 或保留 full_report 字段发送
  （host 对两种字段的接受度未知，需实验）；
- ihslib 的 `IHS_HIDReportHolderAddDelta`（report.c:129）已实现与官方一致的
  delta+size+CRC32 三件套，属"休眠的完整基础设施"，恢复 = 重新接线而非新写；
- 上游 ihslib（8c5a17c）与 plume 实测即运行在此 delta 路径上；**但 plume 可用性用户无法
  独立验证，不作为因果证据，仅作为上游行为参照**。

## 9. 未决问题

1. 官方输入批量的帧可靠性选择（Reliable/Unreliable 帧）：加密流量不可见；D-030 实验曾
   观测"Unreliable 帧类型导致会话秒断"，与官方行为矛盾，说明该实验存在未理解的细节；
2. 官方 delta/full 触发策略与批量节奏的生成逻辑（需动态分析：模拟器 + Frida hook
   `CHIDDeviceReportGenerator::SendBuffer`）；
3. host 对控制流洞的 hold/丢弃计时器时长（~20s）与其是否依赖客户端行为的条件；
4. `reliable_data` 在 host 端的确切语义；
5. 官方 MobileTouch 虚拟设备（`touch://0`）与通用 gamepad 在 host 输入管线上是否
   走不同 apply 路径。

## 10. 与既有决策的关系

- 本文**不推翻** D-039（放弃窗口）与 D-040（单在途/槽位回收/每帧一次）：传输层修复
  已真机验证（不再永久楔死）；
- 候选路线 A 若实施，将**部分推翻 D-034 的"SDL 线上只发完整状态"**——需先写决策记录
  （含 delta 链 + host hold 场景的行为分析），再动代码；
- D-030 时代"delta 链丢失导致永久错状态"的结论需要修正表述：在 host `DeviceRequestFullReport`
  恢复机制（已实现）与 CRC 校验（ihslib 已有）配合下，delta 方案的丢包自愈路径是存在的，
  当年的放弃决策缺少对这套配套机制的分析。

## 11. 十轮全链路核验（2026-08-29，应用户要求启动；代码冻结直至收敛）

核验方法：每轮以一个独立视角走完整条输入链（采集→状态→编码→打包→传输→host 接收→apply），
对官方（逆向+pcap+host 日志）与 ihslib/我们（源码+diag 日志）逐环节比对。发现即记录，
**收敛（连续两轮无新发现）前不改任何代码**。

### R1 wire 编码策略 ✅

【已验证】官方 `SendBuffer`（0x7d00f0）对每帧状态做掩码差分后**自适应选择**：

```
deltaLen + 8 < fullLen  →  set_delta_report（掩码+变化字节）+ delta_report_size + CRC32(全态)
deltaLen + 8 ≥ fullLen  →  set_full_report（完整态直发）+ 重置内部计数
```

（反汇编原文：0x7d035c-0x7d03b8，`cmp x8,x26 / b.cs → full` 分支。）

**更正**：此前"set_full_report 零调用"的断言系 grep 制表符不匹配造成的假阴性，作废。
官方 delta/full 双模自适应；本文件第 3 节相应段落以本条为准。

### R2 报告生成与排队 ✅（触发节奏需动态确认）

【已验证】生成与发送解耦：generator 编码入队（`ImplAddToTail`），
`CHIDDeviceReportThread::Run`（0x7c9e40）为独立消费线程；
`BCollectReports`（0x7cf884，虚表调用——静态 xref 不可达，虚表分派）收集全部生成器。

### R3 传输封装 ✅

【已验证】`SendRemoteHIDMessage`（0x7ab374）：
`CHIDMessageFromRemote` 序列化 → 包进 `CRemoteHIDMsg.data` → 同时录入
`CRecordedInput`（`GetStreamTimestamp()` 时间戳，type==0xd 即 hid）→
**`CStreamClient::SendControlMessage(EStreamControlMessage, MessageLite const&)`**（0x7a7d90）→
`CStreamFrame(type, channel)` → `Put` → `BEncrypt(seq)` → `Send`。
与 ihslib 同构：输入走控制通道、加密、带序列号。**输入的可靠性级别未在此层见到
特殊分支**（帧类型参数来源于更上层，R4 继续）。

### R4 丢失恢复 ◐

【已验证·字符串】传输层序列跟踪五计数器：`recvseq / inorder / dup / lurch / ooo`
（官方术语 "lurch" = 乱序落点）。"Sender sent reliable stream pos… expected next…"。
【未知】host 对控制流洞后消息：hold 后按序补 apply，还是丢弃（客户端 delta 链基线
失配 → CRC 失败 → RequestFullReport 恢复）。两种行为与观测均兼容，需动态实验区分。

### R5 时序与速率 ✅

【已验证】官方输入批量间隔（8684 个间隔）：均值 30.5ms、中位 11.1ms、p90 56.8ms、
p99 414.6ms、最大 1222ms；89% 的间隔 <50ms——**事件驱动逐帧批量，无固定速率**；
221 秒中仅 3 秒无输入批量。

### R6 会话生命周期 ✅（2026-08-29 修正：用户指出后重核，此前推断有误）

【已验证】pcap **完整覆盖**了会话起点（用户重启 app 后开始抓包，信标首包即入）。
host 日志显示 8-29 00:11-00:15 共三次会话（app 重启所致），最终会话（:46304，
00:15:00 起，直进游戏 Girls Made Pudding）自首秒起即有输入批量流动：

- 会话第 1 秒起输入批量（98B）即以 ~41/s 流动（前 49 秒含 2033 个 98B 包），
  **无初始 full 同步**——host 侧虚拟设备中性初始化后，客户端直接以 delta 流积累状态，
  与 `set_full_report` 零调用互证；
- 官方 app 重启 = 新端口新会话（48554→55851→46304），host 每次均正常重建流。

【结论】ihslib 对齐时会话起点无需初始 full：直接进入 delta 流即可（host 中性初始化
+ delta 掩码差分天然从零基线开始）。此前的"pcap 未覆盖会话起点"推断有误，已修正。

### R7 host 接收行为 ✅（现象确认，成因未知）

【已验证】官方会话 host 记录 287 条 CLIENT: 控制消息；我们会话 0 条（ihslib 代码证实
相关消息我们不发）。另：我们会话的 host 帧反馈计时失真（network≈58518652ms）。

### R8 协商能力 ◐

【已验证·代码】ihslib 已加日志输出 host InitMsg 宣告的
`reliable_data / supports_remote_hid / supports_touch_input`（3d16df4），
待下次真机连接采集。【未知】官方实际生效值。

### R9 静态-动态交叉 ✅

【已验证】R1 的自适应策略可解释观测的双簇尺寸分布（82B=轻中度变化的 delta，
98B=大 delta/多设备批量）；无 >140B 的独立 full 态包簇——与"full 仅在
delta 压不下时出现"一致（出现的 full 混在 98B 簇中不可分）。

### R10 端到端事件追踪 ⬜

待 R4 的 host hold 行为区分实验后进行（需动态注入或 Frida）。

## 12. 收敛状态

已产生新发现的轮次：R1（自适应规则，推翻 D-041"不出现 full_report"的假设——
**D-041 附录需再修正：全掩码 delta 应改为官方的自适应规则**）。
收敛判定：未达成。代码改动继续冻结（D-041 已实施的 delta flush 与本轮发现冲突的
部分，待 R2-R10 完成后一并修正）。

## 13. 客户端→host 消息字段逐项审计（2026-08-29）

审计范围：ihslib 客户端模式实际发送的全部控制消息（代码 grep 验证全集），逐字段核对
来源与官方对照。**本节只记录，不含任何代码修改。**

### 13.1 NegotiationSetConfig（三层）

已填字段：config（reliable_data=false、enable_remote_hid=1、音视频编解码选择、
分辨率/帧率）、clientConfig（quality=Balanced、1280x720@60、6000kbps、audio 2ch、
HEVC off、performance_overlay=true、controller_overlay_hotkey="auto"）、
caps（system_info、system_can_suspend、max_decode 30000/burst 90000、codecs、
form_factor=TV）。

发现的问题：

- P1【确认】`supported_colorspaces` 未声明 → host 会话选 BT.601 limited。
  HD 内容按 601 采样存在色彩精度损失。修复方向：声明 BT.709/601（含 Full）。
  对照：官方会话 host 日志无 "Capture colorspace" 行（成因未知，见 P9）。
- P2【确认】`display/quality/runtime/decoder_limit`（CStreamVideoLimit）未声明。
  官方二进制存在该消息族的使用痕迹（待动态确认取值）。
- P3【确认】`enable_unreliable_fec` 未设（false）。与 FEC 队列类
  （CFECIncoming/OutgoingQueue）及输入批量是否走 FEC 通道的问题相关（第 9 节）。
- P4【确认】`reliable_data=false` 硬编码。官方流程从 host InitMsg **读取**该宣告
  （0x7ad6fc），声明方向不同；官方生效值未知。
- P5【观察】caps.system_info 为硬编码假串（263MB RAM、1 核、JN-MD133BFHDR 面板）。
  host 端用途未知。
- P6【观察】quality=Balanced/6M/720p vs 官方 Beautiful/50M/2944x1840——有意低配，
  非缺陷；但影响 SessionStats 的对比基线。

### 13.2 其余消息快审

- AuthRequest：version/steamid/HMAC-SHA256("Steam In-Home Streaming") token ✓ 标准；
- ClientHandshake：network_test=true ✓；
- KeepAlive：空消息，5s 周期 ✓（host 有 ping timeout 断连机制，间隔待对照）；
- StartVideoData：**客户端不发送**（host 推送启动）✓ 与官方一致；
- RemoteHID wrapper：has_active_input=true + active_input=true（0010 补丁），
  官方同字段的取值未逆向（低优先）；
- 帧反馈：CFrameStatsListMsg（k_EStreamStatsFrameEvents，stats 通道，1s 周期，
  reliable）——时间戳单位 bug 已修（见第 12 节前文）；
- 包头 sendTimestamp：ms 修复已推送（ab8712d）。

### 13.3 官方会话 host 日志的独有现象

- 官方会话段无 "Capture colorspace" 行（我们会话均有 BT.601 行）——成因未知（P1 相关）；
- 官方会话段 `Slow framerate: game 16.00` 常驻（游戏自身帧率 16fps，
  非网络/客户端问题）。

### 13.4 审计状态

字段审计完成度：NegotiationSetConfig ✓、AuthRequest/ClientHandshake/KeepAlive ✓、
StartVideo/Audio ✓（不发送）、RemoteHID wrapper ✓（active_input 待官方对照）、
帧反馈 ✓（时间戳已修）、ACK/包头 ✓（ms 已修）。
colorspaces 官方构造点、ControllerConfigMsg 官方载荷：静态到边界，需动态分析（Frida）。

## 14. R2 补充：官方输入报告线程（全量反汇编解析）

【已验证】`CStreamPlayer::CHIDDeviceReportThread::Run()`（0x7c9e40）：

```
while (m_bRunning) {
    UpdateHIDDeviceReports();     // ← 收集所有设备 delta/full + SendRemoteHIDMessage
    ThreadSleep(8);               // ← 8 毫秒
    if (loop_count % 125 == 0) {  // ← ~每秒一次
        UpdateHIDDeviceInfo();    // ← 更新设备信息
    }
    if (has_virtual_controller) {
        UpdateControllerState();  // ← 虚拟控制器状态
    }
}
```

- `ThreadSleep(8)` = **125Hz 发送频率**，每次循环遍历所有设备收集 delta/full 并立即发送
- `UpdateHIDDeviceReports()` 内部（0x7bd838）：
  - 持锁遍历设备列表
  - 对每个设备调用虚函数 `BCollectReports(DeviceInputReports*, bool*)` 收集
  - **有数据就立即调 `SendRemoteHIDMessage` 发送**（不等 ACK、不合并批次）
  - 无数据的设备构造 CloseDevice 消息（设备已断开的通知）

【结论】官方输入不是事件驱动（"有变化才发"），也不是低频心跳，而是**固定 125Hz 高频
轮询发送**——每次都收 delta（或全掩码 delta 当变化太大时），不管 host 是否确认上一包。
单在途限制在官方架构中不存在——125Hz 高频本身就是丢失容忍策略的一部分：
丢一个 delta 8ms 后下一条就到了，不需要重传也不需要等待。

【与我们的对比】
- 历史实现（D-041 后，已由 D-042 取代）：每帧一次 flush（~60Hz 或 vsync 率）+ 100ms 心跳 full + 单在途
- 官方：125Hz 专用线程 + delta/full 自适应 + 不等 ACK + 不限在途

【含义】如果要对齐官方，关键是 **8ms 高频 delta 线程**（而非我们的每帧 flush），
以及**放弃单在途限制**（允许连续多包在途——官方就是这样做的，丢一个不影响后续）。

## 15. R3 完成：传输封装与可靠性选择（全量反汇编解析）

【已验证】`CStreamClient::SendRemoteHIDMessage(CHIDMessageFromRemote const*, bool reliable)`（0x7ab374）：

```
1. 检查连接状态（== 6 即 k_EStreamConnectionStateConnected）
2. CHIDMessageFromRemote::ByteSizeLong() → 序列化到 buffer
3. 构造 CRemoteHIDMsg → data = 序列化后的 DeviceInputReports
4. GetStreamTimestamp() → CRecordedInput.timestamp（16.16 秒时间戳）
5. CRecordedInput.type = 0xd (13 = hid)
6. CRecordedInput.hid = CRemoteHIDMsg
7. 加入 CRecordedInputStream.entries（repeated 批量）
8. CStreamClient::SendControlMessage(k_EStreamControlRemoteHID=106, CRemoteHIDMsg)
```

【已验证】`CStreamClient::SendControlMessage`（0x7a7d90）：
```
CStreamFrame(type=106, channel)
CStreamFrame::Put(protobuf_msg, bool)
CStreamFrame::Send(conn, bool)  ← bool 参数选择路径：
  → CStreamConnection::Send()            (可靠有序)
  → CStreamConnection::SendUnreliable()  (不可靠)
```

【已验证】`Send()` 的 bool 参数决定走 `Send()` 还是 `SendUnreliable()`——
官方客户端有**两条路径**：可靠有序通道 和 不可靠直发通道。

### 对比 ihslib

ihslib 的 `IHS_SessionChannelControlSend` 只有一条路径：所有控制消息（含 RemoteHID）
走 Reliable 有序通道。无 Unreliable 选项。**官方输入可以走不可靠通道，
ihslib 不能——这是架构级差异。**

### 125Hz 不等 ACK 的原因

官方 125Hz 输入线程每次产生新报告，如果走不可靠通道：丢一个 delta 8ms 后下一条就到，
无需重传——**高频不可靠 delta 是官方的丢失容忍策略核心**。
如果走可靠通道，125Hz 会严重塞满上行（每包都需 ACK + 重传），上游 ihslib 注释
已警告过此问题。官方可能默认走不可靠通道用于输入。

## 16. R4 补充：SendRemoteHIDMessage 的可靠性与调用者分析

【已验证】`SendRemoteHIDMessage(msg, bool)` 三个调用点（0x7bd74c / 0x7bd978 / 0x7bd9d8）：

- 前两个传 `w2=wzr`（false）→ `SendControlMessage` → `CStreamFrame::Send(conn, false)`
  → `Send()` 走**可靠有序通道**
- 第三个（0x7bd9d8，UpdateHIDDeviceReports 内的批量发送路径）bool 来自变量——
  需要跟踪上下文确定

【已验证】`SendControlMessage` → `CStreamFrame::Send(conn, bool)`:
```
Send():
  if (bool) → CStreamConnection::SendUnreliable()  
  else      → CStreamConnection::Send()
```

【结论】官方输入至少部分走可靠通道。但 `Send()` 存在 `SendUnreliable()` 路径
意味着官方**具备**不可靠发送输入的能力（可能用于高频场景下的降级）。
ihslib 只有 `IHS_SessionChannelControlSend` 一条路径（全可靠有序）。

传输层序列跟踪（字符串证据）：`recvseq/inorder/dup/lurch/ooo` 五个计数器
——官方传输层独立跟踪乱序/重复，与 ihslib 的 window.c 不同。

## 17. Generic Gamepad wire 模式定论与 is_generic_gamepad 语义（2026-09-07）

背景：上一构建移除 `is_generic_gamepad`（意图让 host 按真实 NS Pro 处理）后
真机实测 host 完全不认输入。本轮对 libmain.so 做了生成器/解析器全链路反汇编，
得到决定性证据。

### 17.1 设备类型→wire 格式选择（ReportGenerator 构造函数 0x7cf2bc）【已验证】

`CHIDDeviceReportGenerator::CHIDDeviceReportGenerator(player, deviceId, IHIDDevice*,
info, int, int)` 对 `CHIDDeviceInfo` 拷贝执行：

```
this+144 = GuessControllerType(info.vendor_id, info.product_id)  // EControllerType
this+148 = wire 类型：
  if (info 偏移 88 处 bool != 0)   → 4 (Generic)
  else switch (EControllerType):
    2,3,4            → 1 (SteamController)
    0xa (10)         → 2 (Triton = SteamControllerV2，BIsTritonDongle VID 0x28de)
    0x2b (43)        → 3
    100..129 ∩ 0x04010007 → 1
    其他             → Log + AssertMsgHelper("unknown controller type")
```

EControllerType 的 Nintendo 值（辅助函数位掩码解码，均【已验证】）：
`BIsSwitchController`（0x745354，掩码 0x01800440000000）→ t ∈ {38,42,51,52}；
`BIsJoyconController`（0x745370）→ t ∈ {39,40,41}。

【结论】**Nintendo 手柄类型不在任何 wire 分支内——没有 generic 标志时，
官方代码对 Nintendo 设备直接走到断言。"声明真实 NS Pro VID/PID 且不带
generic 标志"在官方客户端中不是合法发送模式。** 与真机观察（移除标志后
host 完全不认输入）一致。

### 17.2 官方为 SDL 手柄声明 is_generic_gamepad（EnumerateDevices 0x75480c）【已验证】

`CHIDDeviceListSDL::EnumerateDevices` 构造 CHIDDeviceInfo：

```
w22 = (SDL_GetGamepadType(pad) != XBOX360)
usage = w22 ? 5 : 4
has_bits |= 0x380            // interface_number + ostype + is_generic_gamepad
if (w22) { has_bits |= 0x100000; *(bool*)(info+88) = 1; }  // 0x754bbc
vendor_id/product_id = SDL 真实 VID/PID（GuessControllerType 的输入）
caps_bits/caps_bits2 由辅助函数从 SDL gamepad 能力计算
```

【结论】官方 Android 客户端对 **除 Xbox360 型之外的一切 SDL 手柄
（含 Nintendo Pro/Joycon/PS）** 都置 `is_generic_gamepad=true`，同时保留
真实 VID/PID。我们的 InfoFromHID 恢复该标志即与官方一致。

【2026-10-24 更新】0.7.3 起 Switch 枚举补回真实 `057e:2009`，同时保留
`is_generic_gamepad=true` 和 Generic RAW V3；0.7.2 及更早构建报
`0000:0000` 是 devkitPro SDL 名字型 GUID 使标准解析失败，不是有意省略身份。

### 17.3 Generic wire 格式 = RAW V2 结构体，版本字节 3【已验证】

`CStreamPlayer::CHIDDeviceReportGenerator` 的两个分发器按 this+148 分发
（`BParseGamepadState` 0x7d0044 / `BInjectGamepadState` 0x7d0074）：
1=SteamController、2=Triton、4=Generic（Pack 内联）、其余 false。
`BParseGamepadStateMobileTouch`/`BInjectGamepadStateMobileTouch` 为 8 字节
空桩——触摸不走生成器（对应独立 control 消息族）。

`BParseGamepadStateGenericGamepad`（0x7d089c）双模式，由输入 byte[27] 选择：
- byte[27]==0（ENCODED）：axes = u16[6] @0..11；buttons = byte[12..26] 展开
  为 state+16 u32 位域 bit0..14；随后 state byte[27] = 3（0x7d0a50）。
- byte[27]!=0（RAW）：`memcpy(state, input, min(len,72))`；version≤2 时
  flags `orr 0xc00`（bits 10,11）兼容修正。

客户端链路（BCollectReports 0x7cf884，【已验证】）：
本地 IHIDDevice 读报告 → Parse（Generic 模式，本地报告为 ENCODED）→
`CStreamPlayer::BFilterGamepadState`（0x7bc5c8）过滤 → 注入 wire 缓冲
（Generic= `Pack(state, buf, cap)`，0x7cfd8c → RAW 模式输出整个 V2 结构体）。
Parse 失败的原始报告整条丢弃。

**官方 wire 上的 Generic 报告 = RAW 模式 V2 结构体（最长 72B），byte[27]=3**：
```
[0..11]  axes 6×s16 (LX,LY,RX,RY,LT,RT)
[12..15] flags u32（version=3 时 host 不做修正；官方值未逐位确认）
[16..19] buttons u32，EGamepadButton 位
[20..26] 0
[27]     版本字节 = 3（RAW 选择器）
[28..71] IMU/touch 区；gyro s16[3] @28、accel s16[3] @34、touch @40
```
ENCODED 28B 格式只存在于本地 SDL 层与生成器之间，不上 wire。
host 两种模式都能解析（同一函数），但 RAW+版本 3 是官方实际发送形态。

> **2026-10-22 更正**：上表 [28..71] 不再视为“布局未确认、发 0”。
> 官方 Linux 1.3.9.258 客户端（SHA-256 `191cc079…c149e4`）的
> `CHIDDeviceSDLGamepad::OnSensorEvent @0x269740` 明确写入 gyro `+0x1c`
> 与 accel `+0x22`，并置 flags bit1/bit2；`BInjectGamepadStateGenericGamepad
> @0x2dac70` 在 byte[27]!=0 时最多拷贝 56B。旧的“发 0”实现会丢弃全部
> IMU。单位与 Switch 接入见 §19。

### 17.4 EGamepadButton 枚举值（名表 0x3dabc4 反解，27 项）【已验证】

```
0 BeforeFirst   1 Y(north)      2 DPad_Down    3 A(south)
4 RStick_Right  5 RStick_Up     6 R3           7 RStick_Left
8 LB            9 RB           10 Start       11 Select
12 LStick_Left 13 Trigger_Left 14 Trigger_Right 15 X(west)
16 Steam(guide) 17 LStick_Right 18 L3         19 DPad_Up
20 LStick_Down 21 LStick_Up    22 RStick_Down 23 AfterLast
24 B(east)     25 DPad_Right   26 DPad_Left
```
按钮位域在 state+16（`SetButton` 0x761094：u32 @+16，索引<0x20 断言）。

### 17.5 SendBuffer 发送语义（0x7d00f0）【已验证】

- 与 lastSent（this+328）len+memcmp 相同 → 整包回收**不发送**（去重）；
- 否则填 `DeviceInputReports`（proto 指针存 this+256）：
  `device` = 设备 managed id；`full_report` 整包，或自适应
  **delta_report**：逐字节变化位图 + 变化字节，仅当 `deltaLen+8 < fullLen`
  才发 delta，附 `delta_report_size`=fullLen、`delta_report_crc`=CRC32；
- delta 用的缓冲含上次发送内容（this+320/328 链）。

我们 report.c 的 AddDelta/去重/自适应规则与之一致（本轮已对照）。

### 17.6 生效代码改动

- `control_hid.c InfoFromHID`：恢复 `is_generic_gamepad=true`
  （17.2 证据；2026-09-07 真机移除标志 → host 不认输入的直接回归证据）；
- `sdl_hid_common.h`：`IHS_HIDReportSDLPackWire` 重写为 RAW V2（按钮经
  SDL3→EGamepadButton 映射，byte[27]=3），`IHS_HIDDeviceSDLWireReportLength`
  = min(host 通告长度, 72)；
- 历史 `sdl_hid_event.c` 三条路径（flush delta / resync full-mask / neutral reset；full-mask 依据已撤回）
  统一走共享 packer，缓冲 48→72。

### 17.7 遗留矛盾（待验证，不影响本轮修复）

旧 ihslib 48B 结构体直发（SDL3 按钮位 0..15 直写 +16 位域）在 host 侧按
§17.3 RAW 解析应产生错乱按钮（bit1=Y、bit3=A…），但历史真机按钮基本可用。
最可能解释（【推断】）：历史构建未设 is_generic_gamepad，host 对
Nintendo VID/PID 走了非 generic 解析路径（其行为未逆向，host 侧为 PC Steam，
不在 libmain.so 内）。git 考古可确认历史 InfoFromHID 的标志状态。

### 17.8 部署状态

2026-09-07 构建通过（switch NRO），待 netloader 推送真机验证：
预期 ABXY/十字键/L3/R3 映射按 §17.4 位置语义直达 host Steam Input 的
Generic 模板（Nintendo 布局习惯的 A/B 互换由 host 端 Steam Input 设置承担，
与官方 Android 客户端行为相同）。

### 17.9 更正：按钮位域是 SDL 序，不是 EGamepadButton（2026-09-07 真机回归后）

§17.4 的 EGamepadButton 位分配解算本身没错（枚举名表 0x3dabc4 反解正确），
但**该 protobuf 枚举与 state+16 位域无关**。决定性证据：

`CHIDDeviceSDLGamepad::OnButtonEvent`（0x754034）【已验证】：

```
mask = 1 << ev->button;      // SDL_GamepadButton 值直接移位
if (down) state+16 |= mask;  else state+16 &= ~mask;
// 另：byte[162] 标志置位时 button==5(GUIDE) 被吞掉不上报
```

即 wire 位域位序 = SDL_GamepadButton（0 起始：A=0,B=1,X=2,Y=3,BACK=4,
GUIDE=5,START=6,L3=7,R3=8,LB=9,RB=10,十字 上11/下12/左13/右14,MISC1=15）。
ihslib 原版 `1 << SDL_button` 本来就是对的。

上一构建用 EGamepadButton 位（A=3,B=24,X=15,Y=1）发送，host 按 SDL 序解释，
真机症状与错位预测完全吻合：Y→bit1=host B（触发菜单/返回类动作）、
A→bit3=host Y（UI 无反应）、X→bit15=host MISC1（无反应）、B→bit24（越界丢弃）；
左摇杆走轴字节不受影响。

已回退按钮映射为 SDL 位直通（sdl_hid_common.h），RAW V2 结构体与版本字节 3
不变。本节同时收回 §17.4 末段对该映射的实现引用。

## 18. 2026-09-07 卡死会话取证（prev stream_diag 2791587B 全量离线分析）

会话：20:17:42 开始，~20:26:27 结束（525s），两次 ~20s 卡死（第二次以持续旋转
右摇杆为标记）。按钮映射修复（§17.9）真机确认生效。

### 18.1 客户端健康度（卡死期间）【已验证】

- hidEvAge（事件捕获年龄）全程 max ≤50ms —— SDL 事件泵健康；
- hidrep（报告提交）时间戳全程无 ≥5s 空洞 —— flush/发送线程健康；
- 全程无 Frames window overflow / disconnect / Failed-to-decrypt。

【结论】两次游戏内卡死期间客户端侧事件、提交、发送全部正常，
上述日志只能证明记录到的事件与报告提交仍在发生；不能证明所有报告实际发出、
被 host 正确解密或应用。hidrep 存在截断和排队滞后，详见协议参考 §14.6。
早期 31-51s / 74-97s 两个零事件窗口在进游戏前（40.7s 处有 2.5s 视频停顿），
与游戏内卡死无关。

### 18.2 加密计数器失配：观测保留，所谓“官方成功才递增”撤回

Evidence：`/tmp/stream_diag_prev.log:14412` 起有 45 条序列失配日志；
首条为 `312 (expect 311), id=348`。这证明收到的加密序列与本地期望不同，
不能单凭日志断言是哪一层丢弃、跳过或多消费了消息。

**撤回旧结论**：“官方仅解密成功才递增，因此 8bb05be 修复了确定性 bug”。
重新反汇编同一 `libmain.so`：`0x7ad060 add`、`0x7ad064 str` 先更新计数器，
`0x7ad06c bl BDecrypt` 后解密，`0x7ad070 tbz` 才分成功/失败。
官方实际为 `BDecrypt(recvSequence++, ...)`，失败不回滚；8bb05be 与该语义相反。

此外，我方窗口在解密前就已 Poll 并释放该帧，解密失败仍确认 packet ID。
因此“冻结计数器后迟到的重传自然自愈”也没有成立：已越过的旧 packet ID 不会
重新交付。正确性依赖可靠传输不误确认、不跳洞；详见协议参考 §14.1–14.3。

### 18.3 待验证（需 host 侧证据）

- 两次 ~20s 卡死的确切机制仍未定位。工作假设【推断】：同一时段的丢包
  突发在我们的输入流上造成 host 侧解密计数器失配/窗口洞，host 停止
  apply 我们的输入消息，~20s 后经某种重同步恢复。客户端日志无法区分
  "host 解密丢弃"与"host 内部停滞"，需要 kxn-pc `streaming_log.txt`
  对应时段（20:17-20:27）比对 host 侧是否有解密/序列错误。
- ~27 帧 host 消息百余秒未送达：若官方 host 无限重传未 ACK 包，则
  我们的累计 ACK/NACK 反馈可能让 host 误信洞已填。需结合 host 日志
  与/或下一步 pcap 密文层时序分析。

### 18.4 host 日志比对（kxn-pc streaming_log.txt，2026-09-07 会话）【已验证】

游戏：黑神话：悟空（gameID 2358720），20:17:47 串流开始，20:26:29 退出到桌面。

host 侧唯一异常窗口 **20:24:21**：
```
NVENC: Hardware encoding is too slow, disabling
Encoder stage failed [type=2, codec=4]
CGameStreamVideoStageMFX: Couldn't init session ×4 → MFX not supported
Created encoder X264 (libx264 superfast zerolatency, 4 threads)
>>> Capture method set to Game polled D3D11 NV12 + libx264 main
Setting target framerate: 30.00 [game 35.59ms (game)]
```
与客户端证据对齐：
- 客户端加密失配起点 20:24:00（丢 host 一帧）——比 NVENC 宣告死亡早 21s，
  即 "too slow" 恶化期从 ~20:24:00 已开始（GPU/场景重载）；
- 客户端第二次卡死的右摇杆事件簇 20:25:28-20:25:59 紧跟该窗口之后；
- 20:18:34 game=111ms 尖峰（游戏加载）对应客户端 40.7s 处 2.5s 视频停顿
  与 31-51s 零输入窗口——第一次卡死【推断】为游戏加载期。

**撤回旧结论**：“host 日志没有输入报错，所以没有输入流洞；卡死是 NVENC
负载事件，非协议 bug；协议唯一缺陷已修”。日志未报告错误不能证明接收、
解密、delta CRC 和虚拟控制器应用均成功。Android 客户端汇编也不能证明
Windows host 内部存在某个 20 秒恢复计时器。

本节所引 NVENC 错误在 20:24:21，而列出的右摇杆事件簇在 20:25:28–20:25:59；
不能把两者直接当作同一故障窗口。GPU/编码负载与输入停滞的因果关系仍是
hypothesis。原始 host 日志在此次本地复核中未取得，以上摘录按历史转述保留。
客户端 NACK 双向错误已在真实代码离线复现，不能排除协议问题。

### 18.5 "Slow framerate" 异常统计：旧单位解释不足

host 日志每秒 `Slow framerate: ... network 3118880768.00, decode 3xx,
display 7xx-10xx (network)(decode)(display)` 中 network/decode/display 三列
为 10^8-10^9 量级假值：均由我方上报的帧级时间戳推导，单位仍不正确
（§18.1 期间客户端真实帧率正常、hidEvAge≤50ms）。game 列（host 本机测量）
真实有效。已修的 16.16 定点秒（packet.c）覆盖了帧事件时间戳，
但 FrameEvents 的事件差分编码本身不匹配官方（协议参考 §14.4），
不能继续只按单位解释；统计反馈对输入卡死的影响尚未建立因果证据。

## 19. Switch 六轴（陀螺仪 + 加速度计）接入（2026-10-22）

### 19.1 为什么此前“协议支持但没有打开”【已验证】

1. **SDL 的 Switch 驱动不提供传感器事件。** devkitPro SDL 2.28.5-4 的
   `SWITCH_JoystickSetSensorsEnabled` 只有 `mov w0,#4; b SDL_Error`
   （反汇编 `/opt/devkitpro/portlibs/switch/lib/libSDL2.a` 的 `SDL_sysjoystick.o`），
   即 `SDL_GameControllerSetSensorEnabled()` 在 Switch 上恒失败；`SDL_PollEvent`
   不会产生 `SDL_CONTROLLERSENSORUPDATE`。所以只修 ihslib 的打包逻辑仍不会
   在 Switch 上得到任何样本。
2. **ihslib 的 Generic RAW V2 打包器把 IMU 区清零。** 旧
   `IHS_HIDReportSDLPackWire` 只写 axes/buttons/version，[28..71] 全 0；
   即使桌面 SDL 产生传感器事件，`SetGyro/SetAccel` 写入的内部状态也不会
   进入 wire。历史提交 929767a 曾尝试透传，但 39fcb1d 以“ENCODED 格式
   不含 IMU”为由撤回；该前提只适用于 byte[27]==0 的 ENCODED 分支，
   不适用于本项目实际发送的 RAW+version 3。
3. **host 端协议本来就带 IMU。** 官方 Linux 1.3.9.258 客户端
   （`steamlink`，SHA-256 `191cc079f795cfdff69a8b51041360a2164b3ec64647e9419af2e2d00fc149e4`）
   `CHIDDeviceSDLGamepad::OnSensorEvent @0x269740`：
   - `sensor==2`（GYRO）：把 `event.data[0..2]` 按
     `(v + 34.90659)/69.81317 * 65535 - 32768` 转成三个 s16，写入 state
     `+0x1c/+0x1e/+0x20`，并 `flags |= 0x2`；
   - `sensor==1`（ACCEL）：同样的映射（半量程 19.6133，满量程 39.2266）
     写入 `+0x22/+0x24/+0x26`，并 `flags |= 0x4`。
   常量位于 0x9b72e8–0x9b72fc：65535、32768、34.90659、69.81317、
   19.6133、39.2266。`CHIDDeviceSDLGamepad::Write @0x268e30` 的
   `COMMAND_SET_SENSOR_ENABLED` 分支在 value!=0 时启用 SDL 的
   ACCEL/GYRO 并 `flags |= 0x6`，value==0 时清除 0x6。
   `GetGyroDPSRatingForControllerType @0x161870` 返回 2000，
   `GetAccelerometerGravityConstant @0x161880` 返回 16383（≈1g），
   与“gyro ±2000 dps、accel ±2g 的 s16 定点”一致。
4. **libnx 的底层状态可用。** libnx `HidSixAxisSensorState` 提供
   `angular_velocity`、`acceleration`、`delta_time`、`sampling_number`，
   且可用 `hidGetSixAxisSensorHandles` / `hidStartSixAxisSensor` /
   `hidGetSixAxisSensorStates` 按 `HidNpadStyleTag` 获取。

### 19.2 wire 契约（RAW V2，byte[27]!=0）【已验证】

```
[0..11]  axes 6×s16
[12..15] flags u32：bit1 = gyro present，bit2 = accel present
[16..19] buttons u32（SDL_GamepadButton 位）
[20..26] 0
[27]     version（本项目发 3，非零即 RAW）
[28..33] gyro s16[3]：rad/s × 65535/69.81317，即 ±2000 dps
[34..39] accel s16[3]：m/s² × 65535/39.2266，即 ±2 g
[40..43] touch（本项目仍为 0）
[44..55] 其余 V2 尾部（本项目 0）
```

`BInjectGamepadStateGenericGamepad @0x2dac70` 在 byte[27]!=0 时最多拷贝
56B；Android 1.3.32 的旧记录写 72B，本项目保留 `min(host length, 72)`
的现有上限，但 56B 之后始终为 0。Delta mask 仍按 host 通告长度生成。

### 19.3 单位与轴向对齐【已验证/推断分列】

- **单位（已验证）**：Nintendo 的 `imu_sensor_notes.md` 明确
  “Internally Switch uses revolutions per second instead of degrees”，
  加速度以 g 表示；libnx 直接暴露该状态。SDL 的
  `SDL_SENSOR_GYRO` 是 rad/s、`SDL_SENSOR_ACCEL` 是 m/s²。因此
  `gyro_rad_s = angular_velocity_rev_s * 2π`，
  `accel_m_s2 = acceleration_g * 9.80665`。
- **Switch 共享内存 -> SDL 标准帧（已验证于两个独立模拟器）**：
  yuzu `src/input_common/drivers/sdl_driver.cpp` 把 SDL 样本写成
  `switch_gyro=(x, -z, y)/2π`、`switch_accel=(-x, z, -y)/g`；
  Ryujinx `NpadController.Update` 使用同一轴置换
  `(x, -z, y)`。取逆得到本项目使用的：
  ```
  sdl_gyro  = ( av.x,  av.z, -av.y) * 2π
  sdl_accel = (-acc.x, -acc.z, acc.y) * g
  ```
- **不按 Joy-Con 左右再翻转（推断，待真机确认）**：Switch 的 `hid`
  服务对 Pro/Joy-Con/handheld 输出同一控制器帧；SDL HIDAPI 的
  `(-Y, Z, -X)` 和右手柄 `data[0]/data[1]` 翻转针对的是**绕过服务的
  原始 HID 报告**，不适用于 libnx 已归一化的状态。若真机上左右
  Joy-Con 方向相反，只需在 `sensor_math.c` 增加按 device type 的
  后处理，不改协议层。

### 19.4 实现落点【已验证】

- `third_party/ihslib/src/hid/sdl/src/sdl_hid_report.c`：
  `SetGyro` 置 flags bit1、`SetAccel` 置 bit2；新增
  `IHS_HIDReportSDLClearSensors`。
- `third_party/ihslib/src/hid/sdl/src/sdl_hid_common.h`：
  `IHS_HIDReportSDLPackWire` 复制 flags、gyro、accel、touch 到
  [12..47]；[48..] 保持 0。
- `third_party/ihslib/include/ihslib/hid/sdl.h` + `sdl_hid_event.c`：
  新增 `IHS_HIDSDLInjectSensor(session, joystickId, gyro, accel)`，
  在设备锁内合并到 canonical state；发送仍由既有 8ms flush 线程完成。
- `app/platforms/switch/sensors.c`：
  按 `HidNpadStyleTag` 取 handheld/FullKey/JoyDual/JoyLeft/JoyRight 句柄；
  JoyDual 优先右手柄；每 4ms 取最新 `sampling_number`，转换后注入；
  只从 `pump_sdl_events`（SDL/libnx HID 所属媒体线程）调用，
  会话停止/退出时 `hidStopSixAxisSensor`。
- `app/platforms/common/sensor_math.c`：单位/轴向纯函数，桌面测试覆盖。
- `app/platforms/common/media.c`：会话有效且 input gate 打开时
  `sl_sensors_poll`，否则 `sl_sensors_stop`；shutdown 再 stop 一次。
- 测试：`ihstest_hid_sdl_report` 校验 flags/gyro/accel 的 wire offsets；
  `nsl-sensor-math` 校验单位与轴向；desktop 246/246 通过，Switch
  release NRO 构建通过。

### 19.5 真机六轴采集与轴向结果（2026-10-22，Handheld）

设备地址 `10.10.10.66`，hbmenu netloader + nxlink 推送，未开 Steam 会话。
工具：`nsl-sixaxis-probe`（机器可读采样）和 `nsl-sixaxis-view`（屏幕可视化）。

- **采样路径成立**：`handheld_style=0x2`、`no1_style=0`、`handheld_type=12`；
  `hidGetSixAxisSensorHandles`/`hidStartSixAxisSensor` 均返回 0；只有
  handheld 句柄有数据，fullkey/joydual/joyleft/joyright 句柄返回 0（当前
  没有对应控制器连接）。
- **采样率**：状态 `delta_time=5,000,000 ns`；19.914 秒内 `sampling_number`
  增加 3871，约 **194 Hz**（标称 200 Hz 附近）。
- **静止姿态**：accel 约 `(0.02, -0.29, -1.06) g`，主要沿 **-Z**；转换后
  SDL accel 约 `(-0.2, 10.4, -2.8) m/s²`，主要沿 **+Y**。这符合
  “屏幕朝上/近似平放时重力在 SDL Y 轴”的预期。
- **引导动作（3 轮 REST/YAW/PITCH/ROLL）**：
  - YAW（绕竖直轴左右转）：raw **Z** 范围约 `-0.95..+0.81 rev/s`，
    X/Y 较小；对应 SDL **Y**。
  - PITCH（前后倾）：raw **X** 范围约 `-1.10..+1.19 rev/s`；对应 SDL **X**。
  - ROLL（左右滚）：raw **Y** 范围约 `-0.90..+0.84 rev/s`；对应 SDL **Z**。
  这三组结果支持 `sensor_math.c` 的 `(x, z, -y)` 映射和“libnx X=pitch、
  Y=roll、Z=yaw”的轴向假设。第一轮用户未完全按提示动作，第二轮/第三轮
  的 Z/X/Y 主导关系很清楚。
- **仍未验证**：Pro Controller、单 Joy-Con、左右 Joy-Con 分离时的
  方向/符号；Steam host 是否接受“未先发 `SetSensorEnabled` 即出现 IMU”
  的数据；Steam Input 的 Gyro 正负方向（真机若相反，只需在
  `sensor_math.c` 取反对应分量，wire 格式不变）。

## 20. Steam Windows host 对 Switch Generic IMU 的消费路径（2026-10-23 逆向）

> **历史研究记录，相关推论已撤回。** §20.8 的 parser 拒绝结论、仅供本地使用的
> flags 解释和 slot 推断被 §21 的原始机器码执行证据取代；实机观察本身保留。

本节只记录可复核的证据、结论和假设，不把“host 显示了 Gyro UI”当成“host 消费了
IMU”。这是 2026-10-22/23 多轮真机对照后的收敛结果。

### 20.1 材料

- Windows Steam host：`steamclient64.dll`，FileVersion `11.12.30.07`，编译时间
  `2026-10-14`，SHA-256
  `e9045710ec7ee47d7b7488d1c5dda2d933dec7e251e965967a5c48f4df68eb4e`。
- 该 DLL 内嵌的源码路径（`__FILE__` 字符串）：
  `src/common/hiddevicesdl.cpp`、`src/common/hiddeviceremote.cpp`、
  `src/clientdll/controllerabstraction.cpp`、
  `src/clientdll/controller_sources/controller_source_gyro.cpp`、
  `src/clientdll/controller_gyroprocessing.cpp`。
- host 侧 VDF：`controller_switch_pro.vdf` 的 group 8 为
  `gyro_to_mouse`，preset 中 `"8" "gyro active"`，
  `controller_caps = 0x8B4BFF`（含 `0x800`）。这证明 Gyro 行为是启用状态，
  不是“没保存”。
- 真机日志（NX 侧注入点）：
  `sensor inject motion=0.117 flags=0x6 wire_gyro=50,0,-60`。
  同一会话 host 反复发 `host sensor enable=1/0`，说明 host 主动请求了
  sensor，且客户端把非零 gyro 写进了 wire。

### 20.2 host 侧 `CHIDDeviceSDLGamepad` 的状态布局

通过 RTTI/vtable 定位（镜像基址 `0x138000000`）：

- RTTI type descriptor `.?AVCHIDDeviceSDLGamepad@@` @ `0x17ce878`；
  vtable @ `0x1392fa7a8`。
- `OnSensorEvent` @ `0x1386d30e0`。它从 `SDL_GamepadSensorEvent`
  取 `[event+0x14]` 的 sensor type 和 `[event+0x18/+0x1c/+0x20]` 的三个
  float，写入 state：
  - 正常分支：gyro s16 @ `state+0x1c`，`flags |= 0x402`；accel s16 @
    `state+0x22`，`flags |= 0x4`；
  - 左/备用分支（SDL sensor type 3/4，即 ACCEL_L/GYRO_L）：gyro 写
    `state+0x38`、accel 写 `state+0x3e`，并置 `0xa00`/`0x4`。
  - 两个分支的选择来自 `[event+0x14]` 的 SDL sensor type，不是来自
    wire flags；`0x400` 是 host 内部 state 的标记，不应直接当成
    “remote gyro valid”位。
- 因此 host 本地 SDL 路径的 gyro 布局与 ihslib 的 `+0x1c` 一致；差异是
  host 本地 flags 带 `0x400`，且存在第二套 `+0x38/+0x3e` 布局。
- 真机试过把客户端 flags 从 `0x6` 改成 `0x406`：**整个手柄输入失效**
  （按键/摇杆都无输入），不是“gyro 开始工作”。这说明 `0x400` 是 host
  内部 state 的语义位，不能安全地直接补到 remote Generic wire flags；
  它会让 host 进入另一套解释，和 Generic V2 的 `+0x16`/`+0x1c` 布局
  不兼容。

### 20.3 host 侧 Generic 抽象

- `CGenericGamepadControllerAbstraction` RTTI
  `.?AVCGenericGamepadControllerAbstraction@@` @ `0x17cb5d8`；
  vtable @ `0x1392ea3d0`。
- 候选 `ProcessState` @ `0x1386793e0`（vtable slot 13）把 HID state 视为位于对象 `+0x18`：
  axes 在 `+0x18..+0x22`，version 在 `+0x33`（即 HID state `+0x1b`），
  RAW 分支从 `+0x18` 起拷贝 `0x48` 字节到局部
  `HIDDeviceSDLGamepadStateV2_t`，因此 gyro `+0x34`（HID `+0x1c`）和
  accel `+0x3a`（HID `+0x22`）在拷贝范围内。
- 候选 state-changed 比较 @ `0x138678ea0` 只读
  `+0x18..+0x22`（axes）和 `+0xc..+0x12`（flags/其他），**没有比较
  `+0x34/+0x3a` 的 gyro/accel**；它把 state 缓存到 `+0x108`，长度
  `0xa0`。
- 远程 HID report 相关函数 @ `0x1386cf580`（同文件断言在 `0x1386d0031` 引用下方字符串）走的是
  `hiddeviceremote.cpp` 的 report-type 分发；字符串
  `CHIDDeviceRemote::ProcessInputReport(): Received unknown report type`
  @ `0x1392f9b30`。这条路径按 report byte/type 构造
  `CHIDDeviceInputReport`，不是直接把 Generic V2 的 tail 当作连续 IMU 流。

### 20.4 真机对照结果

| 配置 | host 布局 | Gyro UI | 震动 | 实际 gyro 输出 |
|---|---|---|---|---|
| Generic `0000:0000`，caps `0x8043ff` | 4 项 | 无 | 正常 | 无 |
| Generic + `0x800`，Steam Input 开 | Gyro 出现 | 有 | 正常 | **无** |
| Switch Pro `057e:2009` + `0x800` | 6 项 | 有，校准成功 | 正常 | **无** |
| Switch Pro + `0x800` + flags `0x400` | 输入全失效 | — | — | — |
| DS4 `054c:09cc` + `0x800` | 6 项（含触控板） | 有 | 正常 | **无** |

`controller_switch_pro.vdf` 里 `gyro_to_mouse` active、`gyro_natural_sensitivity=483`，
但桌面指针、右摇杆、游戏内动作都不响应。校准能成功只说明 host 能拿到
一部分运动样本（校准可用 accel/初始样本完成），不等于连续 gyro 被送进
Gyro action group。

### 20.5 结论与假设

**Evidence：**

1. NX 侧 gyro 已进入 canonical state 和 wire：`flags=0x6`，
   `wire_gyro` 非零；host 主动发 `SetSensorEnabled`。
2. host 的 VDF Gyro group active，caps 含 `0x800`；host 本地
   `CHIDDeviceSDLGamepad::OnSensorEvent` 的 gyro 布局也是 `+0x1c`。
3. host 的 `CGenericGamepadControllerAbstraction` 的 state-change 比较
   不含 `+0x34/+0x3a`（gyro/accel）字段；`CHIDDeviceRemote::ProcessInputReport`
   按 report type 分发，不是连续 IMU 流。
4. 官方 Linux 1.3.9.258 的 `BParseGamepadStateSwitch @0x2da140` 只解析
   Switch 原生报告的按键/摇杆，不读 IMU；官方唯一 IMU 路径是
   `CHIDDeviceSDLGamepad::OnSensorEvent`。

**Retraction（2026-10-23）：** 本节初稿把“host 不消费 IMU”写成了结论，这是
下早了。逐位比对 host VDF 的 `controller_caps = 0x8B4BFF` 与
`InfoFromHID` 的基线 caps 后，发现基线是 `0x8343FF`（ABXY/DPAD/双摇杆/
肩键/扳机/Back/Start/Guide/Misc1/XInput/UNK_3/UNK_4），和 host VDF 只差
**`0x800` + `0x80000`**。真机只补过 `0x800`（Gyro UI 出现、无数据），
**`0x80000` 从未测过**。因此下面的“host 不支持”不能作为结论，降级为待验证。

**Conclusion（当前可下的部分）：**

- 客户端没有丢 IMU；host 的 VDF Gyro group active，UI 能出现。
- 只补 `0x800` 时没有数据输出；`0x400` 直接让输入失效，不能补。
- host 的 Switch 原生 parser 不读 IMU（官方 Linux 客户端
  `BParseGamepadStateSwitch @0x2da140`），所以“改成Switch 原生报告”
  不是显然更优的方向。
- `0x80000` 是否是 host 的 motion/数据通路位，仍为 `待验证`；在它验证
  之前，不能判定 host 不支持 Switch Generic gyro。

**Hypothesis（待验证）：**

- host 的 `CGenericGamepadControllerAbstraction` 只在 axes/flags 变化时
  推进 state；纯 IMU 变化（不动摇杆/按键）不会触发 Gyro source 更新。
  这可以解释“校准成功、但转动完全无输出”。
- 若该假设成立，客户端侧较可行的兼容层是发送 host 已有 gyro 支持的
  **DS4/DualSense 原生报告**（host 的 PS4/PS5 gyro 路径是既有的），
  而不是继续改 Generic caps；这需要实现 DS4 的 input report/feature
  report，成本和风险都高于当前 Generic 路径。
- 另一条待验证路径是让 host 在收到 IMU 时也标记 state changed；这属于
  host 内部行为，客户端无法直接控制。

### 20.6 精确消费点：`CGyroSource::Update` 与 flags `0x800/0x400` 门控（2026-10-23 继续逆向）

继续沿 `controller_source_gyro.cpp` 追到实际消费函数，已定位到一条完整、可解释
“校准成功但 gyro 无输出”的路径：

- **消费函数**：`CGyroSource::Update` @ `0x1386702d0`（`CGyroSource` vtable
  slot 9；vtable @ `0x1392e5218`，RTTI `.?AVCGyroSource@@` @ `0x17cb270`）。
- **输入结构**：`rdx` 指向的 controller/raw state 里，
  accel s16 在 `+0x24/+0x26/+0x28`，gyro s16 在 `+0x2a/+0x2c/+0x2e`。
  `Update` 先检查这 6 个 word；若全为 0，直接跳到 `0x138670788` 返回，
  不产生四元数、不更新 prediction，也不通知 Gyro mode。
- **填充函数**：`CGenericGamepadControllerAbstraction` 内
  `0x1386796f0` 起的循环负责把这 6 个字段写进上述输入：
  - slot 0（`edi=0`）要求 **`flags & 0x800`**；
  - slot 1（`edi=1`）要求 **`flags & 0x400`**；
  - accel 另要求 **`flags & 0x4`**；
  - 通过后把 gyro 写到输入 `+0x2a/+0x2e/+0x2c`，accel 写到
    `+0x24/+0x26/+0x28`（并做一次 y/z 交换和 y 取反）。
- **我们的 wire flags 是 `0x6`**：accel 的 `0x4` 有，所以校准能用 accel 完成；
  gyro 需要的 `0x800`（slot 0）和 `0x400`（slot 1）都没有，填充循环直接跳过，
  输入 6 个 word 保持 0，`CGyroSource::Update` 提前返回 → Gyro mode 永远收不到
  样本。
- **slot 与 wire offset 的对应**（由同一段 `0x1386796f0` 的读写位置直接得出）：
  - slot 0（`flags & 0x800`）读 gyro `+0x38`、accel `+0x3e`；
  - slot 1（`flags & 0x400`）读 gyro `+0x1c`、accel `+0x22`。
  ihslib 目前只写 `+0x1c/+0x22`，所以它天然对应 **slot 1（0x400）**；
  要用 slot 0，必须把同样的 gyro/accel 再写到 `+0x38/+0x3e` 并置 `0x800`。
  之前 `0x400` 导致整只手柄失效的那次是 **Switch Pro 身份**构建，不能
  用来否定 Generic + `0x400`；两者混淆了。

这与真机现象完全一致：Gyro UI、校准、震动都正常（accel/flags 0x4 路径通），
但转动无输出（gyro slot 位未被填充）。因此 **caps `0x80000` 不是当前首要
问题；wire gyro flags 缺 `0x400`/`0x800` 才是**。当前包文写的是 `+0x1c/+0x22`，
所以优先补 **slot 1 的 `0x400`**；只有 `CGyroSource` 索引为 0 时才需要
slot 0 的 `+0x38/+0x3e`。之前 20.5 的 “host 不消费” 假设在此被替换。

**待验证的两个对照构建（均为 Generic 身份，不改 caps/VID/PID）：**

1. `build/switch-gyro400/app/nsteamlink.nro`：flags 改为 `0x402`，wire 仍写
   `+0x1c/+0x22`，对应 slot 1。这是与当前包文最匹配的组合，应优先测。
2. `build/switch-alt/app/nsteamlink.nro`：保留 `+0x1c/+0x22`，另把 gyro/accel
   复制到 `+0x38/+0x3e` 并置 `0x800`（slot 0），用于 `CGyroSource` 索引为 0
   的情况。`build/switch-gyro800/app/nsteamlink.nro` 只置了 `0x800` 但没有写
   `+0x38`，不能作为 slot 0 的有效测试，已废弃。

`CGyroSource::Update` 用 `[this+0xc0]` 选输入 slot；该索引由哪个控制器源
初始化仍是 `待验证`，所以先用 slot 1 的 `0x400` 对照，失败再测 slot 0 的
`+0x38` 布局。

**还有一个未验证前提**：`0x1386796f0` 读的 `edx` 是 host 内部 state 的
flags，不是已经证明与 wire flags 一一对应。官方 Linux 客户端
`OnSensorEvent` 只置 `0x2/0x4`，所以 host 的 remote parser 可能自己把
wire flags 翻译成内部 `0x400/0x800`；如果是这样，直接改 wire flags 不会
生效。两个构建（`0x400` 与 `+0x38/0x800`）就是用来区分这两种情况的；
在真机结果出来前，不能把“wire flags 直接等于内部 flags”当成结论。

### 20.7 官方 Windows packer 交叉验证：wire flags 就是内部 flags

为了确认 `0x1386796f0` 读到的 flags 是否就是 wire flags，检查了本机
`steamclient64.dll` 的官方 packer：

- `CHIDDeviceSDLGamepad` 的 pack 函数 @ `0x1386d35e0`。
- version（state `+0x1b`）非 0 时走 RAW 分支 `0x1386d37a6`：
  - `eax = 0x48`（72）；
  - `rbp = min(requested_len, 72)`；
  - `memcpy(dest, source_state, rbp)`。
- 也就是说 **RAW V2 是把内部 state 的 0..0x47 原样拷到 wire**，没有字段
  重排、没有 flags 翻译。内部 state 的 flags（`+0xc`）就是 wire `+0xc`。
- 同一 DLL 的 `OnSensorEvent @0x1386d30e0` 正常 gyro 分支写
  `state+0x1c`、`flags |= 0x402`；accel 写 `state+0x22`、`flags |= 0x4`。
  因此官方 Windows 客户端在本机带 gyro 的 SDL 手柄上，wire 就是：
  - `flags = 0x402`（加 accel 后 `0x406`）
  - gyro @ `+0x1c`
  - accel @ `+0x22`
- 这和 `CGenericGamepadControllerAbstraction` slot 1（`0x400`）读取
  `+0x1c/+0x22` 完全一致，也证明 **wire flags 与内部 flags 一一对应**，
  20.6 末尾的 `待验证` 前提成立。

**结论：我们应该发送的格式与官方 Windows 客户端完全一致：**

```text
flags = 0x2 | 0x4 | 0x400 = 0x406
gyro  = s16[3] @ +0x1c (28)
accel = s16[3] @ +0x22 (34)
version = 3 @ +0x1b (27)
```

`build/switch-gyro400/app/nsteamlink.nro` 就是这一格式：Generic 身份，
`SetGyro` flags `0x402`，`SetAccel` 保持 `0x4`，wire 偏移不变。

slot 0（`0x800` + `+0x38/+0x3e`）是另一条 sensor 布局（host 的
ACCEL_L/GYRO_L 分支），不是本机单 Pro/handheld 的主路径；
`build/switch-alt` 只作为兜底，不应优先使用。

### 20.8 真机反证：`0x400` 不能放进 remote Generic wire flags

2026-10-23 早上在 `10.10.10.66` 推送 `build/switch-gyro400`（Generic 身份，
`SetGyro` flags `0x402`，线上 flags `0x406`）实测：

- **Gyro 选项直接消失**；
- **按键/摇杆全部失效**。

这不是“只加一个 slot 位”的问题。它说明 host 的 remote Generic parser 把
wire flags 的 `0x400` 当成另一种报告模式/布局，直接拒绝或切换了解析；
`0x400` 是 host **内部 state** 的 sensor 位，不是 remote Generic 可安全
写入的 wire 位。

因此 20.7 的“发送 `0x406`”结论**撤回**：

- 官方 Windows packer 的 `0x402` 是**本地 SDL state** 的内部 flags；
- 它不能证明 remote Generic wire 也接受 `0x400`；
- 真机结果证明 remote Generic 不接受。

稳定可用的 wire flags 仍是 `0x6`（gyro presence `0x2` + accel `0x4`）；
不要再把 `0x400`/`0x800` 加进 wire flags。要设置 host 内部 sensor 位，
必须找到 remote parser/`DeviceInputReports` 一侧的机制，而不是直接改 flags。

## 21. Generic RAW V3 的接收契约复核（固定 Windows 样本）

**Retraction（2026-10-23）：** 撤回 §20.8、D-064 和旧交接文档中“remote
Generic 不接受 `0x400`”“该位只属于本地 state”的协议结论。此前用户观察到的
Gyro UI 消失、按键/摇杆失效仍然成立，但不能从该现象推出 parser 的分支行为。
`CHIDDeviceRemote::ProcessInputReport` 实际入口是 `0x1386cfd80`，不是
`0x1386cf580`；其 unknown-report-type 检查的是 protobuf 的 full/delta presence
字段，不是 RAW flags。这里的离线结果不替代那次实机失败，也没有确定其根因。

**Evidence：** 同 SHA-256 `e9045710…68eb4e` 的原始 x86-64 指令离线执行：

- full 分支 `0x1386cfe70..0x1386cfed0` 按长度复制 payload 后入队；delta 分支
  调用 `0x1386cef00` 按 73 字节订阅长度形成的 10 字节掩码重建数据，再调用
  `0x138cfa3e0` 对 `min(delta_report_size, subscribed_length)` 字节验 CRC。
  `0x6/0x406/0x806/0xa06` 均能还原并通过校验，没有 RAW flags 拒绝分支。
- Generic read `0x13867b060` 从 HID Read 直接读取对象 `+0x18`；builder
  `0x1386793e0` 的 RAW 分支复制 72 字节。version < 3 时自动 `flags |= 0xc00`
  （`0x1386795ae..0x1386795b8`），version 3 则要求发送端自己提供 freshness。
- `version=3, flags=0x6` 的两个 IMU slot 均为零；`0x406` 把正常 IMU 写入
  slot 1，同时保留完全相同的按钮/摇杆输出。左 slot 的完整 gyro 条件是
  `0x200 | 0x800`，不是只加 `0x800`。
- Gyro constructor 真正入口为 `0x13866fac0`。factory `0x13866ebe0` 中 source
  type 10 分支 `0x13866ede4` 传 slot=1；type 16 分支传 slot=0。不能因为
  “slot 0”这个编号就推断普通单手柄用 slot 0。
- `CGyroSource::Update` 的 6-word 零检查还带 `slot * 0x34`，不是无条件读
  base+0x24。普通 slot 1 收到 `0x406` 后通过该门槛。
- `OnSensorEvent @0x1386d30e0` 还写 `state+0x34`（sensor timestamp / 100，
  单位 100ns）；`0x1386d2a80` 写 `state+0x30`（event timestamp / 1000，
  单位 µs），新建报告时清 `0xc00` 和两个 sensor timestamp。
  此前 ihslib 把 `+0x30` 以后清零，漏掉时序信息；只按六轴数值比较还会
  合并不同时间但相同量化值的样本。零时间戳会使 `0x13863bc00` 直接返回，
  **不等于已经证明它单独导致全部 gyro 输出失效**。
- `build/switch-gyro400/CMakeCache.txt` 的 `NSL_SWITCH_HOST_CAPS=OFF`；当前
  `InfoFromHID` 基线不含 motion capability `0x800`。历史 Gyro UI 消失不能
  当作只改变 wire flag 的独立实验。

**Conclusion：** Generic 路径存在正常 gyro 输入通路。RAW V3 的数值、presence、
每报告 freshness、时间戳、设备 motion capability 是不同契约，必须一起处理。
无需从上述证据引入 Switch/DS4 身份伪装或额外 sensor 握手。

**Boundary：** 离线执行使用合成对象及显式容器/引用计数/时钟桩；网络解密、真实
线程调度、Steam Input action 配置、最终指针/游戏输出和 Switch 生命周期不在
此证明内。旧输入失效原因仍待验证；§20 中的 state-change 比较未证明是 gyro
更新门禁，不能因未比较 IMU 就宣称它阻止 gyro。

### 21.1 姿态处理与 source 的第二道门槛

**Evidence：** Generic vtable slot 14 的 `0x13867d830` 检查对象 `+0xb8`、
capability `+0x80` 的 `0x800`、sensor enabled `+0x1f4`。随后循环两个 slot：
每个 slot 先积累 15 次 warmup，再检查新 packet index、fresh mask、非零 IMU。
左 slot 另需 capability bit 39；普通 slot 1 不需要该位。
实际 C 报告在合成启用对象上第 31 次进入 `0x13863d430` 校准；缺 `0x800`、
sensor disabled、清 fresh mask 的负例均不进入。capability 不是“仅控制 UI”。

`0x13867c110` 对 Generic 的 setting `57 00 aa` 写 `+0x1f4=1`，
`0x13867c1d0` 将其转成设备 Write `08 01`（21 字节，余字节为零）；
`57 00 bb` 写 `+0x1f4=0` 并转成 `08 00`。这是既有 SetSensorEnabled 流，
不是从远端输入报告中自动推导该状态。离线审计执行了两条原始设置分支。

校准之后调用 filter `0x13863c5d0`、integrator `0x13863d990`、filter
`0x13863d750`。单独运行 integrator，使用已初始化单位姿态、Generic type=30、
无校准偏移的合成 context，保留原始三角函数/四元数运算：
生产 C packer 的 `(1,-2,3) rad/s` 在 dt=4ms 下得到 wxyz 约
`(0.9999720, 0.00197446, -0.00600552, -0.00398705)`；零角速度或 dt=0
的对照保留 `(1,0,0,0)`。不是用 Python 重写积分器来证明自己的实现。

`CGyroSource::Update` 通过六轴非零检查之后，还要求 enable/fresh 和非零
四元数；四元数全零时在 `0x138670780` 返回。将上述原始积分结果送入原始
source Update，可通过该门槛，并在 `0x138670570..0x138670582` 把普通 slot
角速度复制到 source `+8/+0xa/+0xc`。此处测试停在 `0x13867058c`，没有
把 source 更新后的 action dispatch 当成已验证。

**Boundary：** 校准门控和积分/source 是分段试验，不是完整 postprocessor
连续执行。未运行校准收敛、姿态初始定向、实际 Steam Input 配置及最终动作。
复现命令、精确 SHA、桩边界见 [SWITCH_GYRO_HOST_HANDOFF.md](SWITCH_GYRO_HOST_HANDOFF.md)。

### 21.2 实机桌面鼠标输出证据（2026-10-23）

**Evidence：** 用户明确指定 `10.10.10.66` 后，通过 nxlink 部署修复构建。
测试 NRO SHA-256 为 `db3234bafeac32344e951eea2f910d5c502d94ce2029bedff9afc34a2ebe78f4`，
源码为 `8668163` 加本节对应的 RAW V3 工作区修复。运行日志包含
`StartInputReports(id=1, length=73)` 和
`sensor inject motion=0.148 flags=0x406 wire_gyro=0,138,0`。
随后用户反馈：“游戏不清楚是不是实际能用，但是通过修改手柄桌面布局测试是有效的。
可以用来操控鼠标。”

**Conclusion：** 此次真实 Switch→Steam Host→桌面布局链路已产生可观察的鼠标
输出，证据强于离线解析和 source 字段检查。撤回把“最终桌面鼠标输出未验证”
用于描述这个修复构建的说法；§21.1 的离线工具边界仍然有效。

**Boundary：** 游戏内映射、不同游戏兼容性、各手柄形态和校准精度没有由此次
反馈得到验证；也未据此认定旧实验按键失效的根因或 hbmenu 返回状态。

## 22. 震动输出：普通 rumble 与原生效果包（2026-11-17）

### 22.1 证据更正与材料

**Retraction：** 撤回本轮初步调研把“ihslib 当前 SDL 后端只解析双路 rumble”
外推为“Steam Remote Play / Generic 链路最多只能传双路强度”的结论。
`DeviceWrite.data` 是 bytes；官方 SDL 设备协议还包含 `0x0d` 专有效果命令。
这项更正不撤回 D-048 对现有 `0x01` 强度单位、libnx 振幅范围以及固定频率
实现的说明，也不把新发现归因为用户所说“傻震”的已证实原因。

**Evidence：**

- Windows Steam 主机使用 §20.1 的固定 `steamclient64.dll`，SHA-256
  `e9045710ec7ee47d7b7488d1c5dda2d933dec7e251e965967a5c48f4df68eb4e`。
- Android 1.3.32 的 `libmain.so`，SHA-256
  `50e1d3147d5d47b71ef1970e1867a2fe4f3e1ecf2ea6153f927fac88b88ea38e`。
  `CHIDDeviceSDLGamepad::Write @0x7527b4` 的 69B 分支检查 byte 0 为 13，
  从 `+1` 读 u32 长度，从 `+5` 取数据，调用 `SDL_SendGamepadEffect`。
  该动态函数名在 `0x3b1d3e`，初始化绑定位置 `0x756b6c..0x756b9c`。
- 另从 Valve 下载
  [Linux Steam Link 1.4.5.326](https://repo.steampowered.com/steamlink/1.4.5.326/steamlink-1.4.5.326.tgz)，
  归档 SHA-256 `52b8136ca6b9fd03e711519c36acd7ee79ca01d8c0f397eaddb13146848720a3`；
  `package/bin/steamlink` SHA-256
  `92373306ed2c91818bb5dc89d2cd7ba18cc8244f9d436de671c35d776d0425ac`。
  同类 Write 入口 `0x2dbc30`，效果分支 `0x2dbcc8..0x2dbce9`，最终调用
  `SDL_SendGamepadEffect`，函数槽 `0x1441a48`。

普通 `0x01` 与专用 `0x0d` 是同一 Generic SDL 设备的两类输出，不需要因为
发现 `0x0d` 就移除 `is_generic_gamepad`。§17 的输入格式约束仍适用。

### 22.2 固定主机的实际输出粒度

**Evidence：** `CGenericGamepadControllerAbstraction` vtable `0x1392ea3d0`：

| 路径 | 原始代码地址 | 实际输出 |
|---|---|---|
| 普通 rumble，slot 33 | `0x13867bda0..0x13867be8d` | 21B：`01 + u16 low + u16 high + u32 duration_ms + padding`；此分支 duration 为 5000ms |
| 扳机 rumble，slot 34 | `0x13867c920..0x13867c9d1` | 21B `0x06` 命令；不是 Switch 身体震动器增加两个物理马达 |
| Switch 脉冲，slot 32 | `0x13867cce0..0x13867ce98` | 类型 38/51 等检查后输出 69B `0x0d`，内嵌 10B Nintendo `0x10` 报告 |
| PS5 类效果，slot 23 | `0x13867b770..0x13867b89e` | 类型 45/48 门禁，内嵌 47B 效果；不能当成任意 Switch 波形入口 |

`CPulseHapticWorkItem` 的执行路径 `0x138617a90..0x138617b60` 调用 abstraction
vtable `+0x100`，即 slot 32。不能只根据 Steamworks 公共 API 的设备名单，排除
Steam 内部这条 Switch 路径；同样，内部实现存在也不证明某个游戏会调用它。

普通 rumble 的非零更新比较 `Plat_TickDiffMilliSec(...) < 0x32`，即 50ms；
零强度停止绕过这个检查。相同值还受刷新时间判断影响，不是每帧必发。
Switch 脉冲另有每目标 50ms 门禁，且普通 rumble 非零时存在 500ms 的冲突检查。
时钟单位由 PE 导入名 `Plat_TickDiffMilliSec` 确认，不能由常数 50 猜测单位。
这些是固定 DLL 特定分支的行为，不是所有 Steam 版本或所有设备的统一频率上限。

**Evidence / 离线执行：** `tools/steam-rumble-host-audit.py` 执行上述普通与
Switch 脉冲原始指令，并经过原始 `CGenericControllerProtocolHandler` 转发
`0x13867f7f0`，记录最终 HID Write 参数。普通示例得到：

```
01 34 12 cd ab 88 13 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

即两路强度 `0x1234/0xabcd`、5000ms。49ms 不发，50ms 发；零强度可在 0ms 发。

Switch 脉冲的容器为：

```
0d 0a 00 00 00 [10B Nintendo report] [54B zero padding]
```

两个目标与关闭的 10B 报告分别为：

```
target 0: 10 00 00 01 40 40 00 00 05 c0
target 1: 10 00 00 00 05 c0 00 01 40 40
off:      10 00 00 01 40 40 00 01 40 40
```

这里的 target 是原始函数参数值，不在未经过 worker 侧目标变换时直接标为物理左右。
将脉冲 duration 改为 1、1000、65535，并改变间隔和重复次数，得到相同报告。
原始 Switch 分支只用 duration 的零/非零选择固定脉冲/关闭，不编码这些完整参数。

脚本还把产生的命令分别交给固定 Windows SDL Write 和 Linux 1.4.5.326 Write
原始指令执行：普通命令到达 rumble 调用；`0x0d` 的 10B 数据逐字节原样到达
effect 调用。独立的合成 64B payload 用例仅证明两个 receiver 可以无损转交
64B，不证明 Steam 会为 Switch 生成这种数据。

**Conclusion：** 已证明该主机至少有“双路强度”和“Nintendo 原生压缩短脉冲”
两条输出。`0x0d` 是设备专用数据容器；它的存在不等于普通 PC 游戏提供任意
HD 波形，固定 Switch 脉冲分支反而有明确的信息降级。

### 22.3 修复前的客户端缺口与原生 API 边界

**Evidence（修复前，父仓库 `198da0c` 记录的 ihslib）：**
`third_party/ihslib/src/hid/sdl/src/sdl_hid_write.c` 的
`WriteCommandLength` 只接受 `01/05/06/08/09/0a/0b`，不接受 `0d`。
`IHS_HIDDeviceSDLWrite` 在这里对未知命令返回 -1，专有效果不会进入硬件输出。
这是静态解析缺口，不是当前用户游戏已经发送 `0d` 的抓包证据。

`app/platforms/switch/rumble.c` 已经直接调用 libnx `hidSendVibrationValues`，
所以现有普通输出最后一层并没有经过 devkitPro SDL 震动驱动。只替换
`SDL_GameControllerRumble`，不能补上它之前拒收的 `0d` 数据。

[SDL 3.4.0 的 Switch HIDAPI 后端](https://github.com/libsdl-org/SDL/blob/a962f40bbba175e9716557a25d5d7965f134a3d3/src/joystick/hidapi/SDL_hidapi_switch.c)
的 `HIDAPI_DriverSwitch_SendJoystickEffect` 接受 10B、report ID 为 `0x10` 的
完整输出包，保留两组原生 motor data 后调用 `WriteRumble`。因此“SDL 整体
没有设备专有效果能力”也不成立；应区分通用 rumble API、SendEffect API 与
Switch 平台 backend 的具体实现。**引用更正：** 原 `68ef6231…` 链接无法核验，
撤回其作为已核验固定源码的引用，改用已从官方 tag 和 commit 获取的上述源码。
[SDL 2.32.4 对应函数](https://github.com/libsdl-org/SDL/blob/2359383fc187386204c3bb22de89655a494cd128/src/joystick/hidapi/SDL_hidapi_switch.c)
仍返回 Unsupported，不能把 SDL3 能力外推到本项目 SDL2 平台。这里用 SDL3
作为能力实例，不据此认定固定官方 Steam Link 捆绑的 SDL 版本。

libnx 的 `HidVibrationValue` 为每个 actuator 提供两组频率/振幅，
`hidSendVibrationValues` 直接转交 `hid` IPC 206；不是把参数变成双路强度。
`hidGetActualVibrationValue` 是 IPC 202 参数读回，不能据其名字把它当成电机
物理加速度测量或全部短样本的执行证明。

更低层还存在 `btdrvWriteHidData` / `btdrvWriteHidData2` 原始 Bluetooth Output
报告接口。但其存在不能证明 NRO 可与 HOS 的同一手柄输出稳定共存，也不能覆盖
掌机轨道连接。系统输出竞争、报告计数器、设备定位和连接方式都需要独立证据。
未证明普通 libnx 调度无法复现目标效果前，不能把“接管蓝牙”视为必需条件。

### 22.4 压缩样本、保真目标与验证边界

**Evidence：** 使用 MissionControl commit
`71a837fcff422a652bd1326a214f925792ae8449` 的
[decoder](https://github.com/ndeadly/MissionControl/blob/71a837fcff422a652bd1326a214f925792ae8449/mc_mitm/source/controllers/switch_hd_rumble_decoder.cpp)
与 types，在桌面编译原代码（仅补 Stratosphere 的类型、宏与标准库头），按
`40400100 -> c0050000 -> 40400100` 输入，得到：

| motor word | 样本数 | low amp / low Hz / high amp / high Hz |
|---|---|---|
| `40400100` | 1 | `0 / 160 / 0 / 320` |
| `c0050000` | 3 | `0 / 160 / 0 / 320` → `0.0441942 / 160 / 0 / 320` → `0 / 160 / 0 / 320` |
| `40400100` | 1 | `0 / 160 / 0 / 320` |

这说明四字节 motor data 不能一概当作一组静态频率/振幅；它还可能包含多个
有状态的压缩样本。只取第一个样本会把这里整个脉冲变成静音。
MissionControl 的 `switch_vibration_processor.cpp` 明确只取首样本，这是它向
其他硬件转换时的取舍，不能照搬为本项目“HD 保真”的正确性依据。

**Conclusion / 设计边界：** 完整保真输出层应在通用 rumble 与原生效果之间
保留区别：普通命令保留原始强度和时序，按可复核的参考驱动映射；原生命令
保留完整样本序列、每侧解码状态、停止/替换关系及时间顺序，再交给原生输出。
不应先把原生包降级成双路强度，也不应为宣称“更 HD”而给真实原始效果添加
未经参考验证的频率、尾音或平滑。软件层应记录原始命令、解码结果和实际提交
时间，以便定位是哪一层丢了信息。

**Evidence / 输出开关：** 离线脚本也执行原始 eligibility 判断
`0x1386746f0` 及其类型判断。固定 type 38 对象必须同时满足全局
`0x13980d070 != 0` 和对象 `+0x588 != 0` 才输出普通/原生效果；关闭任一
条件都不输出。这里只设置这两个输入值，没有执行完整 Steam 配置/初始化流程。
实际串流还需从原始命令 trace 确认主机已启用原生输出，不能由声明 type 38
推断游戏会发送效果。拒绝的原始命令和激活失败同样进入有界 trace，便于核对。

**Boundary：** 离线脚本使用合成 controller 对象、eligibility 输入、时钟、锁与
最终 HID/SDL 记录终端；执行的是原始 eligibility、producer、protocol forwarder 和 receiver。
没有执行完整 Steam Input API 调用、真实 RemoteHID 网络、Switch IPC 或电机。
MissionControl 解码结果是独立实现对照，也不是 HOS/控制器固件的时间执行证明。
不能从这些结果宣称用户游戏必发原生包、实际样本间距已确认、HD 输出已完成，
也不能推断 Switch 的 hbmenu/loader 生命周期。

“准确”的验收对象应明确为同一输入下参考 Steam/控制器驱动的输出，而不是
主观觉得更强。包级比较验证参数、顺序和丢失；设备 IPC 读回验证系统接受的值；
对同一手柄的直接连接与串流重放进行相同条件的加速度频谱/时间包络测量，才能
进一步评估物理频率、能量和脉冲宽度。手柄自带 IMU 的采样带宽不应默认足以
测量最高频震动；传感器带宽、采样率和安装方式需要说明。

复现（两份固定官方二进制均由操作者自行提供，不随仓库分发）：

```sh
python3 tools/steam-rumble-host-audit.py \
  --host /path/to/steamclient64.dll \
  --linux-client /path/to/steamlink \
  --output /tmp/steam-rumble-host-audit.json
```

### 22.5 原生接收、解码与输出契约

`sdl_hid_write.c` 接受完整 69B `0x0d`，检查内嵌长度不超过 64，复制命令与
单调接收时间后入有界队列。SDL owner drain 将原始效果交给 provider 的可选
`output` 回调；未提供回调的平台仍走 SDL SendEffect。Switch 回调拒绝未知
设备报告，不把它转成普通 rumble。原有输入报告格式及手柄身份保持协议约束。
接收、应用、拒绝与队列丢失均有独立计数；移除每次成功 DeviceWrite 的日志，
避免反向输出制造会话日志背压。

`hd_rumble.c` 用显式掩码和整数对数坐标实现七种压缩格式，包括两侧独立状态、
0–3 个样本、绝对及相对频率/振幅。其输出不是两路强度，而是每个 actuator
的 low/high 两组频率和振幅。普通 `0x01` 使用固定 Valve SDL 的 Nintendo
量化参数，详见 §22.8；不将参数对照当作完整 PC SDL 驱动时序或物理感受证明。

**Evidence / 时间参考：** RenHDrum 的
[5ms 多采样研究](https://github.com/trzz1214/RenHDrum/blob/f5223f63993cb317d24e3c9fd91619b8f527e7fd/docs/research/02-amfm-multisample-and-crisp-notes.md)
描述 15ms 报告中的三个 5ms slot，并公开了原生编码频率的独立声学实验。
本项目据此选用 5ms 软件播放间隔；没有采用其 attack boost、sustain decay
或其他增强处理。该资料是外部研究证据，不是本项目 HOS IPC 或马达的实测。

`haptic_stream.c` 保留每包全部样本和接收时间间隔，按单调绝对期限调度。
输出过晚时只延后与前次实际输出间距不足的样本，原有空闲间隔吸收延迟后恢复
源时间轴；不平移整个源时间基准。禁止把多个过期样本瞬间发完或只取首样本。
96 帧队列溢出明确报错并统计丢样；已经收到的有效压缩状态仍推进，避免后续
相对编码永久漂移。接收队列本身丢包时无法补造源状态，必须按计数判为保真失败。
普通命令显式替换待播原生序列，普通持续时间到期发零；原生最后值维持到下一
命令或会话停止，不添加尾音。断线、会话停止和设备形态切换清空待播帧。

`switch/rumble.c` 在 SDL owner 上定位和激活设备，输出 worker 只持有复制的
原生 handles 和参数，通过自己打开的 `hid` service 发送 IPC 206；不进入 SDL、
全局 libnx HID session 或日志回调。每次成功提交完成后，下一帧至少间隔 5ms，
IPC 等待造成的额外时差保留在 trace。已选中但尚未提交的任务在发送前重新检查
binding、命令和停止状态；取消的任务不再进入 HID，已在途调用完成后必须停旧
binding。设备切换先向旧 binding 发零，失败重试；
退出按 stop → join → close 顺序，停止失败单独计数，不能解释为已停止。

诊断构建保留有界 512 条命令/提交记录和递增 serial，覆盖即计数。应用的只读
`haptics` / `haptic-trace <serial>` 可观察真实游戏是否发送 `0x0d`；不需要在
渲染线程逐包打印。采集工具见 `tools/capture-switch-haptics.py`。

### 22.6 分层验证及判定边界

| 验证 | 证据入口 | 能证明什么 |
|---|---|---|
| 官方 producer / receiver | `steam-rumble-host-audit.py`，固定官方二进制原指令 | 特定主机实际生成的包和接收端转交内容 |
| 独立解码对照 | `check-switch-haptics.py` | 固定输入序列的样本数、顺序、两频带参数；不含时间与硬件 |
| 实际接收链路 | CTest `hid_sdl_output` / `nsl-haptic-pipeline` | 真正 DeviceWrite、复制队列、owner drain、生产解码/线程；最后 HID 为 mock |
| 接收/播放时钟交错 | CTest `nsl-haptic-source-time` | 同一收包历史在不同 drain 延迟下保留相同解码参数/顺序 |
| 夹具自身离线集成 | CTest `nsl-haptics-probe-host` | 真正 NRO 夹具、格式化日志、worker 和分析器相互兼容；HID/socket 为模拟终端 |
| 原生请求序列化 | `check-switch-haptics-ipc.py` | 生产 ARM64 分支与未修改 libnx IPC 206 的请求描述及缓冲字节一致；不执行 HOS |
| 真机自包含夹具 | `switch-haptics-probe.py` + `nsl-haptics-probe.nro` | 实际 IPC 提交、系统参数读回、软件时间、零输出及 worker join |
| 实际马达 | 同一手柄的原生直连与串流重放，加外部带宽充分的传感器 | 频谱、时间包络、能量、脉冲宽度的物理差异 |

独立解码器固定在 MissionControl `71a837fc…8449`，脚本核验三个源文件的 Git
blob hash，只在忽略的 build 目录构建独立参考进程。其 GPL-2.0-only 代码不
复制或链接进本项目；生产实现按协议事实独立编写。对照包含全部 16,384 组
7bit 绝对幅频组合、全部 32 种短命令及固定种子的有状态随机序列；固定语料
共 82,184 words / 139,972 samples。该语料不能等价于穷举所有状态。

真机夹具从实际 IHS DeviceWrite 入口注入两侧各六次官方短脉冲，再注入两频带
80/640Hz 的绝对值和普通停止。刺激全部编译进 NRO，不需 SD fixture 或 Steam
主机。活动期间将提交与读回存入内存，worker join 后才批量发送高频诊断。
读回同时记录调用前后时间；跨越下一次提交的读回不能算某一短样本的证明。
主机分析器使用独立参考进程重新解码原包；缺命令、丢样、错序、参数错误、
停止/清理失败均拒绝软件通过；检查跨包全局顺序、显式停止与最终清理的各自结果。
包内样本间隔或相对接收时间轴的误差超过 500µs、或读回不全，另外失败。
带部署的调用保留 nxlink 返回码和超时，收集失败时即使局部软件记录通过，也不
以零退出码宣称整次验证通过。
500µs 是验证工具的观察阈值，不是控制器规范，也不改变生产调度。
负向对照专门检查这些失败条件，缺日志不能变成通过。

```sh
cmake --build build/desktop --target nsl-hd-rumble-oracle
python3 tools/check-switch-haptics.py \
  --oracle build/desktop/app/nsl-hd-rumble-oracle
cmake --build build/switch --target nsl-haptics-probe_nro
# 仅在用户已准备好 hbmenu netloader 时运行一次；不做 TCP 端口探测。
python3 tools/switch-haptics-probe.py --nro build/switch/app/nsl-haptics-probe.nro
# 已有日志可以独立重验，不部署设备。
python3 tools/switch-haptics-probe.py --log build/haptics-device.log
```

**Boundary：** 上述工具从不将 nxlink 返回、worker join 或 IPC 参数读回写成
`physical_fidelity_verified=true` 或 `loader_return_verified=true`。验证设施和
可交叉编译的输出实现不等于已完成真机验收；某个游戏是否发原生包必须看该会话
的实际 trace。任务验收及尚缺实测只记录在 GitHub Issue，不在参考文档维护进度。

### 22.7 调度与验证反例的更正

**Evidence：** `4c14981` 的 `sl_haptic_advance` 把每次唤醒迟到加到
`playback_epoch`，`emit` 又把 IPC 时长加到同一源时间基准。以 50ms 包间隔
发送 100 个三采样脉冲，每个样本模拟迟到 100µs，得到源时间基准累计偏移
30ms；即使空闲间隔足够也不恢复。旧验证器只检查包内 5ms 间隔，不能拦截
这样的跨包偏移。`test_haptic_stream.c` 的连续包和预入队间隔反例约束此行为：
只推迟实际冲突的待播帧，后续空闲间隔吸收迟到和 IPC 耗时，源锚点固定。

**Evidence：** `test_rumble.c` 的双设备阻塞 IPC 反例确认：停止会话时，旧
worker 仍会发送本批次中后一个尚未提交的非零任务。发送前的世代/命令/停止
检查取消这些任务，`canceled_outputs` 单独计数；无法把已进入 IPC 的调用撤回，
其返回后的旧 binding 零输出仍是必需步骤。

**Retraction：** 撤回将旧 `emitted_samples` 解释为系统成功提交样本数的说法。
它原来在出队、尚未调用系统时就增加。`scheduled_samples` 现在表示出队选择，
`emitted_samples` 仅计成功的原生提交；失败重试、取消和迟到分别观察，迟到以
实际提交时刻而非本批任务被选择的时刻计算。接收时间也在等待 SDL 设备锁之前
获取，锁等待不应消失在接收间隔/交付延迟的证据中。

**Evidence：** 普通命令与后续原生包在同一 owner drain 批次应用时，旧实现从
原生包重新建立时间锚点，导致两者之间的接收间隔消失，并立即取消普通到期
停止。`test_haptic_stream.c` 增加 10ms 普通震动后接 5ms/50ms 原生包的反例。
普通输出也建立接收锚点；播放到期与源时间上的解码状态到期分别处理。普通
先到期时不清除未来的原生帧，原生先播放时取消旧普通计时器；零采样的另一侧
重复到期后的解码状态，避免恢复已经停止的普通强度。

**Evidence：** `test_haptics_verifier.py` 增加整包错序、跨包累积偏移、显式停止
非零、单 actuator 覆盖不足和收集失败/超时的反例。显式停止与退出清理分别
检查；后续任意输出之后的读回不能再用于证明前一个样本。

**Boundary：** 固定 5ms 最小保持时间优先保留所有样本。无空闲的持续满载输入
遇到 IPC 耗时或调度迟到仍可能积压、延迟甚至溢出，不能据此实现宣称硬实时
200Hz；这些是应从 trace 验证的限制，不以源时间基准漂移掩盖，也不靠瞬间
补发样本隐藏。普通 `0x01` 的参数及混合相对解码采用 §22.8 的独立对照；
完整 SDL 驱动的排队/刷新时序与物理等价性仍不由参数对照证明。

### 22.8 接收历史、普通量化与原生序列化的离线交叉验证

**Evidence / 更正：** `eeb225e` 在普通播放计时器到期时仍清除了 wire decoder
状态。如果原生包在到期前收到、到期后才从 IHS owner 队列应用，相对包会继承
错误的静音状态。保留接收历史，只有后续命令的接收时间跨过源到期时间时才
使 wire 状态到期；本地播放停止与之独立。`test_haptic_source_time.c` 覆盖
7×7 两侧格式配对、8 个接收时间点、6 种 owner 延迟、配对/独立两种布局，
共 4,704 组交错。相同源输入的样本参数和顺序必须与无 drain 延迟的基线一致，
不要求延迟后的实际播放时刻仍然按时。旧实现会失败，新实现保留该不变量。

**Evidence / 普通参数：** Valve Linux Steam Link 1.4.5.326 所带
`libSDL3.so.0` SHA-256 为
`dda2c0b8617e63f6824aad07b7917c0cef58dceb8c0785faa783cbc99c1bbdb4`，
内置 revision 为 `SDL-3.5.0-HEAD-HASH-NOTFOUND`，不据此臆造上游 commit。
`check-steam-sdl-rumble.py` 执行其 High/Low amplitude lookup
`0x22e260/0x22e2f0` 的全部 65,536 档输入，以及 complete encoder
`0x230380` 的 1,120 组边界/随机参数；仅 clock 与最终 WritePacket 为终端。

| low / high | 原始完整 10B 报告 | 解码后的活动频率与振幅 |
|---|---|---|
| 0 / 0 | `10 00 00 01 40 40 00 01 40 40` | 两频带静音，默认 160/320Hz |
| 1 / 0 | `10 00 74 00 bd 40 74 00 bd 40` | low 约 0.00552427，活动频率约 149.933Hz |
| 65535 / 65535 | `10 00 74 c8 3d 72 74 c8 3d 72` | 两频带各约 0.5571934、149.933Hz |
| 4660 / 43981 | `10 00 74 a4 bd 46 74 a4 bd 46` | low 约 0.0441942，high 约 0.3772911，均约 149.933Hz |

**Retraction：** 旧线性强度、160/320Hz 仅为 D-048 的近似，撤回其足以代表
固定官方客户端普通参数的解释。合法振幅上限仍为 1.0，不等于参考编码器会
把输入最大值映射为 1.0。`sl_hd_ordinary_word` 采用固定参考的量化表/编码，
再由同一原生 decoder 产生幅频参数并设置精确相对状态，不用 float log 近似
反推 decoder。配对 Joy-Con 保留低/高分侧规则，独立/Pro 保留两频带。
阈值表的源码参考为已核验 SDL `a962f40b…`，版权/许可在
`third_party/sdl-rumble-LICENSE.txt`；只复用量化表，查找/打包作了适配。

生产普通编码与原始 lookup/full-encoder 共 66,656 组参数逐字节对照，其中
low/high 各覆盖全部 uint16 值；不是穷举 2^32 种组合。随后将每个普通 word
接三样本相对原生命令，生产 decoder 与独立 MissionControl 进程共比较
266,624 个样本。此处证明参数/解码状态，不模拟 SDL 30ms 写队列、50ms 刷新、
API duration 行为或所有平台/版本的同一硬件表现。

**Evidence / 夹具与验证器：** `nsl-haptics-probe-host` 编译实际 NRO 夹具，
仅替换 libnx HID/applet 和 nxlink socket 终端。原始刺激、DeviceWrite、worker、
清理及 JSON 格式化均运行真实代码，并将输出交给同一个分析器和独立 decoder。
必须出现完整 13 条原生命令、37 个原生样本、普通停止与退出清理，且原始
刺激逐字节符合固定夹具，不对任意意外刺激自洽地解码后便认证通过。

验证器拒绝停止/清理发错 slot、完成时间早于提交、提交早于自身 deadline、
空/异常 motor 数组。host 模拟用 `platform=host-simulation` 标记，参数读回
可以匹配，但 `ipc_readback_verified` 与 `stopped_ipc_readback_verified` 必须为
false；不能把模拟系统当成真实 HOS。时序是否符合 500µs 阈值独立报告，host
测试只验证软件数据/日志契约，不因模拟器时序抖动而捏造硬实时证明。

**Evidence / 原生请求：** `check-switch-haptics-ipc.py` 用实际 Switch 编译器
编译生产 `io_send`，不定义 `NSL_RUMBLE_TEST`，并链接安装 SDK 中未修改的
`libnx.a(hid.o)`。在 ARM64 中分别运行生产路径与原始 `hidSendVibrationValues`，
仅在 TLS/内存/ARUID 终端和最终 SVC 边界记录。Pro、handheld、JoyDual、单左、
单右与零输出六例的 IPC 206 header、resource ID、两项 pointer descriptor
及对应句柄/幅频缓冲逐字节一致。SDK archive SHA-256 为
`12a59cdfdceab83e39ee129b0275dd803e393e83f19de3922a3b6cc95c767edd`。
这比 host mock 更直接验证目标架构的请求构造，但不执行服务打开、HOS 接受/
响应、连接权限、实际马达或系统调度。

```sh
# 需要本机 Switch SDK、Python unicorn/pyelftools；不连接设备。
python3 tools/check-switch-haptics-ipc.py --output build/haptic-ipc.json
python3 tools/check-steam-sdl-rumble.py --sdl /path/to/libSDL3.so.0 \
  --oracle build/desktop/app/nsl-ordinary-rumble-oracle \
  --decoder build/desktop/app/nsl-hd-rumble-oracle \
  --reference build/haptic-reference/reference --output build/ordinary-parameters.json
python3 app/tests/test_haptics_probe_host.py \
  --probe build/desktop/app/nsl-haptics-probe-host \
  --reference build/haptic-reference/reference --output build/host-fixture.json
```

**Boundary：** 以上是源码、固定原指令及模拟终端的离线证据。真实系统参数/
时序、完整驱动行为、机械波形和 hbmenu 生命周期仍需单独实测。

### 22.9 系统读回与提交边界的区分

**Evidence：** 在 `26d2954` 的一次实际 handheld 夹具记录中，13 条原生命令
解码并成功提交 37 个样本，39 次输出 IPC 全部返回成功，停止后读回为零，
worker starts/joins 为 1/1。包内提交间隔相对 5ms 的最大误差为 31,156ns，
相对接收源时间轴最大误差为 58,828ns。此处是应用提交时间，不是马达播放时间。
固定参考参数/顺序相同，但 571 次系统读回只在原本严格窗口内匹配 15/37 样本。
读回里的短脉冲非零振幅为 0.0515766852，提交的是 0.0441941731；两频带
绝对值读回为 0.256206423，提交的是 0.255474287。频率分别仍为 160Hz、
80/640Hz。部分读回非零值在对应 5ms 提交窗口后出现，部分窗口内持续为零。

**Conclusion：** 这份记录证明 IPC 调用成功、应用参数和提交时序保留，并证明
该设备/系统上的参数读回不能直接当作逐样本原值回显。严格真机验收未通过。
不从迟到/缺失读回推断马达漏播或系统重采样，也不因 IPC 成功推断马达完整播放。

**Hypothesis / 待验证：** 系统/设备的幅频转换、读回刷新粒度、内部排队或
初始化时序都可能参与差异；这份记录不足以区分这些原因。不能据两个振幅点
拟合补偿函数，也不能直接放宽比对误差或把提交间隔改成 15ms 来得到通过。

`switch_haptics_calibration.c` 是隔离这些变量的自包含对照，不修改生产输出：
route 0 在 owner 上调用原始 libnx IPC 201（两侧顺序调用），route 1 调用原始
libnx IPC 206，route 2 通过生产 private worker IPC 206。三条路径使用相同
绝对 motor words：左右低频短脉冲的中间值与双频带 80/640Hz；先用 60ms
保持值观察稳态，再用 5ms 保持观察读回变化。初始零输出观察 200ms，记录
HOS version、设备类型/位置、mounted 和 interface 查询结果；错误码原样保留。
worker 的实际提交和每次读回前后时间全部留在 RAM，stop/join 后再发送日志。

分析器分别报告各路径的稳态值和首次观察变化的时间，不将三个路径彼此一致
等同于源参数一致，也不将长保持对照等同于短样本保真。原生夹具的 500µs
阈值及逐样本读回检查不变。即使本对照全部完成，strict 5ms、physical 和
loader 验证标志仍为 false。重新部署应针对这个明确差异，经用户准备后执行，
不能从 nxlink 正常退出推断设备已回到 hbmenu。

**Evidence / 三路径对照：** `07f9650` 的一次实际记录包含完整 45 条事件、
15 次生产 worker 样本提交及 1,568 条读回，三路径均无调用错误，最终归零并
完成 worker join。设备查询为 HOS 22.1.0、handheld、interface=2（libnx 的
Rail），两个 mounted actuator 的 type=1（LRA）、position=1/2。长保持值在
末尾 20ms 读回窗口稳定，三个路径的读回彼此相同：

| 请求 | libnx IPC 201 | libnx IPC 206 | 生产 private worker IPC 206 |
|---|---|---|---|
| 左侧 low 0.0441941731 / 160Hz，右侧静音 | 左 low 0.0472957939 / 160Hz | 相同 | 相同 |
| 右侧 low 0.0441941731 / 160Hz，左侧静音 | 右 low 0.0472957939 / 160Hz | 相同 | 相同 |
| 两侧 low/high 0.255474287，80/640Hz | 两侧 low/high 0.256206423，80/640Hz | 相同 | 相同 |

长保持期间，首次观察到非零值在提交后约 2.18–14.60ms；这包含读回采样误差，
不是物理马达起振延迟。5ms 短保持的不同路径/侧别有的观察到非零，有的在
整个主动值与停止观察阶段持续为零；不由此推断物理漏播。首轮短脉冲记录中
同一请求振幅的非零读回为 0.0515766852，也与本轮长保持不同。

**Conclusion：** 至少长保持的幅频读回偏差可在直接 libnx 调用复现，不是
生产 private session/worker 独有的结果。三路径稳态一致只是对照证据，仍
未证明系统内部编码正确或错误，更未验证原生压缩脉冲的物理等价。严格原生
夹具的失败不被此诊断完成覆盖。轨道路径不能用仅覆盖 Bluetooth 的 raw
output API 宣称已解决；需要进一步区分读回语义、系统编码与实际设备执行。

### 22.10 原生脉冲的上游能力与配置门禁

**Evidence / 实际串流：** `bff8af7` 的一次真实会话中，Activity 记录为
`gameid=1049320 name=River City Girls`。从 serial 1 开始的 175 条命令/输出记录
连续、无缺号；84 条普通命令中 41 条非零，43 条停止。两个成功接收的 10B
Nintendo 效果都为静音；没有非零原生脉冲。另两个被拒绝的 effect 是 `03 30`
和 `38 01 f0 f0 00` 设置命令，不应当作丢掉的游戏 HD 震动。IPC 输出无失败。
这只证明本次路径未产生非零原生效果，不证明游戏或其他平台版本绝不支持。

**Evidence / 固定主机：** `steam-haptics-routing-audit.py` 继续使用 §20.1 的
DLL/SHA，在离线执行原始指令，而不从静音包或普通震动推断脉冲门禁已开启。

| 阶段 | 原始地址 | 观察 |
|---|---|---|
| 活动配置应用 | `0x138665a20..0x138665c3d` | `+0x649` 仅在 preferences `+0xd4` 非零，且选用的 caps bit 13（`0x2000`）置位时为 true；普通 rumble 在独立 `+0x648` 字段检查 `0x204000` |
| 配置到 slot | `0x13860edae..0x13860edb5`、`0x13860f540..0x13860f547` | 从活动配置 `+0x649` 复制到 `CHIDIOThread + slot*0x2a0 + 0x1d1` |
| 脉冲入队 | `0x138621620` → `0x1386214c0` | flags 低两位为零时，slot 的 pulse-enable byte 为零则不入队；两位任一置位可绕过这道门禁，不能据此把它当作客户端应伪造的协议位 |
| work item | `0x138617a90..0x138617b61` | 使用原始 Generic GetInfo `0x138675e00`，Switch 目标发生转换，再调用 abstraction slot 32 |
| 末端输出 | `0x13867cce0` → `0x13867f7f0` | 仍检查 §22.4 的 writes/enhanced 条件，输出固定 Nintendo 短脉冲 |

11 个路由条件对照覆盖入队开关、flags 和末端 writes/enhanced，4 个配置对照
执行原始配置应用，并把计算的 enable byte 交给上述原始路由：

| 配置输入 | rumble enable | pulse enable | 后续原生非零包 |
|---|---|---|---|
| 当前客户端 caps `0x834bff`、haptics preference=true | true | false | 无 |
| caps 改为 `0x836bff`（加 `0x2000`）、preference=true | true | true | 有 |
| 同一 caps、preference=false | true | false | 无 |
| 不提供 caps override，使用主机 Switch Pro 默认值 | true | false | 无 |

**Conclusion：** 末端 producer 能编码、普通震动成功、Nintendo 身份或静音包
到达，都不足以证明非零脉冲的完整路由已启用。当前声明没有 `0x2000`，这构成
能力/配置阻断的具体候选；不能直接将游戏无原生输出归因于游戏本身。

**Boundary / 待验证：** 主机默认 Switch caps 原始表 `0x1386571d0` 对该类型
也不包含 `0x2000`。不能仅凭新增位使合成配置通过，就判定当前声明违反官方
契约或应立即改变默认行为。脚本没有执行真实 RemoteHID caps → 活动配置、
游戏 API adapter/handle 解析、配置载入或线程队列调度；这些输入/连接和锁、
分配器、时钟、最终 HID 为明确替身。现有默认 caps、真实生效配置及当前 DLL
版本必须继续核对。此处不改生产能力声明，也不宣称某个 Steam 游戏已提供
任意频率波形。

公开能力说明还应区分硬件：[当前英文 Steam Input API 文档](https://partner.steamgames.com/doc/api/isteaminput?l=english)
的 legacy/simple haptic 支持名单为 Steam Controller/Deck；固定二进制仍有
上述 Switch 分支，两者不能互相替代为完整游戏兼容性证明。
[Sony 的 PC 手柄支持页](https://www.playstation.com/en-us/pc/pc-accessories/)
确认 Returnal、Ratchet & Clank: Rift Apart、Spider-Man 系列等的 DualSense
触觉，但不是 Nintendo 原生效果或 Remote Play 保留该效果的保证。

```sh
python3 tools/steam-haptics-routing-audit.py --host /path/to/steamclient64.dll \
  --output build/haptics-routing.json
```

### 22.11 当前 Windows Host 文件与默认布局核对（2026-11-18）

**Evidence / 用户资料：** 用户确认本次使用默认手柄布局，并提供当前
`steamclient64.dll`、`controller.txt`、`streaming_log.txt`。DLL SHA-256 为
`669fa08e259277fbf5a1dfe15ce88afe734e89151194304b44d92f418dcc985b`，
FileVersion `11.23.65.88`，PE timestamp `2026-11-13 02:02:02 UTC`。
它与 §20.1 固定 DLL 不同；不能沿用旧版代码地址或对象布局。

`controller.txt` 的本次记录为：

- 第 35060–35070 行：`2026-11-18 00:50:46`，远程 `sdl://0`、
  `057e:2009`，产品名 `Nintendo Switch Pro Controller`，设备打开并保留
  XInput slot 0。
- 第 35088–35099 行：AppId `1049320` 的 warmed config 多次记录
  `Controller 0 uses xinput : true`，随后排队激活该游戏配置。
- `streaming_log.txt` 第 11120–11129 行确认本次游戏为 `River City Girls`；
  第 11243 行起的 SessionStats 同时记录 `GameControllerVID=1406`、
  `GameControllerPID=8201`（十六进制为 `057e:2009`）。

**Conclusion / 实际会话：** 游戏采用 XInput 映射，与 §22.10 设备端捕获的
84 条普通命令、零条非零原生效果一致。不能把“游戏侧 XInput 映射”与
“远程硬件被误报为 Xbox”混为一谈；这里远程身份是 Nintendo。默认布局和
这些日志都没有记录最终 caps override、haptics preference 或游戏每次 API
调用，因此也不能从它们推断全局触觉偏好或脉冲门禁已打开。

**Evidence / 新版原始指令：** 对上述 SHA 单独定位 RTTI/vtable、函数和字段，
扩充 `tools/steam-haptics-routing-audit.py`。两份 DLL 各执行 20 个路由条件与
4 个配置条件，均通过；配置/对象地址不同，不能用统一地址平移代替重定位。

| 阶段 | 当前 DLL 地址或字段 | 直接观察 |
|---|---|---|
| 缓存信息到活动配置 | `0x13860485d..0x138604897` | 原始复制片段将 supplied cache-info `+0x38` 写到 config `+0x240`；该片段的缓存来源和选择仍是合成输入 |
| 活动配置应用 | `0x13867e0b0..0x13867e2cd` | config `+0x240` 或类型 `+0x230` 的默认 caps，仍要求 `0x2000` 与 preferences `+0xd4`；结果写入 `+0x661`；普通 rumble 使用独立 `+0x660` |
| 配置到 IO slot | `0x13862520e..0x13862521d` | 原始复制片段将 config `+0x661` 写到 slot `+0x1d1`；另一静态复制点为 `0x1386259a0` |
| SteamInput006 legacy pulse adapter/core | `0x138738510` → `0x138637ac0` | 原始 adapter、core 与内部入队路径执行；handle lookup、锁与外部运行环境为替身；无效合成 handle 不入队 |
| 脉冲入队与 work item | `0x138637920` → `0x13862df10` | slot 门禁仍独立于普通震动；flags=0 时关闭则不入队 |
| Nintendo 原生输出 | `0x1386953e0` → `0x138697ef0` | 能生成含非零原生效果的 69B `0x0d` 包；关闭末端 writes/enhanced 时不输出 |

当前 caps `0x834bff`、偏好开启时普通震动为 true、pulse 为 false；
`0x836bff` 且偏好开启时 pulse 为 true 并生成非零包；关闭偏好又恢复为
false。当前 Host 的默认 Switch caps 仍为 `0x24814bff`，也没有 `0x2000`。
当前 DLL 的 API adapter 正对照还核验 duration `1`、`1000`、`65535`：
同一目标的非零效果包逐字节相同，重现 §22.2 的固定短脉冲降级。

**Conclusion / 能力范围：** 当前 Host 仍有 Nintendo 原生短脉冲分支；
我们目前的声明不能打开这道常规 pulse 门禁。这是具体能力缺口候选，不能
把本次只有普通震动直接归咎于游戏。反过来，加能力位只会允许已有的脉冲
调用通过；它不使只提供普通强度的游戏凭空生成频率/波形，也不使这条固定
短脉冲分支成为任意 HD 波形输出。DualSense 游戏触觉支持不能替代本路径验证。

**Boundary / 未验证：** 测试执行原始复制片段与原始 API adapter/core，但
没有执行真实 RemoteHID capability 解析、真实缓存选择、配置载入、handle
解析、游戏调用或 Steam 网络发送。片段连接、对象初始值、分配/锁/时钟、
工作队列及最终 HID 为明确替身；不能称为实际主机上的端到端测试。用户的
默认布局描述不能替代这些运行时证据。尚不能据此宣称增加能力位是完成修复，
或宣称某款 PC 游戏经本链路已提供非零 Nintendo 高级震动。设备 HOS 读回与
物理保真边界仍按 §22.9 保留。

API 槽位顺序另以 Valve 发布的
[Steamworks ISteamInput 头文件](https://github.com/ValveSoftware/Proton/blob/proton_11.0/lsteamclient/steamworks_sdk_163/isteaminput.h)
核对：SteamInput006 `+0x110` 为 legacy pulse；这只解释调用入口，不证明
任何游戏实际调用它。可复核的输入散列、原始反汇编、两版 JSON 报告存于本地
`build/haptics-host-20261118/`；用户二进制与完整日志不纳入仓库。

### 22.12 放行例外、haptics 偏好与效果来源的区分

**Correction：** 撤回面向用户解释中将“缺少 `0x2000`”概括成“所有原生效果
都不能发”的表述。§22.10 的门禁只适用于该 pulse 入队函数 flags 低两位为零
的路径；同节已记录的非零 flags 例外不能省略。也不能将“原生”解释为必须
由游戏制作 Nintendo 编码，或将单个 legacy pulse producer 的限制扩展为
Switch 硬件、全部 Steam 后端或全部 `0x0d` 容器的限制。

**Evidence / 放行例外：** 当前 DLL `0x13863795e` 的 `test bpl,3` 非零时，
跳过 slot `+0x1d1` 检查。既有原始指令对照确认 flags=1/2、pulse-enable=false
也能入队并输出非零 Nintendo 效果；它证明函数允许该路径，不证明真实游戏
或某个 UI 操作必定使用这些 flags。额外定位内部调用 `0x13862a810`：
`0x13862a938..0x13862a98b` 对两个目标各调用一次原始 CSteamController slot
`+0x50`，参数为 duration=0、flags=2。补充对照执行此原始调用，分别设置
pulse-enable=false/true，均入队两个 work item、输出两个原生静音包。独立的
可选设置刷新 predicate 为返回 false 的替身；没有执行完整刷新路径。该调用
可以解释“开关关闭仍能输出停止”的机制，但没有运行时调用栈，不能断言此前
两个实机静音包一定来自这个调用。

**Evidence / haptics 偏好：** `SetControllerHapticsSetting` 的 RPC 字符串
`0x139356820`、dispatch `0x1387d8545` 调用 serialized slot `+0x468`；thunk
`0x1389c0040` 调用 CSteamController slot `+0x5b8`，即 `0x138632090`。
该 setter 将 supplied bool 写到待保存 preferences 的 `+0xd4`，即 §22.11
配置门禁读取的字段。原始偏好构造器 `0x13860e910` 在其子对象 `+0x8c`
初始化为 true（聚合 preferences 的 `+0xd4`）；序列化 `0x13862f258` 使用
KeyValues 名 `haptics`。补充对照执行构造器及 setter 的 false/true 两值，
分别得到默认 true、保存值 `[0,1]`；字符串分配、偏好读取/持久化与释放为
替身，未读取用户设置，也未确认当前 UI 中的具体入口。不能把它描述成
DualSense 专属开关，或说用户已试过、当前关闭导致了问题。

**Evidence / 历史边界：** 当前 root/SDK 历史及这轮真机记录没有将 `0x2000`
加入生产 caps 后故障、再撤销的证据。开启该位的正对照是在离线合成配置中，
本轮部署的 `bff8af7` 未添加该位。此前移除 `is_generic_gamepad` 导致输入失效
是 §17 记录的另一变更；motion `0x800` 也是另一能力，不能混为此次 haptics 位。

**Evidence / 不同入口：** legacy pulse work item `0x13862df10` 调用 Generic
slot `+0x100` 的 Nintendo producer `0x1386953e0`，再通过 `0x0d` 容器输出。
这些位置不同，但属于同一条调用链。另一个 `TriggerSimpleHapticEvent` work
item `0x13862dd60` 调用 abstraction slot `+0x160`；当前 Generic vtable
`0x139318fd0` 的该槽为 `0x1380cf3d0`，只有 `mov al,1; ret`，没有输出。
原始 worker/实际 vtable 目标的离线对照确认该 no-op；此结论只覆盖此 Generic
abstraction，不能代替其他硬件后端的实现。因此不能把所有名为 haptic 的
公共 API 都当作已验证的 Nintendo 非零效果入口。

**Conclusion / 来源：** Nintendo 原生包描述末端格式，不限定谁产生效果。
游戏可调用抽象触觉 API，由 Steam 编码；Steam Input 也可为输入操作生成
反馈。Valve 的[输入模式文档](https://partner.steamgames.com/doc/features/steam_controller/input_source_modes?l=english)
为 mouse/joystick/scroll wheel 等模式列出 Haptics Intensity，不能说 Steam
自身没有触觉来源。该文档不是 Switch 当前默认布局可触发这些反馈的证明。
游戏事件的具体触觉设计、Steam 生成的操作反馈、Nintendo 编码和设备执行，
需要分开验证；普通强度源不能证明恢复了原作者的频率/波形设计。验证非零
原生链路可以使用受控主机 pulse 源，无须将找到某款游戏作为唯一前提。

### 22.13 后端能力声明与受控主机源的实现契约

`IHS_HIDDeviceInfo.native_haptic_pulse` 是 provider 的显式能力；默认 false。
SDL unmanaged 枚举仅在原生 output callback 与 supportsNativeHaptics query
均存在、查询为 true 时转交；InfoFromHID 据此加入 `0x2000`。SDL rumble 的
存在、Nintendo VID/PID 或收到静音效果，都不自动打开它。Switch 的查询只读
带锁快照；owner 刷新 worker、连接和支持的 Npad 样式，枚举线程不调用 SDL/
libnx，也不激活马达。样式/worker 快照表示后端可接受相应命令，不是马达
已安装、IPC 成功或物理保真的 ACK。

该改变保留 `057e:2009`、FeatureCaps Nintendo type、Generic RAW 输入和
传感器契约。普通 `0x01` 与专用 `0x0d` 仍由原生回调处理并进入 libnx；
原生回调拒绝不能静默变成成功，也不递归进入被包装的 SDL rumble。其他未
注册本项目原生回调的平台继续使用 SDL。详见 D-A108。

SDK 的虚拟设备对照从 provider 枚举经过真实 InfoFromHID/protobuf 封装，
在控制发送边界解包核对 caps：无 output、无 query、query=false 时维持
`0x8343ff`；明确 native 支持时为 `0x8363ff`（桌面对照不加 Switch motion
位）。身份字段和 Generic 标志维持枚举输入。Switch 加已有 motion 位后应为
`0x836bff`；这不是对真实 Steam 活动配置的读回。应用集成对照同时使用真实
native query、SDK 枚举、原始效果保留及模拟 HID worker 输出。

`tools/steam-haptics-probe-win.c` 使用 Valve 公开的 SteamClient023 与
SteamInput006 ABI，加载主机自己的 `steamclient64.dll`，不携带 Valve 二进制。
它只在显式 `--probe` 且 Steam Input 唯一识别到一个 Switch Pro handle 时
发出一段普通震动与两目标各三个 1000µs legacy pulse 请求；不使用内部 flags
绕行、不改能力、偏好或布局。普通对照后留 800ms 间隔；结束请求归零、Shutdown、
ReleaseUser、ReleaseSteamPipe、卸载 DLL。`--list` 仅枚举，无震动请求。

Windows x64 编译及 Wine mock DLL 对照只验证工具 ABI 调用参数、目标选择、
请求数量和资源释放；mock 不是 Valve 实现，不证明真实 Steam IPC、进程上下文、
配置或交付成功。真实 Steam Input 是否在该工具进程的 AppID 上下文暴露远程
handle，须实际验证。`requests_complete_delivery_unverified` 只表示请求调用
完成，不能替代 Switch trace。主机原始指令离线审计、Windows 工具 mock、
真实主机运行及设备/机械验收必须分开报告。

### 22.14 真实主机源的空枚举与应用上下文边界

**Evidence：** 用户归档 `nsteamlink-haptics-probe-20261118-114208.zip` 的
`requests.jsonl` 为 steaminput_ready → probe_requires_exactly_one_switch_pro →
probe_failed；没有 controller 行或震动请求阶段。manifest exit_code=1，DLL
仍为 §22.11 的 SHA。controller.txt 同时记录远程 Nintendo 设备打开、Steam UI
769/桌面 413080 配置激活（编号语义更正见 §22.25）；测试工具启动时仅暖载 AppID 1049320 的配置。
设备侧枚举日志 caps=00836bff/nativePulse=1，采集中的成功 10B Nintendo 包
均为静音；不能称为六次源请求或非零原生交付成功。

**Evidence / 原始代码：** SteamInput006 adapter `0x1387380f0` 从自身 `+8`
取 IClientController，并将进程 GameID 作为第二参数调用 slot `+0x60`。
CSteamController 的对应实现 `0x13861a9a0..0x13861ad0b` 对每个有效 handle
检查最多八项 AppID 关联，不是全局物理设备枚举。adapter 构造器 `0x13873b400`
将 engine `+0x1a8` 返回对象放入 `+8`；engine 路径 `0x1389a7d2b` 创建原始
CSteamController（vtable `0x139304dc0`）。这些是区分全局设备存在与进程
GetConnectedControllers 为空的代码依据，尚不能从日志推断每个内部关联字段值。

**Correction：** 撤回把公开工具 Init 成功理解成“无游戏场景已经具备可用
handle”的假设。旧工具对空枚举安全退出，但测试没有发出效果；不能要求用户
更换游戏来掩盖测试器的上下文依赖。

诊断枚举可在固定 DLL SHA、SteamInput006 vtable、其内部 CSteamController
vtable 和 `+0x60` 函数 RVA 全部相符时，只读查询已观察的桌面/UI 上下文
（0、769、413080）及指定 AppID，并去重 handle。它不得修改关联数组、进程
GameID 全局、Steam 配置或采用 pulse 绕行 flags；输出仍调用公开 pulse API。
所有私有布局不匹配即拒绝，不能猜索引或函数签名。此诊断依赖与通用客户端
能力/串流实现分开；实际查询结果仍需真实主机验证。

独立执行当前 DLL 的原始过滤函数，以合成缓存包含一个有效 handle、关联
769/413080 且未关联 1049320 为输入：查询 1049320 返回 0，查询 0/769/413080
各返回 1。它验证过滤机制，不是实际主机关联读回。Windows 修正版的 Wine
测试用专门 PE section 固定 mock 的 vtable/方法 RVA；仅测试 EXE 编译时接受
mock hash，生产 EXE 仍只接受真实主机 SHA。十项对照覆盖正常公开请求、空
公开列表后的只读上下文查询、只读 list、未知 hash、错误 vtable/函数、越界
数量、歧义设备、非 Nintendo 与非法 AppID；mock 通过不证明真实 Steam IPC。

### 22.15 公开请求与主机索引 RPC 的验证边界

**Evidence：** 当前 SHA `669fa08e…` 的 SteamInput006 legacy adapter
`0x138738510` → `0x138637ac0` 先解析 handle，再按 CSteamController
`+0x10922d` 选择 `+0x180` 或 `+0x190` 的 IO 对象，调用其 `+0x48`。
本地入队 `0x138637920` 与远端事件生产函数 `0x1386379f0` 不是同一函数；
后者将 20B 事件写入 `+0xc48` ring。成功返回不构成主机消费或设备交付 ACK。
只读跨 AppID 枚举得到 handle 也不能证明公开输出已进入主机 worker。

**Evidence：** engine 创建控制器的 `0x1389a7ce3` 调用 factory
`0x13876d560`，参数为 `(0, hUser)`。该 factory 分配 32B 对象，最终 vtable
为 `0x139369980`（IClientControllerSerializedMap）。其 `+0x2a8` 是
`0x1388b6b60`，序列化 index、side、duration、off、repeat、flags 并同步发 RPC；
server thunk `0x1389c2e90` 转发至 CSteamController `+0x50`，即
`0x138637a80`，随后进入同一个 IO `+0x48`。`flags=0` 不绕过 pulse-enable。
`+0x338` / `0x13877e0b0` 读回 88B ActualControllerDetails；server
`0x138619090` 从有效 slot 拷贝详情，handle 位于 `+0x20`、内部 type 于
`+0x34`、caps 于 `+8`、valid 于 `+0x40`。Generic GetInfo 原始拷贝
`0x13868e522..0x13868e564` 与 unique-handle getter `0x13868e850` 支持
这些偏移。map 的 deleting destructor `0x1387483d0` 在 flags bit0 为真时
释放 32B；不能将新建 map 当作无所有权的 engine 缓存指针。

**Conclusion：** 此 RPC 可用于隔离“公开进程的输出转发”与“主机 worker
及原生输出”两段，不依赖任意游戏获得活动控制器关联。实际使用必须固定 DLL
SHA、map vtable 与各方法 RVA，按 ActualControllerDetails 的精确 handle/type
匹配唯一目标；不得猜 slot、强开配置、改偏好或使用绕行 flags。新 map 必须在
ReleaseUser/ReleaseSteamPipe/FreeLibrary 前销毁。RPC 成功返回仍不是网络/设备
ACK，更不是公开游戏 API 可用的证明。真实 IPC、门禁状态与交付须分别取证。

主机与设备时钟未校准时不能直接相减。验证工具应按每次请求前后 UDP 读回
trace serial、保留原始响应与主机发送/接收时刻，再关联非零 motor words 和
后续样本；静音、setup、Steam UI 自发反馈、旧 trace 与丢号必须分别处理。
六次请求不必对应六个包，左右缓存、去重、合并及内部门禁都需显式分析；只有
请求数量或 native_commands 增长不能认证交付。

### 22.16 生效状态的只读检查与门禁隔离

`ActualControllerDetails` 的 caps 含 `0x2000` 证明主机设备信息保留该能力，
不证明活动配置 `+0x240` 采用同一值、preferences 开启，或 IO `+0x1d1` 已置位。
静音效果也可能来自停止/安全路径，不能反推正常非零 pulse 门禁开启。

**Evidence / November 布局：** `GetClientEngine` 的 `0x1389a8009` 计算并对齐
静态对象地址 `0x139854550`；构造器 `0x1389a6734` 设置最终 engine vtable
`0x13938f580`。ConnectGlobalUser 的 `0x1389a7d52..0x1389a7deb` 遍历
engine `+0x108` 的树，root 在 tree `+0x18`、nodes 在 `+0x30`，node stride
128、controller pointer 在 node `+0x78`。本地 CSteamController vtable 为
`0x139304dc0`，`+0x10922d=1` 选择 `+0x180` 的 CHIDIOThread，vtable
`0x139304c68`。slot stride `0x2a0`，abstraction 在 `+0x1c0`、配置在
`+0x2a0`。Generic vtable `0x139318fd0` 的 unique getter `0x13868e850`
在 `+0xb8` 有效时返回 `+0x98` handle；`GetInfo` 从 `+0x78` 拷贝，故 caps
在 abstraction `+0x80`。普通强度缓存位于 `+0x552/+0x554`，enhanced 在
`+0x588`；这些是 producer 的门禁输入，不是物理输出读回。

**Evidence / 配置与偏好：** `0x13862520e..0x13862521d` 将 config `+0x661`
拷到 IO slot `+0x1d1`；普通 rumble 对应 `+0x660/+0x1d0`。普通入队函数
`0x13863812a` 将活动 config `+0x258` 与传入 AppID 比较。配置 caps/type
在 `+0x240/+0x230`，零 override 会走类型默认表。GetControllerPersonalization
的 `0x13861bbb0` 从 parent `+0x230 + index*0xe0` 拷贝 preferences，返回
`+0x1f0 + index*0xe0` 的有效位；copy `0x1386024d0`、默认构造及 haptics
setter 支持 preferences `+0x8c` 的字段定位。未置有效位时不能将缓存当成
当前用户偏好。其 serialized RPC 传递进程内指针（`0x13878f5dc` 检查 map
`+0x10`），不能从外部工具盲调该 getter 并把外部缓冲地址交给主机。

诊断器可对明确的 steam.exe 及 SHA 固定的 steamclient64.dll 使用
`ReadProcessMemory`，仅提取以上目标 handle 的少量字段；无写内存、注入、
调试器附加或完整内存转储。树遍历有界，检查 engine/controller/IO/Generic
vtable、handle/type/valid，重读 identity/config 指针；歧义、不支持的 32 位
布局、读取失败或不一致须报告 unavailable，禁止猜地址。它是无锁观察，重读
不构成原子快照保证；读取成功也不是 IPC/物理 ACK。可另收集磁盘配置中
haptic/rumble/vibration 的匹配行，但磁盘值不能代替活动配置或运行时偏好。

**隔离合同：** 专用主机诊断省去非零普通对照，每次停止后等待至少 800ms，
以覆盖 producer 的已知 500ms 普通冲突窗口及 50ms 单侧限制；保留逐请求 UDP
bracket。另设明确标记的 `host_rpc_flags1` 正对照，与 public/host_rpc 的
flags=0 分开记录/认证，不编辑布局、偏好或 capabilities。该正对照仅隔离门禁，
不能作为生产修复，也不能将其成功改称正常游戏 API 已通过。即使该对照失败，
也须保留实际门禁/producer/交付边界，不直接归咎于游戏或 Switch 硬件。

### 22.17 原生 producer 的跨请求缓存与逐次关联规则

**Correction：** 撤回诊断验证器中“单侧请求的另一侧必为静音”和“两次零时长
API 调用必已清空两侧源缓存”的假设。停止请求不是已发送静音包的 ACK；包的
最后播放样本为零，也不代表主机缓存的整个 pulse word 被清除。验证器不得
据这些假设把正确的双侧原包/输出判为客户端串侧，也不得简单允许任意双侧值。

**Evidence / 缓存：** November Generic producer `0x1386954e3..0x138695528`
只修改目标侧缓存 word，另一侧不变；`0x13869553d..0x138695576` 从对象
`+0x548` 开始组装含两侧缓存的 10B Nintendo payload，放入完整 69B 容器。
原始 CPulseHapticWorkItem 的目标转换也必须执行。对同一个对象连续运行原始
worker/producer：left pulse → right pulse → left stop → right stop，对应
payload 为 left-only → both-pulse → right-only → both-neutral。此原始指令
对照由 `native_sequence` 实现，锁、时钟和终端为替身，不是实时主机配置读回。

逐次参数验证从请求前最近一个**同 slot、已接受**的 Nintendo 原包取缓存基线，
检查其后至 before serial 的记录完整性，再以日志中的 side/duration/flags
执行原始 producer，核对完整容器。独立关联器只接受此固定夹具可解释的
neutral/pulse 基线，更新请求侧并保留另一侧；拒绝未知基线、缺号、错侧、
任意更改另一侧、错误样本、少于 5ms 保持及 IPC 失败。两条验证路径分开：
原始编码器检查源包参数；逐次 trace 检查该包对应的三个 libnx 提交样本。
这不是从“收到过非零包”直接推导成功，也不是把正对照成功当正常路径成功。

**Evidence / 相互抑制：** producer 的 clock imports 分别为 tier0_s64.dll
`Plat_RelativeTicks` / `Plat_TickDiffMilliSec`。`0x13869543b..0x138695459`
在 shared timestamp 距现在小于 500ms，且普通强度缓存 `+0x552/+0x554`
非零时拒绝；`0x1386954dc` 在成功组装原生包时刷新同一 shared timestamp，
包括静音请求。普通 producer `0x138694527..0x13869453b` 写普通强度缓存
及该 timestamp。故即使最初普通请求已过去较久，后续静音包也可能继续延长
该条件；不能只在最初普通对照后等待 800ms 就保证每次后续 pulse 不受影响。

原始指令连续对照：在 supplied ordinary cache=0 时，零请求后 150ms 的
pulse 可输出；cache=16384 时，同样的停止/150ms 序列只输出部分静音，
非零 pulse 被拒绝；等待 800ms 后可输出，先前目标 word 未被成功停止时又被
后续另一侧请求带入原包。它证明机制及可复现条件，**不能证明某次真实运行的
ordinary cache 确实非零**。不得将此条件复现改写为实时因果已查明。

状态读取器返回 result=0 表示该固定 client-engine 缓存中未找到匹配的本地
worker，不能解释成 Steam 没连接手柄或 haptics preference=false。它不覆盖
所有服务端对象存储；未得到 result=1 就没有相应 gate/preference 实际读回。
正常 flags=0 的受控请求及对应原包/输出可独立证明该次正常通路，无须把空
状态观察作为成功条件。仍不能据此保证某款游戏会产生这些请求、任意 HD 波形、
系统读回一致或机械保真。

### 22.18 仅使用现有 Steam 的触觉范围与设备协议路线

**Constraint / Retraction：** 产品的 PC 端只允许现有 Steam；不能要求安装虚拟
手柄、驱动、伴随服务或游戏插件。撤回此前把“虚拟 USB DualSense + 专用音频
触觉端点 + 伴随传输”作为本项目工程方案的建议。Foundation Sunshine 等方案
需要额外主机组件，超出此约束，不能作为本项目能力证明。受控 Windows 工具的
既有实验只作为诊断证据，不成为产品运行依赖。

**Correction / 验收层级：** 受控 legacy pulse 通过只能证明相应软件通路，
不能作为“真实 Steam 游戏已启用作者制作的高级触觉”的验收。完整容器相同
也不能证明游戏选择了正确输出接口。面向游戏的验收必须覆盖设备呈现、游戏
启用、真实触觉源和 Switch 实际播放，不能继续用控制台刺激替代游戏启用。

**Evidence / Steam 的转换范围：** Valve 的 [ISteamInput 文档](https://partner.steamgames.com/doc/api/isteaminput?l=english)
把 TriggerVibration 定义为左右普通强度，把 haptic pulse 与 DualSense trigger
effect 列为不同入口。当前文档的 pulse 支持名单为 Steam Controller/Deck；
本项目固定 November DLL 中另外存在 §22.11–22.17 已执行的 Switch legacy
pulse 分支。这些证据不支持“声明为任何高级手柄，Steam 就把所有厂商的高级
触觉通用翻译”的推论。Steam 会适配输入/普通 rumble，也会生成输入配置反馈；
这不等于把原生 DualSense authored haptics 自动重编码成 Nintendo HD Rumble。
[Input Source Modes](https://partner.steamgames.com/doc/features/steam_controller/input_source_modes?l=english)
中的 Haptics Intensity 是操作反馈设置，不能当作某款游戏的专用触觉开关。

**Evidence / Moonlight-Switch：** XITRIX/Moonlight-Switch
`30db8fded38ae1dbeb57f34b130afebfffe8e1c5` 的
[InputManager.cpp](https://github.com/XITRIX/Moonlight-Switch/blob/30db8fded38ae1dbeb57f34b130afebfffe8e1c5/app/src/streaming/InputManager.cpp#L243)
接收 lowFreqMotor/highFreqMotor 两个 uint16 强度，再调用平台 sendRumble。
其固定 common-c `e1d2d135820eda76628f9073d0d8c1527bd0cddd` 的反馈回调含普通/
扳机 rumble、灯光和 adaptive triggers，没有 DualSense 专用触觉 PCM 回调。
固定 Borealis `ecd896f068efe1899c09cf7b14ba548507fe7cec` 的
[sendRumbleInternal](https://github.com/XITRIX/borealis/blob/ecd896f068efe1899c09cf7b14ba548507fe7cec/library/lib/platforms/switch/switch_input.cpp#L367)
从这些强度本地生成 HidVibrationValue，包含强度到频率的换算，再调用 libnx。
这是本地 rumble 转换的源码证据，不是作者专门设计的 HD 触觉流。不能把它
与所有 Moonlight 分支等同，也不能仅凭这些源码证明主观效果好坏。该版本的
amp 字段常量还不能作为本项目合法幅度标定，不能直接照抄并宣称更保真。

**Evidence / 更丰富的实际源：** Sony 的
[PC DualSense 支持说明](https://www.playstation.com/en-us/support/hardware/pair-dualsense-controller-bluetooth/)
说明高级触觉需游戏支持，PC 上要求 USB。Nixxes 的
[Rift Apart 手柄说明](https://support.nixxes.com/hc/en-us/articles/48538226248851-Which-controllers-and-gamepads-are-supported)
列出有线 DualSense 的完整特性，并列出按游戏关闭 Steam Input 的排障方式。
Sony 的 [PC 发布说明](https://blog.playstation.com/?p=382715)明确支持其触觉与
动态扳机。它们证明这类游戏/设备组合可提供专用效果，不证明当前远程 Switch
profile 已能启用这些效果；不能指导用户在当前仅 Switch profile 下盲关 Steam
Input 并期待高级触觉或输入仍可用。

**Evidence / Steam 已有的远程设备通道：** 本仓
`third_party/ihslib/protobuf/hiddevices.proto` 的 CHIDDeviceInfo 声明设备身份，
CHIDMessageToRemote 有 DeviceWrite、DeviceSendFeatureReport、DeviceGetFeatureReport、
DeviceStartInputReports；`include/ihslib/hid.h` 的 provider/device class 提供对应
入口。故现有通道并不限于两路强度，也不需要额外 PC 驱动才能传设备控制字节。
但 SDK 的 `control_hid.c:InfoFromHID` 当前硬编码 generic 标志/usage/caps，
SDL provider 也只回答 Generic 的 feature 查询；它**没有实现原生 Steam
Controller 设备**。自定义 provider 还需扩充枚举描述契约，不能只改 VID/PID。

**Evidence / 当前 Steam 的原生设备分支：** 使用 §22.17 同一 November DLL
（SHA-256 `669fa08e259277fbf5a1dfe15ce88afe734e89151194304b44d92f418dcc985b`），
由 MSVC RTTI 定位以下 vtable，并静态反汇编；本节没有执行完整连接实验：

| 位置 | 原始行为 |
|---|---|
| CSteamControllerAbstraction vtable `0x1393181e0`，slot 32 `0x1386956b0` | 组装 `8f 08 side on:u16 off:u16 repeat:u16 flags:u8`；保留时长、间隔、重复次数，并非 Nintendo 固定短脉冲 |
| slot 33 `0x138694160..0x13869448c` | 依设备/能力选择普通 rumble 路径；其中一条计算重复脉冲参数后调用 slot 32，其他分支调用 slot 61，不能把它们当成同一编码器 |
| CSteamControllerProtocolHandlerV1 `0x139319a60`，slot 5 `0x138697cc0` → slot 9 `0x138697de0` | 为原生命令添加 report-number / padding，转交底层 HID SendFeatureReport |
| CHIDDeviceRemote `0x1393281c0`，slot 5 `0x1386e6c90` | 组装 command case 6，即远程 DeviceSendFeatureReport；这是 Steam 自带路径 |
| CSteamControllerTritonPacketAbstraction `0x139318900`，slot 32 `0x138695820` | 使用另一种 `0x81` 输出，仍携带 side/on/off/repeat；不能复用 Generic/Nintendo 的 `0x0d` 解码器 |
| 同一 Triton vtable slot 44 `0x138695a40` | `0x82` 简单触觉事件输出，不能外推到当前 Generic 的空实现 |

Valve 提供的 SDL
[controller_constants.h](https://github.com/libsdl-org/SDL/blob/SDL2/src/joystick/hidapi/steam/controller_constants.h)
也定义 `ID_TRIGGER_HAPTIC_PULSE=0x8f`、设备属性及设置查询；
[controller_structs.h](https://github.com/libsdl-org/SDL/blob/SDL2/src/joystick/hidapi/steam/controller_structs.h)
和 [SDL_hidapi_steam.c](https://github.com/libsdl-org/SDL/blob/SDL2/src/joystick/hidapi/SDL_hidapi_steam.c)
提供输入结构与初始化参考。Linux
[hid-steam.c](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-steam.c)
同样区分脉冲的 on/off/count、普通 rumble、2015/2026 不同报告；不能把一种
设备的报告发送给另一种设备身份。其 GPL 驱动仅作协议参考，不直接引入本项目。

**Conclusion / 零主机安装的工程方向：** 有源码与 Steam 原始分支支持的候选是：
在 Switch 客户端完整实现 Steam 原生支持的远程设备协议，让 Steam 自己运行
已有的输入兼容和触觉生成，再将返回的原生命令适配到 libnx。PC 不安装虚拟
设备；Steam 内部的远程设备对象也不等于 Windows 中的虚拟 USB/音频设备。
相比继续扩展 Nintendo Generic 的能力位，此路线有望保留更多实际源参数。
完整会话识别、默认布局、游戏输出选择和效果收益仍属**待验证**，不是已打通。

设备选择必须考虑真实 Switch 的双摇杆与按钮。2015 Steam Controller 的右
触控板不能无损等同于右摇杆；不能为启用触觉牺牲默认输入。较新设备协议还须
独立核验识别、握手、输入格式、触控/背键缺席的行为与输出能力，不能从上表
发现 Triton 分支便宣称任何 Steam Controller/Deck 身份都已可用。

| 实际源 | 此约束下的能力边界 |
|---|---|
| 游戏只输出普通左右强度 | 可接收普通源；原生设备路线有望使用 Steam 自带的 rumble→haptics 转换，保留它实际生成的脉冲参数。普通源也有作者设计的时间曲线，但不能据此恢复未输出的独立频率/专用波形 |
| 游戏主动调用 Steam Input haptic API | 原生设备路线是接收完整脉冲模式的候选；需核验游戏确实调用、Steam 确实下发，而非仅有 Steam Input 输入支持标签 |
| Steam 配置产生操作触觉 | 可作为无需选游戏的首次真实 Steam 功能验收；之后还必须在游戏内核验，与剧情/受击等游戏源分开 |
| DualSense 专用音频触觉，或 NS 原作专用效果资源 | 没有找到现有 Steam 远程接口会通用翻译并提供此源的证据；声明设备名称/能力不能补出主机不存在的端点、专用声道或游戏资源，不作为产品承诺 |

**验收合同：** 主机只运行已有 Steam 和游戏；不运行本项目 EXE、注入、驱动或
服务。确认设备识别与单一输入，完整验证双摇杆/按钮/gyro；用 Steam 自己的
操作触觉验证原生返回通道。游戏验收中，对同一场景分别确认普通源的转换与
游戏直接 haptic 源，客户端后台自动标记来源类别、实际参数和提交失败。
Steam Input 原生输入支持不是游戏触觉支持的证据，DualSense 支持名单也不是
此路线的兼容名单。不能把桌面反馈、诊断刺激或逐字节相同替代真实游戏验收。

### 22.19 游戏输出接口、原生设备与远程兼容层

**Correction / 架构选择的前置条件：** §22.18 的原生 Steam Controller 路线是
协议候选，不是已经选定的产品实现。发现更细的输出命令不证明真实游戏使用
它，也不证明其收益足够覆盖完整设备模拟的成本。选择该路线须先取得现行
游戏的实际输出类型证据，不能先投入完整实现再假定有相应游戏源。

**Evidence / 游戏如何制作效果：** Epic 的
[Force Feedback 文档](https://dev.epicgames.com/documentation/en-us/unreal-engine/force-feedback-in-unreal-engine)
把普通效果定义为随时间变化的通道强度曲线，由游戏事件触发；支持按设备覆盖。
其中 Audio Based Vibration 使用左右音频通道驱动左右触觉，文档限定 DualSense；
触发器阻力/震动又是另一组设备属性。它证明作者/引擎选择输出路径，不能把同一
作品在一平台有高级资源外推为其全部 PC 设备路径都有相同资源。

“协议”须按层区分，不能给一个混合游戏 API、HID 编码与网络承载的固定总数：

| 游戏侧接口 | 实际表达及必要设备条件 |
|---|---|
| XInput / SDL 普通 rumble | 两路强度；可随时间连续变化，不是只有开关；不直接指定任意频率或专用 PCM |
| WGI / GameInput 普通 rumble | 低/高频马达与左右扳机四路强度；增加位置，不等于任意波形 |
| Steam Input | 普通强度、legacy pulse 的 on/off/repeat、SimpleHaptic 的强度/增益、DualSense 触发器参数是不同入口 |
| GameInput v2+ 高级触觉 / 游戏原生 DualSense 路径 | 需要相关音频端点，游戏可写触觉波形；不能与 GameInput 普通四强度混同 |
| Nintendo 平台接口与设备 HID | 每个震动器的双频带振幅/频率；有明确设备编码，但不是跨厂商通用 PC 游戏高级触觉 API |

Microsoft 的 [XINPUT_VIBRATION](https://learn.microsoft.com/en-us/windows/win32/api/xinput/ns-xinput-xinput_vibration)
定义两个 WORD；[GameInputRumbleParams](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/input/gameinput/structs/gameinputrumbleparams)
定义四个 float。另一路 [GameInputHapticInfo](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/input/gameinput/structs/gameinputhapticinfo)
查询专用音频端点及各声道位置；[高级触觉硬件要求](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/input/hardware/input-hardware-haptics)
要求输入设备与音频端点关联。微软
[Gamepad/HapticsManager](https://github.com/microsoft/Xbox-GDK-Samples/tree/main/Samples/System/Gamepad/HapticsManager)
使用 WASAPI/XAudio2 播放触觉波形。不存在从“GameInput 支持高级触觉”直接推导
“Steam 远程对象具备对应 Windows 音频端点”的依据。

SDL 的 [SendGamepadEffect](https://wiki.libsdl.org/SDL3/SDL_SendGamepadEffect)
允许设备专用字节，不能把全部 SDL 概括为只会普通 rumble。
[Nintendo 后端](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_switch.c)
分别处理普通强度编码和原生 rumble report；libnx 的 HidVibrationValue 也明确
包含两频带幅度/频率。瓶颈应定位到游戏选择的源接口、Steam 输出后端或本地
实现，不应归咎于“没有 Nintendo 设备协议”。

**Evidence / Sony 的 PC 支持：** Sony 的
[当前支持说明](https://www.playstation.com/en-us/support/hardware/pair-dualsense-controller-bluetooth/)
正式列出 Windows 10/11、USB/Bluetooth；PC 高级触觉须游戏支持并要求 USB。
完整虚拟设备能工作的原理是满足游戏实际使用的设备/端点契约，不是 PC 游戏
天生不可能支持 Sony。该原理不撤销 §22.18 禁止安装主机组件的约束。

**Evidence / Steam 识别设备不等于原生 USB 直通：** Valve 的
[Gamepad Emulation Best Practices](https://partner.steamgames.com/doc/features/steam_controller/steam_input_gamepad_emulation_bestpractices)
说明 Remote Play 使用 Steam Input 提供输入；Windows 上通过 Overlay 接管
传统 API 并提供模拟 Xbox 设备。游戏可经 Steam Input handle 另查真实类型/
图标，但这不是 Sony HID 与触觉音频端点。游戏原生 Sony 输出不会因原设备
类型可查询而自动改用 Steam Input 的输出 API。

2021-12-06 的 [FFXIV Steam Link 原帖](https://steamcommunity.com/app/353380/discussions/7/3193611900717183832/)
中 slouken 明确表示该游戏的 PS5 支持仅限直接连接。它是当时该游戏的边界，
不单独充当所有新版本的定论；当前公开兼容层文档及本项目已有远程 HID 结构
同样不能证明完整 Sony 音频直通。SetDualSenseTriggerEffect 的参数输出不等于
专用触觉 PCM。更换客户端 VID/PID 不能解决这层缺失。

**Evidence / 游戏支持须附平台、接口与版本：**

| 游戏 / 资料 | 已证明的支持 | 不能据此认定 |
|---|---|---|
| Left 4 Dead 2，[Valve 2015-12-18 更新](https://store.steampowered.com/oldnews/19771) | 专门添加 Steam Controller haptics | 现行版本必发重复 pulse；2022 官方另有经 Steam Input 启用 rumble 的变更，需要区分 |
| 历史 CS:GO，[Valve 2015-12-17 更新](https://blog.counter-strike.net/2015/12/13409/) | Steam Controller haptic feedback 及 steam_controller_haptics 开关 | CS2 当前支持相同源 |
| Rift Apart / Returnal，[Nixxes](https://blog.playstation.com/?p=382715) / [PlayStation PC 发布说明](https://blog.playstation.com/2023/01/18/returnal-launches-on-pc-february-15/) | PC 原生 DualSense 触觉及扳机功能 | 经 Steam Link 自动转换给 Switch 或 Steam Controller |
| 1-2-Switch，[Nintendo 玩法说明](https://www.nintendo.com/en-gb/News/2017/March/Let-s-have-a-look-at-some-of-the-fun-ways-to-face-off-in-1-2-Switch--1206994.html) | Joy-Con HD Rumble 数球等专用设计 | Steam PC 游戏支持；该示例也不证明 Pro Controller 兼容 |
| Thumper，[开发者 2018-06-28 回复](https://steamcommunity.com/app/356400/discussions/0/3211505894137045717/) | 明确区分 PC 基本输入与 Switch 版 HD Rumble | 2026 全部版本仍相同，或 PC 其他作品均无 HD |

Portal 2 等的 Steam Input 输入集成、Shadow of the Tomb Raider 的社区高级触觉
报告均不能冒充已核实的现行游戏输出接口。没有现行调用/运行证据时，不把
“未找到认证样本”写成“没有任何游戏支持”，也不填造已验证兼容名单。

**Conclusion / 产品边界：** 零主机安装时，只能消费 Steam 实际取得并通过
既有远程协议输出的源。Steam 普通 rumble 转换与配置反馈有实际用途，但游戏
专门触觉取决于游戏选择的接口；原生 Steam Controller 不是 DualSense 兼容桥。
架构验收须先以现行游戏明确源接口与设备呈现，再要求端到端接收/本机播放。

### 22.20 普通双路震动的利用程度与质量合同

**Correction：** §22.8 的固定 Valve/SDL 普通参数一致性不是 Switch 最佳手感
认证。两路原值进入了客户端，不等于强度、频感、播放时序都已得到最佳利用；
软件参数一致性与机械/感知效果必须分开。

**Evidence / 当前幅频映射：** `hd_rumble.c:sl_hd_ordinary_word` 把每路 uint16
输入按 101 个阈值（含静音）量化，再经原生 decoder 输出。非零状态的两个
频带都约 149.933Hz；最大输入对应每频带振幅约 0.5571934。这与 §22.8 的
固定参考一致，却没有据此证明两路的厚重/细腻区别或弱/强动态范围对本机最优。
libnx 振幅数值不能直接当作感知力度百分比；也不能从 0.557 推导“只发挥了
55.7%”或把所有值放大到 1.0 就宣称更保真。Handheld/JoyDual 将两路分侧，
Pro 向两侧各提供两频带；它是参考布局，不是统一机械等价性的证明。

**Evidence / 普通短事件覆盖：** `sdl_hid_write.c` 先记录 received，再在 media
owner 的 ApplyPendingWrites 循环交给 `sl_haptic_command`。普通分支每条覆盖
`current`，清空 pending native queue，`due=now`，到期从 drain 时刻计算；
与原生样本不同，普通强度变化没有按接收间隔进入播放队列。

直接编译当前生产 `haptic_stream.c` / `hd_rumble.c` 的离线反例：on 在 t 收到、
off 在 t+50ms 收到，分别 advance 得到非零与静音；两条都在 t+60ms 应用，
随后 advance 只得到静音，第二条输出不存在。worker 不必然在两次回调间获得
调度，因此这是合法交错下可复现的信息损失；**未证明用户某次游戏运行遇到
该交错**，也未量出实机发生率。对照材料在
`build/haptics-game-research-20261118/ordinary-audit/`；不执行网络、线程或硬件。

**Conclusion / 基础质量要求：** 普通双路路径仍有可改进空间，不能宣称已最优。
接收、命令生命周期和播放应保持可审计的时间语义，避免 owner 卡顿无声吞掉
短事件；同时为过期/积压明确限制延迟与降级，不能靠无限重放旧事件掩盖问题。
映射应利用本机原生幅频控制，独立保留两路变化，标定弱震阈值、动态范围及
两路频感，不能仅按“更响/更大”认证更好。Nintendo 原生命令的 wire decoder
状态须独立保留，不能因改普通渲染而污染后续相对原生解码。

主机 Generic 非零更新的已有 50ms 限流证据见 §22.2；客户端只可能保留实际
收到的信息，不能重建主机未传输的变化。基础路径的接收/输出/短事件证据与
真实手感评估均是验收条件，固定编码器字节一致不是最终质量条件。

**Scope：** 基础普通震动质量是优先方向；原生 Steam Controller 设备模拟退出
本项目此次实施主线。Sony 身份仅作为 Steam 已有输出路径的可行性研究，
不能未经原生游戏设备呈现与触觉端点证据就转成产品实现。

### 22.21 客户端保真与上游启用的独立合同

**Decision：** 两个工程层分别实现、分别验收。客户端层针对已知反馈的实际
参数、顺序、时序与 Switch 可表达范围；上游层针对设备描述、能力声明、Steam
及游戏选择哪个接口。客户端夹具不得冒充游戏实际来源，上游未启用也不应阻止
客户端独立验证。不依赖安装 PC 驱动、虚拟手柄、伴随服务或游戏插件。

| 客户端输入 | 接收与本机输出合同 | 单独保留的边界 |
|---|---|---|
| Generic `01` 普通强度 | 保留 uint16 两路原值、源时间间隔和 duration；直接连续映射原生幅度，160/320Hz 区分频带 | 频率是渲染策略，非游戏提供；数值线性不是感知线性 |
| Generic `06` trigger rumble | 与身体反馈独立的状态、停止和到期；转换到对应手的高频带，并计为适配 | Switch 没有扳机马达；同频带竞争取较大幅度，不是原物理位置/力学还原 |
| Generic `0d` + Nintendo 10B | 七格式状态解码、完整样本、双侧独立双频带、5ms 最小保持；相对历史与普通渲染独立 | IPC 参数与实际机械响应仍须分别验证 |
| Generic `0d` + Sony 47B | 严格容器/模式/有效位，保留完整 raw 与扳机块；可播放的模拟 rumble 转为普通双路渲染 | 47B 不含专用 PCM；扳机阻力/音频/灯光/未知功率控制显式记为不可还原 |
| 显式专用双侧 PCM 本地入口 | 明确格式、左右布局、采样率、epoch/seq/PTS；FFT 选两峰并拟合幅度，按5ms hop 输出每侧双频带 | 只是准备好的客户端适配器；不定义新 Steam wire，也不证明 Steam 已提供此源 |

**Correction / 普通映射：** §22.8 的编码/量化保留为 Nintendo 相对解码的源
历史及独立参考，不再限制普通播放。默认每频带数值峰值仍取参考约 0.5571934，
避免无标定就放大到 1.0；输入到输出不再经过旧101档。160/320Hz 依据
Linux hid-nintendo 的默认频带及 libnx 参数接口，是可验证的起点，不能宣称
机械最优。Pro 双侧各保留双频带；Handheld/JoyDual 分侧；单 Joy-Con 合并
普通两源到其一只震动器。专用立体 PCM 要求双震动器，单 Joy-Con 拒绝并记账，
不能无声丢掉一侧或把单侧输出称为完整空间还原。

**Timeline：** 身体状态（普通/Nintendo/Sony/PCM）按源事件选择，trigger 是
独立叠加通道。源队列有界；普通收到 start/stop 同批处理时保留接收间隔。
后来的身体状态只替换尚属其未来的旧 Nintendo 样本，已经排在它前面的样本
仍可播放；有意替换、溢出丢弃、过期、实际提交分别记账。普通间隔按提交时刻
保持，不能把每次 IPC 耗时叠进跨包间隔；原生/PCM 样本的5ms最低保持按完成
时刻约束。trigger 更新不能挤占 Nintendo 的源样本节奏。

播放采用250ms软件迟到预算。超预算不无限重放旧事件，而是计数并清零；它
不是网络端到端延迟保证。失败重试不能越过已到期的通道期限，也不能阻塞已
到时刻的同通道停止。partial update 与到期同刻/晚到时，先清实际已过期状态，
不能组合出过期的非零身体反馈。本地退出/断线/换设备直接清队列并归零。

**Continuity：** SDK 接收队列8192B，最多106条69B效果（每条另有8B时间戳）；
保留已接受前缀，发生丢失后拒绝后续命令，owner 在前缀之后通知后端间断。
Switch 后端停止并标记 Nintendo wire 失步；随后相对包拒绝，直到完整绝对
报告或普通源基线恢复。下游播放队满则记录失败、停止并保留已知 wire 历史。
SDK 的结构新增可选 outputDiscontinuity，所有 provider/调用者须一起重编译。
没有高频同步网络日志，也没有新增 detached 线程或退出次序例外。

**Sony 模式依据与更正：** 不能将模拟 rumble 的模式位都当成“缺失则保持”的
普通有效位。未修改 SDL3 原始 UpdateEffects 每次清47B并在 rumble 非零时重新
填模式和 motor；为零时明确恢复音频触觉。独立提取原函数执行的实验与结构
在 `build/sony-haptics-reference/`，只替换 context/硬件终端。因此客户端按
Steam/SDL 效果合同识别 AUDIO/DISABLED handoff，停止此前 Sony 身体反馈；
不把 handoff 当作 PCM。另一来源已接管身体输出时，旧 Sony handoff 不得
将它停掉。功率字段的第三方参考存在矛盾，非零未知值保留/记账，不猜增益。
这些是软件合同与原 producer 证据，不是 Sony 固件机械行为的实测认证。

**专用 PCM 合同：** `pcm_haptics.h` 只接受显式 LEFT_GRIP/RIGHT_GRIP、S16LE /
IEEE F32LE，8/16/48kHz，严格字节长度、有限值、块大小、epoch/序号及样本时钟。
不从两声道猜布局，不把游戏主音效当作者触觉。固定40ms Hann窗、5ms hop，
窗中心到窗尾20ms源时钟分析滞后；实际运算、网络、队列和机械延迟另计。
起点补20ms已知静音，EOS补右半窗并归零；断点清旧分析/待播输出。有限队列
满时整块拒绝，不悄悄丢样或自动增益归一化。

FFT 双峰/幅度拟合保留侧向和主要载波，反馈残差、带外/DC能量、5ms包络误差、
参数 RMS 和限幅。40ms窗口会平滑瞬态，两个载波不能保存任意宽带波形或相位。
拟合内部使用相位只为测表示误差，libnx 没有对应相位字段，不能称原样PCM
播放。分析器必须在渲染、输入与 actuator deadline 热路径之外运行；owner 只
提交分析后的 typed 帧。运行诊断 `pcm_steam_source_ready=false` 明确不宣称
已发现 Steam 传输入口；8f/EA/81/82 等原生设备协议也不冒充 Generic 格式。

**接线边界：** 专用 PCM 分析器及 typed 输出当前由本地库/离线夹具调用，生产
Steam 接收器没有对应来源；成品链接可裁掉未引用的 PCM 入口。这不属于已激活
的 Steam 播放路径。上游层须先证实实际承载和格式，再把该来源接到 typed
接口；不能假定修改设备身份/能力之后会自动出现或自动接通 PCM 数据。

**独立验收：** 保留原 Nintendo 夹具及其验证器；另有内嵌14命令的新反馈矩阵，
覆盖普通批量短震、body/trigger独立停止、Sony模式切换与 Nintendo 样本。
同一 NRO 夹具在桌面以硬件终端替身执行；checker 必须区分 host-simulation 与
Switch。全部离线可核输入/参数/顺序/时间/失败路径，不据此认证游戏已启用、
系统 IPC 读回、机械保真或 hbmenu 屏幕状态。工具用法见 UDP_DEBUG。

**Correction / 组合输出的失败重试：** 以 `current.source` 将失败 IPC 只归到
一个通道并不充分：实际 IPC 值是 body 与 trigger 的合成。可复现反例为先打开
两通道、让 body 更新失败、再停止 trigger；旧实现重试仍携带非零 trigger，
反向亦然。到期的任一通道停止必须先从重试合成值中移除；独立通道停止保留
原失败帧的 identity/deadline，不能将一次 native 重试误算成新 trigger 帧或
丢掉成功完成之后的5ms保持。只停一手/频带而另一侧仍非零也属于停止，保留
新状态与其duration；超过迟到预算的旧帧先归零，不能被独立停止延长重试寿命。
确定性双向交错与实际 worker 失败终端分别验证。
此更正撤回“同通道停止优先即可保证独立停止”的充分性假设；测试入口为
`test_haptic_timeline.c` / `test_rumble.c`，不据此断言用户曾遇到真实 IPC 失败。

**Correction / 专用 PCM 频段边界：** 固定160/320/500Hz音调不能覆盖合法频段
边缘。41/42Hz与1253Hz的源频率估计可能越过边界；宽邻域积分还可能使旁瓣
借到主峰能量而被错误选中。局部峰按自身bin强度排序/选取，邻域能量仅用于
残差估计。距合法边界不超过一个FFT bin的估计投影到41/1253Hz并记入
`frequency_projections`；8/16/48kHz的bin宽分别为15.625/15.625/23.4375Hz，
这是明确的有限窗近似，也会近似无法区分的邻近带外信号，不能称为精准带外
滤波。拟合额外联合常数基底，避免将非整周期低频音调的样本均值当作纯DC
剔除而偏置载波幅度。原DC/带外/拟合残差、限幅指标保留。两格式×三采样率
×十二边界/交叠频率×八相位共576组以独立正弦幅频为参考，不允许静音或
旁瓣顶替通过；入口 `test_pcm_haptics.c:frequency_boundaries`。这些仍是软件
表示证据，非实际Steam来源或马达测量。

**Correction / owner 与输出线程交错：** 播放队列为空不能作为新batch的充分
条件。owner仍在drain时worker可以先取走第一条；旧实现会重设时间锚点，将
已相隔50ms接收的on/off压缩为约1ms。锚点保留到本批初始本地cutoff以及
实际成功提交时刻；已在该时刻前接收的积压事件保持源间隔，普通/trigger/
Sony按提交时刻续锚，不将IPC耗时累加进去。队列空且收到cutoff之后的新
实时事件才重新锚定以吸收空闲时间，不维持固定播放延迟；这不承诺可恢复
尚未到达的数据。native的包内完成后5ms保持与空闲抖动吸收策略独立保留。
`test_haptic_usage.c`以独立事件模型覆盖192种回调交错及四布局40000步正常
持续输入，既验证参数也要求应有输出/到期必须出现、deadline符合预期。
实际SDK pipeline测试强制在两个回调之间等待worker完成，防止偶然调度通过。

**Correction / 选择时刻不是提交时刻：** worker可以一次选出多个slot，前一
个IPC拖慢时，后一条即使选择时合法也可能在提交前过期/超250ms迟到预算。
提交前重新核对当前帧/绑定、两通道expiry与源年龄；过期的未提交job取消并
重新进入状态机，先归零/更新，再处理剩余队列，不补发旧非零。真实worker
替身通过持有前一slot的IPC，分别跨过10ms期限与250ms预算验证。已经进入
的同步IPC无法撤回；检查之后的OS抢占和系统内部机械时刻仍非硬实时保证。
离线软件证据不替代Switch系统读回、负载或机械验收。

### 22.22 频率参考、演示取值与跨设备幅度映射

**Evidence / 接口与编码：** libnx 的公开
[HidVibrationValue](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/hid.h)
分别提供 low/high 幅度与 Hz，幅度字段注释1.0为最大参数值；没有“任意频率
在同一幅度下同样可感知”的保证。固定
[Linux v6.12 hid-nintendo](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-nintendo.c)
列出频率编码表，默认值160/320Hz；不同频带有不同的可编码范围。
[MissionControl 原作者的解码器](https://github.com/ndeadly/MissionControl/blob/71a837fcff422a652bd1326a214f925792ae8449/mc_mitm/source/controllers/switch_hd_rumble_decoder.cpp)
提供压缩样本的中心频率160/320Hz及对数频率/幅度坐标。这些是公开实现与
研究结果，不冒充任天堂闭源SDK推荐、游戏作者的具体效果或机械标定表。

**Evidence / 演示取值：** `haptic_experience.h` 的80/160/320/640Hz是围绕
160/320Hz选取的倍频测试点，不是四档硬件模式，不是从某个游戏提取。AM
编码64由对应原生解码得到约0.2554743；这个场景不经过普通渲染的0.5571934
峰值缩放。前两档使用low、后两档使用high，同时改变频率与参数带，不能
凭它隔离频率响应或认证四档等感强度。它也不构成真实游戏常用频率的统计。

**Evidence / 机械参考：** Alps Alpine 原厂公开的
[AFT原理资料](https://tech.alpsalpine.com/e/products/faq/haptic/reactor/)
描述160/320Hz两个共振点，两个振动方向可组合；其
[产品FAQ](https://tech.alpsalpine.com/e/products/faq/haptic/)
明确说明加速度随输入频率形成有峰值的曲线，并以加速度计测量固定到100g
悬置块上的器件来评价激振力。该原厂资料能支持“频率与相同输入下的输出
强度有关”的机理；没有据此识别调试手柄中的具体料号、装配耦合或系统补偿，
不能把裸器件规格当作该手柄的实测频响/补偿曲线。

**Evidence / 游戏输入：** 微软
[XINPUT_VIBRATION](https://learn.microsoft.com/en-us/windows/win32/api/xinput/ns-xinput-xinput_vibration)
为两路0..65535 motor speed，不含独立Hz字段；Unity的
[Gamepad rumble示例](https://docs.unity3d.com/Packages/com.unity.inputsystem@1.14/manual/Gamepad.html#rumble)
调用`SetMotorSpeeds(0.25f, 0.75f)`，同样不是指定25/75Hz。
Steamworks `TriggerVibration`也是两路强度，不能从通用接口假定游戏作者
给出了任天堂的频率/幅度。原生专用效果和专用PCM属于不同输入合同，见§22.21。

**Conclusion / 映射边界：** 普通渲染的160/320Hz有上游驱动默认参考；
`normalized_strength * 0.5571934`只是保留SDL固定参考峰值的数值策略，
没有证明它是跨设备等效力度、等感知映射或弱震最优值。原生Nintendo样本
则直接按原协议坐标解释，不套用这个普通峰值，不把作者频率压成演示四档。
两者的力度校准问题须分开：普通适配需覆盖弱震阈值/动态范围/左右通道，
原生解码需验证源参数及系统实际响应；读回参数不是机械加速度。

**Hypothesis / 待验证：** 高频感知减弱可能涉及器件/装配频响、幅度或系统
处理；公开资料不能决定该设备的具体原因。隔离验证应固定一只震动器与
频率，在两个参数带的可表达交叠范围内比较，再固定频带逐级改变幅度；
保存活动参数读回、用户观察及可获得的外部机械测量。不能仅凭编码合法或
成功IPC就认为游戏实际弱震/高频效果必然可感知，也不能无标定统一放大。

### 22.23 Windows面板初始化失败与空列表的区别

**Evidence：** 用户`session-20261120-151633-20776.jsonl`记录启动94ms时
`steam_input_init.ready=false, failure_step=5`，约61秒切换到Steam Input时
列表数量为0；全文件没有`steam_enumeration`，1148条请求全是XInput。
`steam-haptics-panel-win.c`的step5对应`CreateSteamPipe`或
`ConnectToGlobalUser`失败，发生在取得SteamInput006/调用Init之前。枚举分支
要求`input_initialized`，因此这份记录中的0是界面空状态，并非公开枚举调用
返回0。原启动函数只调用一次，后续切换/播放未重试，是工具生命周期疏漏。

**Correction：** 撤回将这次空列表直接解释为“公开Steam Input枚举/关联查询
为空”的判断；新记录证实两种查询根本没运行。§22.14所述AppID过滤仍是独立
代码事实，但不能用于解释这次更早的连接失败。也没有依据从本次空列表断定
Switch身份/能力位被Steam拒绝。该客户端实际记录057e:2009、Switch Pro名称、
caps=0x836bff及nativePulse=1，身份/格式/原生能力仍需各自判断。

**Decision：** 保留当前可工作的XInput路径；启动连接失败后，显式切换Steam
Input或点击播放时可重新建立会话。重试先释放旧的部分user/pipe，不重复加载
DLL，不借用别的游戏AppID或改变设备上报；记录独立pipe/user值区分初次失败。
用真实Win32控件分别注入首次CreateSteamPipe失败、首次ConnectToGlobalUser
失败，覆盖后续连接、枚举、非零请求及完整释放。

**Boundary：** 原日志step5未区分pipe或user，第一次连接失败的底层原因没有
由此确定。修复失败状态永久沿用的代码，不据此宣称真实Steam API连接已恢复；
与用户已经确认的XInput远程震动交付分开记录。

### 22.24 Steam Input 测试源的成功前提与快捷方式身份

**Correction：** 撤回“从 Steam 库启动面板，即具备 Steam Input API 使用条件”
的隐含前提。§22.23 的重试只修复永久沿用失败状态，不能建立缺失的应用访问
条件；`GetControllerForGamepadIndex` 也依赖已初始化的同一接口，不能绕过
初始化或进程配置关联。用户反馈新版仍无手柄，是修复未解决实际问题的证据，
不能再把它描述成已恢复。该反馈本身不包含新版失败阶段的机器日志。

**Evidence / 启动身份：** 原始 `session-20261120-151633-20776.jsonl`
的 `SteamAppId=3586707621 (0xd5c8d0a5)`，
`SteamGameId=15404791932542517248 (0xd5c8d0a502000000)`。
按 Valve SDK 的 [CGameID 定义](https://github.com/ValveSoftware/Proton/blob/proton_11.0/lsteamclient/steamworks_sdk_163/steamclientpublic.h)，
这是 type=2 的 shortcut，低 24 位 AppID=0，高 32 位为快捷方式标识。
它是 Steam 正常提供的非 Steam 启动上下文，不能仅因存在或在库里显示，就
当作已注册、可访问 Steamworks 的商店 AppID。面板保留这些环境变量，不曾
将两者互相替换。

**Evidence / 客户端连接：** 对 §22.11 同一 DLL 的原始指令复查：
`0x1389a68e1..0x1389a6905` 将环境变量 `SteamAppId` 解析至 engine `+0x148`；
`ConnectToGlobalUser` 实现 `0x1389a7170` 的
`0x1389a7326..0x1389a7375` 将该值交给接口 `+0x90` 检查。
返回 0 且原 AppID 非 0 时，存在 `ConnectToGlobalUser: Steam denied appID %u`
分支，释放关联并返回 0；错误文本地址为 `0x13938f9a0`。这证明连接失败
不能只解释为启动竞态/全局用户尚未就绪，不证明该机器当时必然触发了这个
分支。原日志 step5 合并了建 pipe 和连 user 的失败，不能补造细分结果。

Valve 的 [SteamAPI 初始化文档](https://partner.steamgames.com/doc/api/steam_api#SteamAPI_Init)
列出客户端运行、应用身份、账户访问许可、应用设置及相同 OS 用户/权限上下文
等失败条件。手工通过 `SteamClient023` 建连接不等于免除这些前提；反过来，
不能仅凭未调用 `SteamAPI_Init` 就断定低层连接方式必然错误。

**Evidence / 接口与配置：** [ISteamInput 文档](https://partner.steamgames.com/doc/api/isteaminput?l=english#GetConnectedControllers)
要求设备对 Steam Input 启用，且枚举前先运行 `RunFrame` 或 callbacks。
面板已在 16ms timer 调用 `RunFrame(true)`，因此首次扫描早于第一帧不能
解释持续空列表。§22.14 的原始实现还证明枚举按进程 GameID/应用关联过滤，
不是“主机全局有设备就一定返回”。需要区分以下条件：

| 条件 | 能证明它的证据 | 不能替代它的现象 |
|---|---|---|
| Steam API 客户端连接成立 | 非零 pipe/user，取得接口 | 存在 Steam 启动环境变量或 XInput 能震动 |
| Input 已初始化并推进 | Init、RunFrame 实际执行 | DLL 加载成功或 mock 返回 true |
| 当前应用获得设备配置关联 | 同一应用上下文的有效 Input handle 与配置记录 | 桌面/UI/另一游戏能枚举，或其他 AppID 的旧探针成功 |
| 请求确实经远程设备输出 | 真实 handle 的请求与 Switch 收包/输出相对应 | API 调用返回、UI 列表非空 |

原来的 `user_retry` mock 故意让第二次连接成功，`empty_public` mock 直接
返回预设的 XInput 关联 handle；它们只能验证代码如何处理这些返回值，
不能验证 Steam 接受快捷方式身份或将其配置关联到远程设备。这是既有测试
范围的缺口，增加同类 mock 次数无法弥补。

**Conclusion：** 已确认面板测试设计混淆了非 Steam 游戏的 XInput 兼容路径
与 Steamworks API 路径；不能用前者的成功推导后者成功，也没有证据据此
改动 Switch 上报。成功必须同时满足上述连接、推进、应用关联和远程输出
条件。普通 `TriggerVibration` 本身仍只传两路力度，不会因为换成 Steam Input
调用就新增载波频率。动作 manifest 用于原生动作定义/配置，不是普通双路
震动的通用强制前提，不能无依据再增加这道门槛。

**Hypothesis：** 当前快捷方式 AppID 在 API 连接检查中被拒绝，是与现有身份
及 DLL 分支相符的候选原因；尚缺真实拒绝信息，不能宣称已唯一定位，也不能
宣称所有非 Steam 程序都无法使用该接口。有效 SDK 应用上下文仍须与实际
输入配置匹配，换成某个有许可的 AppID、改能力位或继续重试，均不自动满足
这些条件。真实原因应从主机连接错误和同一进程的配置关联确定，而非通过
反复更换 Switch 固件或借用另一游戏的成功结果推断。

### 22.25 桌面 Profile、无应用上下文与实际连接失败

**Evidence / 新面板日志：** `session-20261120-153335-23360.jsonl` 中，
78ms 和 4969ms 两次初始化都记录 `pipe=1,user=0,failure_step=5`，
AppID 均为快捷方式标识 3586707621。2766ms 时 XInput 枚举到一只设备，
没有任何 `steam_enumeration`。这将失败明确缩小到 `ConnectToGlobalUser`，
排除了“pipe 没建成”，但仍没有真实拒绝原因文本，不能把 AppID 拒绝分支
当成唯一已证实原因。重试未解决该真实连接问题。

**Evidence / 已有真实上下文查询：** 重新检查用户提供的三份归档
`nsteamlink-haptics-probe-20261118-{121043,123657,131828}.zip` 中
`requests.jsonl`，三次结果一致：

| 内部查询上下文 | 设备数量 |
|---|---:|
| 0（全局查询） | 1 |
| 769（Steam UI） | 1 |
| 413080（桌面配置） | 0 |
| 1049320（旧工具应用上下文） | 0 |

返回的 handle 为 `0057e2009f4e844f`，公开类型为 Switch Pro（10），
remote session 分别为 3、4、5。这是实际 Steam 的内部只读查询结果，
不是合成缓存测试，也不是公开 `GetConnectedControllers` 返回了该 handle。
旧探针设置了自己的应用环境后才执行跨上下文查询，不能据此宣称无应用身份
的独立 EXE 已连接成功。§21.2 用户确认的桌面鼠标输出则证明桌面映射链路
确实使用过本项目的远程手柄；二者都不能证明普通或原生震动输出成功。

**Correction / 编号语义：** 撤回 §22.14 原先“桌面 769 / Steam UI 413080”
的倒置标注。413080 对应 Desktop 配置；[Valve 仓库中的原始复现报告](https://github.com/ValveSoftware/steam-for-linux/issues/13794)
给出 `desktop_neptune.vdf` 与 413080 的实际加载日志，早期
[桌面配置报告](https://github.com/ValveSoftware/steam-for-linux/issues/7788)也作同一标识。
这不改变旧日志的数量，尤其不能把 769 下的 1 解释成 413080 下也为 1。

**Evidence / Profile 与 API 的职责：** Valve [玩家指南](https://partner.steamgames.com/doc/features/steam_controller/getting_started_for_players?l=english)
将 Desktop Configuration 定义为桌面导航的输入绑定，区别于 Big Picture
与游戏配置；它不是独立程序的 SDK 初始化凭据。[SteamAPI 初始化说明](https://partner.steamgames.com/doc/api/steam_api#SteamAPI_Init)
仍有应用身份、账户和权限上下文等条件。直接双击程序不会自动继承桌面 Profile
对应的 API 上下文，改布局也不修复 `ConnectToGlobalUser` 返回 0。

**Evidence / 全局可见不等于可输出：** 同一 DLL 的
`0x1389a7340..0x1389a737a` 在原 AppID=0 时跳过“返回 0 且 AppID 非 0”
的拒绝分支，§22.14 内部枚举也把 0 当全局查询；这给低层独立诊断提供候选，
不代表官方 SDK 保证零 AppID 初始化。普通震动入队函数 `0x1386380f0`
在 `0x138638137..0x13863813d` 比较请求 AppID 与活动 config `+0x258`，
不相等直接返回，并没有“请求 0 就匹配所有配置”的分支。
执行原始指令、以锁/活动配置/最终入队为替身的三项对照：活动配置=413080
时，请求 0、769 都未到达入队，请求 413080 到达；这只证明该函数过滤语义，
不证明真实桌面输出、API 初始化或最终震动。

**Conclusion：** 把非 Steam 快捷方式当作已具备访问前提的原生 API 测试源，
再用空列表判断 Switch 上报，是错误的诊断路径。桌面导航与 Steam 全局
设备可见已有证据，但“旧面板直接双击便能按桌面 Profile 枚举并输出”没有
证据。固定版本的 Steam 全局/当前配置诊断可不依赖非 Steam 快捷方式，
属于内部接口路线，不能包装成任意游戏公开 API 成功；普通游戏的 XInput
兼容测试在 Windows 仍依赖 Steam 的进程接管，不能据桌面 Profile 存在就
删除其启动条件。游戏 API 验证、Steam 内部诊断和桌面输入绑定须分别标明。

### 22.26 桌面直连面板的验证入口

`Start-Desktop-Panel.cmd` 启动面板 `--desktop` 并在退出后自动打包记录。
该模式只将本进程 SteamAppId/SteamGameId 设为 0，使用已安装 Steam 的
全局用户连接；不修改 Steam 配置或 Switch 上报，不依赖快捷方式的游戏身份。
固定 §22.11 DLL 的 owned controller map `+0x338` 查询 88B 设备详情，
按有效 Switch Pro handle 枚举并在每次发送前重新匹配 index；map `+0x2b8`
以 `(activeAppID,index,low,high)` 调用 Steam 的 `TriggerVibration` RPC。
初版 activeAppID 取 client cache `+0x109608`，原始 getter 为
`0x13861c4f0`。**Correction：** §22.29 的真实记录否定了该缓存能代表当前
设备配置的假设；不能再把它作为可靠的活动配置来源。修正版优先取对应设备
最近的激活日志，缺失时才使用明确标为未核验的缓存。Steam 仍自行检查真实
活动配置，日志或调用返回均不证明交付。map 在 Input shutdown/user/pipe 释放前销毁。

`rpc_bridge_evidence` 执行原始 `0x1388b7240`，验证 appID=769、index=4、
A=0x1234/B=0xabcd 的序列化，方法 ID 为 0xbba86a5a；其 buffer/IPC/reply
为替身。Win32 桌面用例覆盖进程身份归零、配置变化、断线、handle 变化及
停止/清理，并保留公开 API、失败重试和实时频率相关回归。真实 Steam 的
零应用连接、当前缓存到 worker 的匹配和最终输出需看实际运行的
`desktop_init`、`desktop_enumeration`、`desktop_dispatch` 及设备回传；
离线检查不代替这些证据。频率仍是 Switch 普通映射配置，A/B 请求经 Steam
Remote Play 发送，桌面内部诊断不改称普通游戏公开 API 验收。

### 22.27 正式 v0.7.3 与普通双路输出的比较基线

**Evidence：** tag `v0.7.3` 的 `app/CMakeLists.txt` 已链接
`--wrap=SDL_GameControllerRumble`，`app/platforms/switch/rumble.c` 将两路
原值除以 65535，直接以 libnx `hidSendVibrationValues` 输出，频率为
160/320Hz、峰值 1.0；Handheld/JoyDual 分侧，Pro 两侧各有两频带。
其 ihslib submodule `d8ee4a7` 的普通命令原样交给 SDL wrapper，无101档量化。
当前普通 renderer 默认同频率/同布局，但额外乘约 0.5571934；该限制对正式
0.7.3 是幅度基线的改变，不能称为保留了正式版本的力度行为。

**Correction：** §22.20 的101档、两频带约149.933Hz描述的是该节当时的
中间实现，不能套到正式 v0.7.3。正式版本早已绕过 SDL 的 Switch 震动后端，
连续保留普通两路幅度；不能把连续幅度、直接 HID 或160/320Hz说成这次相对
正式版才新增的能力。0.5571934 的依据是中间阶段的 SDL 编码参考，不是
正式 v0.7.3 的峰值，也没有机械标定证明这个降低更保真。

**Evidence / 游戏片段：** 用户报告《影之刃零》时，当前客户端实际记录的
普通命令与成功提交参数符合上述新幅度公式；该采集快照有1068条普通命令，
唯一接受的 Nintendo 原生包两侧幅度均为0。未运行正式版真机 A/B。
因此力度大小变化本来就是正式版可表达的内容；该片段没有证明新增原生解码
带来非零高级效果。新版本另保留接收时间并由输出线程排程；正式版媒体线程
逐条即时调用，批量积压时可把不同接收时刻的事件挤在一起。此时序机制差异
不能单独证明本次游戏在旧版发生过短震损失，或新版主观效果一定更好。

### 22.28 桌面触觉偏好与正常原生脉冲请求

固定同一 November DLL 的 controller serialized map `+0x468` 为
`SetControllerHapticsSetting`（`0x138895660`），参数是 index 与 bool，
方法 ID 0x65c94306，尾标识 0x6b13ecdd，回复为 bool；服务端偏好 setter
`0x138632090` 的字段/保存依据见 §22.16。面板仅在用户点击“启用/关闭
Steam触觉”时调用，设置持久化属于 Steam 当前手柄偏好，不改能力或游戏身份。
返回 true 只记为接受设置请求，不能替代偏好生效或原包交付的证据。

面板原生模式经 owned map `+0x2a8` 发送双侧 pulse 的持续/间隔微秒与
重复次数。非零请求固定 flags=0；停止使用 Steam 原停止调用的 flags=2，
不使用强制非零输出的 flags=1。原生模式禁用 A/B 与本地幅频配置，避免
把普通映射设置误当作 Steam 原生请求参数。切换模式、停止和关闭会停止
原生请求；桌面普通路径和 Switch 幅度默认值保持既有行为。

原始 marshaler 的开/关两组序列化与 bool 回复经离线执行核对；Win32 替身
覆盖开关、非默认 pulse 参数、原生停止及播放中关闭。真实 Steam 初始化、
设置生效和非零原包交付仍由运行记录与 Switch trace 核验，不能据离线通过
宣称已启用真实游戏高级反馈。

### 22.29 桌面实测的配置偏差与识别对照

**Evidence：** 用户提供 `desktop-results-20261120-171734.zip`，其中
`session-20261120-171734-26936.jsonl` 的零 AppID 初始化成功，枚举到
Switch Pro；普通请求始终使用缓存 769。同期 `controller.txt` 在17:14:08
记录 controller 0 激活413080，`console_log.txt` 记录该 AppID 加载
`controller_base/streaming_steamdesktop.vdf`。触觉设置返回 accepted=true 后
仍有正常 flags=0 原生请求；没有设备回包，用户报告无震动。
`streaming_log.txt` 记录10.10.10.111的串流。用户明确确认客户端屏幕仍运行，
采集无响应不能推翻该观察。提供的 ZIP 本身也证明该次退出导出成功，
原版只有退出后才导出，不能把运行中未找到 ZIP 当成导出失败。

**Conclusion：** 零应用初始化与远程设备枚举已经真实验证；进程缓存769
与主机设备激活记录不一致。结合 §22.25 的 worker AppID 匹配条件，普通发送
的配置来源有明确缺陷。原生请求不带这个 AppID，不能用同一原因解释其失败。
accepted=true 不证明原生路径各项门禁已满足，更不证明设备收到非零包。

修正版读取 Steam `controller.txt` 尾部128KiB中对应 index 最近的
`Queueing activation`，日志标为 `steam_controller_activation_log`；缺失则
标为 `client_cache_unverified`。这只是激活请求记录，**不是服务端活动 worker
的原子读回**，切换瞬间或日志缺失时仍可能不匹配。替身回归分别维护服务端
配置与进程缓存，覆盖真实记录中的769缓存不变、设备切换413080的情况。

内置识别对照调用固定 DLL owned map `+0x100` 的
`IdentifyControllerRumbleEffect`（`0x138869b70`），参数为 index，
方法ID 0x4c9db0fc、尾标识0x51ba1617、bool回复。底层
`0x138620060` 的 Switch type38 使用 Steam 自带识别反馈，并非游戏原生
脉冲；其内部识别标志不能用来证明正常游戏门禁已满足。原始 marshaler
序列化与替身回复已核验；真实识别震动、修正版普通请求交付及原生失败原因
均不能由离线检查推定。

面板另记录采集地址、请求与 Winsock 错误，窗口内导出会先复制日志快照，
由独立 PowerShell 收集后打包。快照及无回包场景已用 Win32 替身检查；新增
PowerShell 导出分支没有在本地 Windows 上执行，不宣称已完成真实运行验收。

### 22.30 桌面普通输出实测与原生脉冲的诊断范围

**Evidence：** 用户提供 `desktop-results-20261120-173643-282.zip` 与
`desktop-results-20261120-173835-333.zip`，明确报告桌面直连有震动、原生无
可感反应。首份有110次非零桌面请求，AppID均为413080；XInput枚举0，
公开Steam Input枚举1并有49次非零请求。第二份在4719ms记录触觉
enabled=true/accepted=true，6047ms才发正常flags=0双侧原生请求，
不是漏开触觉。两份UDP采集都只有Winsock10060超时，没有设备回包。
因此桌面普通输出有真实用户确认；原生请求没有设备收包或执行证据，
不能把“没感觉”直接写成“Steam未发送”，也不能归因于游戏不支持。

**Evidence / 公开API上下文：** 固定DLL的SteamInput006 slot `+0xf0`
为`0x138738550`，从全局`0x13984db70`的GameID取请求AppID，再交给
controller `+0x228`。原始adapter执行对照核验0、413080和type2快捷方式
三种输入；terminal为替身。结合§22.25的worker匹配条件，枚举到handle
不保证该进程的输出AppID匹配设备配置。此次日志只有环境AppID=0，
并未直接记录这个全局值，故公开API无输出的具体因果仍不能只靠环境值定案。
修正版在固定DLL初始化后记录`public_output_context`的实际只读值，
桌面面板只显示桌面直连与原生脉冲；XInput/公开API保留在游戏启动模式。

**Correction / 原生参数的可感性：** 微秒持续、间隔、重复数是发给Steam
的请求字段，不是Switch端已保留的效果。§22.4、22.11的原始worker与producer
表明Nintendo分支将非零持续值压成固定短脉冲，忽略producer的间隔与重复数；
已核验样本只有5ms的非零段，解码振幅约0.0441942，不能当作等比例体感力度。
因此调大单次API参数不能保证更强或更长的反馈，面板此前不足以让用户辨别
“未交付”与“单次太短弱”。“原生连发2秒”改为每80ms重新发送正常flags=0
请求；这是重复请求对照，不是修改载波频率、原始振幅或证明游戏高级反馈。
停止、切换、断线和关闭会取消连发并发送原有停止请求。

**只读观察边界：** 触觉设置后和原生请求前自动检查固定SHA主机的配置、
偏好、pulse/rumble开关、enhanced及普通强度缓存，并记录全局write开关。
旧reader只遍历engine的API用户树且跳过flag=0对象；该路径未找到目标
不能证明主机没有目标对象。原始forwarder按`+0x10922d`选择`+0x180/+0x190`，
修正版均按实际vtable检查，树外候选通过私有可读内存的有界查找发现。
首次查找最多256MiB/750ms，找到匹配parent后仅复用经重新验证的指针，
不持续扫描，不写主机内存或保存内存转储。只报告通过parent/IO/Generic
vtable、handle、type和配置指针重读的状态；查找不全或读取失败记为未知。
候选查找验证单个parent内的唯一目标，不声称全进程对象全局唯一或原子快照。
Win32内存查询/读取在Wine中的合成对象上执行，实际Steam布局/状态读取
仍未由这项离线测试证明。Win32回归另覆盖连发、到时停止和连发中关闭。
