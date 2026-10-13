"""检查 Arduino 工程并编译、上传，直接使用 firmware 根目录源码。"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def prepare():
    source = ROOT / "firmware"
    if sorted(path.name for path in source.glob("*.ino")) != ["firmware.ino"]:
        raise ValueError("firmware 中必须只有 firmware.ino 一个程序入口")
    if any(path.is_dir() and any(path.iterdir()) for path in source.iterdir()):
        raise ValueError("firmware 源码应直接放在根目录")
    configuration = json.loads((source / "build.json").read_text(encoding="utf-8"))
    if (set(configuration) != {"fqbn", "jobs"} or not isinstance(configuration["fqbn"], str)
            or not configuration["fqbn"].startswith("esp32:esp32:esp32s3:")
            or type(configuration["jobs"]) is not int or not 1 <= configuration["jobs"] <= 32):
        raise ValueError("build.json 必须指定 ESP32-S3 fqbn 和 1–32 的 jobs")
    return source, configuration


def find_cli(requested):
    candidate = requested or shutil.which("arduino-cli")
    if not candidate:
        candidate = str(Path(os.environ.get("LOCALAPPDATA", "")) /
                        "Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe")
    if not Path(candidate).is_file():
        raise FileNotFoundError("未找到 arduino-cli，请安装 Arduino IDE 或使用 --cli 指定路径")
    return candidate


def main():
    parser = argparse.ArgumentParser(description="编译或上传 firmware/firmware.ino")
    parser.add_argument("--prepare-only", action="store_true", help="仅检查工程入口和构建配置")
    parser.add_argument("--upload", action="store_true", help="编译成功后上传")
    parser.add_argument("--port", help="上传串口，例如 COM4")
    parser.add_argument("--cli", help="arduino-cli 可执行文件路径")
    arguments = parser.parse_args()
    if arguments.upload and (not arguments.port or arguments.prepare_only):
        parser.error("上传必须指定 --port，且不能与 --prepare-only 一起使用")
    sketch, configuration = prepare()
    if arguments.prepare_only:
        print(f"Arduino 工程检查通过：{sketch / 'firmware.ino'}")
        return
    cli = find_cli(arguments.cli)
    output = ROOT / ".build" / "firmware"
    command = [cli, "compile", "--fqbn", configuration["fqbn"], "--jobs", str(configuration["jobs"]),
               "--build-path", str(output), str(sketch)]
    subprocess.run(command, cwd=ROOT, check=True)
    if arguments.upload:
        subprocess.run([cli, "upload", "--fqbn", configuration["fqbn"], "--port", arguments.port,
                        "--input-dir", str(output)], cwd=ROOT, check=True)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"固件操作失败：{error}", file=sys.stderr)
        raise SystemExit(1) from error
