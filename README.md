# 教室 CO₂ 预测与通风实验系统

基于 ESP32-S3、SCD41 和触摸屏的 CO₂ 采集、预测与实验原型。当前固件 v1.12.0，默认实时采集；风扇控制仍为界面预览。

## 文档

独立风扇测试验证（2026-10-13）：ESP32-S3 目标编译通过，程序 317835 字节、静态内存 22316 字节；尚未烧录或验证实际调速。

独立风扇测试：Arduino IDE 打开 `fan_test/fan_test.ino`，选择 ESP32S3 Dev Module（ESP32 Arduino core 3.x），沿用原生 USB CDC 设置。GPIO17 输出 25kHz PWM，每 5 秒循环 100%、75%、50%、25%、0%；串口 115200 显示目标档位，不读取转速，0% 不保证停转。参数在 `fan_test/config.h` 并有编译期校验。上传会替换当前运行的主程序。

测试使用 GPIO17 的 3.3V 推挽输出，适用于已确认 PWM 输入高电平最低约 2.8V 的 Delta AFB0412SH 系列；不要用于可能把 5V 上拉直接送回 GPIO 的其他风扇。若更换风扇或使用外部反相晶体管驱动，需重新核对 `openDrainOutput` 和 `invertOutput`。风扇用额定电压的独立电源并共地；DFS1401055G0T 是 5V，不能接 12V。2026-10-13 增加独立测试，不改变主固件的风扇预览逻辑；编译验证与实机验证分别记录。

| 文档 | 内容 |
|---|---|
| [操作 Instruction](docs/Instruction.md) | 杜邦线、供电、编译上传、触摸校准、记录、导出与排障 |
| [系统 System](docs/System.md) | 功能边界、模块职责、存储协议、配置与硬件集成 |
| [模型 Model](docs/Model.md) | 数据、训练、TinyML、预测情景、指标与实验设计 |
| [课题原始方案](课题.docx) | 历史方案；其中风扇电压、预测时距和接入状态已过时，现行操作以以上三份文档为准 |

## 架构

```text
firmware/                  firmware.ino、模块头文件、模型资产、分区及构建配置
model/                     训练、推理和实验脚本
  artifacts/co2/           当前模型、指标及训练结果
  artifacts/rapid-rise/    独立合成箱体候选及评估结果
data/                      数据与来源说明
docs/                      Instruction、System、Model
tools/                     固件构建、数据导出、字模生成及检查工具
pcb/                       已有硬件工程文件
.build/                    编译产物和检查结果
.venv/                     本机 Python 环境
```

原 `model/` 是算法代码，`models/` 是训练产物；现已合并为 `model/`，产物统一放 `artifacts/`，避免两个近似目录名。

## 使用与配置

先按 [操作文档](docs/Instruction.md) 接线。在项目根目录执行：

```powershell
& ./.venv/Scripts/python.exe tools/firmware.py
& ./.venv/Scripts/python.exe tools/firmware.py --upload --port COM4
```

端口按实际设备选择。`firmware/firmware.ino` 是唯一源码入口，与目录同名；Arduino IDE 可直接打开，构建工具也直接使用该目录。引脚和运行参数在 `firmware/config.h`，编译参数在 `firmware/build.json`，模型参数在 `model/config.json`。

模型只使用 CO₂，相对时间即可预测，不要求校时。v1.11.2 通过重新量化将输入覆盖扩大至约 350～5,000 ppm，温湿度继续记录但不进入模型。量化配置位于 `model/config.json` 的 `quantization_bounds`；修改后运行 `model/train.py --requantize`、`model/export_replay.py`，再编译上传。扩大输入覆盖不代表高浓度预测已得到实测验证。

## 验证与部署

USB 导出先保存原始流，再校验生成 CSV；使用方法见 [操作手册](docs/Instruction.md#5-开始记录与导出)，导出问题与验证证据见 [系统文档](docs/System.md#usb-导出检查)。Instruction 仅维护操作说明，不记录逐次执行过程。

2026-10-12 v1.12.0 已上传 COM4 并通过写入哈希校验：保留原双栏主页及字体，仅删除顶部标题栏；设置显示版本并支持持久化 TinyML 开关，新增三页调试和分钟诊断。独立审查、实际页面渲染、针对性桌面测试及完整编译通过。板端已验证关闭后继续采样记录、重启保留关闭状态、校准保留和恢复开启，旧场次数未减少；详见 [系统文档](docs/System.md#v1120-验收)。

本版未改变模型、接线与分区。历史缺口在旧版曾复现并自行恢复，具体原因无法回溯；新版记录坏分钟原因，不放宽有效数据要求。验收结束时已恢复预测开启，重启后正在重新积累 20 个连续有效分钟；尚未验证本版这之后的在线推理、真实屏幕手感或长时稳定性。量化范围及同子集误差沿用 [v1.11.2 检查记录](.build/quantization-verification-v1.11.2.json)。

## 安全边界

12V 必须先降为 5V 再接开发板 5V/VIN。当前风扇标称 5V，不能接 12V。实测数据、模型预测和风扇模拟分别保存；当前模型尚未优于持续性基线，风扇情景未经过实物标定。
