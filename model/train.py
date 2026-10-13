"""负责小型全连接网络训练、基线评估、int8 导出及结果归档。"""
import argparse
import csv
import hashlib
import json
import os
import platform
import sys
from pathlib import Path

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
os.environ.setdefault("TF_ENABLE_ONEDNN_OPTS", "0")

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import tensorflow as tf

from data import FEATURES, ROOT, load_config, make_windows, model_feature_indices, prepare_uci, read_records, split_dates
from runtime import concentrations, infer, interpreter_for, normalize


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def metrics(actual, predicted, horizons, threshold):
    result = {}
    for column, horizon in enumerate(horizons):
        error = predicted[:, column] - actual[:, column]
        positive = actual[:, column] >= threshold
        detected = predicted[:, column] >= threshold
        true_positive = int((positive & detected).sum())
        false_positive = int((~positive & detected).sum())
        false_negative = int((positive & ~detected).sum())
        result[str(horizon)] = {
            "mae_ppm": float(np.mean(np.abs(error))), "rmse_ppm": float(np.sqrt(np.mean(error ** 2))),
            "bias_ppm": float(np.mean(error)), "p95_absolute_error_ppm": float(np.percentile(np.abs(error), 95)),
            "threshold": {"true_positive": true_positive, "false_positive": false_positive,
                          "false_negative": false_negative, "positive_targets": int(positive.sum()),
                          "precision": true_positive / (true_positive + false_positive) if true_positive + false_positive else None,
                          "recall": true_positive / (true_positive + false_negative) if true_positive + false_negative else None}}
    return result


def train_network(inputs, targets, validation_inputs, validation_targets, configuration):
    settings = configuration["network"]
    tf.keras.utils.set_random_seed(settings["seed"])
    tf.config.threading.set_intra_op_parallelism_threads(4)
    tf.config.threading.set_inter_op_parallelism_threads(2)
    tf.config.experimental.enable_op_determinism()
    regularizer = tf.keras.regularizers.L2(settings["weight_decay"])
    model = tf.keras.Sequential([
        tf.keras.layers.Input(shape=(inputs.shape[1],)),
        *[tf.keras.layers.Dense(size, activation="relu", kernel_regularizer=regularizer)
          for size in settings["hidden_units"]],
        tf.keras.layers.Dense(targets.shape[1])], name="co2_forecast")
    model.compile(optimizer=tf.keras.optimizers.Adam(settings["learning_rate"]), loss="mse")
    history = model.fit(inputs, targets, validation_data=(validation_inputs, validation_targets),
                        epochs=settings["epochs"], batch_size=settings["batch_size"], shuffle=False, verbose=0,
                        callbacks=[tf.keras.callbacks.EarlyStopping(monitor="val_loss", patience=settings["patience"],
                                   min_delta=settings["minimum_improvement"], restore_best_weights=True),
                                   tf.keras.callbacks.LambdaCallback(on_epoch_end=lambda epoch, logs:
                                   print(f"epoch={epoch + 1} train={logs['loss']:.5f} validation={logs['val_loss']:.5f}", flush=True)
                                   if epoch % 10 == 0 else None)])
    return model, history.history


def export_models(model, inputs, output, configuration, metadata):
    from tensorflow.python.framework.convert_to_constants import convert_variables_to_constants_v2
    # 明确静态单样本签名，避免板端出现动态形状或额外算子。
    @tf.function(input_signature=[tf.TensorSpec([1, inputs.shape[1]], tf.float32, name="history")])
    def serving(history):
        return {"forecast": model(history, training=False)}
    frozen = convert_variables_to_constants_v2(serving.get_concrete_function())
    floating = tf.lite.TFLiteConverter.from_concrete_functions([frozen]).convert()
    (output / "co2_float.tflite").write_bytes(floating)
    rng = np.random.default_rng(configuration["network"]["seed"])
    count = min(configuration["calibration_samples"], len(inputs))
    indices = np.unique(np.concatenate([rng.choice(len(inputs), count, replace=False),
                                       np.argmin(inputs, axis=0), np.argmax(inputs, axis=0)]))
    converter = tf.lite.TFLiteConverter.from_concrete_functions([frozen])
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    # 范围锚点只用于量化覆盖，不作为训练数据或预测标签。
    anchors = []
    bounds = configuration.get("quantization_bounds", {})
    if bounds:
        mean = np.asarray(metadata["feature_mean"], np.float32)
        scale = np.asarray(metadata["feature_scale"], np.float32)
        width = configuration["window_minutes"]
        for name, (low, high) in bounds.items():
            channel = configuration["features"].index(name)
            for values in (np.full(width, low), np.full(width, high),
                           np.linspace(low, high, width), np.linspace(high, low, width)):
                history = np.tile(mean, (width, 1))
                history[:, channel] = values
                anchors.append(((history - mean) / scale).reshape(1, -1).astype(np.float32))
    def representative_samples():
        for index in indices:
            yield [inputs[index:index + 1]]
        for anchor in anchors:
            yield [anchor]
    converter.representative_dataset = representative_samples
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    # 当前 Arduino TFLite Micro 全连接内核使用逐张量权重量化。
    converter._experimental_disable_per_channel_quantization_for_dense_layers = True
    integer = converter.convert()
    (output / "co2_int8.tflite").write_bytes(integer)
    model.save(output / "co2.keras")
    return indices


def write_firmware(output, configuration, metadata, test, predicted, saturation, records):
    directory = ROOT / configuration["firmware"]["assets_directory"]
    directory.mkdir(parents=True, exist_ok=True)
    available = np.flatnonzero(~saturation)
    if len(available) < 8:
        raise ValueError("没有足够的量化有效样本验证板端")
    indices = available[np.linspace(0, len(available) - 1, 8, dtype=int)]
    data = (output / "co2_int8.tflite").read_bytes()
    rows = [", ".join(f"0x{value:02x}" for value in data[index:index + 16]) for index in range(0, len(data), 16)]
    def floats(values):
        return ", ".join(f"{float(value):.9e}f" for value in np.asarray(values).ravel())
    firmware = configuration["firmware"]
    generated = ["#pragma once", "#include <stdint.h>", "// 此文件由 model/train.py 生成，模型、预处理和回放样本保持同版。",
                 "namespace ModelAssets {", "alignas(16) const unsigned char model[] = {", ",\n".join(rows), "};",
                 f"constexpr unsigned windowMinutes = {configuration['window_minutes']};",
                 f"constexpr unsigned nearMinutes = {configuration['horizons_minutes'][0]};",
                 f"constexpr unsigned farMinutes = {configuration['horizons_minutes'][1]};",
                 f"constexpr char modelSha256[] = \"{metadata['int8_sha256']}\";",
                 f"constexpr unsigned featureCount = {len(configuration['features'])};",
                 f"constexpr unsigned inputSize = {configuration['window_minutes'] * len(configuration['features'])};",
                 f"constexpr unsigned tensorArenaBytes = {firmware['tensor_arena_bytes']};",
                 f"constexpr unsigned serialBaud = {firmware['serial_baud']};",
                 f"constexpr unsigned replayIntervalMilliseconds = {firmware['replay_interval_ms']};",
                 f"constexpr float tolerancePpm = {firmware['self_test_tolerance_ppm']}f;",
                 f"constexpr float featureMean[] = {{{floats(metadata['feature_mean'])}}};",
                 f"constexpr float featureScale[] = {{{floats(metadata['feature_scale'])}}};",
                 f"constexpr float targetMean[] = {{{floats(metadata['target_mean'])}}};",
                 f"constexpr float targetScale[] = {{{floats(metadata['target_scale'])}}};",
                 f"constexpr float featureMinimum[] = {{{floats([configuration['dataset']['bounds'][index][0] for index in model_feature_indices(configuration)])}}};",
                 f"constexpr float featureMaximum[] = {{{floats([configuration['dataset']['bounds'][index][1] for index in model_feature_indices(configuration)])}}};",
                 "constexpr unsigned replayCount = 8;",
                 "constexpr float replayHistory[replayCount][inputSize] = {",
                 ",\n".join("{" + floats(test["inputs"][index]) + "}" for index in indices), "};",
                 "constexpr float replayExpected[replayCount][2] = {",
                 ",\n".join("{" + floats(predicted[index]) + "}" for index in indices), "};", "}"]
    (directory / "model_assets.h").write_text("\n".join(generated) + "\n", encoding="utf-8")
    with (output / "example_history.csv").open("w", encoding="utf-8", newline="") as handle:
        from datetime import datetime, timedelta
        writer = csv.writer(handle)
        writer.writerow(["timestamp", *FEATURES, "valid"])
        end = datetime.fromisoformat(test["times"][indices[0]])
        raw_records = {record[0]: record for record in records}
        for offset in range(configuration["window_minutes"]):
            timestamp = end - timedelta(minutes=configuration["window_minutes"] - 1 - offset)
            record = raw_records[timestamp]
            writer.writerow([timestamp.isoformat(), *record[1], int(record[2])])
    return indices.tolist()


def plots(output, test, predictions, history, configuration):
    figure, axes = plt.subplots(2, 2, figsize=(13, 8), constrained_layout=True)
    for column, horizon in enumerate(configuration["horizons_minutes"]):
        times = np.asarray(test["times"], dtype="datetime64[m]")
        axes[0, column].plot(times, test["targets"][:, column], color="#263C48", linewidth=1, label="Observed target")
        for name, color in [("persistence", "#A9ACA6"), ("ridge", "#E7AA8D"), ("mlp_int8", "#527D93")]:
            axes[0, column].plot(times, predictions[name][:, column], color=color, linewidth=0.8, alpha=0.85, label=name)
        axes[0, column].set(title=f"Held-out test set: +{horizon} min", ylabel="CO2 (ppm)")
        axes[0, column].legend(fontsize=8)
        axes[0, column].tick_params(axis="x", rotation=20)
        names = list(predictions)
        errors = [np.mean(np.abs(predictions[name][:, column] - test["targets"][:, column])) for name in names]
        axes[1, column].bar(names, errors, color=["#A9ACA6", "#E7AA8D", "#A65F47", "#527D93"])
        axes[1, column].set(title=f"+{horizon} min absolute error", ylabel="MAE (ppm)")
        axes[1, column].tick_params(axis="x", rotation=15)
    figure.savefig(output / "evaluation.png", dpi=160)
    plt.close(figure)
    figure, axis = plt.subplots(figsize=(8, 4), constrained_layout=True)
    axis.plot(history["loss"], label="train")
    axis.plot(history["val_loss"], label="validation")
    axis.set(xlabel="Epoch", ylabel="Normalized MSE + regularization", title="Training history (no test data)")
    axis.legend()
    figure.savefig(output / "training.png", dpi=160)
    plt.close(figure)


def run(configuration, source=None, export_only=False, allow_replay=False, requantize=False):
    path = Path(source).resolve() if source else prepare_uci(configuration)
    records, audit = read_records(path, configuration, allow_replay=allow_replay)
    split = split_dates(records, configuration)
    datasets = {name: make_windows(rows, configuration) for name, rows in split.items()}
    training, validation, test = [datasets[name] for name in ("train", "validation", "test")]
    feature_indices = model_feature_indices(configuration)
    valid_rows = np.stack([row[1][list(feature_indices)] for row in split["train"] if row[2]])
    delta = training["targets"] - training["current"]
    metadata = {"version": "1.3.0", "features": list(configuration["features"]),
                "raw_features": list(FEATURES),
                "ignored_features": [name for name in FEATURES if name not in configuration["features"]],
                "feature_indices": list(feature_indices), "flatten_order": "oldest_to_newest_interleaved",
                "window_minutes": configuration["window_minutes"], "horizons_minutes": configuration["horizons_minutes"],
                "feature_mean": valid_rows.mean(axis=0).tolist(), "feature_scale": np.maximum(valid_rows.std(axis=0), 1e-6).tolist(),
                "target_mean": delta.mean(axis=0).tolist(), "target_scale": np.maximum(delta.std(axis=0), 1e-6).tolist(),
                "source_kind": "synthetic_chamber" if audit.get("sources") == ["synthetic_chamber"] else
                    "public_office_replay" if "public_office_replay" in audit.get("sources", []) else
                    "user_measurements" if source else "public_office_measurements",
                "configuration": configuration}
    inputs = {name: normalize(dataset["inputs"], metadata) for name, dataset in datasets.items()}
    targets = (delta - np.asarray(metadata["target_mean"])) / np.asarray(metadata["target_scale"])
    validation_targets = ((validation["targets"] - validation["current"] - np.asarray(metadata["target_mean"])) /
                          np.asarray(metadata["target_scale"]))
    output = ROOT / configuration["output"]
    output.mkdir(parents=True, exist_ok=True)
    reuse_weights = export_only or requantize
    if reuse_weights:
        previous_metadata = json.loads((output / "metadata.json").read_text(encoding="utf-8"))
        previous_audit = json.loads((output / "data_audit.json").read_text(encoding="utf-8"))
        previous_configuration = dict(previous_metadata["configuration"])
        requested_configuration = dict(configuration)
        if requantize:
            previous_configuration.pop("quantization_bounds", None)
            requested_configuration.pop("quantization_bounds", None)
        if previous_configuration != requested_configuration or previous_audit["csv_sha256"] != hashlib.sha256(path.read_bytes()).hexdigest():
            raise ValueError("训练配置或数据已变化，不能复用旧权重")
    write_json(output / "config.json", configuration)
    write_json(output / "data_audit.json", {"source": str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path),
               "csv_sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "cleaning": audit,
               "splits": {name: dataset["statistics"] for name, dataset in datasets.items()},
               "policy": "split complete dates first; reject incomplete/invalid/discontinuous spans; training-only normalization"})
    for name, dataset in datasets.items():
        print(name, dataset["statistics"], flush=True)
    if reuse_weights:
        model = tf.keras.models.load_model(output / "co2.keras", compile=False)
        history = json.loads((output / "history.json").read_text(encoding="utf-8"))
    else:
        model, history = train_network(inputs["train"], targets.astype(np.float32), inputs["validation"],
                                       validation_targets.astype(np.float32), configuration)
    calibration = export_models(model, inputs["train"], output, configuration, metadata)
    floating = interpreter_for(output / "co2_float.tflite")
    integer = interpreter_for(output / "co2_int8.tflite")
    operations = [item["op_name"] for item in integer._get_ops_details() if item["op_name"] != "DELEGATE"]
    if operations != ["FULLY_CONNECTED"] * 3:
        raise ValueError(f"模型算子不符合板端三层全连接要求：{operations}")
    if any(item["dtype"] not in (np.int8, np.int32) for item in integer.get_tensor_details()):
        raise ValueError("模型包含非整数张量")
    input_tensor, output_tensor = integer.get_input_details()[0], integer.get_output_details()[0]
    metadata.update({"parameters": model.count_params(), "operations": operations,
                     "input_quantization": list(input_tensor["quantization"]),
                     "output_quantization": list(output_tensor["quantization"]),
                     "int8_sha256": hashlib.sha256((output / "co2_int8.tflite").read_bytes()).hexdigest(),
                     "float_sha256": hashlib.sha256((output / "co2_float.tflite").read_bytes()).hexdigest(),
                     "training_feature_min": valid_rows.min(axis=0).tolist(), "training_feature_max": valid_rows.max(axis=0).tolist()})
    write_json(output / "metadata.json", metadata)
    write_json(output / "history.json", history)
    # 岭回归预测增量；截距不惩罚，拟合只使用训练集。
    design = np.column_stack([inputs["train"], np.ones(len(inputs["train"]))]).astype(np.float64)
    penalty = np.eye(design.shape[1]) * configuration["ridge_penalty"]
    penalty[-1, -1] = 0
    coefficients = np.linalg.solve(design.T @ design + penalty, design.T @ delta)
    report = {"model": {"architecture": [inputs['train'].shape[1], *configuration['network']['hidden_units'], 2],
                        "parameters": model.count_params(), "epochs_executed": len(history["loss"]),
                        "best_validation_epoch": int(np.argmin(history["val_loss"]) + 1),
                        "float_bytes": (output / "co2_float.tflite").stat().st_size,
                        "int8_bytes": (output / "co2_int8.tflite").stat().st_size,
                        "operations": operations, "calibration_windows": len(calibration),
                        "calibration_source": "train_only_with_range_anchors" if configuration.get("quantization_bounds") else "train_only",
                        "calibration_range_anchors": 4 * len(configuration.get("quantization_bounds", {}))},
              "environment": {"python": platform.python_version(), "tensorflow": tf.__version__, "numpy": np.__version__},
              "experimental_threshold_ppm": configuration["experimental_threshold_ppm"], "results": {}}
    test_predictions = None
    for name in ("validation", "test"):
        dataset = datasets[name]
        float_outputs, _ = infer(floating, inputs[name])
        integer_outputs, saturation = infer(integer, inputs[name])
        predictions = {"persistence": np.repeat(dataset["current"], 2, axis=1),
                       "ridge": dataset["current"] + np.column_stack([inputs[name], np.ones(len(inputs[name]))]) @ coefficients,
                       "mlp_float": concentrations(float_outputs, dataset["current"], metadata),
                       "mlp_int8": concentrations(integer_outputs, dataset["current"], metadata)}
        keras_values = concentrations(model(inputs[name], training=False).numpy(), dataset["current"], metadata)
        conversion_error = float(np.max(np.abs(keras_values - predictions["mlp_float"])))
        if conversion_error > 0.1:
            raise ValueError(f"浮点 TFLite 转换与 Keras 不一致：{conversion_error} ppm")
        report["results"][name] = {key: metrics(dataset["targets"], value, configuration["horizons_minutes"],
                                  configuration["experimental_threshold_ppm"]) for key, value in predictions.items()}
        difference = predictions["mlp_int8"] - predictions["mlp_float"]
        report["results"][name]["quantization"] = {
            "mean_absolute_difference_ppm": np.abs(difference).mean(axis=0).tolist(),
            "maximum_absolute_difference_ppm": np.abs(difference).max(axis=0).tolist(),
            "saturated_windows": int(saturation.sum()), "window_count": len(saturation),
            "keras_tflite_max_difference_ppm": conversion_error,
            "accepted_windows": int((~saturation).sum()),
            "online_availability": int((~saturation).sum()) / dataset["statistics"]["candidate_origins"]}
        report["results"][name]["accepted_window_comparison"] = {
            key: metrics(dataset["targets"][~saturation], value[~saturation], configuration["horizons_minutes"],
                         configuration["experimental_threshold_ppm"]) for key, value in predictions.items()}
        if name == "test":
            test_predictions = predictions
            report["board_replay_test_indices"] = write_firmware(output, configuration, metadata, test, predictions["mlp_int8"], saturation, records)
            with (output / "test_predictions.csv").open("w", encoding="utf-8", newline="") as handle:
                writer = csv.writer(handle)
                writer.writerow(["origin_timestamp", "current_ppm", *[f"actual_{horizon}" for horizon in configuration["horizons_minutes"]],
                                 *[f"{key}_{horizon}" for key in predictions for horizon in configuration["horizons_minutes"]], "input_saturated"])
                for index in range(len(dataset["times"])):
                    writer.writerow([dataset["times"][index], float(dataset["current"][index, 0]), *dataset["targets"][index],
                                     *[item for value in predictions.values() for item in value[index]], int(saturation[index])])
    plots(output, test, test_predictions, history, configuration)
    write_json(output / "metrics.json", report)
    summary = ["# CO₂ 小型全连接模型实验", "", "公开办公室回放日志仅用于流程检查，不能作为教室新实测。" if metadata["source_kind"] == "public_office_replay" else
               "质量守恒合成箱体数据仅用于流程和方法可行性检查，不是传感器或教室实测。" if metadata["source_kind"] == "synthetic_chamber" else
               "公开办公室实测数据用于完整流程验证；不能替代本教室泛化评估。" if not source else "用户数据实验，采集条件以源数据记录为准。", "",
               f"输入：连续 {configuration['window_minutes']} 分钟 CO₂ 单变量；原始温湿度字段保留但不进入模型。", "",
               f"网络：{report['model']['architecture']}，{model.count_params()} 参数；int8 文件 {report['model']['int8_bytes']} 字节。", "",
               f"| 方法 | +{configuration['horizons_minutes'][0]} min MAE / RMSE (ppm) | +{configuration['horizons_minutes'][1]} min MAE / RMSE (ppm) |", "|---|---|---|"]
    horizons = [str(value) for value in configuration["horizons_minutes"]]
    for name, value in report["results"]["test"].items():
        if name in {"quantization", "accepted_window_comparison"}:
            continue
        summary.append(f"| {name} | {value[horizons[0]]['mae_ppm']:.2f} / {value[horizons[0]]['rmse_ppm']:.2f} | {value[horizons[1]]['mae_ppm']:.2f} / {value[horizons[1]]['rmse_ppm']:.2f} |")
    improvement = all(report['results']['test']['mlp_int8'][horizon]['mae_ppm'] <
                      report['results']['test']['persistence'][horizon]['mae_ppm'] for horizon in horizons)
    summary.extend(["", "上表为全部完整测试窗口的离线结果，包含显式标记的输入饱和诊断样本。", "",
                    "两个时距的 MLP MAE 均低于持续性基线，泛化结论仍限于本次测试数据。" if improvement else
                    "当前 MLP 未在两个时距均优于持续性基线；这是流程和部署初版，不能宣称普遍预测优势。", "",
                    f"在线有效预测比例：{report['results']['test']['quantization']['online_availability']:.2%}，输入越界窗口拒绝预测。",
                    "同一有效子集的基线和模型比较见 metrics.json 的 accepted_window_comparison。", "",
                    f"量化校准：{len(calibration)} 条训练窗口；另加 {report['model']['calibration_range_anchors']} 条配置范围锚点（仅校准，不训练、不作实测标签）。",
                    f"配置量化范围：{configuration.get('quantization_bounds', '训练数据范围')}；超出训练分布的预测准确度尚未验证。", "",
                    "完整数据划分及清洗记录见 data_audit.json，阈值识别与量化差异见 metrics.json。", "", "![独立测试评估](evaluation.png)", "", "![训练过程](training.png)", "",
                    "int8 输入越界会在离线评估中显式计数；在线入口拒绝越界，不能将饱和预测当作有效结果。",
                    "该模型仍需本教室至少两周实测重新训练。板端资源和时延只可引用 SHA256 与本模型一致的实测报告；历史报告不能沿用到新模型。"])
    (output / "REPORT.md").write_text("\n".join(summary) + "\n", encoding="utf-8")
    print(json.dumps(report["model"], ensure_ascii=False), flush=True)
    print(json.dumps({name: {horizon: values[horizon]["mae_ppm"] for horizon in horizons}
                      for name, values in report["results"]["test"]["accepted_window_comparison"].items()}, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description="训练 CO₂ 小型全连接模型并导出 int8 部署包")
    parser.add_argument("--config", type=Path, default=ROOT / "model/config.json")
    parser.add_argument("--csv", type=Path, help="使用自采 CSV；不指定则获取公开 UCI 数据")
    parser.add_argument("--export-only", action="store_true", help="数据与配置不变时复用已训练权重，重新量化和评估")
    parser.add_argument("--requantize", action="store_true", help="仅允许改变量化范围，复用权重重新量化和评估")
    parser.add_argument("--allow-replay", action="store_true", help="显式允许单一回放来源，仅用于流程检查，不能冒充教室实测")
    arguments = parser.parse_args()
    try:
        run(load_config(arguments.config), arguments.csv, arguments.export_only, arguments.allow_replay, arguments.requantize)
    except (ValueError, OSError, RuntimeError) as error:
        print(f"训练失败：{error}", file=sys.stderr)
        raise SystemExit(1) from error
