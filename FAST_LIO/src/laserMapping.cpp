// This is an advanced implementation of the algorithm described in the
// following paper:
//   J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
//     Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.

// Modifier: Livox               dev@livoxtech.com

// Copyright 2013, Ji Zhang, Carnegie Mellon University
// Further contributions copyright (c) 2016, Southwest Research Institute
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
#include <omp.h>
#include <mutex>
#include <math.h>
#include <thread>
#include <fstream>
#include <csignal>
#include <chrono>
#include <unistd.h>
#include <Python.h>
#include <so3_math.h>
#include <rclcpp/rclcpp.hpp>
#include <Eigen/Core>

#include "IMU_Processing.hpp"
#include <nav_msgs/msg/odometry.hpp>
#include <nav_msgs/msg/path.hpp>
#include <visualization_msgs/msg/marker.hpp>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/io/pcd_io.h>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <std_srvs/srv/trigger.hpp>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2/convert.h>
#include <tf2_ros/transform_listener.h>
#include <tf2_ros/buffer.h>
#include <tf2_ros/create_timer_ros.h>
#include <tf2_sensor_msgs/tf2_sensor_msgs.hpp>
#include <geometry_msgs/msg/transform_stamped.hpp>
#include <geometry_msgs/msg/vector3.hpp>
#include "preprocess.h"
#include <ikd-Tree/ikd_Tree.h>
#include <pcl/common/transforms.h>  
#include <pcl/kdtree/kdtree_flann.h>

#include <geometry_msgs/msg/pose_stamped.hpp>
#include <tf2_eigen/tf2_eigen.hpp>
#include <Eigen/Geometry>

#include "std_msgs/msg/u_int32.hpp"

#include <map>
#include <unordered_map>

#define INIT_TIME           (0.1)
#define LASER_POINT_COV     (0.001)
#define MAXN                (720000)
#define PUBFRAME_PERIOD     (20)

/*** Time Log Variables ***/
double kdtree_incremental_time = 0.0, kdtree_search_time = 0.0, kdtree_delete_time = 0.0;
double T1[MAXN], s_plot[MAXN], s_plot2[MAXN], s_plot3[MAXN], s_plot4[MAXN], s_plot5[MAXN], s_plot6[MAXN], s_plot7[MAXN], s_plot8[MAXN], s_plot9[MAXN], s_plot10[MAXN], s_plot11[MAXN];
double match_time = 0, solve_time = 0, solve_const_H_time = 0;
int    kdtree_size_st = 0, kdtree_size_end = 0, add_point_size = 0, kdtree_delete_counter = 0;
bool   runtime_pos_log = false, pcd_save_en = false, time_sync_en = false, extrinsic_est_en = true, path_en = true;
bool   traj_save_en = false;
/**************************/

float res_last[100000] = {0.0};
float DET_RANGE = 300.0f;
const float MOV_THRESHOLD = 1.5f;
double time_diff_lidar_to_imu = 0.0;

mutex mtx_buffer;
condition_variable sig_buffer;

string root_dir = ROOT_DIR;
string map_file_path, lid_topic, imu_topic, keyFrame_topic, keyFrame_id_topic;
string traj_file_path;

double res_mean_last = 0.05, total_residual = 0.0;
double last_timestamp_lidar = 0, last_timestamp_imu = -1.0;
double gyr_cov = 0.1, acc_cov = 0.1, b_gyr_cov = 0.0001, b_acc_cov = 0.0001;
double filter_size_surf_min = 0, filter_size_map_min = 0, fov_deg = 0;
double cube_len = 0, HALF_FOV_COS = 0, FOV_DEG = 0, total_distance = 0, lidar_end_time = 0, first_lidar_time = 0.0;
int    effct_feat_num = 0, time_log_counter = 0, scan_count = 0, publish_count = 0;
int    iterCount = 0, feats_down_size = 0, NUM_MAX_ITERATIONS = 0, laserCloudValidNum = 0, pcd_save_interval = -1, pcd_index = 0;
bool   point_selected_surf[100000] = {0};
bool   lidar_pushed, flg_first_scan = true, flg_exit = false, flg_EKF_inited;
bool   scan_pub_en = false, dense_pub_en = false, scan_body_pub_en = false, fusion_pub_en = false;
bool    is_first_lidar = true;

bool    recontructKdTree = false;
bool    updateState = false;
bool    pub_odom_transform = false;
int     updateFrequency = 100;
int KeyidCounter =  0;



int FusionBufferSize = 3;
int LcFreqcount = 0;

string odom_frame_id, base_frame_id, lidar_frame_id, keyframe_topic, keyframe_id_topic;


vector<vector<int>>  pointSearchInd_surf; 
vector<BoxPointType> cub_needrm;
vector<PointVector>  Nearest_Points; 
vector<double>       extrinT(3, 0.0);
vector<double>       extrinR(9, 0.0);
deque<double>                     time_buffer;
deque<PointCloudXYZI::Ptr>        lidar_buffer;
deque<sensor_msgs::msg::Imu::ConstSharedPtr> imu_buffer;

deque<PointCloudXYZI::Ptr> FusionLaserPointBuffer;
 
int FusionbufferIndex = 0;
bool debug_print;

PointCloudXYZI::Ptr featsFromMap(new PointCloudXYZI());
PointCloudXYZI::Ptr feats_undistort(new PointCloudXYZI());
PointCloudXYZI::Ptr feats_down_body(new PointCloudXYZI());
PointCloudXYZI::Ptr feats_down_world(new PointCloudXYZI());
PointCloudXYZI::Ptr normvec(new PointCloudXYZI(100000, 1));
PointCloudXYZI::Ptr laserCloudOri(new PointCloudXYZI(100000, 1));
PointCloudXYZI::Ptr corr_normvect(new PointCloudXYZI(100000, 1));
PointCloudXYZI::Ptr _featsArray;

pcl::VoxelGrid<PointType> downSizeFilterSurf;
pcl::VoxelGrid<PointType> downSizeFilterMap;

KD_TREE<PointType> ikdtree;

V3F XAxisPoint_body(LIDAR_SP_LEN, 0.0, 0.0);
V3F XAxisPoint_world(LIDAR_SP_LEN, 0.0, 0.0);
V3D euler_cur;
V3D position_last(Zero3d);
V3D Lidar_T_wrt_IMU(Zero3d);
M3D Lidar_R_wrt_IMU(Eye3d);

/*** EKF inputs and output ***/
MeasureGroup Measures;
esekfom::esekf<state_ikfom, 12, input_ikfom> kf;
state_ikfom state_point;
vect3 pos_lid;

nav_msgs::msg::Odometry odomAftMapped;
geometry_msgs::msg::Quaternion geoQuat;
geometry_msgs::msg::PoseStamped msg_body_pose, msg_body_pose_updated;

shared_ptr<Preprocess> p_pre(new Preprocess());
shared_ptr<ImuProcess> p_imu(new ImuProcess());


/*** Maintain keyframe mechanism ***/
// cache historical lidar frames, and determine which frame is a key frame based on the subscribed sequence
vector<PointCloudXYZI::Ptr> cloudKeyFrames;  // Store historical keyframe point clouds
queue< pair<uint32_t, PointCloudXYZI::Ptr> > cloudBuff;      // Cache some historical lidar frames to extract key frame point clouds
vector<uint32_t> idKeyFrames;           // keyframes id
queue<uint32_t> idKeyFramesBuff;         // keyframes id buffer
nav_msgs::msg::Path pathKeyFrames, path, path_updated;           // keyframes
uint32_t data_seq;                    // data id 
uint32_t lastKeyFramesId;               
geometry_msgs::msg::Pose lastKeyFramesPose;  
vector<geometry_msgs::msg::Pose> odoms;
/*** save submap ***/
pcl::KdTreeFLANN<pcl::PointXYZ>::Ptr kdtreeSurroundingKeyPoses(new pcl::KdTreeFLANN<pcl::PointXYZ>()); // kdtree of surrounding keyframe poses 
pcl::VoxelGrid<pcl::PointXYZ> downSizeFilterSurroundingKeyPoses; // for surrounding key poses of scan-to-map optimization

void SigHandle(int sig)
{
    flg_exit = true;
    std::cout << "catch sig %d" << sig << std::endl;
    sig_buffer.notify_all();
    rclcpp::shutdown();
}

/**
 * distance between two pointswrite
*/
float pointDistance(pcl::PointXYZ p1, pcl::PointXYZ p2)
{
    return sqrt((p1.x-p2.x)*(p1.x-p2.x) + (p1.y-p2.y)*(p1.y-p2.y) + (p1.z-p2.z)*(p1.z-p2.z));
}



inline void dump_lio_state_to_log(FILE *fp)  
{
    V3D rot_ang(Log(state_point.rot.toRotationMatrix()));
    fprintf(fp, "%lf ", Measures.lidar_beg_time - first_lidar_time);
    fprintf(fp, "%lf %lf %lf ", rot_ang(0), rot_ang(1), rot_ang(2));                         // Angle
    fprintf(fp, "%lf %lf %lf ", state_point.pos(0), state_point.pos(1), state_point.pos(2)); // Pos  
    fprintf(fp, "%lf %lf %lf ", 0.0, 0.0, 0.0);                                              // omega  
    fprintf(fp, "%lf %lf %lf ", state_point.vel(0), state_point.vel(1), state_point.vel(2)); // Vel  
    fprintf(fp, "%lf %lf %lf ", 0.0, 0.0, 0.0);                                              // Acc  
    fprintf(fp, "%lf %lf %lf ", state_point.bg(0), state_point.bg(1), state_point.bg(2));    // Bias_g  
    fprintf(fp, "%lf %lf %lf ", state_point.ba(0), state_point.ba(1), state_point.ba(2));    // Bias_a  
    fprintf(fp, "%lf %lf %lf ", state_point.grav[0], state_point.grav[1], state_point.grav[2]); // Bias_a  
    fprintf(fp, "\r\n");  
    fflush(fp);
}

void pointBodyToWorld_ikfom(PointType const * const pi, PointType * const po, state_ikfom &s)
{
    V3D p_body(pi->x, pi->y, pi->z);
    V3D p_global(s.rot * (s.offset_R_L_I*p_body + s.offset_T_L_I) + s.pos);

    po->x = p_global(0);
    po->y = p_global(1);
    po->z = p_global(2);
    po->intensity = pi->intensity;
}


void pointBodyToWorld(PointType const * const pi, PointType * const po)
{
    V3D p_body(pi->x, pi->y, pi->z);
    V3D p_global(state_point.rot * (state_point.offset_R_L_I*p_body + state_point.offset_T_L_I) + state_point.pos);

    po->x = p_global(0);
    po->y = p_global(1);
    po->z = p_global(2);
    po->intensity = pi->intensity;
}

template<typename T>
void pointBodyToWorld(const Matrix<T, 3, 1> &pi, Matrix<T, 3, 1> &po)
{
    V3D p_body(pi[0], pi[1], pi[2]);
    V3D p_global(state_point.rot * (state_point.offset_R_L_I*p_body + state_point.offset_T_L_I) + state_point.pos);

    po[0] = p_global(0);
    po[1] = p_global(1);
    po[2] = p_global(2);
}

void RGBpointBodyToWorld(PointType const * const pi, PointType * const po)
{
    V3D p_body(pi->x, pi->y, pi->z);
    V3D p_global(state_point.rot * (state_point.offset_R_L_I*p_body + state_point.offset_T_L_I) + state_point.pos);

    po->x = p_global(0);
    po->y = p_global(1);
    po->z = p_global(2);
    po->intensity = pi->intensity;
}

void RGBpointBodyLidarToIMU(PointType const * const pi, PointType * const po)
{
    V3D p_body_lidar(pi->x, pi->y, pi->z);
    V3D p_body_imu(state_point.offset_R_L_I*p_body_lidar + state_point.offset_T_L_I);

    po->x = p_body_imu(0);
    po->y = p_body_imu(1);
    po->z = p_body_imu(2);
    po->intensity = pi->intensity;
}

void points_cache_collect()
{
    PointVector points_history;
    ikdtree.acquire_removed_points(points_history);
    // for (int i = 0; i < points_history.size(); i++) _featsArray->push_back(points_history[i]);
}

BoxPointType LocalMap_Points;
bool Localmap_Initialized = false;
void lasermap_fov_segment()
{
    cub_needrm.clear();
    kdtree_delete_counter = 0;
    kdtree_delete_time = 0.0;    
    pointBodyToWorld(XAxisPoint_body, XAxisPoint_world);
    V3D pos_LiD = pos_lid;
    if (!Localmap_Initialized){
        for (int i = 0; i < 3; i++){
            LocalMap_Points.vertex_min[i] = pos_LiD(i) - cube_len / 2.0;
            LocalMap_Points.vertex_max[i] = pos_LiD(i) + cube_len / 2.0;
        }
        Localmap_Initialized = true;
        return;
    }
    float dist_to_map_edge[3][2];
    bool need_move = false;
    for (int i = 0; i < 3; i++){
        dist_to_map_edge[i][0] = fabs(pos_LiD(i) - LocalMap_Points.vertex_min[i]);
        dist_to_map_edge[i][1] = fabs(pos_LiD(i) - LocalMap_Points.vertex_max[i]);
        if (dist_to_map_edge[i][0] <= MOV_THRESHOLD * DET_RANGE || dist_to_map_edge[i][1] <= MOV_THRESHOLD * DET_RANGE) need_move = true;
    }
    if (!need_move) return;
    BoxPointType New_LocalMap_Points, tmp_boxpoints;
    New_LocalMap_Points = LocalMap_Points;
    float mov_dist = max((cube_len - 2.0 * MOV_THRESHOLD * DET_RANGE) * 0.5 * 0.9, double(DET_RANGE * (MOV_THRESHOLD -1)));
    for (int i = 0; i < 3; i++){
        tmp_boxpoints = LocalMap_Points;
        if (dist_to_map_edge[i][0] <= MOV_THRESHOLD * DET_RANGE){
            New_LocalMap_Points.vertex_max[i] -= mov_dist;
            New_LocalMap_Points.vertex_min[i] -= mov_dist;
            tmp_boxpoints.vertex_min[i] = LocalMap_Points.vertex_max[i] - mov_dist;
            cub_needrm.push_back(tmp_boxpoints);
        } else if (dist_to_map_edge[i][1] <= MOV_THRESHOLD * DET_RANGE){
            New_LocalMap_Points.vertex_max[i] += mov_dist;
            New_LocalMap_Points.vertex_min[i] += mov_dist;
            tmp_boxpoints.vertex_max[i] = LocalMap_Points.vertex_min[i] + mov_dist;
            cub_needrm.push_back(tmp_boxpoints);
        }
    }
    LocalMap_Points = New_LocalMap_Points;

    points_cache_collect();
    double delete_begin = omp_get_wtime();
    if(cub_needrm.size() > 0) kdtree_delete_counter = ikdtree.Delete_Point_Boxes(cub_needrm);
    kdtree_delete_time = omp_get_wtime() - delete_begin;
}

void reset_pos()
{
    state_point.pos(0) = 0.0;
    state_point.pos(1) = 0.0;
    state_point.pos(1) = 0.0;

    state_point.rot.coeffs()[0] = 0.0;
    state_point.rot.coeffs()[1] = 0.0;
    state_point.rot.coeffs()[2] = 0.0;
    state_point.rot.coeffs()[3] = 1.0;
    geoQuat.x = 0.0;
    geoQuat.y = 0.0;
    geoQuat.z = 0.0;
    geoQuat.w = 1.0;

    kf.change_x(state_point);
}



double timediff_lidar_wrt_imu = 0.0;
bool   timediff_set_flg = false;

void imu_cbk(const sensor_msgs::msg::Imu::UniquePtr msg_in)
{
    publish_count ++;
    // cout<<"IMU got at: "<<msg_in->header.stamp.toSec()<<endl;
    sensor_msgs::msg::Imu::SharedPtr msg(new sensor_msgs::msg::Imu(*msg_in));
    

    msg->header.stamp = get_ros_time(get_time_sec(msg_in->header.stamp) - time_diff_lidar_to_imu);
    if (abs(timediff_lidar_wrt_imu) > 0.1 && time_sync_en)
    {
        msg->header.stamp = \
        rclcpp::Time(timediff_lidar_wrt_imu + get_time_sec(msg_in->header.stamp));
    }

    double timestamp = get_time_sec(msg->header.stamp);

    mtx_buffer.lock();

    if (timestamp < last_timestamp_imu)
    {
        std::cerr << "lidar loop back, clear buffer" << std::endl;
        imu_buffer.clear();
        reset_pos();
    }

    last_timestamp_imu = timestamp;

    imu_buffer.push_back(msg);
    mtx_buffer.unlock();
    sig_buffer.notify_all();
}

double lidar_mean_scantime = 0.0;
int    scan_num = 0;


void keyFrame_cbk(const nav_msgs::msg::Path::UniquePtr msg_keyframes){
    pathKeyFrames = *msg_keyframes;

}

void keyFrameId_cbk(const std_msgs::msg::UInt32::UniquePtr msg_keyframe_id){
    idKeyFramesBuff.push(msg_keyframe_id->data);
}

/*
 * 获得同步的lidar和imu数据
*/

bool sync_packages(MeasureGroup &meas)
{
    if (lidar_buffer.empty() || imu_buffer.empty()) {
        return false;
    }

    /*** push a lidar scan ***/
    if(!lidar_pushed)
    {
        meas.lidar = lidar_buffer.front();
        meas.lidar_beg_time = time_buffer.front();
        if (meas.lidar->points.size() <= 1) // time too little
        {
            lidar_end_time = meas.lidar_beg_time + lidar_mean_scantime;
            std::cerr << "Too few input point cloud!\n";
        }
        else if (meas.lidar->points.back().curvature / double(1000) < 0.5 * lidar_mean_scantime)
        {
            lidar_end_time = meas.lidar_beg_time + lidar_mean_scantime;
        }
        else
        {
            scan_num ++;
            lidar_end_time = meas.lidar_beg_time + meas.lidar->points.back().curvature / double(1000);
            lidar_mean_scantime += (meas.lidar->points.back().curvature / double(1000) - lidar_mean_scantime) / scan_num;
        }

        meas.lidar_end_time = lidar_end_time;

        lidar_pushed = true;
    }

    if (last_timestamp_imu < lidar_end_time)
    {
        return false;
    }

    /*** push imu data, and pop from imu buffer ***/
    double imu_time = get_time_sec(imu_buffer.front()->header.stamp);
    meas.imu.clear();
    while ((!imu_buffer.empty()) && (imu_time < lidar_end_time))
    {
        imu_time = get_time_sec(imu_buffer.front()->header.stamp);
        if(imu_time > lidar_end_time) break;
        meas.imu.push_back(imu_buffer.front());
        imu_buffer.pop_front();
    }

    lidar_buffer.pop_front();
    time_buffer.pop_front();
    lidar_pushed = false;
    return true;
}

int process_increments = 0;
void map_incremental()
{
    PointVector PointToAdd;
    PointVector PointNoNeedDownsample;
    PointToAdd.reserve(feats_down_size);
    PointNoNeedDownsample.reserve(feats_down_size);
    for (int i = 0; i < feats_down_size; i++)
    {
        /* transform to world frame */
        pointBodyToWorld(&(feats_down_body->points[i]), &(feats_down_world->points[i]));
        /* decide if need add to map */
        if (!Nearest_Points[i].empty() && flg_EKF_inited)
        {
            const PointVector &points_near = Nearest_Points[i];
            bool need_add = true;
            BoxPointType Box_of_Point;
            PointType downsample_result, mid_point; 
            mid_point.x = floor(feats_down_world->points[i].x/filter_size_map_min)*filter_size_map_min + 0.5 * filter_size_map_min;
            mid_point.y = floor(feats_down_world->points[i].y/filter_size_map_min)*filter_size_map_min + 0.5 * filter_size_map_min;
            mid_point.z = floor(feats_down_world->points[i].z/filter_size_map_min)*filter_size_map_min + 0.5 * filter_size_map_min;
            float dist  = calc_dist(feats_down_world->points[i],mid_point);
            if (fabs(points_near[0].x - mid_point.x) > 0.5 * filter_size_map_min && fabs(points_near[0].y - mid_point.y) > 0.5 * filter_size_map_min && fabs(points_near[0].z - mid_point.z) > 0.5 * filter_size_map_min){
                PointNoNeedDownsample.push_back(feats_down_world->points[i]);
                continue;
            }
            for (int readd_i = 0; readd_i < NUM_MATCH_POINTS; readd_i ++)
            {
                if (points_near.size() < NUM_MATCH_POINTS) break;
                if (calc_dist(points_near[readd_i], mid_point) < dist)
                {
                    need_add = false;
                    break;
                }
            }
            if (need_add) PointToAdd.push_back(feats_down_world->points[i]);
        }
        else
        {
            PointToAdd.push_back(feats_down_world->points[i]);
        }
    }

    double st_time = omp_get_wtime();
    add_point_size = ikdtree.Add_Points(PointToAdd, true);
    ikdtree.Add_Points(PointNoNeedDownsample, false); 
    add_point_size = PointToAdd.size() + PointNoNeedDownsample.size();
    kdtree_incremental_time = omp_get_wtime() - st_time;
}

PointCloudXYZI::Ptr pcl_wait_pub(new PointCloudXYZI());
PointCloudXYZI::Ptr pcl_wait_save(new PointCloudXYZI());
PointCloudXYZI::Ptr pcl_fusion_sum(new PointCloudXYZI());





void publish_frame_world(rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubLaserCloudFull)
{
    
    if(scan_pub_en)
    {
        PointCloudXYZI::Ptr laserCloudFullRes(dense_pub_en ? feats_undistort : feats_down_body);
        int size = laserCloudFullRes->points.size();
        PointCloudXYZI::Ptr laserCloudWorld( \
                        new PointCloudXYZI(size, 1));

        for (int i = 0; i < size; i++)
        {
            RGBpointBodyToWorld(&laserCloudFullRes->points[i], \
                                &laserCloudWorld->points[i]);
        }

        sensor_msgs::msg::PointCloud2 laserCloudmsg;
        pcl::toROSMsg(*laserCloudWorld, laserCloudmsg);
        // laserCloudmsg.header.stamp = ros::Time().fromSec(lidar_end_time);
        laserCloudmsg.header.stamp = get_ros_time(lidar_end_time);
        laserCloudmsg.header.frame_id = odom_frame_id;
        pubLaserCloudFull->publish(laserCloudmsg);
        publish_count -= PUBFRAME_PERIOD;
    }

    /**************** save map ****************/
    /* 1. make sure you have enough memories
    /* 2. noted that pcd save will influence the real-time performences **/
    
        if (pcd_save_en)
    {
        int size = feats_undistort->points.size();
        PointCloudXYZI::Ptr laserCloudWorld( \
                        new PointCloudXYZI(size, 1));

        for (int i = 0; i < size; i++)
        {
            RGBpointBodyToWorld(&feats_undistort->points[i], \
                                &laserCloudWorld->points[i]);
        }
        *pcl_wait_save += *laserCloudWorld;

        static int scan_wait_num = 0;
        scan_wait_num ++;
        if (pcl_wait_save->size() > 0 && pcd_save_interval > 0  && scan_wait_num >= pcd_save_interval)
        {
            pcd_index ++;
            string all_points_dir(string(string(ROOT_DIR) + "PCD/scans_") + to_string(pcd_index) + string(".pcd"));
            pcl::PCDWriter pcd_writer;
            cout << "current scan saved to /PCD/" << all_points_dir << endl;
            pcd_writer.writeBinary(all_points_dir, *pcl_wait_save);
            pcl_wait_save->clear();
            scan_wait_num = 0;
        }
    }
}

void publish_frame_body(rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubLaserCloudFull_body)
{
    int size = feats_undistort->points.size();
    PointCloudXYZI::Ptr laserCloudIMUBody(new PointCloudXYZI(size, 1));

    for (int i = 0; i < size; i++)
    {
        RGBpointBodyLidarToIMU(&feats_undistort->points[i], \
                            &laserCloudIMUBody->points[i]);
    }

    sensor_msgs::msg::PointCloud2 laserCloudmsg;
    pcl::toROSMsg(*laserCloudIMUBody, laserCloudmsg);
    laserCloudmsg.header.stamp = get_ros_time(lidar_end_time);
    laserCloudmsg.header.frame_id = base_frame_id;
    pubLaserCloudFull_body->publish(laserCloudmsg);
    publish_count -= PUBFRAME_PERIOD;
    cloudBuff.push( pair<int, PointCloudXYZI::Ptr>(data_seq ,laserCloudIMUBody) ); // Cache all point clouds sent to the backend
}

void publish_effect_world(rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubLaserCloudEffect)
{
    PointCloudXYZI::Ptr laserCloudWorld( \
                    new PointCloudXYZI(effct_feat_num, 1));
    for (int i = 0; i < effct_feat_num; i++)
    {
        RGBpointBodyToWorld(&laserCloudOri->points[i], \
                            &laserCloudWorld->points[i]);
    }
    sensor_msgs::msg::PointCloud2 laserCloudFullRes3;
    pcl::toROSMsg(*laserCloudWorld, laserCloudFullRes3);
    laserCloudFullRes3.header.stamp = get_ros_time(lidar_end_time);
    laserCloudFullRes3.header.frame_id = odom_frame_id;
    pubLaserCloudEffect->publish(laserCloudFullRes3);
}

void publish_map(rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubLaserCloudMap)
{
    PointCloudXYZI::Ptr laserCloudFullRes(dense_pub_en ? feats_undistort : feats_down_body);
    int size = laserCloudFullRes->points.size();
    PointCloudXYZI::Ptr laserCloudWorld( \
                    new PointCloudXYZI(size, 1));

    for (int i = 0; i < size; i++)
    {
        RGBpointBodyToWorld(&laserCloudFullRes->points[i], \
                            &laserCloudWorld->points[i]);
    }
    *pcl_wait_pub += *laserCloudWorld;

    sensor_msgs::msg::PointCloud2 laserCloudmsg;
    pcl::toROSMsg(*pcl_wait_pub, laserCloudmsg);
    laserCloudmsg.header.stamp = get_ros_time(lidar_end_time);
    laserCloudmsg.header.frame_id = odom_frame_id;
    pubLaserCloudMap->publish(laserCloudmsg);
}

void save_to_pcd()
{
    pcl::PCDWriter pcd_writer;
    pcd_writer.writeBinary(map_file_path, *pcl_wait_pub);
}

template<typename T>
void set_posestamp(T & out)
{
    out.pose.position.x = state_point.pos(0);
    out.pose.position.y = state_point.pos(1);
    out.pose.position.z = state_point.pos(2);
    out.pose.orientation.x = geoQuat.x;
    out.pose.orientation.y = geoQuat.y;
    out.pose.orientation.z = geoQuat.z;
    out.pose.orientation.w = geoQuat.w;
    
}

void publish_odometry(const rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr pubOdomAftMapped, std::unique_ptr<tf2_ros::TransformBroadcaster> & tf_br)
{
    odomAftMapped.header.frame_id = odom_frame_id;
    odomAftMapped.child_frame_id = base_frame_id;
    odomAftMapped.header.stamp = get_ros_time(lidar_end_time);
    //odomAftMapped.twist.covariance[0] = data_seq;
    set_posestamp(odomAftMapped.pose);
    pubOdomAftMapped->publish(odomAftMapped);
    auto P = kf.get_P();
    for (int i = 0; i < 6; i ++)
    {
        int k = i < 3 ? i + 3 : i - 3;
        odomAftMapped.pose.covariance[i*6 + 0] = P(k, 3);
        odomAftMapped.pose.covariance[i*6 + 1] = P(k, 4);
        odomAftMapped.pose.covariance[i*6 + 2] = P(k, 5);
        odomAftMapped.pose.covariance[i*6 + 3] = P(k, 0);
        odomAftMapped.pose.covariance[i*6 + 4] = P(k, 1);
        odomAftMapped.pose.covariance[i*6 + 5] = P(k, 2);
    }
    if(pub_odom_transform) {
        geometry_msgs::msg::TransformStamped trans;
        trans.header.frame_id = odom_frame_id ;
        trans.child_frame_id = base_frame_id;
        trans.transform.translation.x = odomAftMapped.pose.pose.position.x;
        trans.transform.translation.y = odomAftMapped.pose.pose.position.y;
        trans.transform.translation.z = odomAftMapped.pose.pose.position.z;
        trans.transform.rotation.w = odomAftMapped.pose.pose.orientation.w;
        trans.transform.rotation.x = odomAftMapped.pose.pose.orientation.x;
        trans.transform.rotation.y = odomAftMapped.pose.pose.orientation.y;
        trans.transform.rotation.z = odomAftMapped.pose.pose.orientation.z;
        tf_br->sendTransform(trans);
    }

}

void publish_path(rclcpp::Publisher<nav_msgs::msg::Path>::SharedPtr pubPath)
{
    set_posestamp(msg_body_pose);
    msg_body_pose.header.stamp = get_ros_time(lidar_end_time); // ros::Time().fromSec(lidar_end_time);
    msg_body_pose.header.frame_id = odom_frame_id;

    /*** if path is too large, the rvis will crash ***/
    static int jjj = 0;
    jjj++;
    if (jjj % 10 == 0) 
    {
        path.poses.push_back(msg_body_pose);
        pubPath->publish(path);
    }
}

void h_share_model(state_ikfom &s, esekfom::dyn_share_datastruct<double> &ekfom_data)
{
    double match_start = omp_get_wtime();
    laserCloudOri->clear(); 
    corr_normvect->clear(); 
    total_residual = 0.0; 

    /** closest surface search and residual computation **/
    #ifdef MP_EN
        omp_set_num_threads(MP_PROC_NUM);
        #pragma omp parallel for
    #endif
    for (int i = 0; i < feats_down_size; i++)
    {
        PointType &point_body  = feats_down_body->points[i]; 
        PointType &point_world = feats_down_world->points[i]; 

        /* transform to world frame */
        V3D p_body(point_body.x, point_body.y, point_body.z);
        V3D p_global(s.rot * (s.offset_R_L_I*p_body + s.offset_T_L_I) + s.pos);
        point_world.x = p_global(0);
        point_world.y = p_global(1);
        point_world.z = p_global(2);
        point_world.intensity = point_body.intensity;

        vector<float> pointSearchSqDis(NUM_MATCH_POINTS);

        auto &points_near = Nearest_Points[i];

        if (ekfom_data.converge)
        {
            /** Find the closest surfaces in the map **/
            ikdtree.Nearest_Search(point_world, NUM_MATCH_POINTS, points_near, pointSearchSqDis);
            point_selected_surf[i] = points_near.size() < NUM_MATCH_POINTS ? false : pointSearchSqDis[NUM_MATCH_POINTS - 1] > 5 ? false : true;
        }

        if (!point_selected_surf[i]) continue;

        VF(4) pabcd;
        point_selected_surf[i] = false;
        if (esti_plane(pabcd, points_near, 0.1f))
        {
            float pd2 = pabcd(0) * point_world.x + pabcd(1) * point_world.y + pabcd(2) * point_world.z + pabcd(3);
            float s = 1 - 0.9 * fabs(pd2) / sqrt(p_body.norm());

            if (s > 0.9)
            {
                point_selected_surf[i] = true;
                normvec->points[i].x = pabcd(0);
                normvec->points[i].y = pabcd(1);
                normvec->points[i].z = pabcd(2);
                normvec->points[i].intensity = pd2;
                res_last[i] = abs(pd2);
            }
        }
    }
    
    effct_feat_num = 0;

    for (int i = 0; i < feats_down_size; i++)
    {
        if (point_selected_surf[i])
        {
            laserCloudOri->points[effct_feat_num] = feats_down_body->points[i];
            corr_normvect->points[effct_feat_num] = normvec->points[i];
            total_residual += res_last[i];
            effct_feat_num ++;
        }
    }

    if (effct_feat_num < 1)
    {
        ekfom_data.valid = false;
        std::cerr << "No Effective Points!" << std::endl;
        return;
    }

    res_mean_last = total_residual / effct_feat_num;
    match_time  += omp_get_wtime() - match_start;
    double solve_start_  = omp_get_wtime();
    
    /*** Computation of Measuremnt Jacobian matrix H and measurents vector ***/
    ekfom_data.h_x = MatrixXd::Zero(effct_feat_num, 12); //23
    ekfom_data.h.resize(effct_feat_num);

    for (int i = 0; i < effct_feat_num; i++)
    {
        const PointType &laser_p  = laserCloudOri->points[i];
        V3D point_this_be(laser_p.x, laser_p.y, laser_p.z);
        M3D point_be_crossmat;
        point_be_crossmat << SKEW_SYM_MATRX(point_this_be);
        V3D point_this = s.offset_R_L_I * point_this_be + s.offset_T_L_I;
        M3D point_crossmat;
        point_crossmat<<SKEW_SYM_MATRX(point_this);

        /*** get the normal vector of closest surface/corner ***/
        const PointType &norm_p = corr_normvect->points[i];
        V3D norm_vec(norm_p.x, norm_p.y, norm_p.z);

        /*** calculate the Measuremnt Jacobian matrix H ***/
        V3D C(s.rot.conjugate() *norm_vec);
        V3D A(point_crossmat * C);
        if (extrinsic_est_en)
        {
            V3D B(point_be_crossmat * s.offset_R_L_I.conjugate() * C); //s.rot.conjugate()*norm_vec);
            ekfom_data.h_x.block<1, 12>(i,0) << norm_p.x, norm_p.y, norm_p.z, VEC_FROM_ARRAY(A), VEC_FROM_ARRAY(B), VEC_FROM_ARRAY(C);
        }
        else
        {
            ekfom_data.h_x.block<1, 12>(i,0) << norm_p.x, norm_p.y, norm_p.z, VEC_FROM_ARRAY(A), 0.0, 0.0, 0.0, 0.0, 0.0, 0.0;
        }

        /*** Measuremnt: distance to the closest surface/corner ***/
        ekfom_data.h(i) = -norm_p.intensity;
    }
    solve_time += omp_get_wtime() - solve_start_;
}

/*
* @brief : Save the whole trajectory to a txt file (TUM format)
*/
void save_trajectory(const std::string &traj_file) {
    std::string filename(traj_file);
    std::fstream output_fstream;

    output_fstream.open(filename, std::ios_base::out);

    if (!output_fstream.is_open()) {
        std::cerr << "Failed to open " << filename << '\n';
    }

    else {
        output_fstream << "#timestamp x y z q_x q_y q_z q_w" << std::endl;
        for (const auto &p : path.poses) {
            output_fstream << std::setprecision(15) << p.header.stamp.sec + p.header.stamp.nanosec * 1e-9 << " "
                           << p.pose.position.x << " "
                           << p.pose.position.y << " "
                           << p.pose.position.z << " "
                           << p.pose.orientation.x << " "
                           << p.pose.orientation.y << " "
                           << p.pose.orientation.z << " "
                           << p.pose.orientation.w << std::endl;
        }
    }
}

class LaserMappingNode : public rclcpp::Node
{
public:
    LaserMappingNode(const rclcpp::NodeOptions& options = rclcpp::NodeOptions()) : Node("laser_mapping", options)
    {

        odom_frame_id = this->declare_parameter<string>("lio.common.odom_frame_id", "odom");
        base_frame_id = this->declare_parameter<string>("lio.common.base_frame_id", "base_link");
        lidar_frame_id = this->declare_parameter<string>("lio.common.lidar_frame_id", "velodyne");
        map_file_path = this->declare_parameter<string>("lio.common.map_file_path", "");
        NUM_MAX_ITERATIONS = this->declare_parameter<int>("lio.common.max_iteration", 4);
        lid_topic = this->declare_parameter<string>("lio.common.lid_topic", "/velodyne_points");
        imu_topic = this->declare_parameter<string>("lio.common.imu_topic", "/zed_m/zed_mini/imu/data");
        keyframe_topic = this->declare_parameter<string>("lio.common.keyframe_topic", "/aft_pgo_path");
        keyframe_id_topic = this->declare_parameter<string>("lio.common.keyframe_id_topic", "/key_frames_ids");
        time_sync_en = this->declare_parameter<bool>("lio.common.time_sync_en", false);
        time_diff_lidar_to_imu = this->declare_parameter<double>("lio.common.time_offset_lidar_to_imu", 0.0);
        runtime_pos_log = this->declare_parameter<bool>("lio.common.runtime_pos_log_enable", false);
        filter_size_surf_min = this->declare_parameter<double>("lio.common.filter_size_surf", 0.5);
        filter_size_map_min = this->declare_parameter<double>("lio.common.filter_size_map", 0.5);
        cube_len = this->declare_parameter<double>("lio.common.cube_side_length", 1000.0);
        debug_print = this->declare_parameter<bool>("lio.common.debug_print", false);
        

        p_pre->lidar_type = this->declare_parameter<int>("lio.preprocess.lidar_type", 2);
        p_pre->N_SCANS = this->declare_parameter<int>("lio.preprocess.scan_line", 16);
        p_pre->time_unit = this->declare_parameter<int>("lio.preprocess.timestamp_unit", 0);
        p_pre->blind = this->declare_parameter<double>("lio.preprocess.blind", 2.0);
        p_pre->SCAN_RATE = this->declare_parameter<int>("lio.preprocess.scan_rate", 10);
        p_pre->point_filter_num = this->declare_parameter<int>("lio.preprocess.point_filter_num", 4);
        p_pre->feature_enabled = this->declare_parameter<bool>("lio.preprocess.feature_extract_enable", false);


        path_en = this->declare_parameter<bool>("lio.publish.path_en", false);
        scan_pub_en = this->declare_parameter<bool>("lio.publish.scan_publish_en", false);
        dense_pub_en = this->declare_parameter<bool>("lio.publish.dense_publish_en", false);
        scan_body_pub_en = this->declare_parameter<bool>("lio.publish.scan_bodyframe_pub_en", false);
        fusion_pub_en = this->declare_parameter<bool>("lio.publish.fusion_pub_en", false);
        pub_odom_transform = this->declare_parameter<bool>("lio.publish.pub_odom_transform", false);

        recontructKdTree = this->declare_parameter<bool>("lio.loopClosure.recontructKdTree", true);
        updateState = this->declare_parameter<bool>("lio.loopClosure.updateState", false);
        updateFrequency = this->declare_parameter<int>("lio.loopClosure.updateFrequency", 100);

        DET_RANGE = this->declare_parameter<float>("lio.mapping.det_range", 200.);
        fov_deg = this->declare_parameter<double>("lio.mapping.fov_degree", 360.);
        gyr_cov = this->declare_parameter<double>("lio.mapping.gyr_cov", 0.1);
        acc_cov = this->declare_parameter<double>("lio.mapping.acc_cov", 0.1);
        b_gyr_cov = this->declare_parameter<double>("lio.mapping.b_gyr_cov", 0.0001);
        b_acc_cov = this->declare_parameter<double>("lio.mapping.b_acc_cov", 0.0001);
        extrinsic_est_en = this->declare_parameter<bool>("lio.mapping.extrinsic_est_en", true);
        extrinT = this->declare_parameter<vector<double>>("lio.mapping.extrinsic_T", vector<double>());
        extrinR = this->declare_parameter<vector<double>>("lio.mapping.extrinsic_R", vector<double>());


        FusionBufferSize = this->declare_parameter<const int>("lio.fusionCloud.size", 5);

        pcd_save_en = this->declare_parameter<bool>("lio.pcd_save.pcd_save_en", false);
        pcd_save_interval = this->declare_parameter<int>("lio.pcd_save.interval", -1);
        
 
        traj_save_en = this->declare_parameter<bool>("lio.traj_save.traj_save_en", false);
        traj_file_path = this->declare_parameter<string>("lio.traj_save.traj_file_path", "");

        path.header.stamp = this->get_clock()->now();
        path.header.frame_id =odom_frame_id;

        FOV_DEG = (fov_deg + 10.0) > 179.9 ? 179.9 : (fov_deg + 10.0);
        HALF_FOV_COS = cos((FOV_DEG) * 0.5 * PI_M / 180.0);

        _featsArray.reset(new PointCloudXYZI());

        memset(point_selected_surf, true, sizeof(point_selected_surf));
        memset(res_last, -1000.0f, sizeof(res_last));
        downSizeFilterSurf.setLeafSize(filter_size_surf_min, filter_size_surf_min, filter_size_surf_min);
        downSizeFilterMap.setLeafSize(filter_size_map_min, filter_size_map_min, filter_size_map_min);
        downSizeFilterSurroundingKeyPoses.setLeafSize(0.2,0.2,0.2);
        memset(point_selected_surf, true, sizeof(point_selected_surf));
        memset(res_last, -1000.0f, sizeof(res_last));

        Lidar_T_wrt_IMU<<VEC_FROM_ARRAY(extrinT);
        Lidar_R_wrt_IMU<<MAT_FROM_ARRAY(extrinR);
        p_imu->set_extrinsic(Lidar_T_wrt_IMU, Lidar_R_wrt_IMU);
        p_imu->set_gyr_cov(V3D(gyr_cov, gyr_cov, gyr_cov));
        p_imu->set_acc_cov(V3D(acc_cov, acc_cov, acc_cov));
        p_imu->set_gyr_bias_cov(V3D(b_gyr_cov, b_gyr_cov, b_gyr_cov));
        p_imu->set_acc_bias_cov(V3D(b_acc_cov, b_acc_cov, b_acc_cov));

        fill(epsi, epsi+23, 0.001);
        kf.init_dyn_share(get_f, df_dx, df_dw, h_share_model, NUM_MAX_ITERATIONS, epsi);

        data_seq = 0;
        uint32_t LcFreqcount = 1;
        /*** debug record ***/
        // FILE *fp;
        //string pos_log_dir = root_dir + "/Log/pos_log.txt";
        //fp = fopen(pos_log_dir.c_str(),"w");

        // ofstream fout_pre, fout_out, fout_dbg;
        // fout_pre.open(DEBUG_FILE_DIR("mat_pre.txt"),ios::out);
        // fout_out.open(DEBUG_FILE_DIR("mat_out.txt"),ios::out);
        // fout_dbg.open(DEBUG_FILE_DIR("dbg.txt"),ios::out);
        // if (fout_pre && fout_out)
        //     cout << "~~~~"<<ROOT_DIR<<" file opened" << endl;
        // else
        //     cout << "~~~~"<<ROOT_DIR<<" doesn't exist" << endl;

        /*** ROS subscribe initialization ***/
        sub_pcl_pc_ = this->create_subscription<sensor_msgs::msg::PointCloud2>(lid_topic, 20, std::bind(&LaserMappingNode::standard_pcl_cbk, this, std::placeholders::_1));
        sub_imu_ = this->create_subscription<sensor_msgs::msg::Imu>(imu_topic, 10, imu_cbk);
        sub_keyframes_ = this->create_subscription<nav_msgs::msg::Path>(keyframe_topic, 20, keyFrame_cbk);
        sub_keyframes_id_ = this->create_subscription<std_msgs::msg::UInt32>(keyframe_id_topic, 20, keyFrameId_cbk);

        pubLaserCloudFull_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("/cloud_registered", 20);
        pubFusionLaserCloud_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("/cloud_fusion", 20);
        pubdeskewLaserCloud_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("/cloud_deskew", 20);

        pubLaserCloudFull_body_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("/cloud_registered_body", 20);
        pubLaserCloudEffect_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("/cloud_effected", 20);
        pubLaserCloudMap_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("/Laser_map", 20);
        pubOdomAftMapped_ = this->create_publisher<nav_msgs::msg::Odometry>("/Odometry", 20);
        pubPath_ = this->create_publisher<nav_msgs::msg::Path>("/path", 20);
        pubPathUpdated_ = this->create_publisher<nav_msgs::msg::Path>("/path_updated", 20);
        pubKeyFramesMap_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("/keyframes_map", 20);

        tf_broadcaster_ = std::make_unique<tf2_ros::TransformBroadcaster>(*this);

        tf_buffer_ = std::make_shared<tf2_ros::Buffer>(this->get_clock());
        tf_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);


        //------------------------------------------------------------------------------------------------------
        auto period_ms = std::chrono::milliseconds(static_cast<int64_t>(1000.0 / 1000.0));  // 1ms
        timer_ = rclcpp::create_timer(this, this->get_clock(), period_ms, std::bind(&LaserMappingNode::timer_callback, this));

        auto map_period_ms = std::chrono::milliseconds(static_cast<int64_t>(1000.0));       // 1s
        map_pub_timer_ = rclcpp::create_timer(this, this->get_clock(), map_period_ms, std::bind(&LaserMappingNode::map_publish_callback, this));

        map_save_srv_ = this->create_service<std_srvs::srv::Trigger>("map_save", std::bind(&LaserMappingNode::map_save_callback, this, std::placeholders::_1, std::placeholders::_2));

        // init fusion buffer
        FusionLaserPointBuffer.resize(FusionBufferSize);
        for (int i = 0; i < FusionBufferSize; i++) FusionLaserPointBuffer[i].reset(new PointCloudXYZI());

        if(debug_print) RCLCPP_INFO(this->get_logger(), "Node init finished.");
    }

    ~LaserMappingNode()
    {
        // fout_out.close();
        // fout_pre.close();
        // fclose(fp);
    }

private:
    //*** main functions ***//
    void timer_callback()
    {

        //  Receive key frames and loop until one of them is empty (in theory, idKeyFramesBuff should be empty first)
        while( !cloudBuff.empty() && !idKeyFramesBuff.empty() ){
            while( idKeyFramesBuff.front() > cloudBuff.front().first )
            {
                cloudBuff.pop();
            }
            // 此时idKeyFramesBuff.front() == cloudBuff.front().first
            assert(idKeyFramesBuff.front() == cloudBuff.front().first);
            idKeyFrames.push_back(idKeyFramesBuff.front());
            cloudKeyFrames.push_back( cloudBuff.front().second );
            idKeyFramesBuff.pop();
            cloudBuff.pop();
        }
        assert(pathKeyFrames.poses.size() <= cloudKeyFrames.size() );   //It is possible that the ID has been sent, but the node has not been updated yet.
        // Record the latest keyframe information
        if(pathKeyFrames.poses.size() >= 1){
            lastKeyFramesId = idKeyFrames[pathKeyFrames.poses.size() - 1];
            lastKeyFramesPose = pathKeyFrames.poses.back().pose;
        }

        if(sync_packages(Measures))
        {
            if (flg_first_scan)
            {
                first_lidar_time = Measures.lidar_beg_time;
                p_imu->first_lidar_time = first_lidar_time;
                flg_first_scan = false;
                return;
            }

            double t0,t1,t2,t3,t4,t5,match_start, solve_start, svd_time;

            match_time = 0;
            kdtree_search_time = 0.0;
            solve_time = 0;
            solve_const_H_time = 0;
            svd_time   = 0;
            t0 = omp_get_wtime();

            p_imu->Process(Measures, kf, feats_undistort);
            state_point = kf.get_x();
            pos_lid = state_point.pos + state_point.rot * state_point.offset_T_L_I;

            if (feats_undistort->empty() || (feats_undistort == NULL))
            {
                RCLCPP_WARN(this->get_logger(), "No point, skip this scan!\n");
                return;
            }

            flg_EKF_inited = (Measures.lidar_beg_time - first_lidar_time) < INIT_TIME ? \
                            false : true;

            if(LcFreqcount % updateFrequency == 0 ){
                LcFreqcount = 1;
                if(debug_print) std::cout << "updateState: " << updateState << std::endl;
                if(recontructKdTree && pathKeyFrames.poses.size() > 20){
                    if(debug_print) std::cout << "Reconstruct KdTree done " << std::endl;
                    if(debug_print) std::cout << "pathKeyFrames.poses.size(): " << pathKeyFrames.poses.size() << std::endl;
                    /*** 所有关键帧的地图 ***/
                    // PointCloudXYZI::Ptr keyFramesMap(new PointCloudXYZI());
                    // PointCloudXYZI::Ptr keyframesTmp(new PointCloudXYZI());
                    // Eigen::Isometry3d poseTmp;
                    // assert(pathKeyFrames.poses.size() <= cloudKeyFrames.size() );   // 有可能id发过来了，但是节点还未更新
                    // int keyFramesNum = pathKeyFrames.poses.size();
                    // for(int i = 0; i < keyFramesNum; ++i){
                    //     downSizeFilterMap.setInputCloud(cloudKeyFrames[i]);
                    //     downSizeFilterMap.filter(*keyframesTmp);
                    //     tf::poseMsgToEigen(pathKeyFrames.poses[i].pose,poseTmp);
                    //     pcl::transformPointCloud(*keyframesTmp , *keyframesTmp, poseTmp.matrix());
                    //     *keyFramesMap += *keyframesTmp;
                    // }
                    // downSizeFilterMap.setInputCloud(keyFramesMap);
                    // downSizeFilterMap.filter(*keyFramesMap);

                    // ikdtree.reconstruct(keyFramesMap->points);

                    /*** A subgraph composed of close keyframes ***/
                    pcl::PointCloud<pcl::PointXYZ>::Ptr cloudKeyPoses3D(new pcl::PointCloud<pcl::PointXYZ>());    // Historical keyframe pose (position)
                    pcl::PointCloud<pcl::PointXYZ>::Ptr surroundingKeyPoses(new pcl::PointCloud<pcl::PointXYZ>());    
                    pcl::PointCloud<pcl::PointXYZ>::Ptr surroundingKeyPosesDS(new pcl::PointCloud<pcl::PointXYZ>());    

                    for(auto keyFramePose:pathKeyFrames.poses){
                        cloudKeyPoses3D->points.emplace_back(keyFramePose.pose.position.x, 
                                                                keyFramePose.pose.position.y, 
                                                                keyFramePose.pose.position.z);
                    }
                    double surroundingKeyframeSearchRadius = 5;
                    std::vector<int> pointSearchInd;
                    std::vector<float> pointSearchSqDis;
                    kdtreeSurroundingKeyPoses->setInputCloud(cloudKeyPoses3D); 
                    kdtreeSurroundingKeyPoses->radiusSearch(cloudKeyPoses3D->back(), surroundingKeyframeSearchRadius, pointSearchInd, pointSearchSqDis);
                    // go through the search results, pointSearchInd stores the index of the results under cloudKeyPoses3D
                    unordered_map<float, int> keyFramePoseMap;  // Use the x coordinate of pose as the key of the hash table
                    for (int i = 0; i < (int)pointSearchInd.size(); ++i)
                    {
                        int id = pointSearchInd[i];
                        //Add to adjacent keyframe pose collection
                        surroundingKeyPoses->push_back(cloudKeyPoses3D->points[id]);
                        keyFramePoseMap[cloudKeyPoses3D->points[id].x] = id;
                    }

                    // Downsample
                    downSizeFilterSurroundingKeyPoses.setInputCloud(surroundingKeyPoses);
                    downSizeFilterSurroundingKeyPoses.filter(*surroundingKeyPosesDS);

                    //Add offset frames close to the current keyframe. It is reasonable to add these frames.
                    int numPoses = cloudKeyPoses3D->size();
                    int offset = 10;
                    for (int i = numPoses-1; i >= numPoses-1 - offset && i >= 0; --i)
                    {
                        surroundingKeyPosesDS->push_back(cloudKeyPoses3D->points[i]);
                        keyFramePoseMap[cloudKeyPoses3D->points[i].x] = i;
                    }

                    //Add the points corresponding to the adjacent keyframe sets to the local map as a local point cloud map for scan-to-map matching
                    // PointCloudXYZI::Ptr keyFramesSubmap = extractCloud(surroundingKeyPosesDS, keyFramePoseMap);

                    PointCloudXYZI::Ptr keyFramesSubmap(new PointCloudXYZI());
                   // Traverse the current frame (actually take the nearest key frame to find its adjacent key frame set) adjacent key frame set in the space-time dimension

                    for (int i = 0; i < (int)surroundingKeyPosesDS->size(); ++i)
                    {

                        if(debug_print) RCLCPP_INFO( this->get_logger(), "surroundingKeyPosesDS->points[i].x: %f", surroundingKeyPosesDS->points[i].x);
                        if(debug_print) RCLCPP_INFO( this->get_logger(), "surroundingKeyPosesDS->points[i].x: %ld", keyFramePoseMap.size());
                        // assert(keyFramePoseMap.count(surroundingKeyPosesDS->points[i].x) != 0);
                        if(keyFramePoseMap.count(surroundingKeyPosesDS->points[i].x) == 0)
                            continue;

                        if (pointDistance(surroundingKeyPosesDS->points[i], cloudKeyPoses3D->back()) > surroundingKeyframeSearchRadius)    // remove point to far 
                            continue;
                        if(debug_print)  RCLCPP_INFO( this->get_logger(), "key: %ld", keyFramePoseMap.size());
                        // adjacent keyframe index
                        int thisKeyInd = keyFramePoseMap[ surroundingKeyPosesDS->points[i].x ];  // 以intensity作为红黑树的索引
                       
                        PointCloudXYZI::Ptr keyframesTmp(new PointCloudXYZI());
                        Eigen::Isometry3d poseTmp;
                        assert(pathKeyFrames.poses.size() <= cloudKeyFrames.size() );   // 有可能id发过来了，但是节点还未更新
                        int keyFramesNum = pathKeyFrames.poses.size();
                        if(debug_print) RCLCPP_INFO( this->get_logger(), "keyFramesNum: %ld", pathKeyFrames.poses.size());

                        downSizeFilterMap.setInputCloud(cloudKeyFrames[thisKeyInd]);

                        downSizeFilterMap.filter(*keyframesTmp);
                        
                        tf2::fromMsg(pathKeyFrames.poses[thisKeyInd].pose,poseTmp);
                        pcl::transformPointCloud(*keyframesTmp , *keyframesTmp, poseTmp.matrix());
                        *keyFramesSubmap += *keyframesTmp;

                    }
                    downSizeFilterMap.setInputCloud(keyFramesSubmap);
                    downSizeFilterMap.filter(*keyFramesSubmap);

                    ikdtree.reconstruct(keyFramesSubmap->points);
                }
            }

            // update status
            if(updateState)
            {
                state_ikfom state_updated = kf.get_x();
                Eigen::Isometry3d lastPose(state_updated.rot);
                lastPose.pretranslate(state_updated.pos);


                Eigen::Isometry3d lastKeyFramesPoseEigen;       // 最新的关键帧位姿
                tf2::fromMsg(lastKeyFramesPose, lastKeyFramesPoseEigen);

                Eigen::Isometry3d lastKeyFrameOdomPoseEigen;    // 最新的关键帧对应的odom的位姿
                tf2::fromMsg(lastKeyFramesPose, lastKeyFrameOdomPoseEigen);

                // lastPose表示世界坐标系到当前坐标系的变换，下面两个公式等价
                // lastPose = (lastKeyFramesPoseEigen.inverse() * lastKeyFrameOdomPoseEigen* lastPose.inverse()).inverse();
                lastPose = lastPose * lastKeyFrameOdomPoseEigen.inverse() * lastKeyFramesPoseEigen;

                Eigen::Quaterniond lastPoseQuat( lastPose.rotation() );
                Eigen::Vector3d lastPoseQuatPos( lastPose.translation() );
                state_updated.rot = lastPoseQuat;
                state_updated.pos = lastPoseQuatPos;
                kf.change_x(state_updated);

                esekfom::esekf<state_ikfom, 12, input_ikfom>::cov P_updated = kf.get_P();  // 获取当前的状态估计的协方差矩阵
                P_updated.setIdentity();
                //QUESTION: 状态的协方差矩阵是否要更新为一个比较的小的值？ 
                // init_P(0,0) = init_P(1,1) = init_P(2,2) = 0.00001; 
                // init_P(3,3) = init_P(4,4) = init_P(5,5) = 0.00001;
                P_updated(6,6) = P_updated(7,7) = P_updated(8,8) = 0.00001;
                P_updated(9,9) = P_updated(10,10) = P_updated(11,11) = 0.00001;
                P_updated(15,15) = P_updated(16,16) = P_updated(17,17) = 0.0001;
                P_updated(18,18) = P_updated(19,19) = P_updated(20,20) = 0.001;
                P_updated(21,21) = P_updated(22,22) = 0.00001; 
                kf.change_P(P_updated);

                msg_body_pose_updated.pose.position.x = state_updated.pos(0);
                msg_body_pose_updated.pose.position.y = state_updated.pos(1);
                msg_body_pose_updated.pose.position.z = state_updated.pos(2);
                msg_body_pose_updated.pose.orientation.x = state_updated.rot.x();
                msg_body_pose_updated.pose.orientation.y = state_updated.rot.y();
                msg_body_pose_updated.pose.orientation.z = state_updated.rot.z();
                msg_body_pose_updated.pose.orientation.w = state_updated.rot.w();
                msg_body_pose_updated.header.stamp = this->get_clock()->now();
                msg_body_pose_updated.header.frame_id = odom_frame_id;


                path_updated.poses.push_back(msg_body_pose_updated);
                path_updated.header.stamp = this->get_clock()->now();
                path_updated.header.frame_id = odom_frame_id;
                pubPathUpdated_->publish(path_updated);
            }
            LcFreqcount++;
            
            /*** Segment the map in lidar FOV ***/
            lasermap_fov_segment();

            /*** downsample the feature points in a scan ***/
            downSizeFilterSurf.setInputCloud(feats_undistort);
            downSizeFilterSurf.filter(*feats_down_body);
            t1 = omp_get_wtime();
            feats_down_size = feats_down_body->points.size();
            /*** initialize the map kdtree ***/
            if(ikdtree.Root_Node == nullptr)
            {
                if(debug_print) RCLCPP_INFO(this->get_logger(), "Initialize the map kdtree");
                if(feats_down_size > 5)
                {
                    ikdtree.set_downsample_param(filter_size_map_min);
                    feats_down_world->resize(feats_down_size);
                    for(int i = 0; i < feats_down_size; i++)
                    {
                        pointBodyToWorld(&(feats_down_body->points[i]), &(feats_down_world->points[i]));
                    }
                    ikdtree.Build(feats_down_world->points);
                }
                return;
            }
            int featsFromMapNum = ikdtree.validnum();
            kdtree_size_st = ikdtree.size();
            

            /*** ICP and iterated Kalman filter update ***/
            if (feats_down_size < 5)
            {
                RCLCPP_WARN(this->get_logger(), "No point, skip this scan!\n");
                return;
            }
            
            normvec->resize(feats_down_size);
            feats_down_world->resize(feats_down_size);

            V3D ext_euler = SO3ToEuler(state_point.offset_R_L_I);
            // fout_pre<<setw(20)<<Measures.lidar_beg_time - first_lidar_time<<" "<<euler_cur.transpose()<<" "<< state_point.pos.transpose()<<" "<<ext_euler.transpose() << " "<<state_point.offset_T_L_I.transpose()<< " " << state_point.vel.transpose() \
            // <<" "<<state_point.bg.transpose()<<" "<<state_point.ba.transpose()<<" "<<state_point.grav<< endl;

            if(0) // If you need to see map point, change to "if(1)"
            {
                PointVector ().swap(ikdtree.PCL_Storage);
                ikdtree.flatten(ikdtree.Root_Node, ikdtree.PCL_Storage, NOT_RECORD);
                featsFromMap->clear();
                featsFromMap->points = ikdtree.PCL_Storage;
            }

            pointSearchInd_surf.resize(feats_down_size);
            Nearest_Points.resize(feats_down_size);
            int  rematch_num = 0;
            bool nearest_search_en = true; //

            t2 = omp_get_wtime();
            
            /*** iterated state estimation ***/
            double t_update_start = omp_get_wtime();
            double solve_H_time = 0;
            kf.update_iterated_dyn_share_modified(LASER_POINT_COV, solve_H_time);
            state_point = kf.get_x();
            euler_cur = SO3ToEuler(state_point.rot);
            pos_lid = state_point.pos + state_point.rot * state_point.offset_T_L_I;
            geoQuat.x = state_point.rot.coeffs()[0];
            geoQuat.y = state_point.rot.coeffs()[1];
            geoQuat.z = state_point.rot.coeffs()[2];
            geoQuat.w = state_point.rot.coeffs()[3];

            double t_update_end = omp_get_wtime();

            /******* Publish odometry *******/
            publish_odometry(pubOdomAftMapped_, tf_broadcaster_);

            /*** add the feature points to map kdtree ***/
            t3 = omp_get_wtime();
            map_incremental();
            t5 = omp_get_wtime();
            
            /******* Publish points *******/
            if (path_en)                         publish_path(pubPath_);
            if (fusion_pub_en)              publishFusionLaserCloud(pubFusionLaserCloud_);
            if (scan_pub_en)      publish_frame_world(pubLaserCloudFull_);
            if (scan_pub_en && scan_body_pub_en) publish_frame_body(pubLaserCloudFull_body_);
            ++data_seq;
            
            publish_deskwed();

            /*** Debug variables ***/
            if (runtime_pos_log)
            {
                frame_num ++;
                kdtree_size_end = ikdtree.size();
                aver_time_consu = aver_time_consu * (frame_num - 1) / frame_num + (t5 - t0) / frame_num;
                aver_time_icp = aver_time_icp * (frame_num - 1)/frame_num + (t_update_end - t_update_start) / frame_num;
                aver_time_match = aver_time_match * (frame_num - 1)/frame_num + (match_time)/frame_num;
                aver_time_incre = aver_time_incre * (frame_num - 1)/frame_num + (kdtree_incremental_time)/frame_num;
                aver_time_solve = aver_time_solve * (frame_num - 1)/frame_num + (solve_time + solve_H_time)/frame_num;
                aver_time_const_H_time = aver_time_const_H_time * (frame_num - 1)/frame_num + solve_time / frame_num;
                T1[time_log_counter] = Measures.lidar_beg_time;
                s_plot[time_log_counter] = t5 - t0;
                s_plot2[time_log_counter] = feats_undistort->points.size();
                s_plot3[time_log_counter] = kdtree_incremental_time;
                s_plot4[time_log_counter] = kdtree_search_time;
                s_plot5[time_log_counter] = kdtree_delete_counter;
                s_plot6[time_log_counter] = kdtree_delete_time;
                s_plot7[time_log_counter] = kdtree_size_st;
                s_plot8[time_log_counter] = kdtree_size_end;
                s_plot9[time_log_counter] = aver_time_consu;
                s_plot10[time_log_counter] = add_point_size;
                time_log_counter ++;
                if(debug_print) printf("[ mapping ]: time: IMU + Map + Input Downsample: %0.6f ave match: %0.6f ave solve: %0.6f  ave ICP: %0.6f  map incre: %0.6f ave total: %0.6f icp: %0.6f construct H: %0.6f \n",t1-t0,aver_time_match,aver_time_solve,t3-t1,t5-t3,aver_time_consu,aver_time_icp, aver_time_const_H_time);
                ext_euler = SO3ToEuler(state_point.offset_R_L_I);
                // fout_out << setw(20) << Measures.lidar_beg_time - first_lidar_time << " " << euler_cur.transpose() << " " << state_point.pos.transpose()<< " " << ext_euler.transpose() << " "<<state_point.offset_T_L_I.transpose()<<" "<< state_point.vel.transpose() \
                // <<" "<<state_point.bg.transpose()<<" "<<state_point.ba.transpose()<<" "<<state_point.grav<<" "<<feats_undistort->points.size()<<endl;
                // dump_lio_state_to_log(fp);
            }
        }
    }

    void map_publish_callback()
    {
        publish_map(pubLaserCloudMap_);
    }

    void publishFusionLaserCloud(rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubFusionLaserCloud) {

        if(pubFusionLaserCloud->get_subscription_count() > 0) {
            int size = feats_undistort->points.size();
            PointCloudXYZI::Ptr laserCloudWorld( new PointCloudXYZI(size, 1));
            for (int i = 0; i < size; i++)
            {
                RGBpointBodyToWorld(&feats_undistort->points[i], \
                                    &laserCloudWorld->points[i]);
            }
            
            FusionLaserPointBuffer.insert(FusionLaserPointBuffer.begin() + FusionbufferIndex, laserCloudWorld);
            for(int i = 0; i < FusionBufferSize; i++) *pcl_fusion_sum += *FusionLaserPointBuffer.at(i);


            FusionbufferIndex = (FusionbufferIndex + 1) % FusionBufferSize;

            sensor_msgs::msg::PointCloud2 FusionlaserCloudmsg_world, FusionlaserCloudmsg_body;
            pcl::toROSMsg(*pcl_fusion_sum, FusionlaserCloudmsg_world);

            geometry_msgs::msg::TransformStamped transform_stamped;
            try {
                // Get the latest available transform
                transform_stamped = tf_buffer_->lookupTransform(lidar_frame_id, odom_frame_id, tf2::TimePointZero);
            } catch (tf2::TransformException &ex) { 
                RCLCPP_WARN(this->get_logger(), "%s", ex.what());
                return;
            }
            tf2::doTransform(FusionlaserCloudmsg_world, FusionlaserCloudmsg_body, transform_stamped);
            FusionlaserCloudmsg_body.header.stamp = get_ros_time(lidar_end_time);
            FusionlaserCloudmsg_body.header.frame_id = lidar_frame_id;
            pubFusionLaserCloud->publish(FusionlaserCloudmsg_body);
            pcl_fusion_sum->clear();
        }
    }   

    void publish_deskwed() {


        if(pubdeskewLaserCloud_->get_subscription_count() > 0) {
            sensor_msgs::msg::PointCloud2 deskewed_msg;
            pcl::toROSMsg(*feats_undistort, deskewed_msg);
            deskewed_msg.header.stamp = get_ros_time(lidar_end_time);
            deskewed_msg.header.frame_id = lidar_frame_id;
        }
    }



    void map_save_callback(std_srvs::srv::Trigger::Request::ConstSharedPtr req, std_srvs::srv::Trigger::Response::SharedPtr res)
    {
        RCLCPP_INFO(this->get_logger(), "Saving map to %s...", map_file_path.c_str());
        if (pcd_save_en)
        {
            save_to_pcd();
            res->success = true;
            res->message = "Map saved.";
        }
        else
        {
            res->success = false;
            res->message = "Map save disabled.";
        }
    }

    void standard_pcl_cbk(const sensor_msgs::msg::PointCloud2::UniquePtr msg) 
    {
        mtx_buffer.lock();
        scan_count ++;
        double cur_time = get_time_sec(msg->header.stamp);
        double preprocess_start_time = omp_get_wtime();
        if (!is_first_lidar && cur_time < last_timestamp_lidar)
        {
            std::cerr << "lidar loop back, clear buffer" << std::endl;
            lidar_buffer.clear();
            reset_pos();
        }
        if (is_first_lidar)
        {
            is_first_lidar = false;
        }

        PointCloudXYZI::Ptr  ptr(new PointCloudXYZI());
        p_pre->process(msg, ptr);
        if(pubdeskewLaserCloud_->get_subscription_count() > 0)
        {
            sensor_msgs::msg::PointCloud2 deskewed_msg;
            pcl::toROSMsg(p_pre->pl_full, deskewed_msg);
            deskewed_msg.header.stamp = get_ros_time(lidar_end_time);
            deskewed_msg.header.frame_id = lidar_frame_id;
            pubdeskewLaserCloud_->publish(deskewed_msg);
        }
        lidar_buffer.push_back(ptr);
        time_buffer.push_back(cur_time);
        last_timestamp_lidar = cur_time;
        s_plot11[scan_count] = omp_get_wtime() - preprocess_start_time;
        mtx_buffer.unlock();
        sig_buffer.notify_all();
    }


private:
    rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubLaserCloudFull_;
    rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubFusionLaserCloud_;
    rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubdeskewLaserCloud_;

    rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubKeyFramesMap_;

    rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubLaserCloudFull_body_;
    rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubLaserCloudEffect_;
    rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr pubLaserCloudMap_;
    rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr pubOdomAftMapped_;
    rclcpp::Publisher<nav_msgs::msg::Path>::SharedPtr pubPath_;
    rclcpp::Publisher<nav_msgs::msg::Path>::SharedPtr pubPathUpdated_;

    rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr sub_imu_;
    rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr sub_pcl_pc_;
    rclcpp::Subscription<nav_msgs::msg::Path>::SharedPtr sub_keyframes_;
    rclcpp::Subscription<std_msgs::msg::UInt32>::SharedPtr sub_keyframes_id_;

    std::unique_ptr<tf2_ros::TransformBroadcaster> tf_broadcaster_;

    std::shared_ptr<tf2_ros::Buffer> tf_buffer_;
    std::shared_ptr<tf2_ros::TransformListener> tf_listener_;

    rclcpp::TimerBase::SharedPtr timer_;
    rclcpp::TimerBase::SharedPtr map_pub_timer_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr map_save_srv_;

    int effect_feat_num = 0, frame_num = 0;
    double deltaT, deltaR, aver_time_consu = 0, aver_time_icp = 0, aver_time_match = 0, aver_time_incre = 0, aver_time_solve = 0, aver_time_const_H_time = 0;
    bool flg_EKF_converged, EKF_stop_flg = 0;
    double epsi[23] = {0.001};

    // FILE *fp;
    // ofstream fout_pre, fout_out, fout_dbg;
};

int main(int argc, char** argv)
{
    rclcpp::init(argc, argv);
    signal(SIGINT, SigHandle);

    rclcpp::spin(std::make_shared<LaserMappingNode>());

    if (rclcpp::ok())
        rclcpp::shutdown();

    /**************** save trajectory ****************/
    if(traj_save_en){
        // save_trajectory(traj_file_path);
        if(debug_print) std::cout << "Save FAST-LIO2 trajectory !!" << std::endl;  
    }

    /**************** save map ****************/
    /* Make sure you have enough memories to save the map */
    if (pcl_wait_save->size() > 0 && pcd_save_en)
    {
        pcl::PCDWriter pcd_writer;
        cout << "current scan saved to " << map_file_path <<endl;
        pcd_writer.writeBinary(map_file_path, *pcl_wait_save);
    }
    
    /**************** save runtime log ****************/
    if (runtime_pos_log)
    {
        vector<double> t, s_vec, s_vec2, s_vec3, s_vec4, s_vec5, s_vec6, s_vec7;    
        FILE *fp2;
        string log_dir = root_dir + "/Log/fast_lio_time_log.csv";
        fp2 = fopen(log_dir.c_str(),"w");
        fprintf(fp2,"time_stamp, total time, scan point size, incremental time, search time, delete size, delete time, tree size st, tree size end, add point size, preprocess time\n");
        for (int i = 0;i<time_log_counter; i++){
            fprintf(fp2,"%0.8f,%0.8f,%d,%0.8f,%0.8f,%d,%0.8f,%d,%d,%d,%0.8f\n",T1[i],s_plot[i],int(s_plot2[i]),s_plot3[i],s_plot4[i],int(s_plot5[i]),s_plot6[i],int(s_plot7[i]),int(s_plot8[i]), int(s_plot10[i]), s_plot11[i]);
            t.push_back(T1[i]);
            s_vec.push_back(s_plot9[i]);
            s_vec2.push_back(s_plot3[i] + s_plot6[i]);
            s_vec3.push_back(s_plot4[i]);
            s_vec5.push_back(s_plot[i]);
        }
        fclose(fp2);
    }

    return 0;
}