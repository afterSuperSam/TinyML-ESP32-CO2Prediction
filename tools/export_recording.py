"""导出观测、事件与预测快照为 CSV；不删除板上记录。"""
import argparse
import csv
import json
import math
import sys
import tempfile
import time
from datetime import datetime, timedelta
from pathlib import Path

import serial

RECORD_FIELDS = ("seq", "uptime_ms", "sample_minute", "timestamp", "collection_timestamp", "clock_valid",
                 "co2_ppm", "temperature_c", "humidity_pct", "valid", "error_code", "prediction_status",
                 "prediction_15_ppm", "prediction_30_ppm")
EVENT_FIELDS = ("record_type", "detail")
FORECAST_FIELDS = ("seq", "forecast_issue_uptime_ms", "forecast_issue_minute", "forecast_issue_timestamp",
                   "forecast_clock_valid", "forecast_target_5_minute", "forecast_target_15_minute",
                   "forecast_5_ppm", "forecast_15_ppm", "forecast_baseline_5_ppm", "forecast_baseline_15_ppm",
                   "forecast_fan_5_ppm", "forecast_fan_15_ppm", "forecast_status", "forecast_model_sha256",
                   "forecast_detail")
CSV_FIELDS = ("timestamp", "co2_ppm", "temperature_c", "humidity_pct", "valid", "device_id", "session_id",
              "source", "seq", "uptime_ms", "sample_minute", "collection_timestamp", "clock_valid", "error_code",
              "prediction_status", "prediction_5_ppm", "prediction_15_ppm", "prediction_30_ppm", "model_sha256", "session_started_uptime_ms", "environment", *EVENT_FIELDS,
              *FORECAST_FIELDS[1:])
ERROR_MESSAGES = {"storage_unavailable": "存储未初始化或挂载失败", "already_initialized": "存储已挂载，无需初始化",
                  "initialize_failed": "初始化存储失败", "storage_full": "存储空间不足", "write_failed": "写入失败",
                  "usb_export_timeout": "板端 USB 导出发送停滞，导出已中止",
                  "record_unreadable": "记录无法读取", "metadata_unreadable": "场次信息无法读取", "clock_required": "场次未完成 USB 校时"}
MAX_SERIAL_LINE_BYTES = 4096


def read_export(lines, writer, selected_session=None):
    started = False
    session = None
    previous_sequence = 0
    received_records = written_records = 0
    sessions = {}
    writer.writeheader()
    for line in lines:
        if line.startswith("recording_row,"):
            if not started or session is None:
                raise ValueError("记录缺少导出开始或场次信息")
            values = next(csv.reader([line.removeprefix("recording_row,")]))
            fields = RECORD_FIELDS[:12] + ("prediction_5_ppm", "prediction_15_ppm") if session["format"] >= 4 else RECORD_FIELDS
            if session["format"] >= 3:
                fields += EVENT_FIELDS
            if len(values) != len(fields):
                raise ValueError("记录字段数不正确，导出可能不完整")
            row = dict(zip(fields, values, strict=True))
            row.setdefault("record_type", "legacy_observation")
            row.setdefault("detail", "")
            sequence, minute = int(row["seq"]), int(row["sample_minute"])
            if sequence != previous_sequence + 1 or minute < 0 or int(row["uptime_ms"]) < 0:
                raise ValueError("记录序号或运行时间异常")
            if row["valid"] not in {"0", "1"} or row["clock_valid"] not in {"0", "1"}:
                raise ValueError("有效状态或时钟状态异常")
            if row["error_code"] not in {"none", "unavailable", "read_failed", "invalid_value", "bus_failed", "crc_failed", "stale"}:
                raise ValueError("未知观测错误状态")
            measurements = [float(row[name]) for name in ("co2_ppm", "temperature_c", "humidity_pct")]
            if row["valid"] == "1" and (not all(map(math.isfinite, measurements)) or row["error_code"] != "none"):
                raise ValueError("有效观测含错误或非有限值")
            if not 0 <= int(row["prediction_status"]) <= (5 if session["format"] == 4 else 4):
                raise ValueError("未知预测状态")
            if bool(row["collection_timestamp"]) != (row["clock_valid"] == "1"):
                raise ValueError("采集时间与时钟状态不一致")
            if session["source"] == "public_office_replay":
                origin = datetime.fromisoformat(session["sample_origin"])
                row["timestamp"] = (origin + timedelta(minutes=minute)).isoformat()
            row.update({name: session[name] for name in ("device_id", "session_id", "source", "model_sha256")})
            row["session_started_uptime_ms"] = session["started_uptime_ms"]
            row["environment"] = session.get("environment", "unspecified")
            previous_sequence = sequence
            received_records += 1
            sessions[session["session_id"]] += 1
            if selected_session is None or session["session_id"] == selected_session:
                writer.writerow(row)
                written_records += 1
            continue
        if line.startswith("recording_forecast,"):
            if not started or session is None:
                raise ValueError("预测记录缺少导出开始或场次信息")
            values = next(csv.reader([line.removeprefix("recording_forecast,")]))
            if len(values) != len(FORECAST_FIELDS):
                raise ValueError("预测记录字段数不正确，导出可能不完整")
            forecast = dict(zip(FORECAST_FIELDS, values, strict=True))
            sequence = int(forecast["seq"])
            issue_minute = int(forecast["forecast_issue_minute"])
            if sequence != previous_sequence + 1 or issue_minute < 0 or int(forecast["forecast_issue_uptime_ms"]) < 0:
                raise ValueError("预测记录序号或运行时间异常")
            if forecast["forecast_clock_valid"] not in {"0", "1"}:
                raise ValueError("预测记录时钟状态异常")
            finite_forecast = all(math.isfinite(float(forecast[name])) for name in
                                  ("forecast_5_ppm", "forecast_15_ppm", "forecast_baseline_5_ppm", "forecast_baseline_15_ppm",
                                   "forecast_fan_5_ppm", "forecast_fan_15_ppm"))
            if not 0 <= int(forecast["forecast_status"]) <= (5 if session["format"] == 4 else 4):
                raise ValueError("未知预测状态")
            # 预测尚未就绪时，板端明确保存 NaN；成功状态必须有完整的两种情景。
            if not finite_forecast and int(forecast["forecast_status"]) == 0:
                raise ValueError("有效预测含非有限值")
            if int(forecast["forecast_target_5_minute"]) != issue_minute + 5 or int(forecast["forecast_target_15_minute"]) != issue_minute + 15:
                raise ValueError("预测目标分钟与预测窗口不一致")
            row = {field: "" for field in CSV_FIELDS}
            row.update({"timestamp": forecast["forecast_issue_timestamp"], "valid": "0", "device_id": session["device_id"],
                        "session_id": session["session_id"], "source": session["source"], "seq": forecast["seq"],
                        "uptime_ms": forecast["forecast_issue_uptime_ms"], "sample_minute": str(issue_minute),
                        "collection_timestamp": forecast["forecast_issue_timestamp"], "clock_valid": forecast["forecast_clock_valid"],
                        "error_code": "none", "prediction_status": forecast["forecast_status"],
                        "model_sha256": session["model_sha256"], "session_started_uptime_ms": session["started_uptime_ms"],
                        "environment": session.get("environment", "unspecified"), "record_type": "prediction_snapshot",
                        "detail": forecast["forecast_detail"], "forecast_issue_uptime_ms": forecast["forecast_issue_uptime_ms"],
                        "forecast_issue_minute": forecast["forecast_issue_minute"], "forecast_issue_timestamp": forecast["forecast_issue_timestamp"],
                        "forecast_clock_valid": forecast["forecast_clock_valid"], "forecast_target_5_minute": forecast["forecast_target_5_minute"],
                        "forecast_target_15_minute": forecast["forecast_target_15_minute"], "forecast_5_ppm": forecast["forecast_5_ppm"],
                        "forecast_15_ppm": forecast["forecast_15_ppm"], "forecast_baseline_5_ppm": forecast["forecast_baseline_5_ppm"],
                        "forecast_baseline_15_ppm": forecast["forecast_baseline_15_ppm"], "forecast_fan_5_ppm": forecast["forecast_fan_5_ppm"],
                        "forecast_fan_15_ppm": forecast["forecast_fan_15_ppm"], "forecast_status": forecast["forecast_status"],
                        "forecast_model_sha256": forecast["forecast_model_sha256"]})
            previous_sequence = sequence
            received_records += 1
            sessions[session["session_id"]] += 1
            if selected_session is None or session["session_id"] == selected_session:
                writer.writerow(row)
                written_records += 1
            continue
        if not line.startswith("{"):
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError as error:
            raise ValueError("串口 JSON 信息损坏") from error
        event = message.get("event")
        if event == "recording_error":
            code = message.get("code", "unknown")
            raise ValueError(ERROR_MESSAGES.get(code, f"板端存储错误：{code}"))
        if event == "recording_export_begin":
            if started or message.get("format") not in {1, 2, 3, 4}:
                raise ValueError("导出格式不兼容或重复开始")
            started = True
        elif event == "recording_session":
            if not started or message.get("format") not in {1, 2, 3, 4}:
                raise ValueError("场次格式不兼容")
            if not all(name in message for name in ("device_id", "session_id", "source", "sample_origin", "model_sha256", "started_uptime_ms")):
                raise ValueError("场次信息不完整")
            if message.get("format") >= 2 and not all(name in message for name in
                    ("clock_source", "timezone_offset_minutes", "clock_set_epoch_ms", "clock_set_uptime_ms")):
                raise ValueError("校时信息不完整")
            if message["source"] not in {"public_office_replay", "classroom_measurement"}:
                raise ValueError("未知数据来源")
            if message["session_id"] in sessions:
                raise ValueError("场次重复")
            session = message
            previous_sequence = 0
            sessions[session["session_id"]] = 0
        elif event == "recording_export_end":
            if not started or message.get("records") != received_records or message.get("sessions") != len(sessions):
                raise ValueError("导出数量不一致")
            if selected_session is not None and selected_session not in sessions:
                raise ValueError(f"没有场次 {selected_session}")
            return {"records": written_records, "sessions": sessions, "incomplete_rows": message["incomplete_rows"]}
    raise ValueError("未收到完整导出结束标记")


def serial_lines(connection, timeout, raw_handle=None):
    deadline = time.monotonic() + timeout
    pending = bytearray()
    while time.monotonic() < deadline:
        data = connection.read(min(max(connection.in_waiting, 1), 65536))
        if not data:
            continue
        if raw_handle is not None:
            raw_handle.write(data)
        pending.extend(data)
        while (newline := pending.find(b"\n")) >= 0:
            if newline > MAX_SERIAL_LINE_BYTES:
                raise ValueError("USB 数据行过长")
            line = pending[:newline].decode("utf-8").strip()
            del pending[:newline + 1]
            if line:
                if line.startswith(("recording_row,", "recording_forecast,")) or line.startswith('{"event":"recording_'):
                    deadline = time.monotonic() + timeout
                yield line
        if len(pending) > MAX_SERIAL_LINE_BYTES:
            raise ValueError("USB 数据行过长")
    if pending:
        raise ValueError("USB 数据行不完整")
    raise TimeoutError("等待 USB 导出超时")


def export(port, output, selected_session, timeout):
    # 独占新文件，失败时保留明确标记的临时 CSV 供排查。
    partial = output.with_name(output.name + ".partial")
    if output.exists():
        raise FileExistsError(f"文件已存在：{output}")
    with partial.open("x", encoding="utf-8-sig", newline="") as handle:
        # 先持续接收原始块，再离线校验，避免逐行 CSV 处理阻塞 USB 接收。
        raw_directory = Path(__file__).resolve().parents[1] / ".build"
        raw_directory.mkdir(exist_ok=True)
        with tempfile.NamedTemporaryFile(mode="wb", prefix="recording-export-", suffix=".raw.log",
                                         dir=raw_directory, delete=False) as raw_handle:
            raw_path = Path(raw_handle.name)
            receive_error = None
            try:
                with serial.Serial(port=None, baudrate=115200, timeout=1) as connection:
                    connection.port = port
                    connection.dtr = False
                    connection.rts = False
                    connection.open()
                    connection.reset_input_buffer()
                    connection.write(b"e")
                    for line in serial_lines(connection, timeout, raw_handle):
                        if line.startswith("{") and ('"recording_error"' in line or '"recording_export_end"' in line):
                            try:
                                message = json.loads(line)
                            except json.JSONDecodeError:
                                continue
                            if message.get("event") == "recording_error":
                                code = message.get("code", "unknown")
                                raise ValueError(ERROR_MESSAGES.get(code, f"板端存储错误：{code}"))
                            if message.get("event") == "recording_export_end":
                                break
            except (OSError, ValueError, serial.SerialException) as error:
                receive_error = error
        try:
            try:
                with raw_path.open(encoding="utf-8", newline="") as raw_handle:
                    result = read_export((line.strip() for line in raw_handle), csv.DictWriter(handle, CSV_FIELDS), selected_session)
            except (ValueError, KeyError, csv.Error) as error:
                if receive_error is not None:
                    raise receive_error from error
                raise
            if receive_error is not None:
                raise receive_error
        except (OSError, ValueError, KeyError, csv.Error, serial.SerialException):
            print(f"原始 USB 数据：{raw_path}", file=sys.stderr)
            raise
        result["raw_path"] = raw_path
    partial.rename(output)
    return result


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description="USB 导出板载记录；文件存在时拒绝覆盖")
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", type=Path, default=Path("co2-recording.csv"))
    parser.add_argument("--session", help="仅写入指定十位场次编号，如 0000000001")
    parser.add_argument("--timeout", type=float, default=20, help="无数据等待秒数")
    arguments = parser.parse_args()
    if arguments.timeout <= 0:
        parser.error("等待时间必须大于零")
    try:
        result = export(arguments.port, arguments.output, arguments.session, arguments.timeout)
        print(f"已导出 {result['records']} 条到 {arguments.output}；板上记录保留。")
        print(f"原始 USB 数据：{result['raw_path']}")
        print("场次：" + ", ".join(f"{identity}（{count}条）" for identity, count in result["sessions"].items()))
        if result["incomplete_rows"]:
            print(f"注意：跳过 {result['incomplete_rows']} 个断电留下的不完整行/场次。", file=sys.stderr)
    except (OSError, ValueError, KeyError, csv.Error, serial.SerialException) as error:
        print(f"导出失败：{error}；若有 .partial 文件请保留排查，勿用于训练。", file=sys.stderr)
        raise SystemExit(1) from error
