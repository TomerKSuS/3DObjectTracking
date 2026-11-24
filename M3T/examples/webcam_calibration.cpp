// Checkerboard calibration for m3t::WebcamColorCamera.
// Writes camera_matrix, distortion_coefficients, image_width, image_height.

#include <iostream>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

namespace {

struct Settings {
  std::string output_path;
  int device_index = 0;
  int width = 1280;
  int height = 720;
  cv::Size board_size{9, 6};   // INNER corners (a 10x7-square board)
  float square_size = 0.025f;  // meters, measured on the printout
  int focus = -1;              // -1 keeps the device's current focus
};

bool ParseArgs(int argc, char **argv, Settings *s) {
  if (argc < 2) {
    std::cerr << "Usage: calibrate_webcam <output.yaml> [device_index] [width] "
                 "[height] [board_cols] [board_rows] [square_size_m] [focus]\n"
                 "board_cols/board_rows count INNER corners, e.g. 9 6\n";
    return false;
  }
  s->output_path = argv[1];
  if (argc > 2) s->device_index = std::stoi(argv[2]);
  if (argc > 3) s->width = std::stoi(argv[3]);
  if (argc > 4) s->height = std::stoi(argv[4]);
  if (argc > 5) s->board_size.width = std::stoi(argv[5]);
  if (argc > 6) s->board_size.height = std::stoi(argv[6]);
  if (argc > 7) s->square_size = std::stof(argv[7]);
  if (argc > 8) s->focus = std::stoi(argv[8]);
  return true;
}

// Must match WebcamColorCamera::OpenDevice, or the intrinsics won't fit
bool OpenCamera(const Settings &s, cv::VideoCapture *cap) {
#ifdef _WIN32
  const int api = cv::CAP_DSHOW;
#else
  const int api = cv::CAP_ANY;
#endif
  if (!cap->open(s.device_index, api)) {
    std::cerr << "Could not open webcam " << s.device_index << std::endl;
    return false;
  }
  cap->set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
  cap->set(cv::CAP_PROP_FRAME_WIDTH, s.width);
  cap->set(cv::CAP_PROP_FRAME_HEIGHT, s.height);
  cap->set(cv::CAP_PROP_AUTOFOCUS, 0);
  if (s.focus >= 0) cap->set(cv::CAP_PROP_FOCUS, s.focus);

  cv::Mat test;
  if (!cap->read(test) || test.cols != s.width || test.rows != s.height) {
    std::cerr << "Webcam does not deliver " << s.width << "x" << s.height
              << std::endl;
    return false;
  }
  return true;
}

std::vector<cv::Point3f> BoardPoints(const Settings &s) {
  std::vector<cv::Point3f> points;
  for (int r = 0; r < s.board_size.height; ++r)
    for (int c = 0; c < s.board_size.width; ++c)
      points.emplace_back(c * s.square_size, r * s.square_size, 0.0f);
  return points;
}

bool DetectBoard(const cv::Mat &frame, const cv::Size &board_size,
                 std::vector<cv::Point2f> *corners) {
  cv::Mat gray;
  cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
  if (!cv::findChessboardCorners(gray, board_size, *corners,
                                 cv::CALIB_CB_ADAPTIVE_THRESH |
                                     cv::CALIB_CB_NORMALIZE_IMAGE |
                                     cv::CALIB_CB_FAST_CHECK))
    return false;
  cv::cornerSubPix(
      gray, *corners, cv::Size{11, 11}, cv::Size{-1, -1},
      cv::TermCriteria{cv::TermCriteria::EPS + cv::TermCriteria::COUNT, 30,
                       0.001});
  return true;
}

bool Calibrate(const Settings &s,
               const std::vector<std::vector<cv::Point2f>> &image_points,
               cv::Mat *camera_matrix, cv::Mat *distortion, double *rms) {
  std::vector<std::vector<cv::Point3f>> object_points(image_points.size(),
                                                      BoardPoints(s));
  std::vector<cv::Mat> rvecs, tvecs;
  cv::Mat std_intrinsics, std_extrinsics, per_view_errors;
  // k3 fixed: typical webcam lenses don't need it, and it tends to overfit
  *rms = cv::calibrateCamera(object_points, image_points,
                             cv::Size{s.width, s.height}, *camera_matrix,
                             *distortion, rvecs, tvecs, std_intrinsics,
                             std_extrinsics, per_view_errors, cv::CALIB_FIX_K3);

  std::cout << "\nRMS reprojection error: " << *rms << " px\nPer view:\n";
  for (int i = 0; i < per_view_errors.rows; ++i)
    std::cout << "  view " << i << ": " << per_view_errors.at<double>(i)
              << " px\n";
  return cv::checkRange(*camera_matrix) && cv::checkRange(*distortion);
}

bool Save(const Settings &s, const cv::Mat &camera_matrix,
          const cv::Mat &distortion, double rms, int n_views) {
  cv::FileStorage fs{s.output_path, cv::FileStorage::WRITE};
  if (!fs.isOpened()) {
    std::cerr << "Could not write " << s.output_path << std::endl;
    return false;
  }
  fs << "image_width" << s.width;
  fs << "image_height" << s.height;
  fs << "camera_matrix" << camera_matrix;
  fs << "distortion_coefficients" << distortion;
  fs << "avg_reprojection_error" << rms;
  fs << "n_views" << n_views;
  fs << "board_width" << s.board_size.width;
  fs << "board_height" << s.board_size.height;
  fs << "square_size" << s.square_size;
  std::cout << "Saved calibration to " << s.output_path << std::endl;
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Settings s;
  if (!ParseArgs(argc, argv, &s)) return 1;
  cv::VideoCapture cap;
  if (!OpenCamera(s, &cap)) return 1;

  std::cout << "[SPACE] capture view  [C] calibrate + save  "
               "[U] toggle undistorted view  [R] reset  [Q] quit\n";

  std::vector<std::vector<cv::Point2f>> image_points;
  cv::Mat coverage = cv::Mat::zeros(s.height, s.width, CV_8UC3);
  cv::Mat camera_matrix, distortion, map1, map2;
  double rms = 0.0;
  bool calibrated = false;
  bool show_undistorted = false;

  cv::Mat frame, display;
  std::vector<cv::Point2f> corners;
  while (true) {
    if (!cap.read(frame) || frame.empty()) continue;
    const bool found = DetectBoard(frame, s.board_size, &corners);

    if (calibrated && show_undistorted) {
      cv::remap(frame, display, map1, map2, cv::INTER_LINEAR);
    } else {
      cv::add(frame, coverage, display);  // shows where you already captured
      if (found)
        cv::drawChessboardCorners(display, s.board_size, corners, true);
    }

    std::string status = "views: " + std::to_string(image_points.size());
    if (calibrated) status += "   RMS: " + std::to_string(rms) + " px";
    if (calibrated && show_undistorted) status += "   [UNDISTORTED]";
    cv::putText(display, status, cv::Point{20, 40}, cv::FONT_HERSHEY_SIMPLEX,
                1.0, cv::Scalar{0, 255, 255}, 2);
    cv::imshow("calibrate_webcam", display);

    const int key = cv::waitKey(1) & 0xFF;
    if (key == 'q' || key == 27) break;

    if (key == ' ') {
      if (!found) {
        std::cout << "Board not fully detected, hold still and retry\n";
        continue;
      }
      image_points.push_back(corners);
      for (const auto &p : corners)
        cv::circle(coverage, p, 4, cv::Scalar{0, 120, 0}, cv::FILLED);
      std::cout << "Captured view " << image_points.size() << "\n";
    }

    if (key == 'r') {
      image_points.clear();
      coverage.setTo(cv::Scalar::all(0));
      calibrated = show_undistorted = false;
      std::cout << "Reset\n";
    }

    if (key == 'c') {
      if (image_points.size() < 10) {
        std::cout << "Capture at least 10 views first (20-30 recommended)\n";
        continue;
      }
      if (!Calibrate(s, image_points, &camera_matrix, &distortion, &rms)) {
        std::cerr << "Calibration failed\n";
        continue;
      }
      Save(s, camera_matrix, distortion, rms,
           static_cast<int>(image_points.size()));

      // Same undistortion as WebcamColorCamera (alpha = 0)
      const cv::Size size{s.width, s.height};
      cv::Mat new_camera_matrix = cv::getOptimalNewCameraMatrix(
          camera_matrix, distortion, size, 0.0, size);
      cv::initUndistortRectifyMap(camera_matrix, distortion, cv::Mat{},
                                  new_camera_matrix, size, CV_16SC2, map1,
                                  map2);
      calibrated = true;
      show_undistorted = true;
    }

    if (key == 'u' && calibrated) show_undistorted = !show_undistorted;
  }
  return 0;
}