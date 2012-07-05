#include <ros/ros.h>
#include <actionlib/client/simple_action_client.h>
#include <actionlib/client/terminal_state.h>
#include <rc4_path_execution/PathExecutionAction.h>

int main (int argc, char **argv)
{
  ros::init(argc, argv, "test_fibonacci");

  // create the action client
  // true causes the client to spin its own thread
  actionlib::SimpleActionClient<rc4_path_execution::PathExecutionAction> ac("PathExecution", true);

  ROS_INFO("Waiting for action server to start.");
  // wait for the action server to start
  ac.waitForServer(); //will wait for infinite time

  ROS_INFO("Action server started, sending goal.");
  // send a goal to the action
  rc4_path_execution::PathExecutionGoal goal;
  geometry_msgs::PoseStamped test_pose;
  test_pose.header.frame_id = "base_link";
  test_pose.header.stamp = ros::Time::now();
  test_pose.pose.position.x = 0.5;
  test_pose.pose.orientation.w = 1.0;
  goal.path.poses.push_back(test_pose);
  ac.sendGoal(goal);

  //wait for the action to return
  bool finished_before_timeout = ac.waitForResult();

  if (finished_before_timeout)
  {
    actionlib::SimpleClientGoalState state = ac.getState();
    ROS_INFO("Action finished: %s",state.toString().c_str());
  }
  else
    ROS_INFO("Action did not finish before the time out.");

  //exit
  return 0;
}