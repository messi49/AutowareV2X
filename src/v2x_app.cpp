#include "autoware_v2x/v2x_app.hpp"
#include "autoware_v2x/v2x_node.hpp"
#include "autoware_v2x/time_trigger.hpp"
#include "autoware_v2x/router_context.hpp"
#include "autoware_v2x/positioning.hpp"
#include "autoware_v2x/security.hpp"
#include "autoware_v2x/link_layer.hpp"
#include "autoware_v2x/cpm_application.hpp"

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"

#include "tf2/LinearMath/Quaternion.h"
#include "tf2/LinearMath/Matrix3x3.h"

#include <vanetza/asn1/cpm.hpp>
#include <vanetza/facilities/cpm_functions.hpp>
#include <sstream>
#include <memory>
#include <GeographicLib/UTMUPS.hpp>
#include <GeographicLib/MGRS.hpp>
#include <boost/thread.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace gn = vanetza::geonet;

using namespace vanetza;
using namespace vanetza::facilities;
using namespace std::chrono;

namespace v2x
{
  V2XApp::V2XApp(V2XNode *node, rclcpp::Publisher<autoware_v2x_msgs::msg::CpmReceptionStatus>::SharedPtr pub_v2x) : 
    node_(node),
    pub_v2x_(pub_v2x),
    tf_received_(false),
    tf_interval_(0),
    cp_started_(false)
  {
  }

  void V2XApp::objectsCallback(const autoware_perception_msgs::msg::DynamicObjectArray::ConstSharedPtr msg) {
    // RCLCPP_INFO(node_->get_logger(), "V2XApp: msg received");
    if (!tf_received_) {
      RCLCPP_WARN(node_->get_logger(), "[V2XApp::objectsCallback] tf not received yet");
    }
    if (tf_received_ && cp_started_) {
      cp->updateObjectsStack(msg);
    }
  }

  void V2XApp::tfCallback(const tf2_msgs::msg::TFMessage::ConstSharedPtr msg) {

    tf_received_ = true;

    double x = msg->transforms[0].transform.translation.x;
    double y = msg->transforms[0].transform.translation.y;
    double z = msg->transforms[0].transform.translation.z;

    long long timestamp_sec = msg->transforms[0].header.stamp.sec; // seconds
    long long timestamp_nsec = msg->transforms[0].header.stamp.nanosec; // nanoseconds
    timestamp_sec -= 1072915200; // convert to etsi-epoch
    long long timestamp_msec = timestamp_sec * 1000 + timestamp_nsec / 1000000;
    int gdt = timestamp_msec % 65536;
    // RCLCPP_INFO(node_->get_logger(), "[tfCallback] %d,%lld,%lld,%lld", gdt, timestamp_msec, timestamp_sec, timestamp_nsec);

    double rot_x = msg->transforms[0].transform.rotation.x;
    double rot_y = msg->transforms[0].transform.rotation.y;
    double rot_z = msg->transforms[0].transform.rotation.z;
    double rot_w = msg->transforms[0].transform.rotation.w;

    // Convert the quarternion to euler (yaw, pitch, roll)
    tf2::Quaternion quat(rot_x, rot_y, rot_z, rot_w);
    tf2::Matrix3x3 matrix(quat);
    double roll, pitch, yaw;
    matrix.getRPY(roll, pitch, yaw);

    char mgrs[20];
    int zone, prec;
    bool northp;
    double x_mgrs, y_mgrs;
    double lat, lon;
    sprintf(mgrs, "54SVE%.5d%.5d", (int)x, (int)y);

    GeographicLib::MGRS::Reverse(mgrs, zone, northp, x_mgrs, y_mgrs, prec);
    GeographicLib::UTMUPS::Reverse(zone, northp, x_mgrs, y_mgrs, lat, lon);

    if (cp && cp_started_) {
      cp->updateMGRS(&x, &y);
      cp->updateRP(&lat, &lon, &z);
      cp->updateHeading(&yaw);
      cp->updateGenerationDeltaTime(&gdt, &timestamp_msec);
    }
  }

  void V2XApp::start() {
    RCLCPP_INFO(node_->get_logger(), "V2X App Launched");

    boost::asio::io_service io_service;
    TimeTrigger trigger(io_service);

    std::string network_interface;
    node_->get_parameter("network_interface", network_interface);
    RCLCPP_INFO(node_->get_logger(), "Network Interface: %s", network_interface.c_str());

    EthernetDevice device(network_interface.c_str());
    vanetza::MacAddress mac_address = device.address();

    std::stringstream sout;
    sout << mac_address;
    RCLCPP_INFO(node_->get_logger(), "MAC Address: '%s'", sout.str().c_str());

    gn::MIB mib;
    mib.itsGnLocalGnAddr.mid(mac_address);
    mib.itsGnLocalGnAddr.is_manually_configured(true);
    mib.itsGnLocalAddrConfMethod = geonet::AddrConfMethod::Managed;
    mib.itsGnSecurity = false;
    mib.itsGnProtocolVersion = 1;

    auto link_layer =  create_link_layer(io_service, device, "ethernet");
    auto positioning = create_position_provider(io_service, trigger.runtime());
    auto security = create_security_entity(trigger.runtime(), *positioning);
    RouterContext context(mib, trigger, *positioning, security.get());

    context.set_link_layer(link_layer.get());

    cp = new CpmApplication(node_, trigger.runtime(), pub_v2x_);
    
    context.enable(cp);

    cp_started_ = true;

    io_service.run();
  }
}