/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2010, Eric Perko
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
 *   * Neither the name of Eric Perko nor the names of its
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
 *********************************************************************/
#ifndef MAP_GRID_VISUALIZER_H_
#define MAP_GRID_VISUALIZER_H_

#include <ros/ros.h>
#include <map_grid.h>
#include <costmap_2d/costmap_2d.h>
#include <map_grid_cost_point.h>
#include <pcl_ros/publisher.h>

namespace rc4_path_execution {
    class MapGridVisualizer {
        public:
            /**
              * @brief Default constructor
              */
            MapGridVisualizer();

            /**
              * @brief Initializes the MapGridVisualizer
              * @param name The name to be appended to ~/ in order to get the proper namespace for parameters
              * @param costmap The costmap instance to use to get the size of the map to generate a point cloud for
              * @param cost_function The function to use to compute the cost values to be inserted into each point in the output PointCloud as well as whether to include a given point or not
              */
            void initialize(const std::string& name,const costmap_2d::Costmap2D * costmap, boost::function<bool (int cx, int cy, float &path_cost, float &goal_cost, float &occ_cost, float &total_cost)> cost_function);

            /**
              * Destructor for the visualizer
              */
            ~MapGridVisualizer() {}

            /**
              * @brief Build and publish a PointCloud if the publish_cost_grid_pc parameter was true. Only include points for which the cost_function at (cx,cy) returns true.
              */
            void publishCostCloud();

        private:
            std::string name_; ///< @brief The name to get parameters relative to.
            std::string frame_id_; ///< @brief The frame to assign to the output PointCloud
            boost::function<bool (int cx, int cy, float &path_cost, float &goal_cost, float &occ_cost, float &total_cost)> cost_function_; ///< @brief The function to be used to generate the cost components for the output PointCloud
            bool publish_cost_grid_pc_; ///< @brief Whether or not to build and publsih a PointCloud
            const costmap_2d::Costmap2D *costmap_p_; ///< @brief The costmap to use to infer the size of the underlying map grid
            ros::NodeHandle ns_nh_;
            pcl::PointCloud<MapGridCostPoint> cost_cloud_;
            pcl_ros::Publisher<MapGridCostPoint> pub_;
    };
};

#endif
