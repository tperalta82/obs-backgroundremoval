#include <obs-module.h>
#include <media-io/video-scaler.h>

#if defined(__APPLE__)
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include <onnxruntime/core/providers/cpu/cpu_provider_factory.h>
#else
#include <onnxruntime_cxx_api.h>
#include <cpu_provider_factory.h>
#endif
#ifdef _WIN32
#include <dml_provider_factory.h>
#include <wchar.h>
#endif

#include <opencv2/imgproc.hpp>

#include <numeric>
#include <memory>
#include <exception>

#include "plugin-macros.generated.h"

struct background_removal_filter {
	std::unique_ptr<Ort::Session> session;
	std::unique_ptr<Ort::Env> env;
	std::vector<const char*> inputNames;
	std::vector<const char*> outputNames;
	Ort::Value inputTensor;
	Ort::Value outputTensor;
	std::vector<int64_t> inputDims;
	std::vector<int64_t> outputDims;
	std::vector<float> outputTensorValues;
	std::vector<float> inputTensorValues;
	Ort::MemoryInfo memoryInfo;
	float threshold = 0.5f;
	cv::Scalar backgroundColor{0, 0, 0};
	float contourFilter = 0.05f;
	float smoothContour = 0.5f;
    bool useGPU = false;
	const char* modelSelection;

	// Use the media-io converter to both scale and convert the colorspace
	video_scaler_t* scalerToBGR;
	video_scaler_t* scalerFromBGR;

#if _WIN32
    const wchar_t* modelFilepath = nullptr;
#else
    const char* modelFilepath = nullptr;
#endif
};

const char* MODEL_SINET = "SINet_Softmax_simple.onnx";
const char* MODEL_MODNET = "modnet_simple.onnx";

static const char *filter_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Background Removal";
}

template <typename T>
T vectorProduct(const std::vector<T>& v)
{
    return accumulate(v.begin(), v.end(), (T)1, std::multiplies<T>());
}

static void hwc_to_chw(cv::InputArray src, cv::OutputArray dst) {
    std::vector<cv::Mat> channels;
    cv::split(src, channels);

    // Stretch one-channel images to vector
    for (auto &img : channels) {
        img = img.reshape(1, 1);
    }

    // Concatenate three vectors to one
    cv::hconcat( channels, dst );
}


/**                   PROPERTIES                     */

static obs_properties_t *filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *p_threshold = obs_properties_add_float_slider(
		props,
		"threshold",
		obs_module_text("Threshold"),
		0.0,
		1.0,
		0.05);

	obs_property_t *p_contour_filter = obs_properties_add_float_slider(
		props,
		"contour_filter",
		obs_module_text("Contour Filter (% of image)"),
		0.0,
		1.0,
		0.025);

	obs_property_t *p_smooth_contour = obs_properties_add_float_slider(
		props,
		"smooth_contour",
		obs_module_text("Smooth silhouette"),
		0.0,
		1.0,
		0.05);

	obs_property_t *p_color = obs_properties_add_color(
		props,
		"replaceColor",
		obs_module_text("Background Color"));

	obs_property_t *p_use_gpu = obs_properties_add_bool(
		props,
		"useGPU",
		obs_module_text("Use GPU for inference (Windows only)"));

	obs_property_t *p_model_select = obs_properties_add_list(
		props,
		"model_select",
		obs_module_text("Segmentation model"),
		OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING
	);
	obs_property_list_add_string(p_model_select, obs_module_text("SINet"), MODEL_SINET);
	obs_property_list_add_string(p_model_select, obs_module_text("MODNet"), MODEL_MODNET);

#ifndef _WIN32
    obs_property_set_enabled(p_use_gpu, false);
#endif

	UNUSED_PARAMETER(data);
	return props;
}

static void filter_defaults(obs_data_t *settings) {
	obs_data_set_default_double(settings, "threshold", 0.5);
	obs_data_set_default_double(settings, "contour_filter", 0.05);
	obs_data_set_default_double(settings, "smooth_contour", 0.5);
	obs_data_set_default_int(settings, "replaceColor", 0x000000);
	obs_data_set_default_bool(settings, "useGPU", false);
	obs_data_set_default_string(settings, "model_select", MODEL_SINET);
}

static void createOrtSession(struct background_removal_filter *tf) {

    Ort::SessionOptions sessionOptions;

    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    if (tf->useGPU) {
        sessionOptions.DisableMemPattern();
        sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    }

	char* modelFilepath_rawPtr = obs_module_file(tf->modelSelection);
	blog(LOG_INFO, "Model location %s", modelFilepath_rawPtr);

	if (modelFilepath_rawPtr == nullptr) {
		blog(LOG_ERROR, "Unable to get model filename from plugin.");
		return;
	}

	std::string modelFilepath_s(modelFilepath_rawPtr);
    bfree(modelFilepath_rawPtr);

#if _WIN32
    std::wstring modelFilepath_ws(modelFilepath_s.size(), L' ');
    std::copy(modelFilepath_s.begin(), modelFilepath_s.end(), modelFilepath_ws.begin());
    tf->modelFilepath = modelFilepath_ws.c_str();
#else
    tf->modelFilepath = modelFilepath_s.c_str();
#endif

    try {
#ifdef _WIN32
        if (tf->useGPU) {
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(sessionOptions, 0));
        }
#endif
        tf->session.reset(new Ort::Session(*tf->env, tf->modelFilepath, sessionOptions));
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "%s", e.what());
        return;
    }

    Ort::AllocatorWithDefaultOptions allocator;

    tf->inputNames.clear();
    tf->outputNames.clear();

    tf->inputNames.push_back(tf->session->GetInputName(0, allocator));
    tf->outputNames.push_back(tf->session->GetOutputName(0, allocator));

	// Allocate output buffer
    const Ort::TypeInfo outputTypeInfo = tf->session->GetOutputTypeInfo(0);
    const auto outputTensorInfo = outputTypeInfo.GetTensorTypeAndShapeInfo();
    tf->outputDims = outputTensorInfo.GetShape();
    tf->outputTensorValues.resize(vectorProduct(tf->outputDims));

	// Allocate input buffer
    const Ort::TypeInfo inputTypeInfo = tf->session->GetInputTypeInfo(0);
    const auto inputTensorInfo = inputTypeInfo.GetTensorTypeAndShapeInfo();
    tf->inputDims = inputTensorInfo.GetShape();
    tf->inputTensorValues.resize(vectorProduct(tf->inputDims));

	blog(LOG_INFO, "Model input shape %d x %d x %d, output shape %d x %d x %d",
		(int)tf->inputDims[1], (int)tf->inputDims[2], (int)tf->inputDims[3],
		(int)tf->outputDims[1], (int)tf->outputDims[2], (int)tf->outputDims[3]);

	// Build input and output tensors
    tf->memoryInfo = Ort::MemoryInfo::CreateCpu(
        OrtAllocatorType::OrtDeviceAllocator, OrtMemType::OrtMemTypeDefault);
    tf->outputTensor = Ort::Value::CreateTensor<float>(
        	tf->memoryInfo,
			tf->outputTensorValues.data(),
			tf->outputTensorValues.size(),
        	tf->outputDims.data(),
			tf->outputDims.size());
	tf->inputTensor = Ort::Value::CreateTensor<float>(
			tf->memoryInfo,
			tf->inputTensorValues.data(),
			tf->inputTensorValues.size(),
			tf->inputDims.data(),
			tf->inputDims.size());
}


static void destroyScalers(struct background_removal_filter *tf) {
	blog(LOG_INFO, "Destroy scalers.");
	if (tf->scalerToBGR != nullptr) {
		video_scaler_destroy(tf->scalerToBGR);
		tf->scalerToBGR = nullptr;
	}
	if (tf->scalerFromBGR != nullptr) {
		video_scaler_destroy(tf->scalerFromBGR);
		tf->scalerFromBGR = nullptr;
	}
}


static void filter_update(void *data, obs_data_t *settings)
{
	struct background_removal_filter *tf = reinterpret_cast<background_removal_filter *>(data);
	tf->threshold = (float)obs_data_get_double(settings, "threshold");

	uint64_t color = obs_data_get_int(settings, "replaceColor");
	tf->backgroundColor.val[0] = (double)(color & 0x0000ff);
	tf->backgroundColor.val[1] = (double)((color >> 8) & 0x0000ff);
	tf->backgroundColor.val[2] = (double)((color >> 16) & 0x0000ff);

	tf->contourFilter = (float)obs_data_get_double(settings, "contour_filter");
	tf->smoothContour = (float)obs_data_get_double(settings, "smooth_contour");

    tf->useGPU = (bool)obs_data_get_bool(settings, "useGPU");

	tf->modelSelection = obs_data_get_string(settings, "model_select");

	destroyScalers(tf);

    createOrtSession(tf);
}


/**                   FILTER CORE                     */

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct background_removal_filter *tf = reinterpret_cast<background_removal_filter *>(bzalloc(sizeof(struct background_removal_filter)));

	std::string instanceName{"background-removal-inference"};
    tf->env.reset(new Ort::Env(OrtLoggingLevel::ORT_LOGGING_LEVEL_ERROR, instanceName.c_str()));

	filter_update(tf, settings);

	return tf;
}


static void initializeScalers(cv::Size frameSize,
							  enum video_format frameFormat,
							  struct background_removal_filter *tf) {

	struct video_scale_info dst{
		VIDEO_FORMAT_BGR3,
		(uint32_t)frameSize.width,
		(uint32_t)frameSize.height,
		VIDEO_RANGE_DEFAULT,
		VIDEO_CS_DEFAULT
	};
	struct video_scale_info src{
		frameFormat,
		(uint32_t)frameSize.width,
		(uint32_t)frameSize.height,
		VIDEO_RANGE_DEFAULT,
		VIDEO_CS_DEFAULT
	};

	// Check if scalers already defined and release them
	destroyScalers(tf);

	blog(LOG_INFO, "Initialize scalers. Size %d x %d",
		frameSize.width, frameSize.height);

	// Create new scalers
	video_scaler_create(&tf->scalerToBGR, &dst, &src, VIDEO_SCALE_DEFAULT);
	video_scaler_create(&tf->scalerFromBGR, &src, &dst, VIDEO_SCALE_DEFAULT);
}


static cv::Mat convertFrameToBGR(struct obs_source_frame *frame,
								 struct background_removal_filter *tf) {
	const cv::Size frameSize(frame->width, frame->height);

	if (tf->scalerToBGR == nullptr) {
		// Lazy initialize the frame scale & color converter
		initializeScalers(frameSize, frame->format, tf);
	}

	cv::Mat imageBGR(frameSize, CV_8UC3);
	const uint32_t bgrLinesize = (uint32_t)(imageBGR.cols * imageBGR.elemSize());
	video_scaler_scale(tf->scalerToBGR,
		&(imageBGR.data), &(bgrLinesize),
		frame->data, frame->linesize);

	return imageBGR;
}


static void convertBGRToFrame(const cv::Mat& imageBGR,
							  struct obs_source_frame *frame,
							  struct background_removal_filter *tf) {
	if (tf->scalerFromBGR == nullptr) {
		// Lazy initialize the frame scale & color converter
		initializeScalers(cv::Size(frame->width, frame->height), frame->format, tf);
	}

	const uint32_t rgbLinesize = (uint32_t)(imageBGR.cols * imageBGR.elemSize());
	video_scaler_scale(tf->scalerFromBGR,
		frame->data, frame->linesize,
		&(imageBGR.data), &(rgbLinesize));
}


static struct obs_source_frame * filter_render(void *data, struct obs_source_frame *frame)
{
	struct background_removal_filter *tf = reinterpret_cast<background_removal_filter *>(data);

	// Convert to BGR
	cv::Mat imageBGR = convertFrameToBGR(frame, tf);

	// To RGB
	cv::Mat imageRGB;
	cv::cvtColor(imageBGR, imageRGB, cv::COLOR_BGR2RGB);

	// Resize to network input size
	cv::Mat resizedImageRGB;
	cv::resize(imageRGB, resizedImageRGB, cv::Size((int)tf->inputDims.at(2), (int)tf->inputDims.at(3)));

	// Prepare input to nework
    cv::Mat resizedImage, preprocessedImage;
    resizedImageRGB.convertTo(resizedImage, CV_32F);

	if (std::strcmp(tf->modelSelection, MODEL_SINET) == 0) {
		cv::subtract(resizedImage, cv::Scalar(102.890434, 111.25247, 126.91212), resizedImage);
		cv::multiply(resizedImage, cv::Scalar(1.0 / 62.93292,  1.0 / 62.82138, 1.0 / 66.355705) / 255.0, resizedImage);
	} else {
		cv::subtract(resizedImage, cv::Scalar::all(127.5), resizedImage);
		resizedImage = resizedImage / 127.5;
	}

    hwc_to_chw(resizedImage, preprocessedImage);

    tf->inputTensorValues.assign(preprocessedImage.begin<float>(),
                             	 preprocessedImage.end<float>());

	// Run network inference
    tf->session->Run(
		Ort::RunOptions{nullptr},
		tf->inputNames.data(), &(tf->inputTensor), 1,
		tf->outputNames.data(), &(tf->outputTensor), 1);

	// Convert network output mask to input size
    cv::Mat outputImage((int)tf->outputDims.at(2),
						(int)tf->outputDims.at(3),
						CV_32FC1,
						&(tf->outputTensorValues[0]));

	cv::Mat backgroundMask;
	if (std::strcmp(tf->modelSelection, MODEL_SINET) == 0) {
		backgroundMask = outputImage > tf->threshold;
	} else {
		backgroundMask = outputImage < tf->threshold;
	}

	// Contour processing
	if (tf->contourFilter > 0.0 && tf->contourFilter < 1.0) {
		std::vector<std::vector<cv::Point> > contours;
		findContours(backgroundMask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
		std::vector<std::vector<cv::Point> > filteredContours;
		const int64_t contourSizeThreshold = (int64_t)(backgroundMask.total() * tf->contourFilter);
		for (auto& contour : contours) {
			if (cv::contourArea(contour) > contourSizeThreshold) {
				filteredContours.push_back(contour);
			}
		}
		backgroundMask.setTo(0);
		drawContours(backgroundMask, filteredContours, -1, cv::Scalar(255), -1);
	}

	// Mask the input
	cv::resize(backgroundMask, backgroundMask, imageBGR.size());

	// Smooth mask with a fast filter (box)
	if (tf->smoothContour > 0.0) {
		int k_size = (int)(100 * tf->smoothContour);
		cv::boxFilter(backgroundMask, backgroundMask, backgroundMask.depth(), cv::Size(k_size, k_size));
		backgroundMask = backgroundMask > 128;
	}

	imageBGR.setTo(tf->backgroundColor, backgroundMask);

	// Put masked image back on frame
	convertBGRToFrame(imageBGR, frame, tf);
	return frame;
}


static void filter_destroy(void *data)
{
	struct background_removal_filter *tf = reinterpret_cast<background_removal_filter *>(data);

	if (tf) {
		destroyScalers(tf);
		bfree(tf);
	}
}



struct obs_source_info background_removal_filter_info = {
	.id = "background_removal",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC,
	.get_name = filter_getname,
	.create = filter_create,
	.destroy = filter_destroy,
	.get_defaults = filter_defaults,
	.get_properties = filter_properties,
	.update = filter_update,
	.filter_video = filter_render,
};