cmake_minimum_required(VERSION 2.4.6)
include($ENV{ROS_ROOT}/core/rosbuild/rosbuild.cmake)
set(ROS_BUILD_TYPE Release)

rosbuild_find_ros_package(actionlib_msgs)
include(${actionlib_msgs_PACKAGE_PATH}/cmake/actionbuild.cmake)
genaction()

rosbuild_init()

rosbuild_genmsg()

rosbuild_add_boost_directories()

#uncomment for profiling
#set(ROS_COMPILE_FLAGS "-g -pg" ${ROS_COMPILE_FLAGS})
#set(ROS_LINK_FLAGS "-g -pg" ${ROS_LINK_FLAGS})
#set(ROS_COMPILE_FLAGS "-g" ${ROS_COMPILE_FLAGS})
#set(ROS_LINK_FLAGS "-g" ${ROS_LINK_FLAGS})

rosbuild_find_ros_package(dynamic_reconfigure)
include(${dynamic_reconfigure_PACKAGE_PATH}/cmake/cfgbuild.cmake)
gencfg()

include_directories(include/rc4_path_execution)

set(base_local_planner_SRCS
    include/rc4_path_execution/costmap_model.h
    include/rc4_path_execution/map_grid_cost_point.h
    include/rc4_path_execution/planar_laser_scan.h
    include/rc4_path_execution/trajectory_inc.h
    include/rc4_path_execution/voxel_grid_model.h
    include/rc4_path_execution/goal_functions.h
    include/rc4_path_execution/map_grid.h
    include/rc4_path_execution/point_grid.h
    include/rc4_path_execution/trajectory_planner.h
    include/rc4_path_execution/world_model.h
    include/rc4_path_execution/map_cell.h
    include/rc4_path_execution/map_grid_visualizer.h
    include/rc4_path_execution/trajectory.h
    include/rc4_path_execution/trajectory_planner_ros.h
    src/goal_functions.cpp
    src/map_cell.cpp src/map_grid.cpp
#    src/point_grid.cpp
    src/costmap_model.cpp
    src/voxel_grid_model.cpp
    src/trajectory_planner.cpp
    src/trajectory.cpp
    src/trajectory_planner_ros.cpp
    src/map_grid_visualizer.cpp
)
rosbuild_add_executable(path_execution ${base_local_planner_SRCS})
rosbuild_link_boost(path_execution thread)

rosbuild_add_executable(test_path_execution src/test_path_execution.cpp)

