"""导出连续公开实测回放及电脑参考值，保持模型权重不变。"""
import csv
import os
import sys
from datetime import datetime, timedelta

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
os.environ.setdefault("TF_ENABLE_ONEDNN_OPTS", "0")

import numpy as np
from data import ROOT, load_config, read_records
from runtime import concentrations, infer, load_bundle, normalize


def export():
    configuration = load_config()
    if configuration["features"] != ["co2_ppm"]:
        raise ValueError("部署回放仅支持 CO₂ 单特征配置")
    metadata, interpreter = load_bundle(ROOT / configuration["output"])
    if metadata["features"] != ["co2_ppm"] or metadata["feature_indices"] != [0]:
        raise ValueError("部署回放模型必须使用首列 CO₂ 单特征")
    records, _ = read_records(ROOT / configuration["dataset"]["csv"], configuration)
    start = datetime.fromisoformat(configuration["screen_replay"]["start"])
    count = configuration["screen_replay"]["minutes"]
    selected = [row for row in records if start <= row[0] < start + timedelta(minutes=count)]
    if len(selected) != count or any(not row[2] or row[0] != start + timedelta(minutes=index)
                                     for index, row in enumerate(selected)):
        raise ValueError("选定回放区间不完整或包含无效记录，拒绝拼接")
    # 回放仍保留完整三通道供屏幕显示；只有送入模型的窗口按 feature_indices 投影为 CO₂。
    raw_values = np.stack([row[1] for row in selected])
    values = raw_values[:, metadata["feature_indices"]]
    width = metadata["window_minutes"]
    histories = np.stack([values[index - width + 1:index + 1] for index in range(width - 1, count)])
    outputs, saturation = infer(interpreter, normalize(histories, metadata))
    predicted = concentrations(outputs, values[width - 1:, 0:1], metadata)
    references = [(2, float("nan"), float("nan"))] * (width - 1)
    references.extend((3, float("nan"), float("nan")) if rejected else (0, *prediction)
                      for prediction, rejected in zip(predicted, saturation, strict=True))
    def number(value):
        return f"{float(value):.9e}f" if np.isfinite(value) else "NAN"
    lines = ["#pragma once", "#include <math.h>", "#include <stdint.h>",
             "// 由 model/export_replay.py 生成；连续公开实测和桌面参考值，不是实时采集。",
             "namespace ReplayAssets {", f"constexpr unsigned count = {count};",
             f"constexpr unsigned startMinuteOfDay = {start.hour * 60 + start.minute};",
             f'constexpr char date[] = "{start.date()}";',
             f'constexpr char modelSha256[] = "{metadata["int8_sha256"]}";',
             "constexpr float samples[count][3] = {",
             ",\n".join("{" + ", ".join(number(value) for value in sample) + "}" for sample in raw_values), "};",
             "#ifdef AIR_UI_HOST", "struct Reference { unsigned status; float nearConcentration, farConcentration; };",
             "constexpr Reference reference[count] = {",
             ",\n".join("{" + str(status) + ", " + number(near) + ", " + number(far) + "}" for status, near, far in references),
             "};", "#endif", "}"]
    target = ROOT / "firmware/replay_assets.h"
    target.write_text("\n".join(lines) + "\n", encoding="utf-8")
    with (ROOT / configuration["output"] / "ui_replay_reference.csv").open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["index", "timestamp", "status",
                         *[f"prediction_{horizon}_ppm" for horizon in metadata["horizons_minutes"]]])
        writer.writerows((index, selected[index][0].isoformat(), *reference) for index, reference in enumerate(references))
    print(f"Exported {count} continuous minutes; rejected model windows: {saturation.sum()}; model SHA256 {metadata['int8_sha256']}")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    try:
        export()
    except (ValueError, OSError, RuntimeError) as error:
        print(f"回放导出失败：{error}", file=sys.stderr)
        raise SystemExit(1) from error
