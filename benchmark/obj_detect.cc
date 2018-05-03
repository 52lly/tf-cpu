#include <stdio.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <gflags/gflags.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <tensorflow/core/public/session.h>

#include "test_video.hpp"

DEFINE_string(model_file, "", "");
DEFINE_string(labels_file, "", "");

DEFINE_string(video_file, "", "");
DEFINE_string(image_file, "", "");
DEFINE_int32(width, 320, "");
DEFINE_int32(height, 0, "");
DEFINE_string(output, "", "");

DEFINE_int32(ffmpeg_log_level, 8, "");

namespace {

bool ReadLines(const std::string& file_name, std::vector<std::string>* lines) {
    std::ifstream file(file_name);
    if (!file) {
        LOG(ERROR) << "Failed to open file " << file_name;
        return false;
    }
    std::string line;
    while (std::getline(file, line)) lines->push_back(line);
    return true;
}

template<typename T>
const T* TensorData(const tensorflow::Tensor& tensor);

template<>
const float* TensorData(const tensorflow::Tensor& tensor) {
    switch (tensor.dtype()) {
        case tensorflow::DT_FLOAT:
            return tensor.flat<float>().data();
        default:
            LOG(FATAL) << "Should not reach here!";
    }
    return nullptr;
}

template<>
const uint8_t* TensorData(const tensorflow::Tensor& tensor) {
    switch (tensor.dtype()) {
        case tensorflow::DT_UINT8:
            return tensor.flat<uint8_t>().data();
        default:
            LOG(FATAL) << "Should not reach here!";
    }
    return nullptr;
}

// Returns a Mat that refer to the data owned by frame.
std::unique_ptr<cv::Mat> AVFrameToMat(AVFrame* frame) {
    std::unique_ptr<cv::Mat> mat;
    if (frame->format == AV_PIX_FMT_RGB24) {
        mat.reset(new cv::Mat(
            frame->height, frame->width, CV_8UC3, frame->data[0], frame->linesize[0]));
        cv::cvtColor(*mat, *mat, cv::COLOR_RGB2BGR);
    } else if (frame->format == AV_PIX_FMT_GRAY8) {
        mat.reset(new cv::Mat(
            frame->height, frame->width, CV_8UC1, frame->data[0], frame->linesize[0]));
    } else {
        LOG(FATAL) << "Should not reach here!";
    }
    return mat;
}

const char num_detections[] = "num_detections";
const char detection_classes[] = "detection_classes";
const char detection_scores[] = "detection_scores";
const char detection_boxes[] = "detection_boxes";

class ObjDetector {
  public:
    ObjDetector() {};

    bool Init(const std::string& model_file, const std::vector<std::string>& labels) {
        // Load model.
        auto status = tensorflow::ReadBinaryProto(
            tensorflow::Env::Default(), model_file, &graph_def_);
        if (!status.ok()) {
            LOG(ERROR) << "Failed to load mode file " << model_file << status;
            return false;
        }

        // Create graph.
        tensorflow::SessionOptions sess_opts;
        sess_opts.config.set_intra_op_parallelism_threads(1);
        sess_opts.config.set_inter_op_parallelism_threads(1);
        sess_opts.config.set_allow_soft_placement(1);
        sess_opts.config.set_isolate_session_state(1);
        session_.reset(tensorflow::NewSession(sess_opts));
        status = session_->Create(graph_def_);
        if (!status.ok()) {
            LOG(ERROR) << "Failed to create graph: " << status;
            return false;
        }

        // Find input/output nodes.
        std::vector<const tensorflow::NodeDef*> placeholders;
        bool has_num_detections = false;
        bool has_detection_classes = false;
        bool has_detection_scores = false;
        bool has_detection_boxes = false;
        for (const auto& node : graph_def_.node()) {
            if (node.op() == "Placeholder") {
                placeholders.push_back(&node);
            } else if (node.name() == num_detections) {
                has_num_detections = true;
            } else if (node.name() == detection_classes) {
                has_detection_classes = true;
            } else if (node.name() == detection_scores) {
                has_detection_scores = true;
            } else if (node.name() == detection_boxes) {
                has_detection_boxes = true;
            }
        }
        if (placeholders.empty()) {
            LOG(ERROR) << "No input node found!";
            return false;
        }
        const tensorflow::NodeDef* input = placeholders[0];
        VLOG(0) << "Using input node: " << input->DebugString();
        if (!input->attr().count("dtype")) {
            LOG(ERROR) << "Input node " << input->name() << "does not have dtype.";
            return false;
        }
        input_name_ = input->name();
        input_dtype_ = input->attr().at("dtype").type();
        if (input->attr().count("shape")) {
            const auto shape = input->attr().at("shape").shape();
            input_channels_ = shape.dim(3).size();
        }

        labels_ = labels;
        return true;
    }


    bool RunVideo(const std::string& video_file, int width, int height,
                  const std::string& output_name) {
        // Open input video.
        TestVideo test_video(av_pix_fmt(), width, height);
        if (!test_video.Init(video_file, nullptr, true)) {
            LOG(ERROR) << "Failed to open video file " << video_file;
            return false;
        }
        width = test_video.width();
        height = test_video.height();

        InitInputTensor(width, height);

        // Run.
        int frames = 0;
        int total_ms = 0;
        AVFrame* frame = nullptr;
        char image_file_name[1000];
        while ((frame = test_video.NextFrame())) {
            std::vector<tensorflow::Tensor> output_tensors;
            const auto start = std::chrono::high_resolution_clock::now();
            FeedInAVFrame(frame);
            if (!Run(&output_tensors)) return false;
            const std::chrono::duration<double> duration =
                std::chrono::high_resolution_clock::now() - start;
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
            frames++;
            total_ms += elapsed_ms;
            VLOG(0) << frames << ": ms=" << elapsed_ms;
            auto mat = AVFrameToMat(frame);
            AnnotateMat(*mat, output_tensors);
            snprintf(image_file_name, sizeof(image_file_name), "%s.%05d.jpeg",
                     output_name.c_str(), frames);
            cv::imwrite(image_file_name, *mat);
            av_frame_free(&frame);
        }
        printf("%s: %d frames processed in %d ms(%d mspf).\n",
               output_name.c_str(), frames, total_ms, total_ms / frames);
        return true;
    }

    bool RunImage(const std::string file_name, int width, int height,
                  const std::string& output) {
        cv::Mat mat = cv::imread(file_name);
        if (!mat.data) {
            LOG(ERROR) << "Failed to read image " << file_name;
            return false;
        }
        if (width != 0 || height != 0) {
            if (width == 0) {
                width = mat.cols * height / mat.rows;
            } else if (height == 0) {
                height = mat.rows * width / mat.cols;
            }
            cv::Mat resized;
            cv::resize(mat, resized, cv::Size(width, height));
            mat = resized;
        }
        InitInputTensor(mat.cols, mat.rows);
        cv::cvtColor(mat, mat, cv::COLOR_BGR2RGB);
        FeedInMat(mat);
        std::vector<tensorflow::Tensor> output_tensors;
        if (!Run(&output_tensors)) return false;
        AnnotateMat(mat, output_tensors);
        cv::imwrite(output, mat);
        return true;
    }

    bool Run(std::vector<tensorflow::Tensor>* output_tensors) {
        const auto status = session_->Run(
            {{input_name_, *input_tensor_}},
            {num_detections, detection_classes, detection_scores, detection_boxes},
            {},
            output_tensors);
        if (!status.ok()) {
            LOG(ERROR) << "Failed to call Session::Run: " << status;
            return false;
        }
        return true;
    }

    void FeedInAVFrame(AVFrame* frame) {
        const int size = input_tensor_->NumElements();
        const int row_elems = frame->width * input_channels_;
        switch (input_dtype_) {
            case tensorflow::DT_FLOAT:
                {
                    float* data = input_tensor_->flat<float>().data();
                    for (int i = 0; i < size; i++) {
                        const int row = i / row_elems;
                        const int pos = row * frame->linesize[0] + (i % row_elems);
                        data[i] = frame->data[0][pos] / 256.f;
                    }
                    break;
                }
            case tensorflow::DT_UINT8:
                {
                    uint8_t* dst = input_tensor_->flat<uint8_t>().data();
                    uint8_t* src = frame->data[0];
                    for (int row = 0; row < frame->height; row++) {
                        memcpy(dst, src, row_elems);
                        dst += row_elems;
                        src += frame->linesize[0];
                    }
                }
                break;
            default:
                LOG(FATAL) << "Should not reach here!";
        }
    }

    void FeedInMat(const cv::Mat& mat) {
        const int size = input_tensor_->NumElements();
        switch (input_dtype_) {
            case tensorflow::DT_FLOAT:
                if (input_channels_ == 3) {
                    float* data = input_tensor_->flat<float>().data();
                    for (int row = 0; row < mat.rows; row++) {
                        for (int col = 0; col < mat.cols; col++) {
                            const cv::Vec3b& pix = mat.at<cv::Vec3b>(row, col);
                            const int pos = (row * mat.cols + col) * 3;
                            data[pos] = pix[0] / 256.f;
                            data[pos + 1] = pix[1] / 256.f;
                            data[pos + 2] = pix[2] / 256.f;
                        }
                    }
                } else {
                    float* data = input_tensor_->flat<float>().data();
                    for (int row = 0; row < mat.rows; row++) {
                        for (int col = 0; col < mat.cols; col++) {
                            data[row * mat.cols + col] = mat.at<uint8_t>(row, col) / 256.f;
                        }
                    }
                }
                break;
            case tensorflow::DT_UINT8:
                {
                    uint8_t* dst = input_tensor_->flat<uint8_t>().data();
                    const int row_elems = mat.cols * input_channels_;
                    for (int row = 0; row < mat.rows; row++) {
                        memcpy(dst, mat.ptr(row), row_elems);
                        dst += row_elems;
                    }
                }
                break;
            default:
                LOG(FATAL) << "Should not reach here!";
        }
    }

  private:
    enum AVPixelFormat av_pix_fmt() const {
        return input_channels_ == 3 ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_GRAY8;
    }

    void InitInputTensor(int width, int height) {
        if (!input_tensor_ || input_tensor_->dim_size(1) != width ||
            input_tensor_->dim_size(2) != height) {
            // Create input tensor.
            tensorflow::TensorShape input_shape;
            input_shape.AddDim(1);  // batch size
            input_shape.AddDim(height);
            input_shape.AddDim(width);
            input_shape.AddDim(input_channels_);
            input_tensor_.reset(new tensorflow::Tensor(input_dtype_, input_shape));
        }
    }

    void AnnotateMat(cv::Mat& mat, const std::vector<tensorflow::Tensor>& output_tensors) {
        const int num_detections = *TensorData<float>(output_tensors[0]);
        const float* detection_classes = TensorData<float>(output_tensors[1]);
        const float* detection_scores = TensorData<float>(output_tensors[2]);
        const float* detection_boxes = TensorData<float>(output_tensors[3]);
        for (int i = 0; i < num_detections; i++) {
            const float score = detection_scores[i];
            if (score < .5f) break;
            const int cls = detection_classes[i];
            if (cls == 0) continue;
            const int ymin = detection_boxes[4 * i] * mat.rows;
            const int xmin = detection_boxes[4 * i + 1] * mat.cols;
            const int ymax = detection_boxes[4 * i + 2] * mat.rows;
            const int xmax = detection_boxes[4 * i + 3] * mat.cols;
            VLOG(0) << "Detected " << labels_[cls - 1] << " with score " << score
                << " @[" << xmin << "," << ymin << ":" << xmax << "," << ymax << "]";
            cv::rectangle(mat, cv::Rect(xmin, ymin, xmax - xmin, ymax - ymin),
                          cv::Scalar(0, 0, 255), 3);
            cv::putText(mat, labels_[cls - 1], cv::Point(xmin, ymin - 5),
                        cv::FONT_HERSHEY_PLAIN, .8, cv::Scalar(10, 255, 30));
        }
    }

    std::vector<std::string> labels_;
    tensorflow::GraphDef graph_def_;
    std::unique_ptr<tensorflow::Session> session_;

    std::string input_name_;
    tensorflow::DataType input_dtype_;
    int input_channels_ = 3;
    std::unique_ptr<tensorflow::Tensor> input_tensor_;
};

}  // namespace

int main(int argc, char** argv) {
    google::ParseCommandLineFlags(&argc, &argv, true);
    InitFfmpeg(FLAGS_ffmpeg_log_level);
    std::vector<std::string> labels;
    if (!ReadLines(FLAGS_labels_file, &labels)) return 1;
    ObjDetector obj_detector;
    if (!obj_detector.Init(FLAGS_model_file, labels)) return 1;
    if (!FLAGS_video_file.empty()) {
        obj_detector.RunVideo(FLAGS_video_file, FLAGS_width, FLAGS_height, FLAGS_output);
    } else if (!FLAGS_image_file.empty()) {
        obj_detector.RunImage(FLAGS_image_file, FLAGS_width, FLAGS_height, FLAGS_output);
    }
}
