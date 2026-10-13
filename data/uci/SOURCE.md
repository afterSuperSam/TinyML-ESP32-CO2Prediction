# 公开室内实测数据

- 数据：Occupancy Detection，Luis Candanedo，2016。
- 来源：[UCI 数据集页面](https://archive.ics.uci.edu/dataset/357/occupancy%2Bdetection)。
- DOI：https://doi.org/10.24432/C5X01N。
- 许可：[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)。使用或分发时保留作者、来源和修改说明。
- 原始归档 `occupancy.zip`：SHA256 `4ae3f46aa98eedff564a9f6924d1635173e2fd2c816004342a9be93076d3a81a`。三个原始 txt 保存在该归档内，不修改。
- `environment.csv`：由训练脚本提取时间、CO₂、温度、湿度；保留原始文件来源。光照、人数标签和派生湿度不作为预测输入。

数据来自办公室，原任务是占用分类。本项目改为用未来 CO₂ 作为回归标签，仅用于验证训练和 TinyML 部署流程。不能据此报告本教室准确度或风扇通风效果。

原始时间存在整分钟或相差一秒的记录。建窗前按最近整分钟对齐，最大容差一秒；不插值、不跨缺失、不跨重启。清洗计数、划分日期和有效预测比例保存在 `model/artifacts/co2/data_audit.json`。
