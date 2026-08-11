# Open FMO

ESP32-S3 + NV3007 + ES8311 + BK4802/PY32 的开放固件。

- 硬件与功能实施规划：[`docs/implementation-plan.md`](docs/implementation-plan.md)
- 原固件备份：`BD4XGT-bak.bin`（逆向分析：[`docs/firmware-analysis.md`](docs/firmware-analysis.md)）
- FMO/NRL 双体系合并评估：[`docs/fmo-nrl-merge-evaluation.md`](docs/fmo-nrl-merge-evaluation.md)
- FMO 协议网页模拟器：`sim/`（证书、APRS 服务器发现、MQTT 通联验证）
- 正式固件：`firmware/`
- 辅助脚本：`scripts/`

正式固件使用 `C:\esp\esp-idf` 构建，目标芯片为 ESP32-S3。

主要源码布局：

```text
firmware/main/
├─ app/
│  ├─ main.c
│  └─ driver/
│     ├─ board/       # 本板引脚和屏幕参数
│     ├─ fonts/       # 显示字体资源
│     ├─ i2c_bus.*    # 全板共享 I2C 总线
│     ├─ audio_bus.*  # 全板共享 I2S 音频总线
│     └─ ...          # NV3007、ES8311、编码器和状态 IO
└─ services/          # Wi-Fi、配置、OTA、服务器目录和射频 AT
```

未确认的 I2S 引脚不会被主动驱动。
