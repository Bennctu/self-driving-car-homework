#include <ros/ros.h>
#include <cmath>
#include <Eigen/Dense>
#include <visualization_msgs/Marker.h>
#include <iostream>

#include "sensor_msgs/Imu.h"
#include "imuData.h"

using namespace Eigen;
using namespace std;

#define I  MatrixXd::Identity(3,3)

ros::Publisher marker_pub;
ros::Publisher imu_pub;
bool init_time = 1;
vector<Vector3d> positionBuf;
Vector3d g(0,0,9.81);
Vector3d last_velocity;
Vector3d last_position;
Vector3d lastAccInGlobal;
MatrixXd C = I;

void pubodometry(const Eigen::Vector3d &position,const std_msgs::Header &header) {
    visualization_msgs::Marker line_strip;
    line_strip.header.frame_id = "/world";
    line_strip.header.stamp = header.stamp;
    line_strip.ns = "lines";
    line_strip.action = visualization_msgs::Marker::ADD;
    line_strip.pose.orientation.w= 1.0;
    line_strip.id = 1;
    line_strip.type = visualization_msgs::Marker::LINE_STRIP;
    line_strip.scale.x = 0.1;
    // Line strip is blue
    line_strip.color.b = 1.0;
    line_strip.color.a = 1.0;
    ROS_INFO("position: x: %f \t ,y: %f \t ,z: %f",position.x(),position.y(),position.z());
    positionBuf.push_back(position);
    vector<Vector3d>::iterator begin = positionBuf.begin();
    vector<Vector3d>::iterator end = positionBuf.end();
    vector<Vector3d>::iterator it;
    //Output total position data in positionBuf
    for (it = begin ; it != end; it++) {
        geometry_msgs::Point p;
        Vector3d pose = *it;
        p.x = pose.x();
        p.y = pose.y();
        p.z = pose.z();
        line_strip.points.push_back(p);
    }
    marker_pub.publish(line_strip);
}

void tfImuToZed(const sensor_msgs::Imu &pi ,sensor_msgs::Imu &po) {
    sensor_msgs::Imu imu_data = pi;
    Matrix3d imuToCam(3,3);
    Matrix3d camToZed(3,3);
    imuToCam <<0.0225226,0.999745,0.0017194,
               0.0648765,-0.00317777,0.997888,
               0.997639 ,-0.0263635 ,-0.0649315;
    camToZed << 0,0,1,
               -1,0,0,
               0,-1,0;
    Quaterniond Q_imuToCam(imuToCam);
    Quaterniond Q_camToZed(camToZed);
    Quaterniond imuOri(imu_data.orientation.w,imu_data.orientation.x,imu_data.orientation.y,imu_data.orientation.z);
    Vector3d acc(imu_data.linear_acceleration.x,imu_data.linear_acceleration.y,imu_data.linear_acceleration.z);
    Vector3d angular_vel(imu_data.angular_velocity.x,imu_data.angular_velocity.y,imu_data.angular_velocity.z);
    acc = camToZed * imuToCam * acc;
    angular_vel = camToZed * imuToCam * angular_vel;
    imuOri = Q_camToZed * Q_imuToCam * imuOri;
    po.linear_acceleration.x = acc.x();
    po.linear_acceleration.y = acc.y();
    po.linear_acceleration.z = acc.z();
    po.angular_velocity.x = angular_vel.x();
    po.angular_velocity.y = angular_vel.y();
    po.angular_velocity.z = angular_vel.z();
    po.orientation.w = imuOri.w();
    po.orientation.x = imuOri.x();
    po.orientation.y = imuOri.y();
    po.orientation.z = imuOri.z();
}

IMU_content* IMU_last = new IMU_content();
void data_callback(const sensor_msgs::Imu::ConstPtr &msg) {
    sensor_msgs::Imu imu_data = *msg;
    
    //Rotate sensor data from body frame to Zed frame
    tfImuToZed(imu_data,imu_data);
    imu_pub.publish(imu_data);
    IMU_content* IMU_current = new IMU_content(imu_data);
    std_msgs::Header header = imu_data.header;

    //initialization
    if (init_time) {
        IMU_last->moment = imu_data.header.stamp.toSec();
        IMU_last->acc.x() = imu_data.linear_acceleration.x;
        IMU_last->acc.y() = imu_data.linear_acceleration.y;
        IMU_last->acc.z() = imu_data.linear_acceleration.z;
        IMU_last->angular_vel.x() = imu_data.angular_velocity.x;
        IMU_last->angular_vel.y() = imu_data.angular_velocity.y;
        IMU_last->angular_vel.z() = imu_data.angular_velocity.z;
        last_velocity.setZero();
        last_position.setZero();
        lastAccInGlobal.x() = imu_data.linear_acceleration.x;
        lastAccInGlobal.y() = imu_data.linear_acceleration.y;
        lastAccInGlobal.z() = imu_data.linear_acceleration.z;
        positionBuf.push_back(last_position);
        delete IMU_current;
        IMU_current = NULL;
        init_time = 0;
        return;
    }

    //midpoint integration
    double dt = IMU_current->moment - IMU_last->moment;
    Vector3d omega_mean = 0.5*(IMU_last->angular_vel + IMU_current->angular_vel);
    MatrixXd B(3,3);
    B<< 0, -omega_mean.z()*dt, omega_mean.y()*dt,
      omega_mean.z()*dt, 0, -omega_mean.x()*dt,
      -omega_mean.y()*dt, omega_mean.x()*dt,0;
    double sigma = dt*omega_mean.norm();
    C = C*(I + ((sin(sigma))/sigma)*B + ((1-cos(sigma))/(sigma*sigma))*B*B);
    Vector3d currentAccInGlobal = C*IMU_current->acc;
    
    //rotate gravity from body frame to Zed frame
    Matrix3d imuToCam(3,3);
    Matrix3d camToZed(3,3);
    imuToCam <<0.0225226,0.999745,0.0017194,
               0.0648765,-0.00317777,0.997888,
               0.997639 ,-0.0263635 ,-0.0649315;
    camToZed << 0,0,1,
               -1,0,0,
               0,-1,0;
    Vector3d current_velocity = last_velocity + dt*(0.5*(lastAccInGlobal + currentAccInGlobal) - camToZed*imuToCam*g);
    Vector3d current_position = last_position + dt*0.5*(last_velocity + current_velocity);

    //Output to rviz
    pubodometry(current_position,header);

    //terminaion
    IMU_last->angular_vel = IMU_current->angular_vel;
    IMU_last->acc = IMU_current->acc;
    lastAccInGlobal = currentAccInGlobal;
    last_velocity = current_velocity;
    last_position = current_position;
    IMU_last->moment = IMU_current->moment;
    delete IMU_current;
    IMU_current = NULL;
}

int main( int argc, char** argv )
{
    ros::init(argc, argv, "IMU_Integrator");
    ros::NodeHandle n;
    ros::Subscriber IMU_raw_sub = n.subscribe<sensor_msgs::Imu>("/imu/data",200,data_callback);
    marker_pub = n.advertise<visualization_msgs::Marker>("visualization_marker", 200);
    imu_pub = n.advertise<sensor_msgs::Imu>("/imu_data",200);
    ros::spin();
    return 0;
}
