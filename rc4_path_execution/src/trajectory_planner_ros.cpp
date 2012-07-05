/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2008, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
* Author: Eitan Marder-Eppstein
*********************************************************************/

#include <trajectory_planner_ros.h>
#include <ros/console.h>
#include <sys/time.h>
#include <pluginlib/class_list_macros.h>
#include <boost/tokenizer.hpp>

#include "geometry_msgs/PolygonStamped.h"
#include "nav_msgs/Path.h"
#include "goal_functions.h"

using namespace std;
using namespace costmap_2d;

namespace rc4_path_execution {

  void TrajectoryPlannerROS::reconfigureCB(BaseLocalPlannerConfig &config, uint32_t level) {
      if(setup_ && config.restore_defaults) {
        config = default_config_;
        //Avoid looping
        config.restore_defaults = false;
      }
      if(!setup_) {
        default_config_ = config;
        setup_ = true;
      }
      else if(setup_) {
        tc_->reconfigure(config);
      }
  }

  TrajectoryPlannerROS::TrajectoryPlannerROS(const ros::NodeHandle &nh, std::string name, tf::TransformListener* tf, Costmap2DROS* costmap_ros)
    : nh_(nh),
      as_(nh_, name, boost::bind(&TrajectoryPlannerROS::executeCB, this, _1), false),
      world_model_(NULL), tc_(NULL), costmap_ros_(NULL), tf_(NULL), initialized_(false), setup_(false)
  {

      //initialize the planner
      initialize(name, tf, costmap_ros);

      vel_pub_ = nh_.advertise<geometry_msgs::Twist>("cmd_vel", 1);
  }

  void TrajectoryPlannerROS::initialize(std::string name, tf::TransformListener* tf, Costmap2DROS* costmap_ros){
    if(!initialized_){
      tf_ = tf;
      costmap_ros_ = costmap_ros;
      rot_stopped_velocity_ = 1e-2;
      trans_stopped_velocity_ = 1e-2;
      double sim_time, sim_granularity, angular_sim_granularity;
      int vx_samples, vtheta_samples;
      double pdist_scale, gdist_scale, occdist_scale, heading_lookahead, oscillation_reset_dist, escape_reset_dist, escape_reset_theta;
      bool holonomic_robot, dwa, simple_attractor, heading_scoring;
      double heading_scoring_timestep;
      double max_vel_x, min_vel_x;
      double backup_vel;
      double stop_time_buffer;
      string world_model_type;
      rotating_to_goal_ = false;

      //initialize the copy of the costmap the controller will use
      costmap_ros_->getCostmapCopy(costmap_);

      ros::NodeHandle private_nh("~/" + name);
      
      g_plan_pub_ = private_nh.advertise<nav_msgs::Path>("global_plan", 1);
      l_plan_pub_ = private_nh.advertise<nav_msgs::Path>("local_plan", 1);

      global_frame_ = costmap_ros_->getGlobalFrameID();
      robot_base_frame_ = costmap_ros_->getBaseFrameID();
      private_nh.param("prune_plan", prune_plan_, true);

      private_nh.param("yaw_goal_tolerance", yaw_goal_tolerance_, 0.05);
      private_nh.param("xy_goal_tolerance", xy_goal_tolerance_, 0.10);

      //to get odometery information, we need to get a handle to the topic in the global namespace of the node
      ros::NodeHandle global_node;
      odom_sub_ = global_node.subscribe<nav_msgs::Odometry>("odom", 1, boost::bind(&TrajectoryPlannerROS::odomCallback, this, _1));

      //we'll get the parameters for the robot radius from the costmap we're associated with
      inflation_radius_ = costmap_ros_->getInflationRadius();

      private_nh.param("acc_lim_x", acc_lim_x_, 2.5);
      private_nh.param("acc_lim_y", acc_lim_y_, 2.5);
      private_nh.param("acc_lim_th", acc_lim_theta_, 3.2);

      private_nh.param("stop_time_buffer", stop_time_buffer, 0.2);

      private_nh.param("latch_xy_goal_tolerance", latch_xy_goal_tolerance_, false);

      //Since I screwed up nicely in my documentation, I'm going to add errors
      //informing the user if they've set one of the wrong parameters
      if(private_nh.hasParam("acc_limit_x"))
        ROS_ERROR("You are using acc_limit_x where you should be using acc_lim_x. Please change your configuration files appropriately. The documentation used to be wrong on this, sorry for any confusion.");

      if(private_nh.hasParam("acc_limit_y"))
        ROS_ERROR("You are using acc_limit_y where you should be using acc_lim_y. Please change your configuration files appropriately. The documentation used to be wrong on this, sorry for any confusion.");

      if(private_nh.hasParam("acc_limit_th"))
        ROS_ERROR("You are using acc_limit_th where you should be using acc_lim_th. Please change your configuration files appropriately. The documentation used to be wrong on this, sorry for any confusion.");

      //Assuming this planner is being run within the navigation stack, we can
      //just do an upward search for the frequency at which its being run. This
      //also allows the frequency to be overwritten locally.
      std::string controller_frequency_param_name;
      if(!private_nh.searchParam("controller_frequency", controller_frequency_param_name))
        sim_period_ = 0.05;
      else
      {
        double controller_frequency = 0;
        private_nh.param(controller_frequency_param_name, controller_frequency, 20.0);
        if(controller_frequency > 0)
          sim_period_ = 1.0 / controller_frequency;
        else
        {
          ROS_WARN("A controller_frequency less than 0 has been set. Ignoring the parameter, assuming a rate of 20Hz");
          sim_period_ = 0.05;
        }
      }
      ROS_INFO("Sim period is set to %.2f", sim_period_);

      private_nh.param("sim_time", sim_time, 1.0);
      private_nh.param("sim_granularity", sim_granularity, 0.025);
      private_nh.param("angular_sim_granularity", angular_sim_granularity, sim_granularity);
      private_nh.param("vx_samples", vx_samples, 3);
      private_nh.param("vtheta_samples", vtheta_samples, 20);

      private_nh.param("path_distance_bias", pdist_scale, 0.6);
      private_nh.param("goal_distance_bias", gdist_scale, 0.8);

      bool meter_scoring;
      private_nh.param("meter_scoring", meter_scoring, false);

      if(meter_scoring)
      {
        //if we use meter scoring, then we want to multiply the biases by the resolution of the costmap
        double resolution = costmap_ros_->getResolution();
        gdist_scale *= resolution;
        pdist_scale *= resolution;
      }

      private_nh.param("occdist_scale", occdist_scale, 0.01);
      private_nh.param("heading_lookahead", heading_lookahead, 0.325);
      private_nh.param("oscillation_reset_dist", oscillation_reset_dist, 0.05);
      private_nh.param("escape_reset_dist", escape_reset_dist, 0.10);
      private_nh.param("escape_reset_theta", escape_reset_theta, M_PI_4);
      private_nh.param("holonomic_robot", holonomic_robot, true);
      private_nh.param("max_vel_x", max_vel_x, 0.5);
      private_nh.param("min_vel_x", min_vel_x, 0.1);

      double max_rotational_vel;
      private_nh.param("max_rotational_vel", max_rotational_vel, 1.0);
      max_vel_th_ = max_rotational_vel;
      min_vel_th_ = -1.0 * max_rotational_vel;
      private_nh.param("min_in_place_rotational_vel", min_in_place_vel_th_, 0.4);

      backup_vel = -0.1;
      if(private_nh.getParam("backup_vel", backup_vel))
        ROS_WARN("The backup_vel parameter has been deprecated in favor of the escape_vel parameter. To switch, just change the parameter name in your configuration files.");

      //if both backup_vel and escape_vel are set... we'll use escape_vel
      private_nh.getParam("escape_vel", backup_vel);

      if(backup_vel >= 0.0)
        ROS_WARN("You've specified a positive escape velocity. This is probably not what you want and will cause the robot to move forward instead of backward. You should probably change your escape_vel parameter to be negative");

      private_nh.param("world_model", world_model_type, string("costmap")); 
      private_nh.param("dwa", dwa, true);
      private_nh.param("heading_scoring", heading_scoring, false);
      private_nh.param("heading_scoring_timestep", heading_scoring_timestep, 0.8);

      simple_attractor = false;

      //parameters for using the freespace controller
      double min_pt_separation, max_obstacle_height, grid_resolution;
      private_nh.param("point_grid/max_sensor_range", max_sensor_range_, 2.0);
      private_nh.param("point_grid/min_pt_separation", min_pt_separation, 0.01);
      private_nh.param("point_grid/max_obstacle_height", max_obstacle_height, 2.0);
      private_nh.param("point_grid/grid_resolution", grid_resolution, 0.2);

      ROS_ASSERT_MSG(world_model_type == "costmap", "At this time, only costmap world models are supported by this controller");
      world_model_ = new CostmapModel(costmap_);
      std::vector<double> y_vels = loadYVels(private_nh);

      tc_ = new TrajectoryPlanner(*world_model_, costmap_, costmap_ros_->getRobotFootprint(),
          acc_lim_x_, acc_lim_y_, acc_lim_theta_, sim_time, sim_granularity, vx_samples, vtheta_samples, pdist_scale,
          gdist_scale, occdist_scale, heading_lookahead, oscillation_reset_dist, escape_reset_dist, escape_reset_theta, holonomic_robot,
          max_vel_x, min_vel_x, max_vel_th_, min_vel_th_, min_in_place_vel_th_, backup_vel,
          dwa, heading_scoring, heading_scoring_timestep, simple_attractor, y_vels, stop_time_buffer, sim_period_, angular_sim_granularity);

      map_viz_.initialize(name, &costmap_, boost::bind(&TrajectoryPlanner::getCellCosts, tc_, _1, _2, _3, _4, _5, _6));
      initialized_ = true;

      dsrv_ = new dynamic_reconfigure::Server<BaseLocalPlannerConfig>(private_nh);
      dynamic_reconfigure::Server<BaseLocalPlannerConfig>::CallbackType cb = boost::bind(&TrajectoryPlannerROS::reconfigureCB, this, _1, _2);
      dsrv_->setCallback(cb);

    }
    else
      ROS_WARN("This planner has already been initialized, you can't call it twice, doing nothing");
  }

  std::vector<double> TrajectoryPlannerROS::loadYVels(ros::NodeHandle node){
    std::vector<double> y_vels;

    std::string y_vel_list;
    if(node.getParam("y_vels", y_vel_list)){
      typedef boost::tokenizer<boost::char_separator<char> > tokenizer;
      boost::char_separator<char> sep("[], ");
      tokenizer tokens(y_vel_list, sep);

      for(tokenizer::iterator i = tokens.begin(); i != tokens.end(); i++){
        y_vels.push_back(atof((*i).c_str()));
      }
    }
    else{
      //if no values are passed in, we'll provide defaults
      y_vels.push_back(-0.3);
      y_vels.push_back(-0.1);
      y_vels.push_back(0.1);
      y_vels.push_back(0.3);
    }

    return y_vels;
  }

  TrajectoryPlannerROS::~TrajectoryPlannerROS(){
    //make sure to clean things up
    delete dsrv_;

    if(tc_ != NULL)
      delete tc_;

    if(world_model_ != NULL)
      delete world_model_;
  }

  bool TrajectoryPlannerROS::stopWithAccLimits(const tf::Stamped<tf::Pose>& global_pose, const tf::Stamped<tf::Pose>& robot_vel, geometry_msgs::Twist& cmd_vel){
    //slow down with the maximum possible acceleration... we should really use the frequency that we're running at to determine what is feasible
    //but we'll use a tenth of a second to be consistent with the implementation of the local planner.
    double vx = sign(robot_vel.getOrigin().x()) * std::max(0.0, (fabs(robot_vel.getOrigin().x()) - acc_lim_x_ * sim_period_));
    double vy = sign(robot_vel.getOrigin().y()) * std::max(0.0, (fabs(robot_vel.getOrigin().y()) - acc_lim_y_ * sim_period_));

    double vel_yaw = tf::getYaw(robot_vel.getRotation());
    double vth = sign(vel_yaw) * std::max(0.0, (fabs(vel_yaw) - acc_lim_theta_ * sim_period_));

    //we do want to check whether or not the command is valid
    double yaw = tf::getYaw(global_pose.getRotation());
    bool valid_cmd = tc_->checkTrajectory(global_pose.getOrigin().getX(), global_pose.getOrigin().getY(), yaw, 
        robot_vel.getOrigin().getX(), robot_vel.getOrigin().getY(), vel_yaw, vx, vy, vth);

    //if we have a valid command, we'll pass it on, otherwise we'll command all zeros
    if(valid_cmd){
      ROS_DEBUG("Slowing down... using vx, vy, vth: %.2f, %.2f, %.2f", vx, vy, vth);
      cmd_vel.linear.x = vx;
      cmd_vel.linear.y = vy;
      cmd_vel.angular.z = vth;
      return true;
    }

    cmd_vel.linear.x = 0.0;
    cmd_vel.linear.y = 0.0;
    cmd_vel.angular.z = 0.0;
    return false;
  }

  bool TrajectoryPlannerROS::rotateToGoal(const tf::Stamped<tf::Pose>& global_pose, const tf::Stamped<tf::Pose>& robot_vel, double goal_th, geometry_msgs::Twist& cmd_vel){
    double yaw = tf::getYaw(global_pose.getRotation());
    double vel_yaw = tf::getYaw(robot_vel.getRotation());
    cmd_vel.linear.x = 0;
    cmd_vel.linear.y = 0;
    double ang_diff = angles::shortest_angular_distance(yaw, goal_th);

    double v_theta_samp = ang_diff > 0.0 ? std::min(max_vel_th_,
        std::max(min_in_place_vel_th_, ang_diff)) : std::max(min_vel_th_,
        std::min(-1.0 * min_in_place_vel_th_, ang_diff));

    //take the acceleration limits of the robot into account
    double max_acc_vel = fabs(vel_yaw) + acc_lim_theta_ * sim_period_;
    double min_acc_vel = fabs(vel_yaw) - acc_lim_theta_ * sim_period_;

    v_theta_samp = sign(v_theta_samp) * std::min(std::max(fabs(v_theta_samp), min_acc_vel), max_acc_vel);

    //we also want to make sure to send a velocity that allows us to stop when we reach the goal given our acceleration limits
    double max_speed_to_stop = sqrt(2 * acc_lim_theta_ * fabs(ang_diff)); 

    v_theta_samp = sign(v_theta_samp) * std::min(max_speed_to_stop, fabs(v_theta_samp));

    //we still want to lay down the footprint of the robot and check if the action is legal
    bool valid_cmd = tc_->checkTrajectory(global_pose.getOrigin().getX(), global_pose.getOrigin().getY(), yaw, 
        robot_vel.getOrigin().getX(), robot_vel.getOrigin().getY(), vel_yaw, 0.0, 0.0, v_theta_samp);

    ROS_DEBUG("Moving to desired goal orientation, th cmd: %.2f, valid_cmd: %d", v_theta_samp, valid_cmd);

    if(valid_cmd){
      cmd_vel.angular.z = v_theta_samp;
      return true;
    }

    cmd_vel.angular.z = 0.0;
    return false;

  }

  void TrajectoryPlannerROS::odomCallback(const nav_msgs::Odometry::ConstPtr& msg){
    //we assume that the odometry is published in the frame of the base
    boost::recursive_mutex::scoped_lock lock(odom_lock_);
    base_odom_.twist.twist.linear.x = msg->twist.twist.linear.x;
    base_odom_.twist.twist.linear.y = msg->twist.twist.linear.y;
    base_odom_.twist.twist.angular.z = msg->twist.twist.angular.z;
    ROS_DEBUG("In the odometry callback with velocity values: (%.2f, %.2f, %.2f)",
        base_odom_.twist.twist.linear.x, base_odom_.twist.twist.linear.y, base_odom_.twist.twist.angular.z);
  }

  bool TrajectoryPlannerROS::setPlan(const std::vector<geometry_msgs::PoseStamped>& orig_global_plan){
    if(!initialized_){
      ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
      return false;
    }

    //reset the global plan
    global_plan_.clear();
    global_plan_ = orig_global_plan;

    //when we get a new plan, we also want to clear any latch we may have on goal tolerances
    xy_tolerance_latch_ = false;

    return true;
  }

  bool TrajectoryPlannerROS::computeVelocityCommands(geometry_msgs::Twist& cmd_vel){
    if(!initialized_){
      ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
      return false;
    }

    std::vector<geometry_msgs::PoseStamped> local_plan;
    tf::Stamped<tf::Pose> global_pose;
    if(!costmap_ros_->getRobotPose(global_pose))
      return false;

    std::vector<geometry_msgs::PoseStamped> transformed_plan;
    //get the global plan in our frame
    if(!transformGlobalPlan(*tf_, global_plan_, *costmap_ros_, global_frame_, transformed_plan)){
      ROS_WARN("Could not transform the global plan to the frame of the controller");
      return false;
    }

    //now we'll prune the plan based on the position of the robot
    if(prune_plan_)
      prunePlan(global_pose, transformed_plan, global_plan_);


    //we also want to clear the robot footprint from the costmap we're using
    costmap_ros_->clearRobotFootprint();

    //make sure to update the costmap we'll use for this cycle
    costmap_ros_->getCostmapCopy(costmap_);

    // Set current velocities from odometry
    geometry_msgs::Twist global_vel;

    odom_lock_.lock();
    global_vel.linear.x = base_odom_.twist.twist.linear.x;
    global_vel.linear.y = base_odom_.twist.twist.linear.y;
    global_vel.angular.z = base_odom_.twist.twist.angular.z;
    odom_lock_.unlock();

    tf::Stamped<tf::Pose> drive_cmds;
    drive_cmds.frame_id_ = robot_base_frame_;

    tf::Stamped<tf::Pose> robot_vel;
    robot_vel.setData(btTransform(tf::createQuaternionFromYaw(global_vel.angular.z), btVector3(global_vel.linear.x, global_vel.linear.y, 0)));
    robot_vel.frame_id_ = robot_base_frame_;
    robot_vel.stamp_ = ros::Time();

    /* For timing uncomment
    struct timeval start, end;
    double start_t, end_t, t_diff;
    gettimeofday(&start, NULL);
    */

    //if the global plan passed in is empty... we won't do anything
    if(transformed_plan.empty())
      return false;

    tf::Stamped<tf::Pose> goal_point;
    tf::poseStampedMsgToTF(transformed_plan.back(), goal_point);
    //we assume the global goal is the last point in the global plan
    double goal_x = goal_point.getOrigin().getX();
    double goal_y = goal_point.getOrigin().getY();

    double yaw = tf::getYaw(goal_point.getRotation());

    double goal_th = yaw;

    //check to see if we've reached the goal position
    if(goalPositionReached(global_pose, goal_x, goal_y, xy_goal_tolerance_) || xy_tolerance_latch_){

      //if the user wants to latch goal tolerance, if we ever reach the goal location, we'll
      //just rotate in place
      if(latch_xy_goal_tolerance_)
        xy_tolerance_latch_ = true;

      //check to see if the goal orientation has been reached
      if(goalOrientationReached(global_pose, goal_th, yaw_goal_tolerance_)){
        //set the velocity command to zero
        cmd_vel.linear.x = 0.0;
        cmd_vel.linear.y = 0.0;
        cmd_vel.angular.z = 0.0;
        rotating_to_goal_ = false;
        xy_tolerance_latch_ = false;
      }
      else {
        //we need to call the next two lines to make sure that the trajectory
        //planner updates its path distance and goal distance grids
        tc_->updatePlan(transformed_plan);
        Trajectory path = tc_->findBestPath(global_pose, robot_vel, drive_cmds);
        map_viz_.publishCostCloud();

        //copy over the odometry information
        nav_msgs::Odometry base_odom;
        {
          boost::recursive_mutex::scoped_lock lock(odom_lock_);
          base_odom = base_odom_;
        }

        //if we're not stopped yet... we want to stop... taking into account the acceleration limits of the robot
        if(!rotating_to_goal_ && !rc4_path_execution::stopped(base_odom, rot_stopped_velocity_, trans_stopped_velocity_)){
          if(!stopWithAccLimits(global_pose, robot_vel, cmd_vel))
            return false;
        }
        //if we're stopped... then we want to rotate to goal
        else{
          //set this so that we know its OK to be moving
          rotating_to_goal_ = true;
          if(!rotateToGoal(global_pose, robot_vel, goal_th, cmd_vel))
            return false;
        }
      }

      //publish an empty plan because we've reached our goal position
      publishPlan(transformed_plan, g_plan_pub_, 0.0, 1.0, 0.0, 0.0);
      publishPlan(local_plan, l_plan_pub_, 0.0, 0.0, 1.0, 0.0);

      //we don't actually want to run the controller when we're just rotating to goal
      return true;
    }

    tc_->updatePlan(transformed_plan);

    //compute what trajectory to drive along
    Trajectory path = tc_->findBestPath(global_pose, robot_vel, drive_cmds);

    map_viz_.publishCostCloud();
    /* For timing uncomment
    gettimeofday(&end, NULL);
    start_t = start.tv_sec + double(start.tv_usec) / 1e6;
    end_t = end.tv_sec + double(end.tv_usec) / 1e6;
    t_diff = end_t - start_t;
    ROS_INFO("Cycle time: %.9f", t_diff);
    */

    //pass along drive commands
    cmd_vel.linear.x = drive_cmds.getOrigin().getX();
    cmd_vel.linear.y = drive_cmds.getOrigin().getY();
    yaw = tf::getYaw(drive_cmds.getRotation());

    cmd_vel.angular.z = yaw;

    //if we cannot move... tell someone
    if(path.cost_ < 0){
      local_plan.clear();
      publishPlan(transformed_plan, g_plan_pub_, 0.0, 1.0, 0.0, 0.0);
      publishPlan(local_plan, l_plan_pub_, 0.0, 0.0, 1.0, 0.0);
      return false;
    }

    // Fill out the local plan
    for(unsigned int i = 0; i < path.getPointsSize(); ++i){
      double p_x, p_y, p_th;
      path.getPoint(i, p_x, p_y, p_th);

      tf::Stamped<tf::Pose> p = tf::Stamped<tf::Pose>(tf::Pose(tf::createQuaternionFromYaw(p_th), tf::Point(p_x, p_y, 0.0)), ros::Time::now(), global_frame_);
      geometry_msgs::PoseStamped pose;
      tf::poseStampedTFToMsg(p, pose);
      local_plan.push_back(pose);
    }

    //publish information to the visualizer
    publishPlan(transformed_plan, g_plan_pub_, 0.0, 1.0, 0.0, 0.0);
    publishPlan(local_plan, l_plan_pub_, 0.0, 0.0, 1.0, 0.0);
    return true;
  }

  bool TrajectoryPlannerROS::checkTrajectory(double vx_samp, double vy_samp, double vtheta_samp, bool update_map){
    tf::Stamped<tf::Pose> global_pose;
    if(costmap_ros_->getRobotPose(global_pose)){
      if(update_map){
        //we also want to clear the robot footprint from the costmap we're using
        costmap_ros_->clearRobotFootprint();

        //make sure to update the costmap we'll use for this cycle
        costmap_ros_->getCostmapCopy(costmap_);

        //we need to give the planne some sort of global plan, since we're only checking for legality
        //we'll just give the robots current position
        std::vector<geometry_msgs::PoseStamped> plan;
        geometry_msgs::PoseStamped pose_msg;
        tf::poseStampedTFToMsg(global_pose, pose_msg);
        plan.push_back(pose_msg);
        tc_->updatePlan(plan, true);
      }

      //copy over the odometry information
      nav_msgs::Odometry base_odom;
      {
        boost::recursive_mutex::scoped_lock lock(odom_lock_);
        base_odom = base_odom_;
      }

      return tc_->checkTrajectory(global_pose.getOrigin().x(), global_pose.getOrigin().y(), tf::getYaw(global_pose.getRotation()),
          base_odom.twist.twist.linear.x,
          base_odom.twist.twist.linear.y,
          base_odom.twist.twist.angular.z, vx_samp, vy_samp, vtheta_samp);

    }
    ROS_WARN("Failed to get the pose of the robot. No trajectories will pass as legal in this case.");
    return false;
  }


  double TrajectoryPlannerROS::scoreTrajectory(double vx_samp, double vy_samp, double vtheta_samp, bool update_map){
    // Copy of checkTrajectory that returns a score instead of True / False
    tf::Stamped<tf::Pose> global_pose;
    if(costmap_ros_->getRobotPose(global_pose)){
      if(update_map){
        //we also want to clear the robot ros::NodeHandle nhfootprint from the costmap we're using
        costmap_ros_->clearRobotFootprint();

        //make sure to update the costmap we'll use for this cycle
        costmap_ros_->getCostmapCopy(costmap_);

        //we need to give the planne some sort of global plan, since we're only checking for legality
        //we'll just give the robots current position
        std::vector<geometry_msgs::PoseStamped> plan;
        geometry_msgs::PoseStamped pose_msg;
        tf::poseStampedTFToMsg(global_pose, pose_msg);
        plan.push_back(pose_msg);
        tc_->updatePlan(plan, true);
      }

      //copy over the odometry information
      nav_msgs::Odometry base_odom;
      {
        boost::recursive_mutex::scoped_lock lock(odom_lock_);
        base_odom = base_odom_;
      }

      return tc_->scoreTrajectory(global_pose.getOrigin().x(), global_pose.getOrigin().y(), tf::getYaw(global_pose.getRotation()),
          base_odom.twist.twist.linear.x,
          base_odom.twist.twist.linear.y,
          base_odom.twist.twist.angular.z, vx_samp, vy_samp, vtheta_samp);

    }
    ROS_WARN("Failed to get the pose of the robot. No trajectories will pass as legal in this case.");
    return -1.0;
  }

  bool TrajectoryPlannerROS::isGoalReached(){
    if(!initialized_){
      ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
      return false;
    }

    //copy over the odometry information
    nav_msgs::Odometry base_odom;
    {
      boost::recursive_mutex::scoped_lock lock(odom_lock_);
      base_odom = base_odom_;
    }

    return rc4_path_execution::isGoalReached(*tf_, global_plan_, *costmap_ros_, global_frame_, base_odom,
        rot_stopped_velocity_, trans_stopped_velocity_, xy_goal_tolerance_, yaw_goal_tolerance_);
  }

  void TrajectoryPlannerROS::executeCB(const rc4_path_execution::PathExecutionGoalConstPtr &goal)
  {
      geometry_msgs::Twist cmd_vel;

      std::vector<geometry_msgs::PoseStamped> temp_plan = goal->path.poses;

      if(!setPlan(temp_plan))
      {
          as_.setAborted(rc4_path_execution::PathExecutionResult(), "Failed to pass global plan to the controller.");
          return;
      }

      // controlling mode
      while(true)
      {
          // check that preempt has not been requested by the client
          if (as_.isPreemptRequested() || !ros::ok())
          {
              as_.setPreempted();
              break;
          }

          //check to see if we've reached our goal
          if(isGoalReached())
          {
              ROS_DEBUG_NAMED("move_base","Goal reached!");

              as_.setSucceeded(rc4_path_execution::PathExecutionResult(), "Goal reached.");
              break;
          }

          if(computeVelocityCommands(cmd_vel))
          {
              ROS_DEBUG_NAMED("move_base", "Got a valid command from the local planner.");
              //make sure that we send the velocity command to the base
              vel_pub_.publish(cmd_vel);
          }
          else {
              ROS_DEBUG_NAMED("move_base", "The local planner could not find a valid plan.");
              as_.setAborted(rc4_path_execution::PathExecutionResult(), "The local planner could not find a valid plan.");
              break;
          }
      }

  }
};

int main(int argc, char **argv)
{
    // Initialize ROS
    ros::init(argc, argv, "PathExecution");
    ros::NodeHandle nh;

    tf::TransformListener tf;
    Costmap2DROS costmap_ros("temp_coatmap",tf);

    rc4_path_execution::TrajectoryPlannerROS(nh, "PathExecution", &tf, &costmap_ros);

    ros::spin();

    return 0;
}
