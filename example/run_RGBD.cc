#ifdef USE_PANGOLIN_VIEWER
#include "pangolin_viewer/viewer.h"
#elif USE_SOCKET_PUBLISHER
#include "socket_publisher/publisher.h"
#endif

#include "openvslam/system.h"
#include "openvslam/config.h"
#include "openvslam/util/stereo_rectifier.h"

#include <iostream>
#include <chrono>
#include <numeric>

#include <opencv2/core/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <spdlog/spdlog.h>
#include <popl.hpp>
#include <librealsense2/rs.hpp>

#include "util/cv-helpers.hpp"

#ifdef USE_STACK_TRACE_LOGGER
#include <glog/logging.h>
#endif

#ifdef USE_GOOGLE_PERFTOOLS
#include <gperftools/profiler.h>
#endif

void RGBD_tracking(const std::shared_ptr<openvslam::config>& cfg,
                   const std::string& vocab_file_path, const unsigned int cam_num, const std::string& mask_img_path,
                   const float scale, const std::string& map_db_path) {
    const cv::Mat mask = mask_img_path.empty() ? cv::Mat{} : cv::imread(mask_img_path, cv::IMREAD_GRAYSCALE);
    // build a SLAM system
    openvslam::system SLAM(cfg, vocab_file_path);
    // startup the SLAM process
    SLAM.startup();
    // use pangolin viewer 
    pangolin_viewer::viewer viewer(cfg, &SLAM, SLAM.get_frame_publisher(), SLAM.get_map_publisher());

    
    // Declare depth colorizer for pretty visualization of depth data
    rs2::colorizer color_map;
    // Declare RealSense pipeline, encapsulating the actual device and sensors
    rs2::pipeline pipe;
    // Start streaming with default recommended configuration
    pipe.start();
 	
 	rs2::frameset data = pipe.wait_for_frames();
    
	cv::Mat frame;
    double timestamp = 0.0;
    //rs2::frameset data = pipe.wait_for_frames();
    rs2::frame depth = data.get_depth_frame().apply_filter(color_map);

    bool is_not_end = true;

    std::thread thread([&]() {
        while (is_not_end) {
            // check if the termination of SLAM system is requested or not
            if (SLAM.terminate_is_requested()) {
                break;
            }


            rs2::frameset data = pipe.wait_for_frames(); // Wait for next set of frames from the camera
            rs2::frame depth_frame = data.get_depth_frame();
            rs2::frame color_frame = data.get_color_frame();

            auto color_mat = frame_to_mat(color_frame);
            auto depth_mat = frame_to_mat(depth_frame);

            /*
            cv::Mat image(Size(w, h), CV_8UC3, (void*)color_frame.get_data(), Mat::AUTO_STEP);
            cv::Mat image(Size(w, h), CV_8UC3, (void*)depth_frame.get_data(), Mat::AUTO_STEP);
            */

            SLAM.feed_RGBD_frame(color_mat, depth_mat, timestamp, mask);
            timestamp += 1.0 / cfg->camera_->fps_;
        }
    });
// run the viewer in the current thread
#ifdef USE_PANGOLIN_VIEWER
    viewer.run();
#elif USE_SOCKET_PUBLISHER
    publisher.run();
#endif
    thread.join();

    // shutdown the SLAM process
    SLAM.shutdown();

    if (!map_db_path.empty()) {
        // output the map database
        SLAM.save_map_database(map_db_path);
    }
}



void stereo_tracking(const std::shared_ptr<openvslam::config>& cfg,
                     const std::string& vocab_file_path, const unsigned int cam_num, const std::string& mask_img_path,
                     const float scale, const std::string& map_db_path) {
    const cv::Mat mask = mask_img_path.empty() ? cv::Mat{} : cv::imread(mask_img_path, cv::IMREAD_GRAYSCALE);
    // build a SLAM system
    openvslam::system SLAM(cfg, vocab_file_path);
    // startup the SLAM process
    SLAM.startup();

    // create a viewer object
    // and pass the frame_publisher and the map_publisher
#ifdef USE_PANGOLIN_VIEWER
    pangolin_viewer::viewer viewer(cfg, &SLAM, SLAM.get_frame_publisher(), SLAM.get_map_publisher());
#elif USE_SOCKET_PUBLISHER
    socket_publisher::publisher publisher(cfg, &SLAM, SLAM.get_frame_publisher(), SLAM.get_map_publisher());
#endif

    cv::VideoCapture videos[2];
    for (int i = 0; i < 2; i++) {
        videos[i] = cv::VideoCapture(cam_num + i);
        if (!videos[i].isOpened()) {
            spdlog::critical("cannot open a camera {}", cam_num + i);
            SLAM.shutdown();
            return;
        }
    }

    const openvslam::util::stereo_rectifier rectifier(cfg);

    cv::Mat frames[2];
    cv::Mat frames_rectified[2];
    double timestamp = 0.0;
    std::vector<double> track_times;
    unsigned int num_frame = 0;

    bool is_not_end = true;
    // run the SLAM in another thread
    std::thread thread([&]() {
        while (is_not_end) {
            // check if the termination of SLAM system is requested or not
            if (SLAM.terminate_is_requested()) {
                break;
            }

            is_not_end = videos[0].read(frames[0]) && videos[1].read(frames[1]);
            if (frames[0].empty() || frames[1].empty()) {
                continue;
            }
            for (int i = 0; i < 2; i++) {
                if (scale != 1.0) {
                    cv::resize(frames[i], frames[i], cv::Size(), scale, scale, cv::INTER_LINEAR);
                }
            }
            rectifier.rectify(frames[0], frames[1], frames_rectified[0], frames_rectified[1]);

            const auto tp_1 = std::chrono::steady_clock::now();

            // input the current frame and estimate the camera pose
            SLAM.feed_stereo_frame(frames_rectified[0], frames_rectified[1], timestamp, mask);

            const auto tp_2 = std::chrono::steady_clock::now();

            const auto track_time = std::chrono::duration_cast<std::chrono::duration<double>>(tp_2 - tp_1).count();
            track_times.push_back(track_time);

            timestamp += 1.0 / cfg->camera_->fps_;
            ++num_frame;
        }

        // wait until the loop BA is finished
        while (SLAM.loop_BA_is_running()) {
            std::this_thread::sleep_for(std::chrono::microseconds(5000));
        }
    });

    // run the viewer in the current thread
#ifdef USE_PANGOLIN_VIEWER
    viewer.run();
#elif USE_SOCKET_PUBLISHER
    publisher.run();
#endif

    thread.join();

    // shutdown the SLAM process
    SLAM.shutdown();

    if (!map_db_path.empty()) {
        // output the map database
        SLAM.save_map_database(map_db_path);
    }

    std::sort(track_times.begin(), track_times.end());
    const auto total_track_time = std::accumulate(track_times.begin(), track_times.end(), 0.0);
    std::cout << "median tracking time: " << track_times.at(track_times.size() / 2) << "[s]" << std::endl;
    std::cout << "mean tracking time: " << total_track_time / track_times.size() << "[s]" << std::endl;
}

int main(int argc, char* argv[]) {
#ifdef USE_STACK_TRACE_LOGGER
    google::InitGoogleLogging(argv[0]);
    google::InstallFailureSignalHandler();
#endif

    // create options
    popl::OptionParser op("Allowed options");
    auto help = op.add<popl::Switch>("h", "help", "produce help message");
    auto vocab_file_path = op.add<popl::Value<std::string>>("v", "vocab", "vocabulary file path");
    auto cam_num = op.add<popl::Value<unsigned int>>("n", "number", "camera number");
    auto config_file_path = op.add<popl::Value<std::string>>("c", "config", "config file path");
    auto mask_img_path = op.add<popl::Value<std::string>>("", "mask", "mask image path", "");
    auto scale = op.add<popl::Value<float>>("s", "scale", "scaling ratio of images", 1.0);
    auto map_db_path = op.add<popl::Value<std::string>>("p", "map-db", "store a map database at this path after SLAM", "");
    auto debug_mode = op.add<popl::Switch>("", "debug", "debug mode");
    try {
        op.parse(argc, argv);
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        std::cerr << std::endl;
        std::cerr << op << std::endl;
        return EXIT_FAILURE;
    }

    // check validness of options
    if (help->is_set()) {
        std::cerr << op << std::endl;
        return EXIT_FAILURE;
    }
    if (!vocab_file_path->is_set() || !cam_num->is_set()
        || !config_file_path->is_set()) {
        std::cerr << "invalid arguments" << std::endl;
        std::cerr << std::endl;
        std::cerr << op << std::endl;
        return EXIT_FAILURE;
    }

    // setup logger
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] %^[%L] %v%$");
    if (debug_mode->is_set()) {
        spdlog::set_level(spdlog::level::debug);
    }
    else {
        spdlog::set_level(spdlog::level::info);
    }

    // load configuration
    std::shared_ptr<openvslam::config> cfg;
    try {
        cfg = std::make_shared<openvslam::config>(config_file_path->value());
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

#ifdef USE_GOOGLE_PERFTOOLS
    ProfilerStart("slam.prof");
#endif

    // run tracking
    if (cfg->camera_->setup_type_ == openvslam::camera::setup_type_t::Monocular) {
        RGBD_tracking(cfg, vocab_file_path->value(), cam_num->value(), mask_img_path->value(),
                      scale->value(), map_db_path->value());
    }
    else if (cfg->camera_->setup_type_ == openvslam::camera::setup_type_t::Stereo) {
        stereo_tracking(cfg, vocab_file_path->value(), cam_num->value(), mask_img_path->value(),
                        scale->value(), map_db_path->value());
    }
    else {
        throw std::runtime_error("Invalid setup type: " + cfg->camera_->get_setup_type_string());
    }

#ifdef USE_GOOGLE_PERFTOOLS
    ProfilerStop();
#endif

    return EXIT_SUCCESS;
}