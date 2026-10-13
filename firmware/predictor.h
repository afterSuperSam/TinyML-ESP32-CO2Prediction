#pragma once
#include <math.h>
#include <Chirale_TensorFlowLite.h>
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "model_assets.h"

// 管理本地量化推理；调用方提供按分钟连续且有效的历史。
namespace Predictor {
alignas(16) inline uint8_t tensorArena[ModelAssets::tensorArenaBytes];
inline tflite::MicroInterpreter* interpreter = nullptr;
inline TfLiteTensor* input = nullptr;
inline TfLiteTensor* output = nullptr;

inline bool begin() {
    const auto* model = tflite::GetModel(ModelAssets::model);
    if (model->version() != TFLITE_SCHEMA_VERSION) return false;
    static tflite::MicroMutableOpResolver<1> resolver;
    if (resolver.AddFullyConnected() != kTfLiteOk) return false;
    static tflite::MicroInterpreter instance(model, resolver, tensorArena, sizeof(tensorArena));
    interpreter = &instance;
    if (interpreter->AllocateTensors() != kTfLiteOk) return false;
    input = interpreter->input(0);
    output = interpreter->output(0);
    return input->type == kTfLiteInt8 && output->type == kTfLiteInt8 &&
        input->bytes == ModelAssets::inputSize && output->bytes == 2 &&
        input->params.scale > 0 && output->params.scale > 0;
}

enum class Status { Success, NotReady, InvalidHistory, OutsideCalibration, InferenceFailed };
struct Result {
    Status status = Status::NotReady;
    float nearConcentration = NAN;
    float farConcentration = NAN;
    uint32_t durationMicroseconds = 0;
};

inline Result predict(const float* history, bool continuousAndValid) {
    Result result;
    if (!interpreter || !input || !output) return result;
    if (!continuousAndValid || !history) { result.status = Status::InvalidHistory; return result; }
    for (unsigned index = 0; index < ModelAssets::inputSize; ++index) {
        if (!isfinite(history[index])) { result.status = Status::InvalidHistory; return result; }
        const unsigned channel = index % ModelAssets::featureCount;
        if (history[index] < ModelAssets::featureMinimum[channel] || history[index] > ModelAssets::featureMaximum[channel]) {
            result.status = Status::InvalidHistory; return result;
        }
        const float normalized = (history[index] - ModelAssets::featureMean[channel]) / ModelAssets::featureScale[channel];
        const float encoded = nearbyintf(normalized / input->params.scale + input->params.zero_point);
        if (encoded < -128 || encoded > 127) { result.status = Status::OutsideCalibration; return result; }
        input->data.int8[index] = static_cast<int8_t>(encoded);
    }
    const uint32_t started = micros();
    if (interpreter->Invoke() != kTfLiteOk) { result.status = Status::InferenceFailed; return result; }
    result.durationMicroseconds = micros() - started;
    float predictions[2];
    for (unsigned column = 0; column < 2; ++column) {
        const float normalized = (output->data.int8[column] - output->params.zero_point) * output->params.scale;
        predictions[column] = history[ModelAssets::inputSize - ModelAssets::featureCount] + normalized * ModelAssets::targetScale[column] + ModelAssets::targetMean[column];
    }
    result.nearConcentration = predictions[0];
    result.farConcentration = predictions[1];
    result.status = Status::Success;
    return result;
}
}
