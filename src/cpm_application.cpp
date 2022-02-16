#include "autoware_v2x/cpm_application.hpp"
#include "autoware_v2x/positioning.hpp"
#include "autoware_v2x/security.hpp"
#include "autoware_v2x/link_layer.hpp"
#include "autoware_v2x/v2x_node.hpp"

#include "tf2/LinearMath/Quaternion.h"
#include "tf2/LinearMath/Matrix3x3.h"

#include "rclcpp/rclcpp.hpp"
#include <vanetza/btp/ports.hpp>
#include <vanetza/asn1/cpm.hpp>
#include <vanetza/asn1/packet_visitor.hpp>
#include <vanetza/facilities/cpm_functions.hpp>
#include <chrono>
#include <functional>
#include <iostream>
#include <sstream>
#include <exception>
#include <GeographicLib/UTMUPS.hpp>
#include <GeographicLib/MGRS.hpp>
#include <string>

#define _USE_MATH_DEFINES
#include <math.h>

using namespace vanetza;
using namespace vanetza::facilities;
using namespace std::chrono;

namespace v2x {
  CpmApplication::CpmApplication(V2XNode *node, Runtime &rt, rclcpp::Publisher<autoware_v2x_msgs::msg::CpmReceptionStatus>::SharedPtr pub_v2x) :     
    node_(node),
    runtime_(rt),
    pub_v2x_(pub_v2x),
    ego_(),
    generationDeltaTime_(0),
    updating_objects_stack_(false),
    sending_(false),
    is_sender_(true),
    reflect_packet_(false)
  {
    RCLCPP_INFO(node_->get_logger(), "CpmApplication started...");
    set_interval(milliseconds(100));
  }

  void CpmApplication::set_interval(Clock::duration interval) {
    cpm_interval_ = interval;
    runtime_.cancel(this);
    schedule_timer();
  }

  void CpmApplication::schedule_timer() {
    runtime_.schedule(cpm_interval_, std::bind(&CpmApplication::on_timer, this, std::placeholders::_1), this);
  }

  void CpmApplication::on_timer(Clock::time_point) {
    schedule_timer();
    send();
  }

  CpmApplication::PortType CpmApplication::port() {
    return btp::ports::CPM;
  }

  void CpmApplication::indicate(const DataIndication &indication, UpPacketPtr packet) {

    asn1::PacketVisitor<asn1::Cpm> visitor;
    std::shared_ptr<const asn1::Cpm> cpm = boost::apply_visitor(visitor, *packet);

    if (cpm) {
      RCLCPP_INFO(node_->get_logger(), "[CpmApplication::indicate] Received decodable CPM content");

      rclcpp::Time current_time = node_->now();
      RCLCPP_INFO(node_->get_logger(), "[CpmApplication::indicate] [measure] T_receive_r1 %ld", current_time.nanoseconds());

      asn1::Cpm message = *cpm;
      ItsPduHeader_t &header = message->header;

      // Calculate GDT and get GDT from CPM and calculate the "Age of CPM"
      GenerationDeltaTime_t gdt_cpm = message->cpm.generationDeltaTime;
      const auto time_now = duration_cast<milliseconds> (runtime_.now().time_since_epoch());
      uint16_t gdt = time_now.count();
      int gdt_diff = (65536 + (gdt - gdt_cpm) % 65536) % 65536;
      RCLCPP_INFO(node_->get_logger(), "[CpmApplication::indicate] [measure] GDT_CPM: %ld", gdt_cpm);
      RCLCPP_INFO(node_->get_logger(), "[CpmApplication::indicate] [measure] GDT: %u", gdt);
      RCLCPP_INFO(node_->get_logger(), "[CpmApplication::indicate] [measure] T_R1R2: %d", gdt_diff);


      CpmManagementContainer_t &management = message->cpm.cpmParameters.managementContainer;
      double lat = management.referencePosition.latitude / 1.0e7;
      double lon = management.referencePosition.longitude / 1.0e7;
      // RCLCPP_INFO(node_->get_logger(), "cpm.(reference position) = %f, %f", lat, lon);

      std::string mgrs;
      int zone;
      bool northp;
      double x, y;
      GeographicLib::UTMUPS::Forward(lat, lon, zone, northp, x, y);
      GeographicLib::MGRS::Forward(zone, northp, x, y, lat, 5, mgrs);

      int x_mgrs = std::stoi(mgrs.substr(5, 5));
      int y_mgrs = std::stoi(mgrs.substr(10, 5));
      // RCLCPP_INFO(node_->get_logger(), "cpm.(RP).mgrs = %s, %d, %d", mgrs.c_str(), x_mgrs, y_mgrs);

      // Calculate orientation from Heading
      OriginatingVehicleContainer_t &ovc = message->cpm.cpmParameters.stationDataContainer->choice.originatingVehicleContainer;
      int heading = ovc.heading.headingValue;
      // double orientation = 1.5708 - (M_PI * heading / 10) / 180;
      double orientation = (90.0 - heading / 10.0) * M_PI / 180;
      if (orientation < 0.0) orientation += (2 * M_PI);
      // RCLCPP_INFO(node_->get_logger(), "cpm: heading = %d, orientation = %f", heading, orientation);

      // Get PerceivedObjects
      receivedObjectsStack.clear();

      PerceivedObjectContainer_t *&poc = message->cpm.cpmParameters.perceivedObjectContainer;

      if (poc != NULL) {
        for (int i = 0; i < poc->list.count; ++i) {
          RCLCPP_INFO(node_->get_logger(), "[INDICATE] Object: #%d", poc->list.array[i]->objectID);

          CpmApplication::Object object;
          double x1 = poc->list.array[i]->xDistance.value;
          double y1 = poc->list.array[i]->yDistance.value;
          // RCLCPP_INFO(node_->get_logger(), "cpm object: xDistance: %f, yDistance: %f", x1, y1);
          x1 = x1 / 100.0;
          y1 = y1 / 100.0;
          object.position_x = x_mgrs + (cos(orientation) * x1 - sin(orientation) * y1);
          object.position_y = y_mgrs + (sin(orientation) * x1 + cos(orientation) * y1);
          // RCLCPP_INFO(node_->get_logger(), "cpm object: %f, %f, %f, %f", x1, y1, object.position_x, object.position_y);
          
          object.shape_x = poc->list.array[i]->planarObjectDimension2->value;
          object.shape_y = poc->list.array[i]->planarObjectDimension1->value;
          object.shape_z = poc->list.array[i]->verticalObjectDimension->value;

          object.yawAngle = poc->list.array[i]->yawAngle->value;
          double yaw_radian = (M_PI * object.yawAngle / 10) / 180;

          tf2::Quaternion quat;
          quat.setRPY(0, 0, yaw_radian);
          object.orientation_x = quat.x();
          object.orientation_y = quat.y();
          object.orientation_z = quat.z();
          object.orientation_w = quat.w();

          receivedObjectsStack.push_back(object);
        }
        node_->publishObjects(&receivedObjectsStack);
      } else {
        RCLCPP_INFO(node_->get_logger(), "[INDICATE] Empty POC");
      }

      if (reflect_packet_) {
        Application::DownPacketPtr packet{new DownPacket()};
        std::unique_ptr<geonet::DownPacket> payload{new geonet::DownPacket()};

        payload->layer(OsiLayer::Application) = std::move(message);

        Application::DataRequest request;
        request.its_aid = aid::CP;
        request.transport_type = geonet::TransportType::SHB;
        request.communication_profile = geonet::CommunicationProfile::ITS_G5;

        Application::DataConfirm confirm = Application::request(request, std::move(payload), node_);

        if (!confirm.accepted()) {
          throw std::runtime_error("[CpmApplication::indicate] Packet reflection failed");
        }
      }

      auto msg_v2x = autoware_v2x_msgs::msg::CpmReceptionStatus();
      msg_v2x.cpm_receiving = true;
      pub_v2x_->publish(msg_v2x);


    } else {
      RCLCPP_INFO(node_->get_logger(), "[INDICATE] Received broken content");
    }
  }

  void CpmApplication::updateMGRS(double *x, double *y) {
    ego_.mgrs_x = *x;
    ego_.mgrs_y = *y;
  }

  void CpmApplication::updateRP(double *lat, double *lon, double *altitude) {
    ego_.latitude = *lat;
    ego_.longitude = *lon;
    ego_.altitude = *altitude;
  }

  void CpmApplication::updateGenerationDeltaTime(int *gdt, long long *gdt_timestamp) {
    generationDeltaTime_ = *gdt;
    gdt_timestamp_ = *gdt_timestamp; // ETSI-epoch milliseconds timestamp
  }

  void CpmApplication::updateHeading(double *yaw) {
    ego_.heading = *yaw;
  }

  void CpmApplication::updateObjectsStack(const autoware_perception_msgs::msg::DynamicObjectArray::ConstSharedPtr msg) {
    updating_objects_stack_ = true;

    if (sending_) {
      RCLCPP_INFO(node_->get_logger(), "updateObjectsStack Skipped...");
      return;
    } else {
      objectsStack.clear();
    }

    if (msg->objects.size() > 0) {
      int i = 0;
      for (auto obj : msg->objects) {
        CpmApplication::Object object;
        object.objectID = i;
        object.timestamp = msg->header.stamp;
        object.position_x = obj.state.pose_covariance.pose.position.x; // MGRS
        object.position_y = obj.state.pose_covariance.pose.position.y;
        object.position_z = obj.state.pose_covariance.pose.position.z;
        object.orientation_x = obj.state.pose_covariance.pose.orientation.x;
        object.orientation_y = obj.state.pose_covariance.pose.orientation.y;
        object.orientation_z = obj.state.pose_covariance.pose.orientation.z;
        object.orientation_w = obj.state.pose_covariance.pose.orientation.w;
        object.shape_x = std::lround(obj.shape.dimensions.x * 10.0);
        object.shape_y = std::lround(obj.shape.dimensions.y * 10.0);
        object.shape_z = std::lround(obj.shape.dimensions.z * 10.0);
        // object.xDistance = std::lround(((object.position_x - ego_x_) * cos(-ego_heading_) - (object.position_y - ego_y_) * sin(-ego_heading_)) * 100.0);
        // object.yDistance = std::lround(((object.position_x - ego_x_) * sin(-ego_heading_) + (object.position_y - ego_y_) * cos(-ego_heading_)) * 100.0);
        object.xDistance = std::lround(((object.position_x - ego_.mgrs_x) * cos(-ego_.heading) - (object.position_y - ego_.mgrs_y) * sin(-ego_.heading)) * 100.0);
        object.yDistance = std::lround(((object.position_x - ego_.mgrs_x) * sin(-ego_.heading) + (object.position_y - ego_.mgrs_y) * cos(-ego_.heading)) * 100.0);
        if (object.xDistance < -132768 || object.xDistance > 132767) {
          continue;
        }
        if (object.yDistance < -132768 || object.yDistance > 132767) {
          continue;
        }
        object.xSpeed = 0;
        object.ySpeed = 0;

        tf2::Quaternion quat(object.orientation_x, object.orientation_y, object.orientation_z, object.orientation_w);
        tf2::Matrix3x3 matrix(quat);
        double roll, pitch, yaw;
        matrix.getRPY(roll, pitch, yaw);
        if (yaw < 0) {
          object.yawAngle = std::lround(((yaw + 2*M_PI) * 180.0 / M_PI) * 10.0); // 0 - 3600
        } else {
          object.yawAngle = std::lround((yaw * 180.0 / M_PI) * 10.0); // 0 - 3600
        }
        
        long long msg_timestamp_sec = msg->header.stamp.sec;
        long long msg_timestamp_nsec = msg->header.stamp.nanosec;
        msg_timestamp_sec -= 1072915200; // convert to etsi-epoch
        long long msg_timestamp_msec = msg_timestamp_sec * 1000 + msg_timestamp_nsec / 1000000;
        // long long timestamp = msg->header.stamp.sec * 1e9 + msg->header.stamp.nanosec;
        object.timeOfMeasurement = gdt_timestamp_ - msg_timestamp_msec;
        // RCLCPP_INFO(node_->get_logger(), "[updateObjectsStack] %ld %ld %d", gdt_timestamp_, timestamp, object.timeOfMeasurement);
        if (object.timeOfMeasurement < -1500 || object.timeOfMeasurement > 1500) {
          RCLCPP_INFO(node_->get_logger(), "[updateObjectsStack] timeOfMeasurement out of bounds: %d", object.timeOfMeasurement);
          continue;
        }
        objectsStack.push_back(object);
        ++i;
        
        RCLCPP_INFO(node_->get_logger(), "Added to stack: #%d (%d, %d) (%d, %d) (%d, %d, %d) (%f: %d)", object.objectID, object.xDistance, object.yDistance, object.xSpeed, object.ySpeed, object.shape_x, object.shape_y, object.shape_z, yaw, object.yawAngle);
      }
    }
    RCLCPP_INFO(node_->get_logger(), "ObjectsStack: %d objects", objectsStack.size());
    rclcpp::Time current_time = node_->now();
    RCLCPP_INFO(node_->get_logger(), "[CpmApplication::updateObjectsStack] [measure] T_objstack_updated %ld", current_time.nanoseconds());
    updating_objects_stack_ = false;
  }

  void CpmApplication::send() {

    if (is_sender_) {
      sending_ = true;
      
      RCLCPP_INFO(node_->get_logger(), "[CpmApplication::send] Sending CPM...");

      vanetza::asn1::Cpm message;

      // ITS PDU Header
      ItsPduHeader_t &header = message->header;
      header.protocolVersion = 1;
      header.messageID = 14;
      header.stationID = 1;

      CollectivePerceptionMessage_t &cpm = message->cpm;

      // Set GenerationDeltaTime
      cpm.generationDeltaTime = generationDeltaTime_ * GenerationDeltaTime_oneMilliSec;

      CpmManagementContainer_t &management = cpm.cpmParameters.managementContainer;
      management.stationType = StationType_passengerCar;
      PositionFix fix;
      fix.latitude = ego_.latitude * units::degree;
      fix.longitude = ego_.longitude * units::degree;
      fix.confidence.semi_major = 1.0 * units::si::meter;
      fix.confidence.semi_minor = fix.confidence.semi_major;
      copy(fix, management.referencePosition);
      cpm.cpmParameters.numberOfPerceivedObjects = objectsStack.size();

      StationDataContainer_t *&sdc = cpm.cpmParameters.stationDataContainer;
      sdc = vanetza::asn1::allocate<StationDataContainer_t>();
      sdc->present = StationDataContainer_PR_originatingVehicleContainer;
      OriginatingVehicleContainer_t &ovc = sdc->choice.originatingVehicleContainer;
      ovc.speed.speedValue = 0;
      ovc.speed.speedConfidence = 1;
      int heading = std::lround(((-ego_.heading * 180.0 / M_PI) + 90.0) * 10.0);
      if (heading < 0) heading += 3600;
      ovc.heading.headingValue = heading;
      ovc.heading.headingConfidence = 1;

      if (objectsStack.size() > 0) {
        PerceivedObjectContainer_t *&poc = cpm.cpmParameters.perceivedObjectContainer;
        poc = vanetza::asn1::allocate<PerceivedObjectContainer_t>();

        for (CpmApplication::Object object : objectsStack) {
          PerceivedObject *pObj = vanetza::asn1::allocate<PerceivedObject>();
          pObj->objectID = object.objectID;
          pObj->timeOfMeasurement = object.timeOfMeasurement;
          pObj->xDistance.value = object.xDistance;
          pObj->xDistance.confidence = 1;
          pObj->yDistance.value = object.yDistance;
          pObj->yDistance.confidence = 1;
          pObj->xSpeed.value = object.xSpeed;
          pObj->xSpeed.confidence = 1;
          pObj->ySpeed.value = object.ySpeed;
          pObj->ySpeed.confidence = 1;

          pObj->planarObjectDimension1 = vanetza::asn1::allocate<ObjectDimension_t>();
          pObj->planarObjectDimension2 = vanetza::asn1::allocate<ObjectDimension_t>();
          pObj->verticalObjectDimension = vanetza::asn1::allocate<ObjectDimension_t>();

          (*(pObj->planarObjectDimension1)).value = object.shape_y;
          (*(pObj->planarObjectDimension1)).confidence = 1;
          (*(pObj->planarObjectDimension2)).value = object.shape_x;
          (*(pObj->planarObjectDimension2)).confidence = 1;
          (*(pObj->verticalObjectDimension)).value = object.shape_z;
          (*(pObj->verticalObjectDimension)).confidence = 1;

          pObj->yawAngle = vanetza::asn1::allocate<CartesianAngle>();
          (*(pObj->yawAngle)).value = object.yawAngle;
          (*(pObj->yawAngle)).confidence = 1;

          RCLCPP_INFO(node_->get_logger(), "[SEND] Added: #%d (%d, %d) (%d, %d) (%d, %d, %d) %d", object.objectID, object.xDistance, object.yDistance, object.xSpeed, object.ySpeed, object.shape_y, object.shape_x, object.shape_z, object.yawAngle);

          ASN_SEQUENCE_ADD(poc, pObj);
        }
      } else {
        cpm.cpmParameters.perceivedObjectContainer = NULL;
        RCLCPP_INFO(node_->get_logger(), "[CpmApplication::send] Empty POC");
      }
      
      Application::DownPacketPtr packet{new DownPacket()};
      std::unique_ptr<geonet::DownPacket> payload{new geonet::DownPacket()};

      payload->layer(OsiLayer::Application) = std::move(message);

      Application::DataRequest request;
      request.its_aid = aid::CP;
      request.transport_type = geonet::TransportType::SHB;
      request.communication_profile = geonet::CommunicationProfile::ITS_G5;

      Application::DataConfirm confirm = Application::request(request, std::move(payload), node_);

      if (!confirm.accepted()) {
        throw std::runtime_error("[CpmApplication::send] CPM application data request failed");
      }

      sending_ = false;
      rclcpp::Time current_time = node_->now();
      RCLCPP_INFO(node_->get_logger(), "[CpmApplication::send] [measure] T_depart_r1 %ld", current_time.nanoseconds());

    }
  }
}