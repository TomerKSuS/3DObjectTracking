#ifndef M3T_INCLUDE_M3T_WEBCAM_COLOR_CAMERA_H_
#define M3T_INCLUDE_M3T_WEBCAM_COLOR_CAMERA_H_

#include <filesystem/filesystem.h>
#include <m3t/camera.h>
#include <m3t/common.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <string>
#include <thread>

namespace m3t {

/**
 * \brief \ref ColorCamera that streams live, undistorted images from a webcam
 * through OpenCV. A background thread grabs frames continuously so that
 * `UpdateImage()` always uses the newest frame. Intrinsics are computed from an
 * OpenCV calibration file (camera_matrix, distortion_coefficients,
 * image_width, image_height).
 */
class WebcamColorCamera : public ColorCamera {
 public:
  // Constructors, destructor, and setup method
  WebcamColorCamera(const std::string &name, int device_index,
                    const std::filesystem::path &calibration_path,
                    int width = 1280, int height = 720);
  WebcamColorCamera(const std::string &name,
                    const std::filesystem::path &metafile_path);
  WebcamColorCamera(const WebcamColorCamera &) = delete;
  WebcamColorCamera &operator=(const WebcamColorCamera &) = delete;
  ~WebcamColorCamera();
  bool SetUp() override;

  // Main method
  bool UpdateImage(bool synchronized) override;

 private:
  // Helper methods
  bool LoadMetaData();
  bool OpenDevice();
  bool LoadCalibration();
  void StartCaptureThread();
  void StopCaptureThread();
  void CaptureLoop();

  // Parameters
  int device_index_ = 0;
  int width_ = 1280;
  int height_ = 720;
  int fps_ = 30;
  bool disable_autofocus_ = true;
  int focus_ = -1;                // -1 keeps the device's current focus
  float undistort_alpha_ = 0.0f;  // 0 = crop to valid pixels only
  std::filesystem::path calibration_path_{};

  // Device and undistortion
  cv::VideoCapture capture_;
  cv::Mat map1_;
  cv::Mat map2_;
  cv::Mat raw_image_;

  // Shared between capture thread and UpdateImage()
  std::thread capture_thread_;
  std::atomic<bool> running_{false};
  std::mutex frame_mutex_;
  std::condition_variable frame_cv_;
  cv::Mat latest_frame_;
  uint64_t latest_frame_id_ = 0;
  uint64_t consumed_frame_id_ = 0;
};

}  // namespace m3t

#endif  // M3T_INCLUDE_M3T_WEBCAM_COLOR_CAMERA_H_