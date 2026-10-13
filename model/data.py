"""负责配置校验、可追溯数据清洗和无泄漏的时间窗口。"""
import csv
import hashlib
import io
import json
import math
import urllib.request
import zipfile
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
FEATURES = ("co2_ppm", "temperature_c", "humidity_pct")


def model_feature_indices(configuration):
    """Return the positions of the raw channels used by the deployed model."""
    names = configuration.get("features", list(FEATURES))
    if not names or any(name not in FEATURES for name in names) or len(set(names)) != len(names):
        raise ValueError("模型 features 必须是原始 CO₂、温度、湿度字段的非空子集")
    return tuple(FEATURES.index(name) for name in names)


def load_config(path=ROOT / "model/config.json"):
    configuration = json.loads(Path(path).read_text(encoding="utf-8"))
    expected = {"dataset", "features", "window_minutes", "horizons_minutes", "split_fractions",
                "network", "ridge_penalty", "calibration_samples",
                "experimental_threshold_ppm", "output", "firmware", "screen_replay"}
    if not expected.issubset(configuration) or set(configuration) - expected - {"quantization_bounds"}:
        raise ValueError("配置字段缺失或包含未知字段")
    dataset = configuration["dataset"]
    network = configuration["network"]
    firmware = configuration["firmware"]
    if set(dataset) != {"url", "sha256", "archive", "csv", "bounds", "timestamp_tolerance_seconds"}:
        raise ValueError("数据配置字段不正确")
    if set(network) != {"hidden_units", "seed", "epochs", "batch_size", "learning_rate",
                        "patience", "minimum_improvement", "weight_decay"}:
        raise ValueError("网络配置字段不正确")
    if set(firmware) != {"directory", "assets_directory", "tensor_arena_bytes", "serial_baud", "replay_interval_ms", "self_test_tolerance_ppm"}:
        raise ValueError("固件配置字段不正确")
    integers = [configuration["window_minutes"], configuration["calibration_samples"],
                *configuration["horizons_minutes"], *network["hidden_units"], network["epochs"],
                network["batch_size"], network["patience"], firmware["tensor_arena_bytes"],
                firmware["serial_baud"], firmware["replay_interval_ms"]]
    if any(type(value) is not int or value <= 0 for value in integers):
        raise ValueError("窗口、网络和固件整数参数必须为正整数")
    if configuration["window_minutes"] < 2 or len(configuration["horizons_minutes"]) != 2:
        raise ValueError("历史至少两条，必须配置两个预测时距")
    if sorted(set(configuration["horizons_minutes"])) != configuration["horizons_minutes"]:
        raise ValueError("预测时距必须严格递增")
    if len(network["hidden_units"]) != 2 or type(network["seed"]) is not int:
        raise ValueError("网络必须配置两个隐藏层和整数随机种子")
    fractions = configuration["split_fractions"]
    if len(fractions) != 3 or any(not 0 < value < 1 for value in fractions) or not math.isclose(sum(fractions), 1):
        raise ValueError("训练、验证、测试比例必须为三个正数且和为一")
    model_feature_indices(configuration)
    bounds = dataset["bounds"]
    if len(bounds) != 3 or any(len(bound) != 2 or not bound[0] < bound[1] for bound in bounds):
        raise ValueError("必须配置 CO₂、温度和湿度的有效范围")
    quantization = configuration.get("quantization_bounds", {})
    if not isinstance(quantization, dict) or set(quantization) - set(configuration["features"]):
        raise ValueError("量化范围只能配置当前模型输入特征")
    for name, bound in quantization.items():
        low, high = bounds[FEATURES.index(name)]
        if (not isinstance(bound, list) or len(bound) != 2 or
                any(type(value) not in (int, float) or not math.isfinite(value) for value in bound) or
                not low <= bound[0] < bound[1] <= high):
            raise ValueError(f"量化范围 {name} 必须在数据有效范围内严格递增")
    if not 0 <= dataset["timestamp_tolerance_seconds"] < 30:
        raise ValueError("时间对齐容差必须小于半分钟")
    if any(not math.isfinite(value) or value <= 0 for value in
           [network["learning_rate"], configuration["ridge_penalty"],
            configuration["experimental_threshold_ppm"], firmware["self_test_tolerance_ppm"]]):
        raise ValueError("学习率、正则项、阈值和验证容差必须为有限正数")
    if any(not math.isfinite(value) or value < 0 for value in
           [network["minimum_improvement"], network["weight_decay"]]):
        raise ValueError("训练改进阈值和权重正则项必须非负")
    replay = configuration["screen_replay"]
    if set(replay) != {"start", "minutes"} or type(replay["minutes"]) is not int or replay["minutes"] < 61:
        raise ValueError("屏幕回放必须配置起点和至少 61 条分钟记录")
    datetime.fromisoformat(replay["start"])
    for value in [dataset["archive"], dataset["csv"], configuration["output"], firmware["directory"], firmware["assets_directory"]]:
        resolved = (ROOT / value).resolve()
        if not resolved.is_relative_to(ROOT) or resolved == ROOT:
            raise ValueError("数据、模型和固件路径必须位于项目子目录")
    if not dataset["url"].startswith("https://") or len(dataset["sha256"]) != 64:
        raise ValueError("公开数据必须配置 HTTPS 地址和 SHA256")
    return configuration


def prepare_uci(configuration):
    dataset = configuration["dataset"]
    archive = ROOT / dataset["archive"]
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        raw = urllib.request.urlopen(dataset["url"], timeout=60).read()
        if hashlib.sha256(raw).hexdigest() != dataset["sha256"]:
            raise ValueError("下载数据校验失败，未写入原始文件")
        archive.write_bytes(raw)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != dataset["sha256"]:
        raise ValueError("原始数据校验失败，请检查数据版本")
    destination = ROOT / dataset["csv"]
    rows = []
    with zipfile.ZipFile(archive) as container:
        for name in sorted(container.namelist()):
            if not name.endswith(".txt"):
                continue
            reader = csv.reader(io.StringIO(container.read(name).decode("utf-8-sig")))
            header = next(reader)
            for row in reader:
                # UCI 每行多一个未在表头中声明的编号。
                values = dict(zip(header, row[1:], strict=True))
                rows.append((values["date"], values["CO2"], values["Temperature"], values["Humidity"], "1", name))
    with destination.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["timestamp", *FEATURES, "valid", "source_file"])
        writer.writerows(sorted(rows))
    return destination


def read_records(path, configuration, allow_replay=False):
    records = {}
    audit = {"raw_rows": 0, "invalid_rows": 0, "duplicate_rows": 0, "aligned_timestamps": 0}
    sources = set()
    tolerance = configuration["dataset"]["timestamp_tolerance_seconds"]
    with Path(path).open(encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        if not {"timestamp", *FEATURES}.issubset(reader.fieldnames or []):
            raise ValueError("CSV 缺少时间、CO₂、温度或湿度字段")
        for row in reader:
            audit["raw_rows"] += 1
            source = (row.get("source") or "").strip()
            if "source" in row:
                sources.add(source or "unspecified")
            if source == "public_office_replay" and not allow_replay:
                raise ValueError("CSV 含公开办公室回放，不能作为教室新实测；仅流程检查可显式传入 --allow-replay")
            if "public_office_replay" in sources and len(sources) > 1:
                raise ValueError("不能混合回放与其他来源，请按来源和场次分别整理")
            if source == "classroom_measurement" and (row.get("clock_valid") == "0" or not row["timestamp"]):
                raise ValueError("教室记录时钟未同步；请先核对真实采集时间，禁止按运行时长编造日期")
            try:
                original = datetime.fromisoformat(row["timestamp"])
                if original.tzinfo is not None:
                    raise ValueError("请统一为当地无时区时间，禁止混用时区")
                timestamp = (original + timedelta(seconds=30)).replace(second=0, microsecond=0)
                aligned = original != timestamp
                valid = abs((original - timestamp).total_seconds()) <= tolerance
                values = np.asarray([float(row[field]) for field in FEATURES], dtype=np.float32)
                valid = valid and row.get("valid", "1").strip() in {"1", "true", "True"}
                valid = valid and bool(np.isfinite(values).all())
                valid = valid and all(low <= value <= high for value, (low, high) in
                                      zip(values, configuration["dataset"]["bounds"], strict=True))
            except (ValueError, TypeError) as error:
                raise ValueError(f"CSV 第 {audit['raw_rows'] + 1} 行无法解析：{error}") from error
            audit["aligned_timestamps"] += int(aligned)
            audit["invalid_rows"] += int(not valid)
            identity = (row.get("device_id", "room"), row.get("session_id", "continuous"))
            if timestamp in records:
                audit["duplicate_rows"] += 1
                previous = records[timestamp]
                if not np.array_equal(previous[1], values, equal_nan=True) or previous[2:] != (valid, identity):
                    raise ValueError(f"同一分钟存在冲突记录：{timestamp}，请先按设备和实验场次整理")
                continue
            records[timestamp] = (timestamp, values, valid, identity)
    ordered = sorted(records.values(), key=lambda row: row[0])
    if not ordered:
        raise ValueError("数据文件为空")
    audit["unique_minutes"] = len(ordered)
    if sources:
        audit["sources"] = sorted(sources)
    audit["missing_minutes"] = int((ordered[-1][0] - ordered[0][0]).total_seconds() // 60 + 1 - len(ordered))
    return ordered, audit


def split_dates(records, configuration):
    dates = sorted({row[0].date() for row in records})
    training_count = int(len(dates) * configuration["split_fractions"][0])
    validation_count = int(len(dates) * configuration["split_fractions"][1])
    if min(training_count, validation_count, len(dates) - training_count - validation_count) < 1:
        raise ValueError("有效日期不足，无法按完整日期划分三个集合")
    groups = (dates[:training_count], dates[training_count:training_count + validation_count],
              dates[training_count + validation_count:])
    return {name: [row for row in records if row[0].date() in days]
            for name, days in zip(("train", "validation", "test"), groups, strict=True)}


def make_windows(records, configuration):
    width = configuration["window_minutes"]
    horizons = configuration["horizons_minutes"]
    inputs, targets, times, current = [], [], [], []
    candidates = max(0, int((records[-1][0] - records[0][0]).total_seconds() // 60) + 1 - width + 1 - max(horizons))
    feature_indices = model_feature_indices(configuration)
    for index in range(width - 1, len(records)):
        timestamp = records[index][0]
        if timestamp + timedelta(minutes=max(horizons)) > records[-1][0]:
            continue
        span = records[index - width + 1:index + max(horizons) + 1]
        if len(span) != width + max(horizons) or not all(row[2] for row in span):
            continue
        if any(row[3] != span[0][3] or row[0] != span[0][0] + timedelta(minutes=offset)
               for offset, row in enumerate(span)):
            continue
        inputs.append(np.stack([row[1][list(feature_indices)] for row in span[:width]]))
        targets.append([span[width - 1 + horizon][1][0] for horizon in horizons])
        times.append(timestamp.isoformat())
        current.append(records[index][1][0])
    if not inputs:
        raise ValueError("没有完整有效的历史和未来目标窗口")
    statistics = {"first_timestamp": records[0][0].isoformat(), "last_timestamp": records[-1][0].isoformat(),
                  "dates": sorted({str(row[0].date()) for row in records}), "rows": len(records),
                  "candidate_origins": candidates, "windows": len(inputs),
                  "availability": len(inputs) / candidates}
    return {"inputs": np.asarray(inputs, np.float32), "targets": np.asarray(targets, np.float32),
            "current": np.asarray(current, np.float32)[:, None], "times": np.asarray(times),
            "statistics": statistics}


def check_data_invariants(configuration):
    length = configuration["window_minutes"] + max(configuration["horizons_minutes"]) + 10
    start = datetime(2026, 1, 1)
    records = [(start + timedelta(minutes=index), np.asarray([500 + index, 25, 50], np.float32), True, ("a", "1"))
               for index in range(length)]
    windows = make_windows(records, configuration)
    assert windows["inputs"].shape[1:] == (configuration["window_minutes"], len(model_feature_indices(configuration)))
    assert np.all(windows["targets"] - windows["current"] == configuration["horizons_minutes"])
    removed = records[:25] + records[26:]
    try:
        make_windows(removed, configuration)
    except ValueError as error:
        assert "窗口" in str(error)
    else:
        raise AssertionError("错误地跨缺失构造了窗口")
    restarted = [(time, value, valid, ("a", "2" if index >= 25 else "1"))
                 for index, (time, value, valid, _) in enumerate(records)]
    try:
        make_windows(restarted, configuration)
    except ValueError as error:
        assert "窗口" in str(error)
    else:
        raise AssertionError("错误地跨重启构造了窗口")
