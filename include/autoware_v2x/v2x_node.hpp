#ifndef V2X_NODE_HPP_EUIC2VFR
#define V2X_NODE_HPP_EUIC2VFR

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"
#include "autoware_auto_perception_msgs/msg/predicted_objects.hpp"
#include "tf2_msgs/msg/tf_message.hpp"
#include <boost/asio/io_service.hpp>
#include "autoware_v2x/v2x_app.hpp"
#include "autoware_v2x/cpm_application.hpp"
#include "autoware_v2x/tcpip_application.hpp"
#include "autoware_v2x/time_trigger.hpp"
#include "autoware_v2x/link_layer.hpp"
#include "autoware_v2x/ethernet_device.hpp"
#include "autoware_v2x/positioning.hpp"
#include "autoware_v2x/security.hpp"
#include "autoware_v2x/router_context.hpp"
#include <fstream>

#include <vanetza/asn1/cpm.hpp>

namespace v2x
{
  class V2XNode : public rclcpp::Node
  {
  public:
    explicit V2XNode(const rclcpp::NodeOptions &node_options);
    V2XApp *app;
    void publishObjects(std::vector<CpmApplication::Object> *, int cpm_num);
    void publishCpmSenderObject(double, double, double);

    TcpIpApplication *tcpip_app;
    CpmApplication *cpm_app;
    
    std::ofstream latency_log_file;

    vanetza::asn1::Cpm cpm_;
    vanetza::asn1::Cpm cpm_received_lte_;

    bool cpm_initialized;

  private:
    void objectsCallback(const autoware_auto_perception_msgs::msg::PredictedObjects::ConstSharedPtr msg);
    void tfCallback(const tf2_msgs::msg::TFMessage::ConstSharedPtr msg);

    rclcpp::Subscription<autoware_auto_perception_msgs::msg::PredictedObjects>::SharedPtr objects_sub_;
    rclcpp::Subscription<tf2_msgs::msg::TFMessage>::SharedPtr tf_sub_;
    rclcpp::Publisher<autoware_auto_perception_msgs::msg::PredictedObjects>::SharedPtr cpm_objects_pub_;
    rclcpp::Publisher<autoware_auto_perception_msgs::msg::PredictedObjects>::SharedPtr cpm_sender_pub_;

    double pos_lat_;
    double pos_lon_;

    bool is_sender_;
  };
}

#endif