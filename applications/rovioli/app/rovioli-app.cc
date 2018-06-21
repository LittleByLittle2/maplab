#include <memory>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <localization-summary-map/localization-summary-map-creation.h>
#include <localization-summary-map/localization-summary-map.h>
#include <maplab-common/sigint-breaker.h>
#include <maplab-common/threading-helpers.h>
#include <message-flow/message-dispatcher-fifo.h>
#include <message-flow/message-flow.h>
#include <ros/ros.h>
#include <sensors/imu.h>
#include <sensors/sensor-factory.h>
#include <signal.h>
#include <std_srvs/Empty.h>
#include <vi-map/vi-map-serialization.h>

#include "rovioli/rovioli-node.h"

DEFINE_string(vio_localization_map_folder, "",
              "Path to a localization summary map or a full VI-map used for "
              "localization.");
DEFINE_string(ncamera_calibration, "ncamera.yaml",
              "Path to camera calibration yaml.");
DEFINE_string(imu_parameters_maplab, "imu-maplab.yaml",
              "Path to the imu configuration yaml for MAPLAB.");
DEFINE_string(
    external_imu_parameters_rovio, "",
    "Optional, path to the IMU configuration yaml for ROVIO. If none is "
    "provided the maplab values will be used for ROVIO as well.");
DEFINE_string(save_map_folder, "",
              "Save map to folder; if empty nothing is saved.");
DEFINE_bool(
    overwrite_existing_map, false,
    "If set to true, an existing map will be overwritten on save. Otherwise, a "
    "number will be appended to save_map_folder to obtain an available "
    "folder.");
DEFINE_bool(optimize_map_to_localization_map, false,
            "Optimize and process the map into a localization map before "
            "saving it.");
DEFINE_bool(save_map_on_shutdown, true,
            "Save the map on exit. If this is set to false, then the map must "
            "be saved using a service call.");

DECLARE_bool(map_builder_save_image_as_resources);
DECLARE_string(datasource_type);
DECLARE_string(datasource_rosbag);
DECLARE_bool(publish_debug_markers);
DECLARE_bool(rovio_enable_frame_visualization);

namespace rovioli {

class RovioliApp {
 public:
  RovioliApp(const ros::NodeHandle& nh, const ros::NodeHandle& nh_private);

  // Load the localization map and do all the other setup. MUST be called
  // before run().
  bool init();

  // Start the app.
  bool run();

  // Save a map.
  bool saveMap();

  // Check if the app *should* to be stopped (i.e., finished processing bag).
  std::atomic<bool>& shouldExit();

  void shutdown();

  // TODO(helenol): also add callbacks for saving a map! Loading is I guess
  // out of the question.
  bool saveMapCallback(std_srvs::Empty::Request& request,
                       std_srvs::Empty::Response& response);

 private:
  // ROS stuff.
  ros::NodeHandle nh_;
  ros::NodeHandle nh_private_;
  ros::ServiceServer save_map_srv_;

  // Settings.
  bool initialized_;
  std::string save_map_folder_;

  // State for running rovioli.
  ros::AsyncSpinner rovioli_spinner_;
  std::unique_ptr<message_flow::MessageFlow> message_flow_;
  std::unique_ptr<summary_map::LocalizationSummaryMap> localization_map_;
  std::unique_ptr<rovioli::RovioliNode> rovio_localization_node_;
};

RovioliApp::RovioliApp(const ros::NodeHandle& nh,
                       const ros::NodeHandle& nh_private)
    : nh_(nh),
      nh_private_(nh_private),
      initialized_(false),
      rovioli_spinner_(common::getNumHardwareThreads()) {
  // Add ROS params that, if specified, overwrite flag defaults.
  // Note that the flag default or specified values are always used as ROS param
  // defaults.
  nh_private_.param("vio_localization_map_folder",
                    FLAGS_vio_localization_map_folder,
                    FLAGS_vio_localization_map_folder);
  nh_private_.param("ncamera_calibration", FLAGS_ncamera_calibration,
                    FLAGS_ncamera_calibration);
  nh_private_.param("imu_parameters_maplab", FLAGS_imu_parameters_maplab,
                    FLAGS_imu_parameters_maplab);
  nh_private_.param("save_map_folder", FLAGS_save_map_folder,
                    FLAGS_save_map_folder);
  nh_private_.param("overwrite_existing_map", FLAGS_overwrite_existing_map,
                    FLAGS_overwrite_existing_map);
  nh_private_.param("optimize_map_to_localization_map",
                    FLAGS_optimize_map_to_localization_map,
                    FLAGS_optimize_map_to_localization_map);
  nh_private_.param("save_map_on_shutdown", FLAGS_save_map_on_shutdown,
                    FLAGS_save_map_on_shutdown);
  nh_private_.param("publish_debug_markers", FLAGS_publish_debug_markers,
                    FLAGS_publish_debug_markers);
  nh_private_.param("frame_visualization",
                    FLAGS_rovio_enable_frame_visualization,
                    FLAGS_rovio_enable_frame_visualization);
  // How data is loaded. If bagfile is specified, then it is used. Otherwise
  // the topics are used.
  std::string bagfile;
  nh_private_.param("bagfile", bagfile, bagfile);
  if (!bagfile.empty() && FLAGS_datasource_type != "rostopic") {
    ROS_INFO_STREAM("Using bagfile source: " << bagfile);
    FLAGS_datasource_type = "rosbag";
    FLAGS_datasource_rosbag = bagfile;
  } else if (FLAGS_datasource_type == "rosbag" &&
             FLAGS_datasource_rosbag.empty()) {
    ROS_INFO_STREAM("Using rostopic source.");
    FLAGS_datasource_type = "rostopic";
  }

  // Add a ROS service call to save the map.
  save_map_srv_ = nh_private_.advertiseService(
      "save_map", &RovioliApp::saveMapCallback, this);
}

bool RovioliApp::init() {
  // Optionally load localization map.
  if (!FLAGS_vio_localization_map_folder.empty()) {
    localization_map_.reset(new summary_map::LocalizationSummaryMap);
    if (!localization_map_->loadFromFolder(FLAGS_vio_localization_map_folder)) {
      LOG(WARNING) << "Could not load a localization summary map from "
                   << FLAGS_vio_localization_map_folder
                   << ". Will try to load it as a full VI map.";
      vi_map::VIMap vi_map;
      CHECK(vi_map::serialization::loadMapFromFolder(
          FLAGS_vio_localization_map_folder, &vi_map))
          << "Loading a VI map failed. Either provide a valid localization map "
          << "or leave the map folder flag empty.";

      localization_map_.reset(new summary_map::LocalizationSummaryMap);
      summary_map::createLocalizationSummaryMapForWellConstrainedLandmarks(
          vi_map, localization_map_.get());
      // Make sure the localization map is not empty.
      CHECK_GT(localization_map_->GLandmarkPosition().cols(), 0);
    }
  }

  // Load camera calibration and imu parameters.
  aslam::NCamera::Ptr camera_system =
      aslam::NCamera::loadFromYaml(FLAGS_ncamera_calibration);
  CHECK(camera_system) << "Could not load the camera calibration from: \'"
                       << FLAGS_ncamera_calibration << "\'";

  vi_map::Imu::UniquePtr maplab_imu_sensor =
      vi_map::createFromYaml<vi_map::Imu>(FLAGS_imu_parameters_maplab);
  CHECK(maplab_imu_sensor)
      << "Could not load IMU parameters for MAPLAB from: \'"
      << FLAGS_imu_parameters_maplab << "\'";
  CHECK(maplab_imu_sensor->getImuSigmas().isValid());

  // Optionally, load external values for the ROVIO sigmas; otherwise also use
  // the maplab values for ROVIO.
  vi_map::ImuSigmas rovio_imu_sigmas;
  if (!FLAGS_external_imu_parameters_rovio.empty()) {
    CHECK(rovio_imu_sigmas.loadFromYaml(FLAGS_external_imu_parameters_rovio))
        << "Could not load IMU parameters for ROVIO from: \'"
        << FLAGS_external_imu_parameters_rovio << "\'";
    CHECK(rovio_imu_sigmas.isValid());
  } else {
    rovio_imu_sigmas = maplab_imu_sensor->getImuSigmas();
  }

  if (FLAGS_map_builder_save_image_as_resources &&
      FLAGS_save_map_folder.empty()) {
    LOG(ERROR) << "If you would like to save the resources, "
               << "please also set a map folder with: --save_map_folder";
    return false;
  }

  // If a map will be saved (i.e., if the save map folder is not empty), append
  // a number to the name until a name is found that is free.
  save_map_folder_ = FLAGS_save_map_folder;
  if (!FLAGS_save_map_folder.empty()) {
    size_t counter = 0u;
    while (common::fileExists(save_map_folder_) ||
           (!FLAGS_overwrite_existing_map &&
            common::pathExists(save_map_folder_))) {
      save_map_folder_ =
          FLAGS_save_map_folder + "_" + std::to_string(counter++);
    }
  }

  // Construct the application.
  message_flow_.reset(
      message_flow::MessageFlow::create<message_flow::MessageDispatcherFifo>(
          common::getNumHardwareThreads()));

  rovio_localization_node_.reset(new rovioli::RovioliNode(
      camera_system, std::move(maplab_imu_sensor), rovio_imu_sigmas,
      save_map_folder_, localization_map_.get(), message_flow_.get()));

  initialized_ = true;
  return true;
}

bool RovioliApp::run() {
  if (!initialized_) {
    return false;
  }

  // Start the pipeline. The ROS spinner will handle SIGINT for us and abort
  // the application on CTRL+C.
  rovioli_spinner_.start();
  rovio_localization_node_->start();
  return true;
}

std::atomic<bool>& RovioliApp::shouldExit() {
  CHECK(rovio_localization_node_);
  return rovio_localization_node_->isDataSourceExhausted();
}

bool RovioliApp::saveMapCallback(
    std_srvs::Empty::Request& /*request*/,
    std_srvs::Empty::Response& /*response*/) {  // NOLINT
  return saveMap();
}

bool RovioliApp::saveMap() {
  if (!save_map_folder_.empty()) {
    rovio_localization_node_->saveMapAndOptionallyOptimize(
        save_map_folder_, FLAGS_overwrite_existing_map,
        FLAGS_optimize_map_to_localization_map);
    return true;
  } else {
    return false;
  }
}

void RovioliApp::shutdown() {
  rovio_localization_node_->shutdown();
  message_flow_->shutdown();
  message_flow_->waitUntilIdle();
}

}  // namespace rovioli

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InstallFailureSignalHandler();
  FLAGS_alsologtostderr = true;
  FLAGS_colorlogtostderr = true;

  ros::init(argc, argv, "rovioli");
  ros::NodeHandle nh, nh_private("~");

  rovioli::RovioliApp rovioli_app(nh, nh_private);

  if (!rovioli_app.init()) {
    ROS_FATAL("Failed to initialize the rovioli app!");
    ros::shutdown();
  }

  if (!rovioli_app.run()) {
    ROS_FATAL("Failed to start running the rovioli app!");
    ros::shutdown();
  }

  std::atomic<bool>& end_of_days_signal_received = rovioli_app.shouldExit();
  while (ros::ok() && !end_of_days_signal_received.load()) {
    // VLOG_EVERY_N(1, 10) << "\n" << flow->printDeliveryQueueStatistics();
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  rovioli_app.shutdown();
  if (FLAGS_save_map_on_shutdown) {
    rovioli_app.saveMap();
  }
  return 0;
}
