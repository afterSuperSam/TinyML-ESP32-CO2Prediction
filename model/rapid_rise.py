"""负责独立箱体合成数据、快速上升训练和按场次评估；不更换板端模型。"""
import argparse
import csv
import hashlib
import json
import math
import sys
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np

from data import ROOT, load_config, make_windows, read_records, split_dates


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def load_experiment(path):
    settings = json.loads(path.read_text(encoding="utf-8"))
    expected = {"training_config", "data_directory", "output_directory", "features", "seed", "session_count", "session_minutes",
                "simulation_step_seconds", "synthetic_start_date", "volume_liters", "ambient_co2_ppm", "initial_co2_ppm",
                "static_co2_ppm", "leakage_per_minute", "source_ml_per_minute", "fast_source_ml_per_minute", "maximum_release_ml",
                "release_start_minute", "release_duration_minutes", "ventilation_start_minute", "ventilation_stop_minute",
                "fan_exchange_per_minute", "opening_exchange_per_minute", "pulse_spacing_minutes", "pulse_count", "pulse_release_ml",
                "pulse_decay_minutes", "sensor_response_seconds", "co2_noise_standard_deviation_ppm", "co2_bias_ppm",
                "temperature_c", "humidity_pct", "temperature_drift_c_per_minute", "humidity_drift_pct_per_minute",
                "temperature_noise_c", "humidity_noise_pct", "scenarios", "horizons_minutes", "epochs", "patience",
                "rapid_rise_history_minutes", "rapid_rise_change_ppm", "operating_co2_ppm"}
    if set(settings) != expected:
        raise ValueError("箱体实验配置字段缺失或包含未知字段")
    for name, value in settings.items():
        if isinstance(value, (int, float)) and (not math.isfinite(value) or value <= 0):
            raise ValueError(f"实验参数 {name} 必须为有限正数")
        if isinstance(value, list) and name not in {"scenarios", "horizons_minutes", "features"}:
            if len(value) != 2 or not all(math.isfinite(item) for item in value) or value[0] >= value[1]:
                raise ValueError(f"实验范围 {name} 必须为两个递增有限数")
    for name in ["session_count", "session_minutes", "simulation_step_seconds", "pulse_count", "epochs", "patience", "rapid_rise_history_minutes"]:
        if type(settings[name]) is not int:
            raise ValueError(f"实验参数 {name} 必须为整数")
    if 60 % settings["simulation_step_seconds"] or settings["session_minutes"] > 1440:
        raise ValueError("模拟步长必须整除60秒，每场不超过一天")
    if set(settings["scenarios"]) != {"continuous_release", "fast_release_stop", "pulse_release", "fan_change", "opening_change", "static_decay"}:
        raise ValueError("实验必须包含六种已定义工况")
    for name in ["training_config", "data_directory", "output_directory"]:
        resolved = (ROOT / settings[name]).resolve()
        if not resolved.is_relative_to(ROOT) or resolved == ROOT:
            raise ValueError("实验路径必须位于项目子目录")
    configuration = load_config(ROOT / settings["training_config"])
    configuration.pop("quantization_bounds", None)
    configuration.update({"features": settings["features"], "horizons_minutes": settings["horizons_minutes"], "output": settings["output_directory"]})
    configuration["network"].update({"epochs": settings["epochs"], "patience": settings["patience"]})
    configuration["firmware"]["assets_directory"] = settings["output_directory"]
    configuration["firmware"]["directory"] = settings["output_directory"]
    directory = ROOT / settings["data_directory"]
    directory.mkdir(parents=True, exist_ok=True)
    write_json(directory / "training_config.json", configuration)
    configuration = load_config(directory / "training_config.json")
    if settings["session_minutes"] <= configuration["window_minutes"] + max(configuration["horizons_minutes"]):
        raise ValueError("每场记录不足以构造历史与未来目标")
    if settings["rapid_rise_history_minutes"] >= configuration["window_minutes"]:
        raise ValueError("快速上升判定必须包含在输入历史中")
    return settings, configuration


def generate(settings, configuration):
    rng = np.random.default_rng(settings["seed"])
    draw = lambda name: float(rng.uniform(*settings[name]))
    rows, sessions = [], []
    step_minutes = settings["simulation_step_seconds"] / 60
    response = 1 - math.exp(-settings["simulation_step_seconds"] / settings["sensor_response_seconds"])
    start_date = datetime.fromisoformat(settings["synthetic_start_date"])
    for number in range(settings["session_count"]):
        scenario = settings["scenarios"][number % len(settings["scenarios"])]
        volume, ambient, leakage = draw("volume_liters"), draw("ambient_co2_ppm"), draw("leakage_per_minute")
        initial = draw("static_co2_ppm" if scenario == "static_decay" else "initial_co2_ppm")
        source_rate = draw("fast_source_ml_per_minute" if scenario == "fast_release_stop" else "source_ml_per_minute")
        release_start = draw("release_start_minute")
        release_stop = release_start + min(draw("release_duration_minutes"), settings["maximum_release_ml"] / source_rate)
        ventilation_start, ventilation_stop = draw("ventilation_start_minute"), draw("ventilation_stop_minute")
        exchange = draw("opening_exchange_per_minute" if scenario == "opening_change" else "fan_exchange_per_minute")
        pulse_amount, pulse_decay = draw("pulse_release_ml"), draw("pulse_decay_minutes")
        temperature, humidity = draw("temperature_c"), draw("humidity_pct")
        temperature_drift, humidity_drift, bias = draw("temperature_drift_c_per_minute"), draw("humidity_drift_pct_per_minute"), draw("co2_bias_ppm")
        session_id = f"synthetic-{number + 1:03d}"
        start = start_date + timedelta(days=number)
        concentration = sensor = initial
        released_ml = 0.0
        sessions.append({"session_id": session_id, "scenario": scenario, "date": start.date().isoformat(),
                         "volume_liters": volume, "ambient_co2_ppm": ambient, "initial_co2_ppm": initial,
                         "leakage_per_minute": leakage, "source_ml_per_minute": source_rate,
                         "release_start_minute": release_start, "release_stop_minute": release_stop,
                         "ventilation_start_minute": ventilation_start, "ventilation_stop_minute": ventilation_stop,
                         "extra_exchange_per_minute": exchange, "pulse_release_ml": pulse_amount,
                         "pulse_decay_minutes": pulse_decay, "sensor_bias_ppm": bias})
        for minute in range(settings["session_minutes"]):
            rate, ventilation = 0.0, leakage
            if minute:
                for offset in range(60 // settings["simulation_step_seconds"]):
                    time = minute - 1 + (offset + 0.5) * step_minutes
                    if scenario == "pulse_release":
                        rate = sum(pulse_amount / pulse_decay * math.exp(-(time - event) / pulse_decay)
                                   for event in [release_start + index * settings["pulse_spacing_minutes"] for index in range(settings["pulse_count"])]
                                   if time >= event)
                    else:
                        rate = source_rate if scenario != "static_decay" and release_start <= time < release_stop else 0.0
                    rate = min(rate, max(0, settings["maximum_release_ml"] - released_ml) / step_minutes)
                    ventilation = leakage + (exchange if scenario in {"fan_change", "opening_change", "static_decay"}
                                              and ventilation_start <= time < ventilation_stop else 0)
                    # 单区充分混合：纯CO₂注入同时置换等体积气体，换气补入背景空气。
                    removal = ventilation + rate / (1000 * volume)
                    equilibrium = (1000 * rate / volume + ventilation * ambient) / removal
                    concentration = equilibrium + (concentration - equilibrium) * math.exp(-removal * step_minutes)
                    sensor += response * (concentration - sensor)
                    released_ml += rate * step_minutes
            measured = sensor + bias + rng.normal(0, settings["co2_noise_standard_deviation_ppm"])
            rows.append({"timestamp": (start + timedelta(minutes=minute)).isoformat(), "co2_ppm": f"{measured:.6f}",
                         "temperature_c": f"{temperature + temperature_drift * minute + rng.normal(0, settings['temperature_noise_c']):.6f}",
                         "humidity_pct": f"{humidity + humidity_drift * minute + rng.normal(0, settings['humidity_noise_pct']):.6f}",
                         "valid": 1, "device_id": "synthetic-chamber", "session_id": session_id,
                         "source": "synthetic_chamber", "scenario": scenario, "simulation_minute": minute,
                         "true_co2_ppm": f"{concentration:.6f}", "source_ml_per_minute": f"{rate:.6f}",
                         "exchange_per_minute": f"{ventilation:.6f}"})
        sessions[-1]["total_released_ml"] = released_ml
    directory = ROOT / settings["data_directory"]
    path = directory / "environment.csv"
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    write_json(directory / "sessions.json", {"source": "synthetic_chamber", "configuration": settings, "sessions": sessions,
                                             "csv_sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    return path


def screen_uci(settings, configuration):
    records, audit = read_records(ROOT / configuration["dataset"]["csv"], configuration)
    minutes = settings["rapid_rise_history_minutes"]
    threshold = settings["rapid_rise_change_ppm"]
    lookup = {row[0]: row for row in records}
    rises = []
    for row in records:
        span = [lookup.get(row[0] - timedelta(minutes=offset)) for offset in range(minutes + 1)]
        if all(value is not None and value[2] for value in span):
            change = float(row[1][0] - span[-1][1][0])
            if change >= threshold:
                rises.append({"end_timestamp": row[0].isoformat(), "past_change_ppm": change, "current_co2_ppm": float(row[1][0])})
    result = {"source_kind": "public_office_measurements", "criterion": f"past {minutes} minute CO2 rise >= {threshold} ppm; continuous valid samples",
              "raw_rows": audit["raw_rows"], "rapid_rise_origins": len(rises),
              "maximum_change_ppm": max((row["past_change_ppm"] for row in rises), default=None),
              "origins": rises, "interpretation": "Office measurements, not sealed-chamber measurements; not used for training or selection."}
    write_json(ROOT / settings["data_directory"] / "uci_rise_audit.json", result)


def evaluate(settings, configuration, path):
    from train import metrics
    from runtime import infer, interpreter_for, normalize
    directory = ROOT / settings["output_directory"]
    metadata = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
    audit = json.loads((directory / "data_audit.json").read_text(encoding="utf-8"))
    if metadata["configuration"] != configuration or audit["csv_sha256"] != hashlib.sha256(path.read_bytes()).hexdigest():
        raise ValueError("评估数据或训练配置已变化，不能复用旧模型预测")
    records, _ = read_records(path, configuration)
    groups = split_dates(records, configuration)
    windows = {name: make_windows(rows, configuration) for name, rows in groups.items()}
    report = json.loads((directory / "metrics.json").read_text(encoding="utf-8"))
    width, maximum = configuration["window_minutes"], max(configuration["horizons_minutes"])
    denominator = settings["session_minutes"] - width + 1 - maximum
    session_lookup = {row[0].isoformat(): row[3][1] for row in records}
    scenario_lookup = {item["session_id"]: item["scenario"] for item in json.loads((path.parent / "sessions.json").read_text(encoding="utf-8"))["sessions"]}
    result = {"source_kind": "synthetic_chamber", "rapid_rise_definition": f"past {settings['rapid_rise_history_minutes']} minute rise >= {settings['rapid_rise_change_ppm']} ppm; history only",
              "split_policy": "One independent synthetic session per artificial date; split complete dates first, then construct windows.", "splits": {}}
    for name, dataset in windows.items():
        session_ids = sorted({row[3][1] for row in groups[name]})
        candidates = len(session_ids) * denominator
        audit["splits"][name].update({"sessions": session_ids, "candidate_origins": candidates, "availability": len(dataset["times"]) / candidates})
        result["splits"][name] = {"session_count": len(session_ids), "candidate_origins": candidates, "windows": len(dataset["times"])}
        if name in report["results"]:
            report["results"][name]["quantization"]["online_availability"] = report["results"][name]["quantization"]["accepted_windows"] / candidates
    audit["policy"] += "; candidate denominator sums each independent session, excluding inter-session gaps"
    test = windows["test"]
    with (directory / "test_predictions.csv").open(encoding="utf-8", newline="") as handle:
        predicted_rows = list(csv.DictReader(handle))
    if [row["origin_timestamp"] for row in predicted_rows] != test["times"].tolist():
        raise ValueError("模型预测与独立测试窗口顺序不一致")
    horizons = configuration["horizons_minutes"]
    methods = ["persistence", "ridge", "mlp_float", "mlp_int8"]
    predictions = {method: np.asarray([[float(row[f"{method}_{horizon}"]) for horizon in horizons] for row in predicted_rows]) for method in methods}
    accepted = np.asarray([row["input_saturated"] == "0" for row in predicted_rows])
    rise = test["inputs"][:, -1, 0] - test["inputs"][:, -1 - settings["rapid_rise_history_minutes"], 0] >= settings["rapid_rise_change_ppm"]
    subsets = {"all_test": np.ones(len(rise), dtype=bool), "accepted_test": accepted, "rapid_rise_test": rise, "accepted_rapid_rise_test": rise & accepted}
    scenarios = np.asarray([scenario_lookup[session_lookup[time]] for time in test["times"]])
    low, high = settings["operating_co2_ppm"]
    subsets["operating_range_test"] = ((test["inputs"][:, :, 0] >= low) & (test["inputs"][:, :, 0] <= high)).all(axis=1) & ((test["targets"] >= low) & (test["targets"] <= high)).all(axis=1)
    subsets.update({f"scenario_{scenario}": scenarios == scenario for scenario in settings["scenarios"]})
    result["test_subsets"] = {}
    for label, mask in subsets.items():
        result["test_subsets"][label] = {"windows": int(mask.sum()), "rejected_windows": int((mask & ~accepted).sum()),
                                       "methods": {method: metrics(test["targets"][mask], values[mask], horizons, configuration["experimental_threshold_ppm"])
                                                   for method, values in predictions.items()} if mask.any() else {}}
    result["per_test_session"] = {}
    for session_id in sorted({session_lookup[time] for time in test["times"]}):
        mask = np.asarray([session_lookup[time] == session_id for time in test["times"]])
        result["per_test_session"][session_id] = {"scenario": scenario_lookup[session_id], "windows": int(mask.sum()),
                                               "methods": {method: metrics(test["targets"][mask], values[mask], horizons, configuration["experimental_threshold_ppm"])
                                                           for method, values in predictions.items()}}
    result["threshold_lead"] = threshold_lead(groups["test"], test, predictions, accepted, configuration["experimental_threshold_ppm"])
    write_json(directory / "metrics.json", report)
    write_json(directory / "data_audit.json", audit)
    write_json(directory / "rapid_rise_metrics.json", result)
    # 用冻结候选模型核对训练量化范围，并重建只取自训练集的校准工况统计。
    normalized = normalize(windows["train"]["inputs"], metadata)
    _, saturation = infer(interpreter_for(directory / "co2_int8.tflite"), normalized)
    result["training_input_rejected_windows"] = int(saturation.sum())
    rng = np.random.default_rng(configuration["network"]["seed"])
    indices = np.unique(np.concatenate([rng.choice(len(normalized), min(configuration["calibration_samples"], len(normalized)), replace=False),
                                       np.argmin(normalized, axis=0), np.argmax(normalized, axis=0)]))
    calibration_scenarios = [scenario_lookup[session_lookup[time]] for time in windows["train"]["times"][indices]]
    result["calibration_scenario_counts"] = {scenario: calibration_scenarios.count(scenario) for scenario in settings["scenarios"]}
    write_json(directory / "rapid_rise_metrics.json", result)
    write_report(directory, settings, report, result)
    plot_sessions(directory, test, predictions, session_lookup, scenario_lookup, settings)
    print(json.dumps({"model": report["model"], "subsets": {name: {"windows": value["windows"], "rejected_windows": value["rejected_windows"],
                      "mae_ppm": {method: [score[str(horizon)]["mae_ppm"] for horizon in horizons] for method, score in value["methods"].items()}}
                      for name, value in result["test_subsets"].items() if name in {"all_test", "accepted_test", "rapid_rise_test"}}}, ensure_ascii=False), flush=True)


def threshold_lead(records, test, predictions, accepted, threshold):
    result = {"threshold_ppm": threshold, "definition": "Earliest accepted forecast reaching the threshold at either configured horizon while current concentration is below threshold; before first actual crossing. No controller, hysteresis or actuator simulation.", "methods": {}}
    for method, values in predictions.items():
        leads, rows, false_alarm_sessions, crossing_sessions = [], [], 0, 0
        for session_id in sorted({row[3][1] for row in records}):
            observed = [row for row in records if row[3][1] == session_id]
            if observed[0][1][0] >= threshold:
                continue
            crossings = [row[0] for row in observed if row[1][0] >= threshold]
            crossing = crossings[0] if crossings else None
            crossing_sessions += int(crossing is not None)
            start, end = observed[0][0], observed[-1][0]
            indices = [index for index, time in enumerate(test["times"]) if start <= datetime.fromisoformat(time) <= end]
            triggers = [datetime.fromisoformat(test["times"][index]) for index in indices
                        if accepted[index] and test["current"][index, 0] < threshold and np.max(values[index]) >= threshold
                        and (crossing is None or datetime.fromisoformat(test["times"][index]) < crossing)]
            lead = (crossing - triggers[0]).total_seconds() / 60 if crossing and triggers else None
            if lead is not None:
                leads.append(lead)
            false_alarm_sessions += int(crossing is None and bool(triggers))
            rows.append({"session_id": session_id, "first_actual_crossing": crossing.isoformat() if crossing else None,
                         "first_early_trigger": triggers[0].isoformat() if triggers else None, "lead_minutes": lead})
        result["methods"][method] = {"initially_below_threshold_sessions": len(rows), "actual_crossing_sessions": crossing_sessions,
                                     "early_trigger_sessions": len(leads), "no_early_trigger_crossing_sessions": crossing_sessions - len(leads),
                                     "false_alarm_sessions_without_crossing": false_alarm_sessions,
                                     "median_lead_minutes": float(np.median(leads)) if leads else None,
                                     "minimum_lead_minutes": min(leads) if leads else None, "maximum_lead_minutes": max(leads) if leads else None,
                                     "sessions": rows}
    return result


def plot_sessions(directory, test, predictions, session_lookup, scenario_lookup, settings):
    import matplotlib.pyplot as plt
    figure, axes = plt.subplots(2, 3, figsize=(13, 7), constrained_layout=True)
    horizon = settings["horizons_minutes"][-1]
    for axis, scenario in zip(axes.flat, settings["scenarios"], strict=True):
        session_id = next(session_lookup[time] for time in test["times"] if scenario_lookup[session_lookup[time]] == scenario)
        mask = np.asarray([session_lookup[time] == session_id for time in test["times"]])
        origins = [datetime.fromisoformat(time).hour * 60 + datetime.fromisoformat(time).minute for time in test["times"][mask]]
        axis.plot(origins, test["targets"][mask, -1], color="#263C48", label=f"synthetic target t+{horizon}")
        for method, color in [("persistence", "#A9ACA6"), ("ridge", "#E7AA8D"), ("mlp_int8", "#527D93")]:
            axis.plot(origins, predictions[method][mask, -1], color=color, linewidth=1, label=method)
        axis.set(title=f"{scenario}: {session_id}", xlabel="Forecast origin (simulation minute)", ylabel="CO2 (ppm)")
    axes[0, 0].legend(fontsize=7)
    figure.suptitle(f"Independent synthetic test sessions, +{horizon} min; first held-out session per scenario")
    figure.savefig(directory / "evaluation.png", dpi=150)
    plt.close(figure)


def write_report(directory, settings, report, result):
    horizons = [str(value) for value in settings["horizons_minutes"]]
    near, far = settings["horizons_minutes"]
    split_counts = " / ".join(str(result["splits"][name]["session_count"]) for name in ["train", "validation", "test"])
    lines = ["# 快速上升箱体候选模型", "", "本实验使用明确标记为 `synthetic_chamber` 的单区质量守恒合成数据，不是箱体、传感器或教室实测。",
             f"研究时距为 +{near}/+{far} 分钟；本候选独立保存，未替换运行固件中的模型。", "",
             "## 数据与模型", "", f"{settings['session_count']} 个独立合成场次，每场 {settings['session_minutes']} 分钟；{settings['session_count'] * settings['session_minutes']:,} 条一分钟记录。人工日期只用于隔离场次，不代表实际采集时间。",
             f"{split_counts} 场按完整人工日期先划分为训练 / 验证 / 测试，再分别构造窗口。没有同场次或重叠片段跨集合。",
             f"网络 {report['model']['architecture']}，{report['model']['parameters']} 参数；int8 {report['model']['int8_bytes']} B。",
             f"实际训练 {report['model']['epochs_executed']} 轮，最佳验证第 {report['model']['best_validation_epoch']} 轮；仅训练集标准化与 {report['model']['calibration_windows']} 条量化校准。", "",
             "生成方程、参数和许可见 [数据说明](../../../data/rapid-rise/SOURCE.md)，每场参数见 [场次配置](../../../data/rapid-rise/sessions.json)。", "",
             "## 冻结测试结果", "", f"快速上升子集定义：历史最近 {settings['rapid_rise_history_minutes']} 分钟增加至少 {settings['rapid_rise_change_ppm']} ppm，判定只读取输入历史，不读取未来目标。",
             "有效子集排除 int8 输入越界；全量含越界饱和诊断。各方法在同一子集比较。", ""]
    for label, title in [("all_test", "全量测试"), ("accepted_test", "量化有效测试"), ("rapid_rise_test", "快速上升测试"), ("accepted_rapid_rise_test", "量化有效快速上升")]:
        subset = result["test_subsets"][label]
        lines.extend([f"### {title}（{subset['windows']} 窗口；越界 {subset['rejected_windows']}）", "",
                      f"| 方法 | +{near} MAE / RMSE（ppm） | +{far} MAE / RMSE（ppm） |", "|---|---:|---:|"])
        for method, values in subset["methods"].items():
            lines.append(f"| {method} | {values[horizons[0]]['mae_ppm']:.2f} / {values[horizons[0]]['rmse_ppm']:.2f} | {values[horizons[1]]['mae_ppm']:.2f} / {values[horizons[1]]['rmse_ppm']:.2f} |")
        lines.append("")
    quantization = report["results"]["test"]["quantization"]
    lines.extend([f"场次内应预测分母为 {result['splits']['test']['candidate_origins']}；量化有效比例 {quantization['online_availability']:.2%}，输入越界 {quantization['saturated_windows']} / {quantization['window_count']}。场次间无记录的时间不计入分母。",
                  f"int8 / float 平均绝对差 {quantization['mean_absolute_difference_ppm']} ppm，最大绝对差 {quantization['maximum_absolute_difference_ppm']} ppm。",
                  "逐场误差、六种工况与工作浓度范围子集见 rapid_rise_metrics.json；阈值识别与完整量化诊断见 metrics.json。", "",
                  "![冻结独立测试](evaluation.png)", "", "![训练与验证](training.png)", "", "## 结论与边界", ""])
    comparison = result["test_subsets"]["accepted_test"]["methods"]
    for baseline in ["persistence", "ridge"]:
        better = [comparison["mlp_int8"][horizon]["mae_ppm"] < comparison[baseline][horizon]["mae_ppm"] for horizon in horizons]
        relation = "均低于" if all(better) else "均未低于" if not any(better) else "部分低于"
        lines.append(f"int8 模型在两个时距的有效测试 MAE {relation} {baseline} 基线；结论只适用于此次合成分布。")
    lead = result["threshold_lead"]["methods"]["mlp_int8"]
    lines.extend(["", f"仅作冻结序列阈值诊断：初始低于{report['experimental_threshold_ppm']} ppm的 {lead['initially_below_threshold_sessions']} 场中，{lead['actual_crossing_sessions']} 场后来达到阈值；int8在 {lead['early_trigger_sessions']} 场给出提前阈值信号，已提前场次的中位提前量 {lead['median_lead_minutes']} 分钟。未跨阈值场次误报 {lead['false_alarm_sessions_without_crossing']} 场。",
                  "此统计使用两个时距中任一达到阈值的首次有效预测，未模拟回差、最短启停、风扇或干预后的浓度；只能说明合成曲线上出现提前信号，不能证明策略优势。详细基线与各场时刻见 rapid_rise_metrics.json 的 threshold_lead。"])
    lines.extend(["", "泄漏、有效排风率、释放流量、充分混合、噪声和一阶传感器响应都是建模假设，未由真实箱体辨识。温湿度独立模拟，不替代化学释放的实际热湿耦合。",
                  "模型只接收配置中的历史通道；未来投料、开孔、排风动作未作为输入，突变无法从相同历史中唯一推断。这轮没有闭环控制实验；标签是外部预设工况下生成的未来传感器读数，不是模型预测或未通风反事实。",
                  "误差包含合成未来观测噪声，却不覆盖真实传感器校准误差、混合不充分、安装位置和真实化学反应重复性。不能据此宣称真实箱体预测准确、优于阈值控制、节能或教室通风改善。",
                  "候选部署资源仅生成在 model/artifacts/rapid-rise；未测新模型板端时延、内存或长时稳定性。需真实箱体场次重新评估，并统一固件界面/记录时距及重新验板后，才能部署。", ""])
    (directory / "REPORT.md").write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="生成明确标记的箱体合成数据并独立训练 +5/+15 分钟候选模型")
    parser.add_argument("--config", type=Path, default=ROOT / "model/rapid_rise_config.json")
    parser.add_argument("--prepare-only", action="store_true", help="仅生成数据和办公室快速上升统计")
    parser.add_argument("--evaluate-only", action="store_true", help="复用现有独立候选模型，仅补充场次评估")
    arguments = parser.parse_args()
    settings, configuration = load_experiment(arguments.config)
    path = ROOT / settings["data_directory"] / "environment.csv"
    if not arguments.evaluate_only:
        path = generate(settings, configuration)
        screen_uci(settings, configuration)
    if not arguments.prepare_only:
        if not arguments.evaluate_only:
            from train import run
            run(configuration, source=path)
        evaluate(settings, configuration, path)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    try:
        main()
    except (ValueError, OSError, RuntimeError) as error:
        print(f"箱体实验失败：{error}", file=sys.stderr)
        raise SystemExit(1) from error
