#include "builder.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include "log_stream.hpp"

#include <numeric>
#include <functional>
#include <algorithm>

#include <unistd.h>

static auto LOG = spdlog::stdout_color_mt("MODEL_BUILDER");

namespace gallopwave
{

void ModelBuilder::getSdkVersion()
{
    int32_t result = 0;
    const char* sdkProp = "ro.build.version.sdk";
    char sdkVersion[PROP_VALUE_MAX];
    int length = __system_property_get(sdkProp, sdkVersion);
    if (length != 0) 
    {
        for (int i = 0; i < length; ++i)
        {
            int digit = sdkVersion[i] - '0';
            if (digit < 0 || digit > 9)
            {
                SLOG_INFO << "Non-numeric SDK version, assume it's higher than expected" << std::endl;
            }
            result = result * 10 + digit;
        }
        SLOG_INFO << "Android SDK version: " << result << std::endl;
    }
    else
    {
        SLOG_INFO << "Failed parsing Android SDK version" << std::endl;
    }
}


void ModelBuilder::getDevices()
{
    uint32_t deviceCount;
    CHECK_NNAPI_ERROR( ANeuralNetworks_getDeviceCount(&deviceCount) );
    SLOG_INFO << "NNAPI device list: " << std::endl;
    for (int i = 0; i < deviceCount; ++i)
    {
        ANeuralNetworksDevice *nnDevice;
        CHECK_NNAPI_ERROR( ANeuralNetworks_getDevice(i, &nnDevice) );
        const char *nnDeviceName;
        CHECK_NNAPI_ERROR( ANeuralNetworksDevice_getName(nnDevice, &nnDeviceName) );
        int64_t featureLevel;
        CHECK_NNAPI_ERROR( ANeuralNetworksDevice_getFeatureLevel(nnDevice, &featureLevel) );
        int32_t type;
        CHECK_NNAPI_ERROR( ANeuralNetworksDevice_getType(nnDevice, &type) );
        const char *nnVersion;
        CHECK_NNAPI_ERROR( ANeuralNetworksDevice_getVersion(nnDevice, &nnVersion) );

        SLOG_INFO << i << ": " << nnDeviceName << ',' << featureLevel << ',' << type << ',' << nnVersion << std::endl;
        devices.push_back(nnDevice);
    }
}

ModelBuilder::~ModelBuilder()
{
    for (auto it: outputOps)
    {
        munmap(it.data, it.sizeBytes);
        ANeuralNetworksMemory_free(it.nnMemPtr);
        close(it.fd);
    }
    ANeuralNetworksEvent_free(event);
    ANeuralNetworksExecution_free(execution);
    ANeuralNetworksCompilation_free(compilation);
    ANeuralNetworksModel_free(model);
}

ModelBuilder::ModelBuilder()
{
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_create(&model) );
    opIdx = 0;
}

void ModelBuilder::addTensor (std::string name,
                            std::vector<uint32_t> dims,
                            const void *srcbuffer,
                            float scale,
                            int32_t zeroPoint
                            )
{
    ANeuralNetworksOperandType operandType;
    operandType.type = ANEURALNETWORKS_TENSOR_FLOAT32; // TODO: Support only FP32 now
    operandType.dimensionCount = static_cast<uint32_t>(dims.size());
    operandType.dimensions = dims.data();
    operandType.scale = scale;
    operandType.zeroPoint = zeroPoint;
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_addOperand(model, &operandType) );
    operandIdxes[name] = opIdx;
    shapeIdxes[name] = dims;

    if (srcbuffer != nullptr)
    {
        size_t elementSize = 4; // TODO: Support only FP32 now
        const size_t bytes = elementSize * std::accumulate(dims.begin(), dims.end(), 1, std::multiplies<uint32_t>());
        CHECK_NNAPI_ERROR( ANeuralNetworksModel_setOperandValue(model, opIdx, srcbuffer, bytes) );
    }
    ++opIdx;
}

void ModelBuilder::conv2d (std::string name,
                        const std::string &input,
                        const std::string &weight,
                        const std::string &bias,
                        const int32_t padLeft,
                        const int32_t padRight,
                        const int32_t padTop,
                        const int32_t padBottom,
                        const int32_t strideX,
                        const int32_t strideY,
                        const FuseCode fusecode,
                        const bool isNCHW,
                        const int32_t dilationX,
                        const int32_t dilationY,
                        const std::string &output
                        )
{
    std::vector<uint32_t> parameterIdxes;

    const auto inputIdx = operandIdxes.at(input);
    const auto weightIdx = operandIdxes.at(weight);
    const auto biasIdx = operandIdxes.at(bias);
    parameterIdxes.push_back(inputIdx);
    parameterIdxes.push_back(weightIdx);
    parameterIdxes.push_back(biasIdx);


    ANeuralNetworksOperandType operandType;
    operandType.type = ANEURALNETWORKS_INT32;
    operandType.dimensionCount = 0;
    operandType.dimensions = nullptr;
    operandType.scale = 0.0f;
    operandType.zeroPoint = 0;
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_addOperand(model, &operandType));
    operandIdxes[name + "_padLeft"] = opIdx;
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_setOperandValue(model, opIdx, &padLeft, sizeof(padLeft)) );
    parameterIdxes.push_back(opIdx);
    ++opIdx;

    CHECK_NNAPI_ERROR( ANeuralNetworksModel_addOperand(model, &operandType) );
    operandIdxes[name + "_padRight"] = opIdx;
    CHECK_NNAPI_ERROR ( ANeuralNetworksModel_setOperandValue(model, opIdx, &padRight, sizeof(padRight)) );
    parameterIdxes.push_back(opIdx);
    ++opIdx;

    CHECK_NNAPI_ERROR( ANeuralNetworksModel_addOperand(model, &operandType) );
    operandIdxes[name + "_padTop"] = opIdx;
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_setOperandValue(model, opIdx, &padTop, sizeof(padTop)) );
    parameterIdxes.push_back(opIdx);
    ++opIdx;

    CHECK_NNAPI_ERROR( ANeuralNetworksModel_addOperand(model, &operandType) );
    operandIdxes[name + "_padBottom"] = opIdx;
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_setOperandValue(model, opIdx, &padBottom, sizeof(padBottom)) );
    parameterIdxes.push_back(opIdx);
    ++opIdx;

    CHECK_NNAPI_ERROR( ANeuralNetworksModel_addOperand(model, &operandType) );
    operandIdxes[name + "_strideX"] = opIdx;
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_setOperandValue(model, opIdx, &strideX, sizeof(strideX)) );
    parameterIdxes.push_back(opIdx);
    ++opIdx;

    CHECK_NNAPI_ERROR( ANeuralNetworksModel_addOperand(model, &operandType) );
    operandIdxes[name + "_strideY"] = opIdx;
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_setOperandValue(model, opIdx, &strideY, sizeof(strideY)) );
    parameterIdxes.push_back(opIdx);
    ++opIdx;

    CHECK_NNAPI_ERROR( ANeuralNetworksModel_addOperand(model, &operandType) );
    operandIdxes[name + "_activation"] = opIdx;
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_setOperandValue(model, opIdx, &fusecode, sizeof(fusecode)) );
    parameterIdxes.push_back(opIdx);
    ++opIdx;

#if 0
    operandType.type = ANEURALNETWORKS_BOOL;
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_addOperand(model, &operandType) );
    operandIdxes[name + "_isNCHW"] = opIdx;
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_setOperandValue(model, opIdx, &isNCHW, sizeof(isNCHW)) );
    parameterIdxes.push_back(opIdx);
    ++opIdx;

    operandType.type = ANEURALNETWORKS_INT32;
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_addOperand(model, &operandType) );
    operandIdxes[name + "_dilationX"] = opIdx;
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_setOperandValue(model, opIdx, &dilationX, sizeof(dilationX)) );
    parameterIdxes.push_back(opIdx);
    ++opIdx;

    CHECK_NNAPI_ERROR( ANeuralNetworksModel_addOperand(model, &operandType) );
    operandIdxes[name + "_dilationY"] = opIdx;
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_setOperandValue(model, opIdx, &dilationY, sizeof(dilationY)) );
    parameterIdxes.push_back(opIdx);
    ++opIdx;
#endif


    const auto inDims = shapeIdxes.at(input);
    const auto wDims = shapeIdxes.at(weight);
    const uint32_t outN = inDims[0];
    const uint32_t outH = (inDims[1] - ((wDims[1] - 1) * dilationY + 1) + padTop + padBottom) / strideY + 1;
    const uint32_t outW = (inDims[2] - ((wDims[2] - 1) * dilationX + 1) + padLeft + padRight) / strideX + 1;
    const uint32_t outC = wDims[0];
    std::vector<uint32_t> outDims = {outN, outH, outW, outC};

    std::vector<uint32_t> outIdxes;
    operandType.type = ANEURALNETWORKS_TENSOR_FLOAT32; // TODO: Support only FP32 now
    operandType.dimensionCount = static_cast<uint32_t>(inDims.size());
    operandType.dimensions = outDims.data();
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_addOperand(model, &operandType) );

    operandIdxes[output] = opIdx;
    shapeIdxes[output] = {outN, outH, outW, outC};

    outIdxes.push_back(opIdx);
    ++opIdx;
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_addOperation(model, ANEURALNETWORKS_CONV_2D, parameterIdxes.size(), &parameterIdxes[0], outIdxes.size(), &outIdxes[0]) );
}

void ModelBuilder::setInputOps (std::string name, float* dataptr)
{
    uint32_t idx = operandIdxes.at(name);
    std::vector<uint32_t> shape = shapeIdxes.at(name);
    uint32_t sizebyte = std::accumulate(shape.begin(), shape.end(), 1, std::multiplies<uint32_t>()) * sizeof(float); // TODO: Support only FP32 now
    inputOps.push_back({idx, shape, sizebyte, name, dataptr});
}

void ModelBuilder::setOutputOps (std::string name)
{
    uint32_t idx = operandIdxes.at(name);
    std::vector<uint32_t> shape = shapeIdxes.at(name);
    uint32_t sizebyte = std::accumulate(shape.begin(), shape.end(), 1, std::multiplies<uint32_t>()) * sizeof(float); // TODO: Support only FP32 now
    int fd = ASharedMemory_create("an_optional_name", sizebyte);
    ANeuralNetworksMemory *memptr;
    CHECK_NNAPI_ERROR( ANeuralNetworksMemory_createFromFd(sizebyte, PROT_READ | PROT_WRITE, fd, 0, &memptr) );
    float* dataptr = reinterpret_cast<float *>(mmap(nullptr, sizebyte, PROT_READ, MAP_SHARED, fd, 0));
    outputOps.push_back({idx, shape, sizebyte, name, dataptr, fd, memptr});
}

void ModelBuilder::compile (int32_t dIdx)
{
    std::vector<uint32_t> inputIndices;
    std::vector<uint32_t> outputIndices;
    for (auto it: inputOps)
    {
        inputIndices.push_back(it.index);
    }
    for (auto it: outputOps)
    {
        outputIndices.push_back(it.index);
    }
    // The values of constant and intermediate operands cannot be altered after the finish function is called
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_identifyInputsAndOutputs(model, inputIndices.size(), inputIndices.data(), outputIndices.size(), outputIndices.data()) );
    CHECK_NNAPI_ERROR( ANeuralNetworksModel_finish(model) );
    if (dIdx != -1)
    {
        // TODO: Here use only one device :)
        ANeuralNetworksDevice *devicePtr = devices[dIdx];
        bool supportedOps[10];
        for (int i = 0; i < 10; ++i)
        {
            supportedOps[i] = false;
        }
        CHECK_NNAPI_ERROR( ANeuralNetworksModel_getSupportedOperationsForDevices(model, &devicePtr, 1, supportedOps) );
        for (int i = 0; i < 10; ++i)
        {
            SLOG_WARN << supportedOps[i] << std::endl;
        }
        CHECK_NNAPI_ERROR( ANeuralNetworksCompilation_createForDevices(model, &devicePtr, 1, &compilation) );
    }
    else
    {
        CHECK_NNAPI_ERROR( ANeuralNetworksCompilation_create(model, &compilation) );
    }
    CHECK_NNAPI_ERROR( ANeuralNetworksCompilation_setPreference(compilation, ANEURALNETWORKS_PREFER_FAST_SINGLE_ANSWER) );
    CHECK_NNAPI_ERROR( ANeuralNetworksCompilation_finish(compilation) );
    // Multiple concurrent execution instances could be created from the same compiled model.
    CHECK_NNAPI_ERROR( ANeuralNetworksExecution_create(compilation, &execution) );
    // Associate to the model inputs. Note that the index here uses the operand of the model input list, not all operand list
    for (size_t i = 0; i < inputOps.size(); ++i)
    {
        CHECK_NNAPI_ERROR( ANeuralNetworksExecution_setInput(execution, static_cast<int32_t>(i), nullptr, inputOps[i].data, inputOps[i].sizeBytes) );
    }
    // Set the output tensor that will be filled by executing the model. Shared memory here to minimize the copies needed for getting the output data.
    // Note that the index here uses the operand of the model output list, not all operand list
    for (size_t i = 0; i < outputOps.size(); ++i)
    {
        CHECK_NNAPI_ERROR( ANeuralNetworksExecution_setOutputFromMemory(execution, static_cast<int32_t>(i), nullptr, outputOps[i].nnMemPtr, 0, outputOps[i].sizeBytes) );
    }
}

void ModelBuilder::execute (void)
{
    // Start the execution of the model.
    // Note that the execution here is asynchronous, event will be created to monitor the status of the execution.
    CHECK_NNAPI_ERROR( ANeuralNetworksExecution_startCompute(execution, &event) );
    // Wait until the completion of the execution. This could be done on a different thread.
    // By waiting immediately, we effectively make this a synchronous call.
    CHECK_NNAPI_ERROR( ANeuralNetworksEvent_wait(event) );
}

std::vector<float *> ModelBuilder::getOutput(void)
{
    std::vector<float *> outputTensorPtrs;
    for (auto it: outputOps)
    {
        outputTensorPtrs.push_back(it.data);
    }

    return outputTensorPtrs;
}



}

