/*******************************************************
 * Copyright (C) 2019, Aerial Robotics Group, Hong Kong University of Science and Technology
 *
 * This file is part of VINS.
 *
 * Licensed under the GNU General Public License v3.0;
 * you may not use this file except in compliance with the License.
 *
 * Author: Qin Tong (qintonguav@gmail.com)
 *
 * Reference:
 * Thread: http://www.runoob.com/w3cnote/cpp-std-thread.html
 *******************************************************/

#include <iostream>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <iomanip>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <opencv2/opencv.hpp>

#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/kdtree/kdtree_flann.h>

#include <ros/ros.h>
#include <std_msgs/Header.h>
#include <std_msgs/Float32.h>
#include <std_msgs/Bool.h>
#include <nav_msgs/Odometry.h>
#include <nav_msgs/Path.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/Imu.h>

#include "common/common.hpp"
#include "estimator/estimator.h"
#include "estimator/parameters.h"
#include "utility/visualization.h"
#include "utility/cloud_visualizer.h"

using namespace std;

Estimator estimator;

// message buffer
queue<sensor_msgs::PointCloud2ConstPtr> cloud0_buf;
queue<sensor_msgs::PointCloud2ConstPtr> cloud1_buf;
std::mutex m_buf;

// laser path groundtruth
nav_msgs::Path laser_path;
ros::Publisher pub_laser_path;
Pose pose_world_ref_ini;

void cloud0_callback(const sensor_msgs::PointCloud2ConstPtr &cloud_msg)
{
    m_buf.lock();
    cloud0_buf.push(cloud_msg);
    m_buf.unlock();
}

void cloud1_callback(const sensor_msgs::PointCloud2ConstPtr &cloud_msg)
{
    m_buf.lock();
    cloud1_buf.push(cloud_msg);
    m_buf.unlock();
}

pcl::PointCloud<pcl::PointXYZ> getCloudFromMsg(const sensor_msgs::PointCloud2ConstPtr &cloud_msg)
{
    pcl::PointCloud<pcl::PointXYZ> laser_cloud;
    pcl::fromROSMsg(*cloud_msg, laser_cloud);
    std::vector<int> indices;
    pcl::removeNaNFromPointCloud(laser_cloud, laser_cloud, indices);
    if (ROI_RANGE < 5)
    {
        common::removeROIPointCloud(laser_cloud, laser_cloud, ROI_RANGE, "inside");
    }
    else
    {
        common::removeROIPointCloud(laser_cloud, laser_cloud, ROI_RANGE, "outside");
    }
    return laser_cloud;
}

// extract images with same timestamp from two topics
// independent from ros::spin()
void sync_process()
{
    while(1)
    {
        if(NUM_OF_LASER == 2)
        {
            pcl::PointCloud<pcl::PointXYZ> laser_cloud0, laser_cloud1;
            std::vector<pcl::PointCloud<pcl::PointXYZ> >v_laser_cloud;
            std_msgs::Header header;
            double time = 0;
            m_buf.lock();
            if (!cloud0_buf.empty() && !cloud1_buf.empty())
            {
                double time0 = cloud0_buf.front()->header.stamp.toSec();
                double time1 = cloud1_buf.front()->header.stamp.toSec();
                printf("\ntimestamps: (%.3f, %.3f)\n", time0, time1);
                // 0.07s sync tolerance
                if(time0 < time1 - LASER_SYNC_THRESHOLD)
                {
                    cloud0_buf.pop();
                    printf("throw cloud0\n");
                }
                else if(time0 > time1 + LASER_SYNC_THRESHOLD)
                {
                    cloud1_buf.pop();
                    printf("throw cloud1\n");
                }
                else
                {
                    time = cloud0_buf.front()->header.stamp.toSec();
                    header = cloud0_buf.front()->header;

                    laser_cloud0 = getCloudFromMsg(cloud0_buf.front());
                    v_laser_cloud.push_back(laser_cloud0);
                    cloud0_buf.pop();

                    laser_cloud1 = getCloudFromMsg(cloud1_buf.front());
                    v_laser_cloud.push_back(laser_cloud1);
                    cloud1_buf.pop();
                    printf("size of finding laser_cloud0: %d, laser_cloud1: %d\n", laser_cloud0.points.size(), laser_cloud1.points.size());
                }
            }
            m_buf.unlock();
            if ((laser_cloud0.points.size() != 0) && (laser_cloud1.points.size() != 0))
            {
                estimator.inputCloud(time, v_laser_cloud);
            }
        }
        else if (NUM_OF_LASER == 1)
        {
            pcl::PointCloud<pcl::PointXYZ> laser_cloud0;
            std_msgs::Header header;
            double time = 0;
            m_buf.lock();
            if (!cloud0_buf.empty())
            {
                time = cloud0_buf.front()->header.stamp.toSec();
                header = cloud0_buf.front()->header;
                laser_cloud0 = getCloudFromMsg(cloud0_buf.front());
                cloud0_buf.pop();
                printf("find laser_cloud0: %d \n", laser_cloud0.points.size());
            }
            m_buf.unlock();
            if (laser_cloud0.points.size() != 0)
            {
                estimator.inputCloud(time, laser_cloud0);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void restart_callback(const std_msgs::BoolConstPtr &restart_msg)
{
    if (restart_msg->data != 0)
    {
        ROS_WARN("restart the estimator!");
        estimator.clearState();
        estimator.setParameter();
    }
}

void odom_gt_callback(const nav_msgs::Odometry &odom_msg)
{
    Pose pose_world_base(odom_msg);
    Pose pose_base_ref(Eigen::Quaterniond(0.976, -0.216, 0, 0), Eigen::Vector3d(0, 0.266, 0.734));
    Pose pose_world_ref(pose_world_base * pose_base_ref);
    if (laser_path.poses.size() == 0) pose_world_ref_ini = pose_world_ref;
    Pose pose_ref_ini_cur(pose_world_ref_ini.inverse() * pose_world_ref);

    geometry_msgs::PoseStamped laser_pose;
    laser_pose.header = odom_msg.header;
    laser_pose.header.frame_id = "/world";
    laser_pose.pose.orientation.x = pose_ref_ini_cur.q_.x();
    laser_pose.pose.orientation.y = pose_ref_ini_cur.q_.y();
    laser_pose.pose.orientation.z = pose_ref_ini_cur.q_.z();
    laser_pose.pose.orientation.w = pose_ref_ini_cur.q_.w();
    laser_pose.pose.position.x = pose_ref_ini_cur.t_(0);
    laser_pose.pose.position.y = pose_ref_ini_cur.t_(1);
    laser_pose.pose.position.z = pose_ref_ini_cur.t_(2);
    laser_path.header = laser_pose.header;
    laser_path.poses.push_back(laser_pose);
    pub_laser_path.publish(laser_path);
    estimator.laser_path_gt_ = laser_path;
}

int main(int argc, char **argv)
{
    if(argc < 2)
    {
        cout << "please intput: rosrun mlod mlod_node [config file] " << endl
             << "for example: "
             << "rosrun mloam mloam_node "
             << "~/catkin_ws/src/M-LOAM/config/config_handheld.yaml" << endl;
        return 1;
    }

    google::InitGoogleLogging(argv[0]);
    google::ParseCommandLineFlags(&argc, &argv, true);

    ros::init(argc, argv, "mloam_node");
    ros::NodeHandle n("~");
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);

    string config_file = argv[1];
    cout << "config_file: " << argv[1] << endl;
    readParameters(config_file);
    estimator.setParameter();

// #ifdef EIGEN_DONT_PARALLELIZE
//     ROS_DEBUG("EIGEN_DONT_PARALLELIZE");
// #endif
    ROS_WARN("waiting for cloud...");

    registerPub(n);
    ros::Subscriber sub_cloud0 = n.subscribe(CLOUD0_TOPIC, 100, cloud0_callback);
    ros::Subscriber sub_cloud1 = n.subscribe(CLOUD1_TOPIC, 100, cloud1_callback);
    ros::Subscriber sub_restart = n.subscribe("/mlod_restart", 100, restart_callback);
    ros::Subscriber sub_odom_gt = n.subscribe("/base_odom_gt", 100, odom_gt_callback);

    pub_laser_path = n.advertise<nav_msgs::Path>("/laser_odom_path_gt", 100);

    std::thread sync_thread(sync_process);
    std::thread cloud_visualizer_thread;
    if (PCL_VIEWER)
    {
        cloud_visualizer_thread = std::thread(&PlaneNormalVisualizer::Spin, &estimator.plane_normal_vis_);
    }
    ros::spin();

    cloud_visualizer_thread.join();
    sync_thread.join();

    return 0;

}




//
