#include <m3t/webcam_color_camera.h>

#include <chrono>
#include <iostream>

namespace m3t {

WebcamColorCamera::WebcamColorCamera(
    const std::string &name, int device_index,
    const std::filesystem::path &calibration_path, int width, int height)
    : ColorCamera{name},
      device_index_{device_index},
      width_{width},
      height_{height},
      calibration_path_{calibration_path} {}

WebcamColorCamera::WebcamColorCamera(const std::string &name,
                                     const std::filesystem::path &metafile_path)
    : ColorCamera{name, metafile_path} {}

WebcamColorCamera::~WebcamColorCamera() {
  StopCaptureThread();
  if (capture_.isOpened()) capture_.release();
}

bool WebcamColorCamera::SetUp() {
  set_up_ = false;
  StopCaptureThread();
  if (!metafile_path_.empty())
    if (!LoadMetaData()) return false;
  if (!OpenDevice()) return false;
  if (!LoadCalibration()) return false;
  SaveMetaDataIfDesired();
  StartCaptureThread();
  set_up_ = true;
  return UpdateImage(true);
}

bool WebcamColorCamera::UpdateImage(bool synchronized) {
  if (!set_up_) {
    std::cerr << "Set up webcam color camera " << name_ << " first"
              << std::endl;
    return false;
  }

  {
    std::unique_lock<std::mutex> lock{frame_mutex_};
    const uint64_t required_id = synchronized ? consumed_frame_id_ + 1 : 1;
    if (!frame_cv_.wait_for(lock, std::chrono::seconds{1},
                            [&] { return latest_frame_id_ >= required_id; })) {
      std::cerr << "Webcam " << name_ << " delivered no new frame within 1 s"
                << std::endl;
      return false;
    }
    // Nothing new since last call: keep the current image
    if (latest_frame_id_ == consumed_frame_id_ && !image_.empty()) return true;
    latest_frame_.copyTo(raw_image_);
    consumed_frame_id_ = latest_frame_id_;
  }

  cv::remap(raw_image_, image_, map1_, map2_, cv::INTER_LINEAR);
  SaveImageIfDesired();
  return true;
}

bool WebcamColorCamera::LoadMetaData() {
  cv::FileStorage fs;
  if (!OpenYamlFileStorage(metafile_path_, &fs)) return false;

  if (!ReadRequiredValueFromYaml(fs, "calibration_path", &calibration_path_)) {
    std::cerr << "Could not read all required camera parameters from "
              << metafile_path_ << std::endl;
    return false;
  }
  ReadOptionalValueFromYaml(fs, "device_index", &device_index_);
  ReadOptionalValueFromYaml(fs, "width", &width_);
  ReadOptionalValueFromYaml(fs, "height", &height_);
  ReadOptionalValueFromYaml(fs, "fps", &fps_);
  ReadOptionalValueFromYaml(fs, "disable_autofocus", &disable_autofocus_);
  ReadOptionalValueFromYaml(fs, "focus", &focus_);
  ReadOptionalValueFromYaml(fs, "undistort_alpha", &undistort_alpha_);
  ReadOptionalValueFromYaml(fs, "camera2world_pose", &camera2world_pose_);
  ReadOptionalValueFromYaml(fs, "save_directory", &save_directory_);
  ReadOptionalValueFromYaml(fs, "save_index", &save_index_);
  ReadOptionalValueFromYaml(fs, "save_image_type", &save_image_type_);
  ReadOptionalValueFromYaml(fs, "save_images", &save_images_);
  fs.release();

  if (calibration_path_.is_relative())
    calibration_path_ = metafile_path_.parent_path() / calibration_path_;
  if (save_directory_.is_relative())
    save_directory_ = metafile_path_.parent_path() / save_directory_;
  world2camera_pose_ = camera2world_pose_.inverse();
  return true;
}

bool WebcamColorCamera::OpenDevice() {
  if (capture_.isOpened()) capture_.release();
#ifdef _WIN32
  const int api = cv::CAP_DSHOW;  // exposes focus controls, opens fast
#else
  const int api = cv::CAP_ANY;
#endif
  if (!capture_.open(device_index_, api)) {
    std::cerr << "Could not open webcam " << device_index_ << " for camera "
              << name_ << std::endl;
    return false;
  }

  // MJPG lets most webcams reach full frame rate at 720p and above
  capture_.set(cv::CAP_PROP_FOURCC,
               cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
  capture_.set(cv::CAP_PROP_FRAME_WIDTH, width_);
  capture_.set(cv::CAP_PROP_FRAME_HEIGHT, height_);
  capture_.set(cv::CAP_PROP_FPS, fps_);
  if (disable_autofocus_) capture_.set(cv::CAP_PROP_AUTOFOCUS, 0);
  if (focus_ >= 0) capture_.set(cv::CAP_PROP_FOCUS, focus_);

  // Drivers can silently fall back to another resolution: check a real frame
  cv::Mat test_frame;
  if (!capture_.read(test_frame) || test_frame.empty()) {
    std::cerr << "Webcam " << name_ << " opened but delivered no frame"
              << std::endl;
    return false;
  }
  if (test_frame.cols != width_ || test_frame.rows != height_ ||
      test_frame.type() != CV_8UC3) {
    std::cerr << "Webcam " << name_ << " delivers " << test_frame.cols << "x"
              << test_frame.rows << " instead of requested " << width_ << "x"
              << height_ << " BGR" << std::endl;
    return false;
  }
  return true;
}

bool WebcamColorCamera::LoadCalibration() {
  cv::FileStorage fs{calibration_path_.string(), cv::FileStorage::READ};
  if (!fs.isOpened()) {
    std::cerr << "Could not open calibration file " << calibration_path_
              << std::endl;
    return false;
  }
  cv::Mat camera_matrix, distortion;
  int calib_width = 0, calib_height = 0;
  fs["camera_matrix"] >> camera_matrix;
  fs["distortion_coefficients"] >> distortion;
  fs["image_width"] >> calib_width;
  fs["image_height"] >> calib_height;
  fs.release();

  if (camera_matrix.empty() || distortion.empty()) {
    std::cerr << "Calibration file " << calibration_path_
              << " is missing camera_matrix or distortion_coefficients"
              << std::endl;
    return false;
  }
  if (calib_width != width_ || calib_height != height_) {
    std::cerr << "Calibration was made at " << calib_width << "x"
              << calib_height << " but camera runs at " << width_ << "x"
              << height_ << std::endl;
    return false;
  }

  // M3T has no distortion model: undistort images, report the new intrinsics
  const cv::Size size{width_, height_};
  cv::Mat new_camera_matrix = cv::getOptimalNewCameraMatrix(
      camera_matrix, distortion, size, undistort_alpha_, size);
  cv::initUndistortRectifyMap(camera_matrix, distortion, cv::Mat{},
                              new_camera_matrix, size, CV_16SC2, map1_, map2_);

  intrinsics_.fu = static_cast<float>(new_camera_matrix.at<double>(0, 0));
  intrinsics_.fv = static_cast<float>(new_camera_matrix.at<double>(1, 1));
  intrinsics_.ppu = static_cast<float>(new_camera_matrix.at<double>(0, 2));
  intrinsics_.ppv = static_cast<float>(new_camera_matrix.at<double>(1, 2));
  intrinsics_.width = width_;
  intrinsics_.height = height_;
  return true;
}

void WebcamColorCamera::StartCaptureThread() {
  {
    std::lock_guard<std::mutex> lock{frame_mutex_};
    latest_frame_.release();
    latest_frame_id_ = 0;
    consumed_frame_id_ = 0;
  }
  running_ = true;
  capture_thread_ = std::thread{&WebcamColorCamera::CaptureLoop, this};
}

void WebcamColorCamera::StopCaptureThread() {
  running_ = false;
  if (capture_thread_.joinable()) capture_thread_.join();
}

void WebcamColorCamera::CaptureLoop() {
  cv::Mat frame;
  while (running_) {
    if (!capture_.read(frame) || frame.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
      continue;
    }
    {
      std::lock_guard<std::mutex> lock{frame_mutex_};
      cv::swap(latest_frame_, frame);  // no copy; old buffer gets reused
      ++latest_frame_id_;
    }
    frame_cv_.notify_one();
  }
}

}  // namespace m3t