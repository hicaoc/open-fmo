# FMO / NRL 双体系合并评估

> 依据：`sim/` 模拟器实网验证结果（2026-08，详见 `sim/docs/verification.md`）、
> `BD4XGT-bak.bin` 逆向分析（`docs/firmware-analysis.md`）、
> FMO 官方公开文档（bg5esn.com/docs/）与 SAS 开源实现。
> 本文回答：在现有开发板固件（已实现 NRL）上如何支持 FMO，两套体系如何共存。

## 1. 结论先行

**推荐方案 A：双协议并存、运行时切换（"网络制式"开关）。**

- NRL 与 FMO 在技术上是完全独立的两条网络栈，互不共享会话状态，共存没有协议冲突；
- 开发板同一时刻只需要一条语音通路（PTT 仲裁本来就排他），双栈常驻内存代价可接受（粗估 +60~90KB RAM、+150KB flash）；
- 合并工作量集中在：FMO 证书存储/SAS 自签、MQTT 客户端、FMO 帧封包、APRS 发现解析（复用现有 aprs_service 的传输层）、制式切换 UI。
- **不建议做 NRL↔FMO 语音桥接**（见 §4 方案 B 的合规分析）。

## 2. 两套协议栈对比

| 维度 | NRL（现 firmware/ 已实现） | FMO-V4（sim/ 已验证） |
| --- | --- | --- |
| 发现/目录 | HTTPS API `www.nrlptt.com/api/platform-servers` + 内置主站兜底；NRLBOX APRS 信标（`@udp://…:60050`） | **APRS-IS 广播**（FMO-V4 STATION，10 分钟周期，带 PKI 证书+签名）；无中心 API |
| 身份认证 | 无 PKI；呼号自报 | 三级 PKI（Root→Intermediate→User），Ed25519；eFuse HMAC 保护本地私钥 |
| 连接认证 | 服务器侧自定 | **SAS HTTP 认证**：password=base64url(JSON 证书包+Ed25519 proof)，EMQX 转发 SAS 验证 |
| 语音传输 | **UDP** 直连服务器:60050 | **MQTT**（EMQX，1883 明文）`FMO/RAW` |
| 语音编码 | G.711（已通）/ Opus 16kHz（预留） | 双格式：**IMA ADPCM 4bit 8kHz**（0x02，2026-08 起实网主流，32kbps 定码率，逐块 80ms/320B 带状态头）+ **Opus SILK NB 8kHz、40ms**（0x01 老格式，TOC=0x10，~5-12kbps VBR）；sim 收发均已实现 |
| 帧格式 | nrl 私有 UDP 包 | 34B 头（呼号明文/时间戳/总长/块数/CRC32）+ 块链（5B 内层头+载荷，内层 type 0x01=Opus / 0x02=ADPCM） |
| 呼叫信令 | 服务器内 | APRS 消息（APFM00：CALL/QTHQRY/…）+ MQTT topic |
| 服务器生态 | nrlptt.com 官方节点 | 爱好者自建（实网 30+ 台活跃，EMQX+SAS） |
| 治理 | 平台管理 | CRL 吊销 + 服务器本地 ACL（角色 user/super/admin） |

关键共性：**两者都把 APRS 当发现/广播层用**（NRL 用 NRLBOX 信标，FMO 用 APFMO4 信标），现有 `aprs_service` 传输层可复用。

## 3. ESP32-S3 资源评估

- **Flash**：新增 FMO 模块（证书/MQTT/帧/APRS 解析）约 +120~180KB，当前分区 app0/app1 各 6MB，余量充足；
- **RAM/PSRAM**：MQTT 客户端（esp-mqtt）~20KB，SAS 凭据构建（CBOR+Ed25519，mbedtls 已有）~10KB，编解码：IMA ADPCM 纯查表 ~2KB（实网主流格式，成本远低于 Opus），Opus 8kHz 实例 ~30KB（仅兼容老格式时需要，现有 16kHz 实例复用同一库），帧缓冲 <10KB；峰值增量 <100KB，PSRAM 无压力；
- **任务**：新增 `fmo_link` 任务（MQTT+帧封包）与 `fmo_discover`（APRS 解析，可并入 aprs_service 任务）；优先级与 nrl_link 同级即可；
- **音频路由**：AudioRouter 增加 FMO 源/汇（8kHz ADPCM/Opus ↔ I2S），与 NRL（8/16kHz）共存；PTT 仲裁沿用现有事件队列——**同一时刻只允许一个制式收发**；
- **证书存储**：userCert/devicekey/int CA 约 1.5KB，建议存 NVS blob（json）或 LittleFS
- **风险**：MQTT 长连接与 Wi-Fi 共存已属常态；无明显硬阻塞点。

## 4. 合并形态

### 方案 A：双协议切换（推荐）

- SYSTEM/NRL 菜单加"网络制式"：`NRL / FMO / AUTO`；
- FMO 模式下：APRS 发现 → 服务器列表（复用主屏服务器选择交互）→ SAS 自签连接 → FMO/RAW 语音；
- 两栈常驻但只激活一条语音路；切换时断开另一路；
- 工作量：中（估计 1.5~2 周含实机回归）。

### 方案 B：NRL↔FMO 桥接互通（不推荐）

- 把 NRL 语音桥进 FMO 服务器（或反向）；
- **合规问题**：FMO 的信任模型要求每个身份都是经 CRAC 核验的持证火腿（用户证书）；把无证书的 NRL 用户桥进 FMO 等于注入未核验身份，破坏"每条语音都可追溯"的设计，也会触发服务器审计/封禁风险；
- 技术上还需双向转码（G.711/Opus16k ↔ Opus SILK NB）与身份映射，复杂度高、收益低。

### 方案 C：仅 FMO 客户端最小集

- 只做"发现 + 连接 + 收听 + PTT"，不做 APRS 信标/事件/远控；
- 工作量最小（1 周内），适合先出一版验证；
- 后续可增量补 M4 的完整信令。

**建议路线：C（最小可用）→ A（完整双制式）。**

## 5. 模块迁移映射（sim → firmware）

| sim/backend（Python 已验证） | firmware 目标模块（C） | 说明 |
| --- | --- | --- |
| `fmo_auth.py`（SAS 凭据） | `services/fmo_auth.c` | CBOR+Ed25519 用 mbedtls；cbor2 → 手写 200 行编码器（字段固定） |
| `fmo_frame.py`（帧封包/CRC32） | `services/fmo_frame.c` | 已 3053/3053 字节级验证，直接翻译 |
| `mqtt_client.py` | `services/fmo_link.c` | 用 esp-mqtt；keepalive=60；client id `FMO-<呼号>-<uid>-<4位启动随机后缀>` |
| `aprs.py`（FMO-V4/STATION/CERT 解析） | 并入 `services/aprs_service.c` | 新增 FMO 载荷解析器；GBK/UTF-8 双编码兼容注意 |
| `fmo_station.py`（pubStationList.csv） | `services/server_directory.c` 扩展 | 服务器表缓存格式兼容（可选） |
| `audio_tx.py`/`audio_rx.py`（Opus 8k） | `audio/opus_voice.c` 扩展 | 现有 opus 封装加 8kHz/40ms 配置档 |
| `certstore.py` + eFuse HMAC 流程 | `services/config_store.c` 扩展 | 证书 JSON 存 NVS；不做原厂 .enc 兼容 |
| `protocol.py`（Ed25519/JWT/CERT CBOR） | `services/fmo_cert.c` | 含 CERT blob 编解码、指纹、链验签、CRL 检查 |

## 6. 风险与开放项

- **VOCAL/ONLINE 信标与 T-CALL 细节**：发射时是否需要先发 VOCAL/在线状态 TELE，实机联调时确认（sim 已能抓包比对）；
- **服务器侧 ACL 对 PTT 的限制**：个别服务器可能限制发言角色，以实机测试为准；
- **APRS 上行**：verified 登录（passcode 已有）后发 STATION/ONLINE 信标的时机与频率限制（"Server broadcast too frequent"）；
- **合规**：双制式固件默认 RF DISABLE 原则不变；FMO 侧遵循其社区规则（证书身份、不作商业用途）。

## 7. sim 已验证清单（作为合并的验收基线）

1. 证书链：eFuse HMAC 解密 → Ed25519 私钥/userCert/指纹/中级 CA 验签通过；
2. 发现：APRS 全馈 10 分钟内 30 台服务器，297 条报文 100% 解析（含 UTF-8/GBK 双编码）；
3. 认证：SAS 自签凭据连接实网最大服务器成功；
4. 收语音：FMO/RAW 抓 3053 帧 → Opus SILK NB 解码出声（人耳确认）；
5. 发链路：帧构造 3053/3053 字节级一致；PTT→服务器→回环确认通过。
