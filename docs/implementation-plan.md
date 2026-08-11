# Open FMO 固件实施规划

## 1. 目标与约束

目标硬件为 ESP32-S3（16 MB Flash、2 MB PSRAM）+ NV3007 2.79 英寸屏幕 + ES8311 + PY32/BK4802 射频背板。固件需要完成：

- NRL 服务器目录、服务器切换、G.711/Opus 语音通联和状态显示；
- 编码器主屏旋转切换服务器，短按进入/确认，长按返回；
- Wi-Fi STA/AP Web 配网与 BLE 配网；
- BK4802 射频参数的本机菜单、Web 和串口 AT 配置；
- 模拟 CTCSS（哑音）收发、MDC1200 编解码；
- APRS-IS、GPS/NMEA、Bell 202 AFSK 收发；
- NVS 配置、OTA、故障回退与运行日志。

参考代码 `nrl-esp32` 与 `FMO-Radio-Module-BK4802-V2.00` 都采用 MIT 许可证。迁移文件需保留原版权声明，并在发行包中同时保留两份许可证。

## 2. 已确认硬件接口

| 模块 | ESP32-S3 GPIO / 参数 | 状态 |
| --- | --- | --- |
| NV3007 背光 | GPIO34，高电平有效 | 实机确认 |
| NV3007 CS/RST/DC | GPIO35 / 36 / 37 | 原固件逆向并实机确认 |
| NV3007 CLK/MOSI/MISO | GPIO38 / 39 / 45，SPI mode 0，33 MHz | 原固件逆向并实机确认 |
| 射频 AT UART | TX15 / RX16，UART0，19200 8N1 | 实机确认 |
| 射频 PTT | GPIO6，高电平发射 | 已确认 |
| 网络 PTT 输入 | GPIO17 | 已确认 |
| ES8311 I2C | SCL12 / SDA14，地址 0x18，20 kHz | 原厂代码与断电重启实机 ACK 双重确认；GPIO5 旧探测结果作废 |
| 功放使能 | GPIO21，高电平有效 | 已确认 |
| 编码器 | A40 / B41 / SW42 | 已确认 |
| 状态 LED | GPIO1 网络下行 PTT（高有效）/ GPIO2 射频 SQL（高有效）/ GPIO4 NRL 心跳（高有效） | 已确认 |

I2S 的 MCLK/BCLK/WS/DOUT/DIN 尚未写入当前引脚表。在启用扬声器或麦克风前必须从原固件或实机时钟探测中确认，禁止猜测后驱动，避免与其它外设冲突。

## 3. 软件分层

```text
UI / Web / BLE provisioning
             |
          AppState  <---- NVS ConfigStore
             |
  +----------+-----------+-------------+
  |                      |             |
NRL Session          RadioService   APRS/Signaling
UDP + codec          BK4802 AT      MDC/CTCSS/AFSK
  |                      |             |
  +--------------- AudioRouter --------+
                         |
                   I2S + ES8311
```

- `app/main.c`：应用入口与任务装配；
- `app/driver/board/`：当前硬件唯一的 GPIO 与屏幕参数来源；
- `app/driver/`：共享 I2C/I2S 总线、NV3007、编码器、ES8311 和状态 IO；
- `services/`：配置存储、Wi-Fi/BLE、服务器目录、NRL、射频 AT、APRS、信令；
- `audio/`：统一的 8/16/48 kHz 音频帧、重采样和多源路由；
- `app/driver/app_ui.*`：唯一写屏模块，消费 AppState 快照，不能直接阻塞网络或音频任务；
- `web/`：REST 配置接口和静态 Portal，不直接操作驱动。

NRL、射频和 APRS 之间通过事件队列与 AudioRouter 协作，避免多个任务同时访问 I2S、PTT 或扬声器。

## 4. 屏幕与菜单设计

屏幕旋转为 **428 × 142 横屏**。其宽高比 3.01，与参考图 742 × 256 的 2.90 接近，能保留参考布局而不压缩成竖屏列表。

主屏布局：

```text
+----------------------------------------------------------------------------+
| T:438.500  R:438.500      22:22       WiFi BT Batt                         | 18
+---------------------------+------------------------------------------------+
| 本机呼号（橙色大字）      | 当前 NRL 服务器（橙色选择条）                 | 48
|                           | 正在通话的远端呼号（白色大字）                 |
+---------------------------+------------------------------------------------+
| APRS距离/网格/消息  S表   | 延迟、在线人数、链路和收藏状态                 | 48
+----------------------------------------------------------------------------+
| 运行提示 / APRS 或信令消息                                     RX/TX 进度 | 28
+----------------------------------------------------------------------------+
```

交互规则：

- 主屏旋转：切换服务器候选；停止 800 ms 后保存并重连，防止快速旋转造成重复 DNS/UDP 重建；
- 主屏短按：进入设置菜单；菜单内旋转选择，短按进入/确认；
- 长按 800 ms：返回上一层；主屏长按可按配置触发 Wi-Fi/BLE 配网；
- 网络发射/接收时锁定危险设置，频率和功率修改需在退出发射后应用；
- 所有服务器、频率和开关调整先显示候选值，确认后才写 NVS。

菜单层级：

1. `NRL`：服务器、频道、呼号/SSID、编码、包长、PTT 超时；
2. `RADIO`：收/发频率、静噪、收/发音量、功率、频偏、RF 开关；
3. `TONE`：收/发 CTCSS、MDC ID/Opcode/Argument、信令路由；
4. `APRS`：APRS-IS、SSID、符号、路径、周期、固定坐标、立即信标；
5. `NETWORK`：Wi-Fi、BLE 配网、配置 AP、IP、时间同步；
6. `AUDIO`：麦克风、扬声器、ES8311 增益、滤波、AEC/降噪；
7. `SYSTEM`：屏幕亮度、旋转方向、版本、OTA、恢复默认、重启。

## 5. 外部协议

### NRL 服务器目录

- 主数据源：`https://www.nrlptt.com/api/platform-servers`；
- 解析 `data[]` 中的 `name/host/port/online/total/hidden/sort_order/status`；
- 忽略隐藏或端口非法条目，按 `sort_order` 排序；
- API 获取失败时继续使用 NVS 缓存与内置主站 `m.nrlptt.com:60050`；
- `host` 可能附带管理/上报端口（例如 `:9943` 或 `:8443`），NRL UDP 端口仍取独立 `port` 字段，主机名需剥离附带端口。

### BK4802/PY32 AT

串口为 19200 8N1，查询返回 `COMMAND:value\nOK\n`，设置成功返回 `SUCCESS\n`。ESP32 侧实现以下命令：

- `AT?`, `AT+NAME?`, `AT+VER?`, `AT+BANDCAP?`, `AT+SMETER?`；
- `AT+SQL?/=0..10`；
- `AT+TXFREQ?/=MHz`, `AT+RXFREQ?/=MHz`；
- `AT+RXVOL?/=0..10`, `AT+TXVOL?/=0..10`；
- `AT+TCTCSS?/=Hz`, `AT+RCTCSS?/=Hz`，`0.0` 表示关闭；
- `AT+TXPWR?/=LOW|MID|HIGH`；
- `AT+FREQTUNE?/=Hz`, `AT+RF?/=ENABLE|DISABLE`, `AT+SYS=RESET`。

Web 和本机菜单只修改统一的 `RadioConfig`，由 RadioService 串行化 AT 请求、校验响应和重试，不能从 HTTP 回调直接写 UART。

## 6. 迁移映射

| 功能 | `nrl-esp32` 来源 | 策略 |
| --- | --- | --- |
| NRL UDP/G.711 | `src/lib/nrl_audio_bridge.*`, `nrl_g711.*` | 先解除 UI/板级耦合，再迁入 |
| Opus | `src/media/opus_voice.*` | G.711 通联稳定后启用 |
| ES8311 | `src/app/driver/es8311.*`, `audio_passthrough.*` | 保留寄存器驱动，替换板级引脚 |
| Wi-Fi/Web | `nrl_wifi.*`, `wifi_config_portal.*` | 精简页面，保留 API 和多 Wi-Fi |
| BLE 配网 | `ble_config.*`, `improv_protocol.*` | 使用 NimBLE，禁用无关 HFP |
| MDC1200 | `components/mdc1200/` | 原样迁移 MIT 组件并接 AudioRouter |
| CTCSS | `src/lib/ctcss_decoder.*` | 解码接 RX 音频；发射优先用 BK4802 硬件音调 |
| APRS | `aprs_service.*`, `src/lib/aprs/*` | 分 APRS-IS、NMEA、AFSK 三步启用 |
| BK4802 | `FMO.../user/atCommand.*` | 仅复用协议定义，ESP32 写 AT 客户端 |

## 7. 实施阶段与验收

### P0：硬件基线与 UI

- 建立独立 ESP-IDF 工程、分区表、统一引脚表；
- NV3007 428 × 142 横屏驱动、双缓冲/分块刷新、背光；
- 编码器事件与菜单状态机；
- 验收：主屏完整显示，旋转切换模拟服务器，短按进菜单，长按返回，连续运行 2 小时无花屏。

### P1：配置、配网和服务器目录

- NVS schema/version、Web Portal、Wi-Fi STA/AP、BLE provisioning；
- HTTPS 获取并缓存服务器列表；
- 验收：无配置可进入配网，断网可使用缓存，旋钮切换后正确持久化。

### P2：音频与 NRL 通联

- 确认 I2S 引脚，ES8311 全双工，AudioRouter；
- G.711 收发、心跳、呼号/频道/延迟状态；随后接 Opus；
- 验收：连续双向通联 30 分钟，无明显断音、内存增长或 PTT 卡死。

### P3：射频控制

- BK4802 AT 队列、超时/重试、频率/静噪/音量/功率/SMeter；
- 本机、Web、串口三种配置入口；
- 验收：设置后读回一致，发射超时保护有效，异常复位不误发射。

### P4：信令

- CTCSS RX 检测和 TX，MDC1200 编解码与路由；
- 验收：标准音调测试集识别，MDC ID/Opcode/Argument 与参考实现互通。

### P5：APRS

- APRS-IS 登录/信标、NMEA、站点列表；Bell 202 AFSK 收发；
- 验收：IS 上可见信标，RF 测试帧 CRC 正确，主屏站点信息更新。

### P6：整机与发布

- OTA、配置迁移、看门狗、崩溃恢复、射频法规保护；
- 实机回归矩阵：断网、服务器失联、BLE/Wi-Fi 共存、长时收发、掉电恢复。

## 8. 风险与当前阻塞点

- I2S 五根信号仍需确认，这是 P2 实机音频的硬件前置条件，但不阻塞 P0/P1；
- 参考 NRL bridge 与原板 UI/状态 IO 耦合较深，迁移时必须通过适配层拆开，不能整文件无修改地直接注册全部任务；
- 射频发射属于外部状态改变，联调固件默认 `RF DISABLE`，仅在用户明确操作后启用；
- HTTPS 服务器目录需要证书包和系统时间；启动早期先使用缓存，SNTP 后再刷新。
