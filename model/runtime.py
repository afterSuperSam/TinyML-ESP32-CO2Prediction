"""负责电脑端与板端共用的输入规范和 TFLite 数值转换。"""
import json
from pathlib import Path

import numpy as np
import tensorflow as tf


def normalize(histories, metadata):
    return ((histories - np.asarray(metadata["feature_mean"], np.float32)) /
            np.asarray(metadata["feature_scale"], np.float32)).reshape(len(histories), -1).astype(np.float32)


def concentrations(outputs, current, metadata):
    return (current + outputs * np.asarray(metadata["target_scale"], np.float32) +
            np.asarray(metadata["target_mean"], np.float32))


def interpreter_for(path):
    interpreter = tf.lite.Interpreter(model_path=str(path), num_threads=1,
                                      experimental_op_resolver_type=tf.lite.experimental.OpResolverType.BUILTIN_REF)
    interpreter.allocate_tensors()
    return interpreter


def infer(interpreter, inputs):
    input_tensor = interpreter.get_input_details()[0]
    output_tensor = interpreter.get_output_details()[0]
    results, saturated = [], []
    quantized = input_tensor["dtype"] == np.int8
    for values in inputs:
        if quantized:
            scale, zero = input_tensor["quantization"]
            encoded = np.rint(values / scale + zero)
            saturated.append(bool(((encoded < -128) | (encoded > 127)).any()))
            values = np.clip(encoded, -128, 127).astype(np.int8)
        else:
            saturated.append(False)
        interpreter.set_tensor(input_tensor["index"], values.reshape(1, -1))
        interpreter.invoke()
        output = interpreter.get_tensor(output_tensor["index"])[0]
        if quantized:
            scale, zero = output_tensor["quantization"]
            output = (output.astype(np.float32) - zero) * scale
        results.append(output)
    return np.asarray(results, np.float32), np.asarray(saturated)


def load_bundle(directory):
    directory = Path(directory)
    metadata = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
    return metadata, interpreter_for(directory / "co2_int8.tflite")
