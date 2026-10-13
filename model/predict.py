"""负责读取连续历史并执行本地 int8 预测，不访问未来数据。"""
import argparse
import json
import os
import sys
from datetime import timedelta
from pathlib import Path

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
os.environ.setdefault("TF_ENABLE_ONEDNN_OPTS", "0")

import numpy as np

from data import ROOT, read_records
from runtime import concentrations, infer, load_bundle, normalize


def predict(path, directory):
    metadata, interpreter = load_bundle(directory)
    records, _ = read_records(path, metadata["configuration"])
    width = metadata["window_minutes"]
    if len(records) < width:
        raise ValueError(f"至少需要 {width} 分钟有效历史")
    history = records[-width:]
    if any(not row[2] or row[3] != history[0][3] or row[0] != history[0][0] + timedelta(minutes=index)
           for index, row in enumerate(history)):
        raise ValueError("最近历史含缺失、无效值、设备切换或重启，拒绝预测")
    values = np.stack([row[1][metadata["feature_indices"]] for row in history])[None]
    normalized = normalize(values, metadata)
    output, saturation = infer(interpreter, normalized)
    if saturation.any():
        raise ValueError("输入超出 int8 校准范围，请使用本教室数据重新训练，拒绝饱和预测")
    current = history[-1][1][0]
    predicted = concentrations(output, np.asarray([[current]]), metadata)[0]
    return {"origin_timestamp": history[-1][0].isoformat(), "source_kind": metadata["source_kind"],
            "current_ppm": float(current), "model_sha256": metadata["int8_sha256"],
            "predictions": {str(horizon): float(value) for horizon, value in
                            zip(metadata["horizons_minutes"], predicted, strict=True)}}


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description="使用连续历史在电脑本地运行 int8 CO₂ 模型")
    parser.add_argument("--csv", required=True, type=Path)
    parser.add_argument("--model", type=Path, default=ROOT / "model/artifacts/co2")
    arguments = parser.parse_args()
    try:
        print(json.dumps(predict(arguments.csv, arguments.model), ensure_ascii=False, indent=2))
    except (ValueError, OSError, RuntimeError) as error:
        print(f"预测失败：{error}", file=sys.stderr)
        raise SystemExit(1) from error
