#include "octomap_world/octomap_manager.h"

#include <glog/logging.h>
#include <minkindr_conversions/kindr_tf.h>
#include <minkindr_conversions/kindr_msg.h>

namespace volumetric_mapping {

OctomapManager::OctomapManager(const ros::NodeHandle& nh,
                               const ros::NodeHandle& nh_private)
    : nh_(nh),
      nh_private_(nh_private),
      world_frame_("world"),
      Q_initialized_(false),
      Q_(Eigen::Matrix4d::Identity()),
      full_image_size_(752, 480),
      map_publish_frequency_(0.0) {
  setParametersFromROS();
  subscribe();
  advertiseServices();
  advertisePublishers();
}

void OctomapManager::setParametersFromROS() {
  OctomapParameters params;
  nh_private_.param("tf_frame", world_frame_, world_frame_);
  nh_private_.param("resolution", params.resolution, params.resolution);
  nh_private_.param("probability_hit", params.probability_hit,
                    params.probability_hit);
  nh_private_.param("probability_miss", params.probability_miss,
                    params.probability_miss);
  nh_private_.param("threshold_min", params.threshold_min,
                    params.threshold_min);
  nh_private_.param("threshold_max", params.threshold_max,
                    params.threshold_max);
  nh_private_.param("threshold_occupancy", params.threshold_occupancy,
                    params.threshold_occupancy);
  nh_private_.param("filter_speckles", params.filter_speckles,
                    params.filter_speckles);
  nh_private_.param("sensor_max_range", params.sensor_max_range,
                    params.sensor_max_range);
  nh_private_.param("visualize_min_z", params.visualize_min_z,
                    params.visualize_min_z);
  nh_private_.param("visualize_max_z", params.visualize_max_z,
                    params.visualize_max_z);
  nh_private_.param("full_image_width", full_image_size_.x(),
                    full_image_size_.x());
  nh_private_.param("full_image_height", full_image_size_.y(),
                    full_image_size_.y());
  nh_private_.param("map_publish_frequency", map_publish_frequency_,
                    map_publish_frequency_);

  // Try to initialize Q matrix from parameters, if available.
  std::vector<double> Q_vec;
  if (nh_private_.getParam("Q", Q_vec)) {
    Q_initialized_ = setQFromParams(&Q_vec);
  }

  // Set the parent class parameters.
  setOctomapParameters(params);
}

bool OctomapManager::setQFromParams(std::vector<double>* Q_vec) {
  if (Q_vec->size() != 16) {
    ROS_ERROR_STREAM("Invalid Q matrix size, expected size: 16, actual size: "
                     << Q_vec->size());
    return false;
  }

  // Try to map the vector as coefficients.
  Eigen::Map<Eigen::Matrix<double, 4, 4, Eigen::RowMajor> > Q_vec_map(
      Q_vec->data());
  // Copy over to the Q member.
  Q_ = Q_vec_map;

  return true;
}

void OctomapManager::subscribe() {
  left_info_sub_ = nh_.subscribe("cam0/camera_info", 1,
                                 &OctomapManager::leftCameraInfoCallback, this);
  right_info_sub_ = nh_.subscribe(
      "cam1/camera_info", 1, &OctomapManager::rightCameraInfoCallback, this);
  disparity_sub_ = nh_.subscribe(
      "disparity", 40, &OctomapManager::insertDisparityImageWithTf, this);
  pointcloud_sub_ = nh_.subscribe(
      "pointcloud", 40, &OctomapManager::insertPointcloudWithTf, this);
}

void OctomapManager::advertiseServices() {
  reset_map_service_ = nh_private_.advertiseService(
      "reset_map", &OctomapManager::resetMapCallback, this);
  publish_all_service_ = nh_private_.advertiseService(
      "publish_all", &OctomapManager::publishAllCallback, this);
  get_map_service_ = nh_private_.advertiseService(
      "get_map", &OctomapManager::getOctomapCallback, this);
  save_octree_service_ = nh_private_.advertiseService(
      "save_map", &OctomapManager::saveOctomapCallback, this);
  load_octree_service_ = nh_private_.advertiseService(
      "load_map", &OctomapManager::loadOctomapCallback, this);
  load_octree_service_ = nh_private_.advertiseService(
      "set_box_occupancy", &OctomapManager::setBoxOccupancyCallback, this);
}

void OctomapManager::advertisePublishers() {
  occupied_nodes_pub_ = nh_private_.advertise<visualization_msgs::MarkerArray>(
      "octomap_occupied", 1, true);
  free_nodes_pub_ = nh_private_.advertise<visualization_msgs::MarkerArray>(
      "octomap_free", 1, true);

  binary_map_pub_ =
      nh_private_.advertise<octomap_msgs::Octomap>("octomap_binary", 1, true);
  full_map_pub_ =
      nh_private_.advertise<octomap_msgs::Octomap>("octomap_full", 1, true);

  if (map_publish_frequency_ > 0.0)
    map_publish_timer_ =
        nh_private_.createTimer(ros::Duration(1.0 / map_publish_frequency_),
                                &OctomapManager::publishAllEvent, this);
}

void OctomapManager::publishAll() {
  visualization_msgs::MarkerArray occupied_nodes, free_nodes;
  generateMarkerArray(world_frame_, &occupied_nodes, &free_nodes);

  occupied_nodes_pub_.publish(occupied_nodes);
  free_nodes_pub_.publish(free_nodes);

  octomap_msgs::Octomap binary_map, full_map;
  getOctomapBinaryMsg(&binary_map);
  getOctomapFullMsg(&full_map);

  binary_map.header.frame_id = world_frame_;
  full_map.header.frame_id = world_frame_;

  binary_map_pub_.publish(binary_map);
  full_map_pub_.publish(full_map);
}

void OctomapManager::publishAllEvent(const ros::TimerEvent& e) { publishAll(); }

bool OctomapManager::resetMapCallback(std_srvs::Empty::Request& request,
                                      std_srvs::Empty::Response& response) {
  resetMap();
  return true;
}

bool OctomapManager::publishAllCallback(std_srvs::Empty::Request& request,
                                        std_srvs::Empty::Response& response) {
  publishAll();
  return true;
}

bool OctomapManager::getOctomapCallback(
    octomap_msgs::GetOctomap::Request& request,
    octomap_msgs::GetOctomap::Response& response) {
  return getOctomapFullMsg(&response.map);
}

bool OctomapManager::loadOctomapCallback(
    volumetric_msgs::LoadMap::Request& request,
    volumetric_msgs::LoadMap::Response& response) {
  return loadOctomapFromFile(request.file_path);
}

bool OctomapManager::saveOctomapCallback(
    volumetric_msgs::SaveMap::Request& request,
    volumetric_msgs::SaveMap::Response& response) {
  return writeOctomapToFile(request.file_path);
}

bool OctomapManager::setBoxOccupancyCallback(
    volumetric_msgs::SetBoxOccupancy::Request& request,
    volumetric_msgs::SetBoxOccupancy::Response& response) {
  Eigen::Vector3d bounding_box_center;
  Eigen::Vector3d bounding_box_size;

  tf::vectorMsgToKindr(request.box_center, &bounding_box_center);
  tf::vectorMsgToKindr(request.box_size, &bounding_box_size);
  bool set_occupied = request.set_occupied;

  if (set_occupied) {
    setOccupied(bounding_box_center, bounding_box_size);
  } else {
    setFree(bounding_box_center, bounding_box_size);
  }
}

void OctomapManager::leftCameraInfoCallback(
    const sensor_msgs::CameraInfoPtr& left_info) {
  left_info_ = left_info;
  if (left_info_ && right_info_ && !Q_initialized_) {
    calculateQ();
  }
}
void OctomapManager::rightCameraInfoCallback(
    const sensor_msgs::CameraInfoPtr& right_info) {
  right_info_ = right_info;
  if (left_info_ && right_info_ && !Q_initialized_) {
    calculateQ();
  }
}

void OctomapManager::calculateQ() {
  Q_ = getQForROSCameras(*left_info_, *right_info_);
  full_image_size_.x() = left_info_->width;
  full_image_size_.y() = left_info_->height;
  Q_initialized_ = true;
}

void OctomapManager::insertDisparityImageWithTf(
    const stereo_msgs::DisparityImageConstPtr& disparity) {
  if (!Q_initialized_) {
    ROS_WARN_THROTTLE(
        1, "No camera info available yet, skipping adding disparity.");
    return;
  }

  // Look up transform from sensor frame to world frame.
  Transformation sensor_to_world;
  if (lookupTransform(disparity->header.frame_id, world_frame_,
                      disparity->header.stamp, &sensor_to_world)) {
    insertDisparityImage(sensor_to_world, disparity, Q_, full_image_size_);
  }
}

void OctomapManager::insertPointcloudWithTf(
    const sensor_msgs::PointCloud2::ConstPtr& pointcloud) {
  // Look up transform from sensor frame to world frame.
  Transformation sensor_to_world;
  if (lookupTransform(pointcloud->header.frame_id, world_frame_,
                      pointcloud->header.stamp, &sensor_to_world)) {
    insertPointcloud(sensor_to_world, pointcloud);
  }
}

bool OctomapManager::lookupTransform(const std::string& from_frame,
                                     const std::string& to_frame,
                                     const ros::Time& timestamp,
                                     Transformation* transform) {
  tf::StampedTransform tf_transform;

  ros::Time time_to_lookup = timestamp;

  // If this transform isn't possible at the time, then try to just look up
  // the latest (this is to work with bag files and static transform publisher,
  // etc).
  if (!tf_listener_.canTransform(to_frame, from_frame, time_to_lookup)) {
    time_to_lookup = ros::Time(0);
    ROS_WARN("Using latest TF transform instead of timestamp match.");
  }

  try {
    tf_listener_.lookupTransform(to_frame, from_frame, time_to_lookup,
                                 tf_transform);
  }
  catch (tf::TransformException& ex) {
    ROS_ERROR_STREAM(
        "Error getting TF transform from sensor data: " << ex.what());
    return false;
  }

  tf::transformTFToKindr(tf_transform, transform);
  return true;
}

}  // namespace volumetric_mapping
