#ifndef IMAGE_ENHANCER_H
#define IMAGE_ENHANCER_H

#include <string>
#include <vector>
#include <memory>
#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>

// 图像增强类（单例模式，避免重复加载模型）
class ImageEnhancer {
public:
    // 单例获取接口
    static ImageEnhancer* GetInstance(const std::string& model_path = "LPGPNet.onnx");
    
    // 图像增强接口（输入OpenCV图像，输出增强后图像）
    bool Enhance(const cv::Mat& input_img, cv::Mat& output_img);
    
    // 禁止拷贝构造和赋值
    ImageEnhancer(const ImageEnhancer&) = delete;
    ImageEnhancer& operator=(const ImageEnhancer&) = delete;

private:
    static ImageEnhancer* instance_;
    
    // 私有构造（单例模式）
    ImageEnhancer(const std::string& model_path);
    ~ImageEnhancer() = default;

    // 预处理：图像缩放、归一化、通道转换（HWC→CHW）
    bool Preprocess(const cv::Mat& input_img, std::vector<float>& input_tensor_data);
    
    // 后处理：张量转图像、反归一化、通道转换（CHW→HWC）
    bool Postprocess(const std::vector<float>& output_tensor_data, cv::Mat& output_img);

private:
    Ort::Env env_;                          // ONNX Runtime环境
    std::unique_ptr<Ort::Session> session_; // ONNX会话（智能指针管理内存）
    Ort::AllocatorWithDefaultOptions alloc_;// 内存分配器
    std::vector<const char*> input_names_;  // 模型输入节点名
    std::vector<const char*> output_names_; // 模型输出节点名
    std::vector<int64_t> input_shape_;      // 模型输入形状（[batch, channel, height, width]）
    cv::Size input_size_;                   // 模型输入图像尺寸（width, height）
    int input_channel_;                     // 模型输入通道数（通常为3）
};

#endif // IMAGE_ENHANCER_H