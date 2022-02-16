#ifndef V2X_NODE_HPP_EUIC2VFR
#define V2X_NODE_HPP_EUIC2VFR

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"
#include "autoware_perception_msgs/msg/dynamic_object_array.hpp"
#include "autoware_v2x_msgs/msg/cpm_reception_status.hpp"
#include "tf2_msgs/msg/tf_message.hpp"
#include <boost/asio/io_service.hpp>
#include "autoware_v2x/v2x_app.hpp"
#include "autoware_v2x/cpm_application.hpp"
#include "autoware_v2x/time_trigger.hpp"
#include "autoware_v2x/link_layer.hpp"
#include "autoware_v2x/ethernet_device.hpp"
#include "autoware_v2x/positioning.hpp"
#include "autoware_v2x/security.hpp"
#include "autoware_v2x/router_context.hpp"

namespace v2x
{
  class V2XNode : public rclcpp::Node
  {
  public:
    explicit V2XNode(const rclcpp::NodeOptions &node_options);
    V2XApp *app;
    void publishObjects(std::vector<CpmApplication::Object> *);

  private:
    void objectsCallback(
        const autoware_perception_msgs::msg::DynamicObjectArray::ConstSharedPtr msg);
    void tfCallback(const tf2_msgs::msg::TFMessage::ConstSharedPtr msg);

    rclcpp::Subscription<autoware_perception_msgs::msg::DynamicObjectArray>::SharedPtr subscription_;
    rclcpp::Subscription<tf2_msgs::msg::TFMessage>::SharedPtr subscription_pos_;
    rclcpp::Publisher<autoware_perception_msgs::msg::DynamicObjectArray>::SharedPtr publisher_;
    rclcpp::Publisher<autoware_v2x_msgs::msg::CpmReceptionStatus>::SharedPtr pub_v2x_;

    double pos_lat_;
    double pos_lon_;
  };
}

#endif