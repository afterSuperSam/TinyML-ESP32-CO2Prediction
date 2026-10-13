# 快速上升实验数据来源

2026-10-11 准备。主数据 `environment.csv` 全部为 `synthetic_chamber`，不是实测。生成器为 `model/rapid_rise.py`，参数为 `model/rapid_rise_config.json`。没有导入模型预测作为标签。

## 公开实测筛查

- 先检查项目已有 [UCI Occupancy Detection](https://archive.ics.uci.edu/dataset/357/occupancy%2Bdetection)，Luis Candanedo，2016，许可 [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)。原归档 SHA256：`4ae3f46aa98eedff564a9f6924d1635173e2fd2c816004342a9be93076d3a81a`；原文件和 `data/uci/SOURCE.md` 保留。
- 连续有效历史过去5分钟增长至少100 ppm，共发现81个起点，最大5分钟增长1117.5 ppm。起点及浓度保存在 `uci_rise_audit.json`。这些是办公室传感器记录，并未据此确认箱体、释放量或气密程度；极端上升不单独证明物理条件。此数据没有参与候选训练、校准或调参。
- 有限检索检查了 [Mendeley closed-chamber dataset](https://data.mendeley.com/datasets/35sc3vnrtg/2) 和 [气体积累箱研究](https://www.sciencedirect.com/science/article/pii/S2772375525001406)。前者详情页本次读取失败，后者为环境通量装置研究；本次未取得可核验、满足40–60L、分钟级CO₂/温湿度与释放/换气条件的数据。没有下载或重新分发它们的数据，也不声称不存在其他适用公开数据。

## 合成方法与假设

参考 [NIST CONTAM 用户文档](https://www.nist.gov/publications/contam-user-guide-and-program-documentation-version-34) 的单区污染物质量守恒方法。此处方程由本项目独立实现，NIST没有为这套箱体提供参数或数据。设箱体有效容积 V（L）、纯CO₂释放 g（mL/min）、背景浓度 Cₒ（ppm）、泄漏及有效对外换气率 λ（min⁻¹）：

`dC/dt = (1000 g / V) × (1 − C/10⁶) − λ(C − Cₒ)`。

CO₂气体进入时置换等体积混合气体，箱体容积保持不变；恒定六秒步内采用解析指数积分，不剪裁真实浓度。充分混合、固定气体密度与环境背景，没有多区、液体溶解、压强变化或化学反应动力学。

传感器观测增加固定场次偏差与高斯噪声，并使用60秒时间常数的一阶响应。60秒尺度参考 [Sensirion SCD4x 数据手册](https://sensirion.com/media/documents/48C4B7FB/67FE0194/CD_DS_SCD4x_Datasheet_D1.pdf) 的CO₂响应指标；一阶形式、偏差和噪声大小均是本实验假设，不是SCD41标定或精度证明。标签为后续生成的传感器观测，与部署读取值的定义一致；`true_co2_ppm` 仅留作物理诊断，不作为训练输入或标签。

| 参数 | 预设范围或值 | 性质 |
|---|---|---|
| 容积 | 40–60 L | 课题箱体方案范围，未实测 |
| 背景 / 初始CO₂ | 410–480 / 450–1300 ppm | 自设覆盖范围 |
| 静态高浓度初值 | 1800–3000 ppm | 自设响应场景 |
| 泄漏率 | 0.002–0.02 min⁻¹，即0.12–1.2 h⁻¹ | 自设，未辨识 |
| 普通 / 快速纯CO₂释放 | 0.6–1.8 / 2–4 mL/min | 自设，不等同化学投料流量 |
| 释放上限 | 90 mL/场 | 控制模拟尺度，不是实物制气建议 |
| 对外排风 / 开孔增加换气率 | 0.05–0.15 / 0.02–0.07 min⁻¹ | 自设有效换气，非风扇标称流量 |
| 单次衰减脉冲 | 10–25 mL，衰减时间4–9分钟；3次间隔18分钟 | 模拟分次持续扰动，不当作恒流 |
| CO₂噪声 / 场次偏差 | 标准差5 ppm / −25至25 ppm | 数值假设，非传感器精度 |
| 温度 / 湿度 | 20–30°C / 30–70% | 独立背景、漂移与噪声，无热湿耦合 |
| 采样 / 积分步长 | 1分钟 / 6秒 | 按物理时间模拟，不是加速实测 |

六种工况等量出现：连续释放、快速释放及停止、分次衰减脉冲、排风开始/停止、开孔开始/停止、初始高浓度衰减。实际每场全部参数与总释放量见 `sessions.json`。预测窗口会包含部分未来投料或换气变化；这些动作不在输入中，无法由历史唯一预见，应按实际误差报告。

## 划分与可复现

生成种子20261011，网络/校准种子42。150场×150分钟=22,500条；每场独立参数、噪声和唯一场次号，用不同人工日期表示。日期不表示真实采集；先按完整日期分90/30/30场，再构造20条历史至+5/+15分钟目标。每场116个应预测起点，训练10,440、验证3,480、测试3,480窗口。场次间无记录的人工日期空档不作为应预测分钟。

代表性量化只取训练窗口，包括固定种子抽样和每维特征极值；测试不参加选参、早停、标准化或校准。划分场次号、数据校验见 `model/artifacts/rapid-rise/data_audit.json`，校准工况计数和每个留出场次误差见 `model/artifacts/rapid-rise/rapid_rise_metrics.json`。CSV SHA256由生成器保存至 `sessions.json`。

自生成数据与脚本随本项目保存，第三方数据没有混入主CSV；UCI派生筛查保持CC BY 4.0作者、来源及修改说明。当前合成数据不额外声明项目级开源许可证。

```powershell
& .\.venv\Scripts\python.exe model/rapid_rise.py
# 已训练后只重算补充指标，不重新训练
& .\.venv\Scripts\python.exe model/rapid_rise.py --evaluate-only
```

输出位于 `model/artifacts/rapid-rise/`，候选头文件为其中的 `model_assets.h`。不会写入 `model/artifacts/co2/`、当前固件模型或屏幕回放资产。合成误差不能代表真实箱体或教室泛化；进入实物阶段后，应保存原始箱体场次并重新划分评估。
