// Copyright (c) 2019, Map IV, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of the Map IV, Inc. nor the names of its contributors
//   may be used to endorse or promote products derived from this software
//   without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL COPYRIGHT HOLDER BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/*
 * tag_serial_driver.cpp
 * Tamagawa IMU Driver
 * Author MapIV Sekino
 */

#include "ros/ros.h"
#include "sensor_msgs/Imu.h"
#include "std_msgs/Int32.h"
#include <string>
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <math.h>
#include <stdio.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <boost/asio.hpp>
#include <diagnostic_updater/diagnostic_updater.h>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2/convert.h>
#include <cmath>

using namespace boost::asio;

static std::string device = "/dev/ttyUSB0";
static std::string imu_type = "noGPS";
static std::string rate = "50";
static bool use_fog = false;
static bool ready = false;
static sensor_msgs::Imu imu_msg;
static int16_t imu_status;

static diagnostic_updater::Updater* p_updater;


static void check_bit_error(diagnostic_updater::DiagnosticStatusWrapper& stat) 
{
  uint8_t level = diagnostic_msgs::DiagnosticStatus::OK;
  std::string msg = "OK";

  if (imu_status >> 15)
  {
    level = diagnostic_msgs::DiagnosticStatus::ERROR;
    msg = "Built-In Test error";
  }

  stat.summary(level, msg);
}

static void check_connection(diagnostic_updater::DiagnosticStatusWrapper& stat) 
{
  size_t level = diagnostic_msgs::DiagnosticStatus::OK;
  std::string msg = "OK";

  ros::Time now = ros::Time::now();

  if (now - imu_msg.header.stamp > ros::Duration(1.0)) {
    level = diagnostic_msgs::DiagnosticStatus::ERROR;
    msg = "Message timeout";
  }

  stat.summary(level, msg);
}

void diagnostic_timer_callback(const ros::TimerEvent& event)
{
  if(ready)
  {
    p_updater->force_update();
    ready = false;
  }
}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "tag_serial_driver", ros::init_options::NoSigintHandler);
  ros::NodeHandle nh;
  ros::NodeHandle pnh("~");
  ros::Publisher pub = nh.advertise<sensor_msgs::Imu>("data_raw", 1000);
  io_service io;

  ros::Timer diagnostics_timer = nh.createTimer(ros::Duration(1.0), diagnostic_timer_callback);

  diagnostic_updater::Updater updater;
  p_updater = &updater;
  updater.setHardwareID("tamagawa");
  updater.add("imu_bit_error", check_bit_error);
  updater.add("imu_connection", check_connection);

  pnh.param<std::string>("device", device, "/dev/ttyS0");
  pnh.param<std::string>("imu_type", imu_type, "noGPS");
  pnh.param<std::string>("rate", rate, "50");
  pnh.param<bool>("use_fog", use_fog, true);

  std::cout << "device= " << device << " imu_type= " << imu_type << " rate= " << rate << " use_fog= " << std::boolalpha << use_fog << std::endl;

  serial_port serial_port(io, device);
  serial_port.set_option(serial_port_base::baud_rate(115200));
  serial_port.set_option(serial_port_base::character_size(8));
  serial_port.set_option(serial_port_base::flow_control(serial_port_base::flow_control::none));
  serial_port.set_option(serial_port_base::parity(serial_port_base::parity::none));
  serial_port.set_option(serial_port_base::stop_bits(serial_port_base::stop_bits::one));

  // Data output request to IMU
  std::string wbuf = "$TSC,BIN,";
  wbuf += rate;
  wbuf += "\x0d\x0a";
  serial_port.write_some(buffer(wbuf));
  std::cout << "request: " << wbuf << std::endl;

  imu_msg.header.frame_id = "imu";
  imu_msg.orientation.x = 0.0;
  imu_msg.orientation.y = 0.0;
  imu_msg.orientation.z = 0.0;
  imu_msg.orientation.w = 1.0;

  double roll = M_PI/4;      // ロール (45度)
  double pitch = M_PI/6;     // ピッチ (30度)
  double yaw = M_PI/3;       // ヨー (60度)

  size_t counter;
  int16_t raw_data;
  int32_t raw_data_2;

  while (ros::ok())
  {
    //ros::spinOnce();
    boost::asio::streambuf response;
    boost::asio::read_until(serial_port, response, "\n");
    std::string rbuf(boost::asio::buffers_begin(response.data()), boost::asio::buffers_end(response.data()));

    if (rbuf[5] == 'B' && rbuf[6] == 'I' && rbuf[7] == 'N' && rbuf[8] == ',')
    {
      imu_msg.header.stamp = ros::Time::now();
      if (use_fog)
      {
        ROS_INFO_ONCE("BIN-w/FOG");
        counter = ((rbuf[11] << 24) & 0xFF000000) | ((rbuf[12] << 16) & 0x00FF0000);
        imu_status = ((rbuf[13] << 8) & 0xFFFFFF00) | (rbuf[14] & 0x000000FF);
        raw_data = ((((rbuf[15] << 8) & 0xFFFFFF00) | (rbuf[16] & 0x000000FF)));
        imu_msg.angular_velocity.x =
          raw_data * (200 / pow(2, 15)) * M_PI / 180;  // LSB & unit [deg/s] => [rad/s]
        raw_data = ((((rbuf[17] << 8) & 0xFFFFFF00) | (rbuf[18] & 0x000000FF)));
        imu_msg.angular_velocity.y =
            raw_data * (200 / pow(2, 15)) * M_PI / 180;  // LSB & unit [deg/s] => [rad/s]
        raw_data_2 = ((rbuf[19] << 24) & 0xFF000000) | ((rbuf[20] << 16) & 0x00FF0000) | ((rbuf[21] << 8) & 0x0000FF00) |
                  (rbuf[22] & 0x000000FF);
        imu_msg.angular_velocity.z =
            raw_data_2 * (200 / pow(2, 31)) * M_PI / 180;  // LSB & unit [deg/s] => [rad/s]
        
        raw_data = ((((rbuf[23] << 8) & 0xFFFFFF00) | (rbuf[24] & 0x000000FF)));
        imu_msg.linear_acceleration.x = raw_data * (100 / pow(2, 15));  // LSB & unit [m/s^2]
        raw_data = ((((rbuf[25] << 8) & 0xFFFFFF00) | (rbuf[26] & 0x000000FF)));
        imu_msg.linear_acceleration.y = raw_data * (100 / pow(2, 15));  // LSB & unit [m/s^2]
        raw_data = ((((rbuf[27] << 8) & 0xFFFFFF00) | (rbuf[28] & 0x000000FF)));
        imu_msg.linear_acceleration.z = raw_data * (100 / pow(2, 15));  // LSB & unit [m/s^2]

        raw_data = ((((rbuf[29] << 8) & 0xFFFFFF00) | (rbuf[30] & 0x000000FF)));
        roll  = raw_data * (180 / pow(2, 15))* M_PI / 180.0;  // LSB & unit [rad]
        raw_data = ((((rbuf[31] << 8) & 0xFFFFFF00) | (rbuf[32] & 0x000000FF)));
        pitch = - raw_data * (180 / pow(2, 15))* M_PI / 180.0;  // LSB & unit [rad]
        raw_data = ((((rbuf[33] << 8) & 0xFFFFFF00) | (rbuf[34] & 0x000000FF)));
        yaw = - raw_data * (180 / pow(2, 15))* M_PI / 180.0;  // LSB & unit [rad]

        // オイラー角をクオータニオンに変換
        tf2::Quaternion quaternion;
        quaternion.setRPY(roll, pitch, yaw);  
        
        imu_msg.orientation.x = quaternion.x();
        imu_msg.orientation.y = quaternion.y();
        imu_msg.orientation.z = quaternion.z();
        imu_msg.orientation.w = quaternion.w();
        
        pub.publish(imu_msg);
      }
      else
      {
        ROS_INFO_ONCE("BIN-w/oFOG");
        counter = ((rbuf[11] << 8) & 0x0000FF00) | (rbuf[12] & 0x000000FF);
        imu_status = ((rbuf[13] << 8) & 0xFFFFFF00) | (rbuf[14] & 0x000000FF);
        raw_data = ((((rbuf[15] << 8) & 0xFFFFFF00) | (rbuf[16] & 0x000000FF)));
        imu_msg.angular_velocity.x =
            raw_data * (200 / pow(2, 15)) * M_PI / 180;  // LSB & unit [deg/s] => [rad/s]
        raw_data = ((((rbuf[17] << 8) & 0xFFFFFF00) | (rbuf[18] & 0x000000FF)));
        imu_msg.angular_velocity.y =
            raw_data * (200 / pow(2, 15)) * M_PI / 180;  // LSB & unit [deg/s] => [rad/s]
        raw_data = ((((rbuf[19] << 8) & 0xFFFFFF00) | (rbuf[20] & 0x000000FF)));
        imu_msg.angular_velocity.z =
            raw_data * (200 / pow(2, 15)) * M_PI / 180;  // LSB & unit [deg/s] => [rad/s]
        raw_data = ((((rbuf[21] << 8) & 0xFFFFFF00) | (rbuf[22] & 0x000000FF)));
        imu_msg.linear_acceleration.x = raw_data * (100 / pow(2, 15));  // LSB & unit [m/s^2]
        raw_data = ((((rbuf[23] << 8) & 0xFFFFFF00) | (rbuf[24] & 0x000000FF)));
        imu_msg.linear_acceleration.y = raw_data * (100 / pow(2, 15));  // LSB & unit [m/s^2]
        raw_data = ((((rbuf[25] << 8) & 0xFFFFFF00) | (rbuf[26] & 0x000000FF)));
        imu_msg.linear_acceleration.z = raw_data * (100 / pow(2, 15));  // LSB & unit [m/s^2]
        pub.publish(imu_msg);
        //std::cout << counter << std::endl;
      }
      ready = true;
    }
  }
  return 0;
}
