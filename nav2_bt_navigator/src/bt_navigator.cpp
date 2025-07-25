// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "nav2_bt_navigator/bt_navigator.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "nav2_util/geometry_utils.hpp"
#include "nav2_ros_common/node_utils.hpp"
#include "nav2_util/string_utils.hpp"
#include "nav2_util/robot_utils.hpp"
#include "nav2_behavior_tree/bt_utils.hpp"

#include "nav2_behavior_tree/plugins_list.hpp"

using nav2::declare_parameter_if_not_declared;

namespace nav2_bt_navigator
{

BtNavigator::BtNavigator(rclcpp::NodeOptions options)
: nav2::LifecycleNode("bt_navigator", "",
    options.automatically_declare_parameters_from_overrides(true)),
  class_loader_("nav2_core", "nav2_core::NavigatorBase")
{
  RCLCPP_INFO(get_logger(), "Creating");
}

BtNavigator::~BtNavigator()
{
}

nav2::CallbackReturn
BtNavigator::on_configure(const rclcpp_lifecycle::State & state)
{
  RCLCPP_INFO(get_logger(), "Configuring");

  tf_ = std::make_shared<tf2_ros::Buffer>(get_clock());
  auto timer_interface = std::make_shared<tf2_ros::CreateTimerROS>(
    get_node_base_interface(), get_node_timers_interface());
  tf_->setCreateTimerInterface(timer_interface);
  tf_->setUsingDedicatedThread(true);
  tf_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf_, this, false);

  global_frame_ = this->declare_or_get_parameter("global_frame", std::string("map"));
  robot_frame_ = this->declare_or_get_parameter("robot_base_frame", std::string("base_link"));
  transform_tolerance_ = this->declare_or_get_parameter("transform_tolerance", 0.1);
  odom_topic_ = this->declare_or_get_parameter("odom_topic", std::string("odom"));
  filter_duration_ = this->declare_or_get_parameter("filter_duration", 0.3);

  // Libraries to pull plugins (BT Nodes) from
  std::vector<std::string> plugin_lib_names;
  plugin_lib_names = nav2_util::split(nav2::details::BT_BUILTIN_PLUGINS, ';');

  auto user_defined_plugins =
    this->declare_or_get_parameter("plugin_lib_names", std::vector<std::string>{});
  // append user_defined_plugins to plugin_lib_names
  plugin_lib_names.insert(
    plugin_lib_names.end(), user_defined_plugins.begin(),
    user_defined_plugins.end());

  nav2_core::FeedbackUtils feedback_utils;
  feedback_utils.tf = tf_;
  feedback_utils.global_frame = global_frame_;
  feedback_utils.robot_frame = robot_frame_;
  feedback_utils.transform_tolerance = transform_tolerance_;

  // Odometry smoother object for getting current speed
  auto node = shared_from_this();
  odom_smoother_ = std::make_shared<nav2_util::OdomSmoother>(node, filter_duration_, odom_topic_);

  // Navigator defaults
  const std::vector<std::string> default_navigator_ids = {
    "navigate_to_pose",
    "navigate_through_poses"
  };
  const std::vector<std::string> default_navigator_types = {
    "nav2_bt_navigator::NavigateToPoseNavigator",
    "nav2_bt_navigator::NavigateThroughPosesNavigator"
  };

  std::vector<std::string> navigator_ids;
  navigator_ids = this->declare_or_get_parameter("navigators", default_navigator_ids);
  if (navigator_ids == default_navigator_ids) {
    for (size_t i = 0; i < default_navigator_ids.size(); ++i) {
      declare_parameter_if_not_declared(
        node, default_navigator_ids[i] + ".plugin",
        rclcpp::ParameterValue(default_navigator_types[i]));
    }
  }

  // Load navigator plugins
  for (size_t i = 0; i != navigator_ids.size(); i++) {
    try {
      std::string navigator_type = nav2::get_plugin_type_param(node, navigator_ids[i]);
      RCLCPP_INFO(
        get_logger(), "Creating navigator id %s of type %s",
        navigator_ids[i].c_str(), navigator_type.c_str());
      navigators_.push_back(class_loader_.createUniqueInstance(navigator_type));
      if (!navigators_.back()->on_configure(
          node, plugin_lib_names, feedback_utils,
          &plugin_muxer_, odom_smoother_))
      {
        return nav2::CallbackReturn::FAILURE;
      }
    } catch (const std::exception & ex) {
      RCLCPP_FATAL(
        get_logger(), "Failed to create navigator id %s."
        " Exception: %s", navigator_ids[i].c_str(), ex.what());
      on_cleanup(state);
      return nav2::CallbackReturn::FAILURE;
    }
  }

  return nav2::CallbackReturn::SUCCESS;
}

nav2::CallbackReturn
BtNavigator::on_activate(const rclcpp_lifecycle::State & state)
{
  RCLCPP_INFO(get_logger(), "Activating");
  for (size_t i = 0; i != navigators_.size(); i++) {
    if (!navigators_[i]->on_activate()) {
      on_deactivate(state);
      return nav2::CallbackReturn::FAILURE;
    }
  }

  // create bond connection
  createBond();

  return nav2::CallbackReturn::SUCCESS;
}

nav2::CallbackReturn
BtNavigator::on_deactivate(const rclcpp_lifecycle::State & /*state*/)
{
  RCLCPP_INFO(get_logger(), "Deactivating");
  for (size_t i = 0; i != navigators_.size(); i++) {
    if (!navigators_[i]->on_deactivate()) {
      return nav2::CallbackReturn::FAILURE;
    }
  }

  // destroy bond connection
  destroyBond();

  return nav2::CallbackReturn::SUCCESS;
}

nav2::CallbackReturn
BtNavigator::on_cleanup(const rclcpp_lifecycle::State & /*state*/)
{
  RCLCPP_INFO(get_logger(), "Cleaning up");

  // Reset the listener before the buffer
  tf_listener_.reset();
  tf_.reset();

  for (size_t i = 0; i != navigators_.size(); i++) {
    if (!navigators_[i]->on_cleanup()) {
      return nav2::CallbackReturn::FAILURE;
    }
  }

  navigators_.clear();
  RCLCPP_INFO(get_logger(), "Completed Cleaning up");
  return nav2::CallbackReturn::SUCCESS;
}

nav2::CallbackReturn
BtNavigator::on_shutdown(const rclcpp_lifecycle::State & /*state*/)
{
  RCLCPP_INFO(get_logger(), "Shutting down");
  return nav2::CallbackReturn::SUCCESS;
}

}  // namespace nav2_bt_navigator

#include "rclcpp_components/register_node_macro.hpp"

// Register the component with class_loader.
// This acts as a sort of entry point, allowing the component to be discoverable when its library
// is being loaded into a running process.
RCLCPP_COMPONENTS_REGISTER_NODE(nav2_bt_navigator::BtNavigator)
