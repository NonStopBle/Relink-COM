// nav_msgs_pubsub -- publishes and subscribes every nav_msgs type
// ReLink ships (relink/include/relink/standard_msgs.hpp), one topic
// per type: Odometry, MapMetaData, Path, OccupancyGrid, GridCells.
//
// Build:
//   g++ -std=c++17 -I relink/include -pthread examples/cpp/nav_msgs_pubsub.cpp -o nav_msgs_pubsub
// Run (in two terminals, or on two machines):
//   ./nav_msgs_pubsub

#include "relink/relink.hpp"
#include <cstdio>
#include <chrono>
#include <thread>

using namespace geometry_msgs;
using namespace nav_msgs;

int main() {
    RelinkNode node;
    node.use_multicast_discovery();   // zero setup -- see Step 3

    node.subscribe<Odometry>("/relink/nav/odometry", [](const Odometry& m) {
        std::printf("nav_msgs/Odometry child=%s\n", m.child_frame_id_str().c_str());
    });
    node.subscribe<MapMetaData>("/relink/nav/map_meta", [](const MapMetaData& m) {
        std::printf("nav_msgs/MapMetaData %ux%u res=%.2f\n", m.width, m.height, m.resolution);
    });
    node.subscribe<Path>("/relink/nav/path", [](const Path& m) {
        std::printf("nav_msgs/Path count=%u\n", m.count);
    });
    node.subscribe<OccupancyGrid>("/relink/nav/occupancy_grid", [](const OccupancyGrid& m) {
        std::printf("nav_msgs/OccupancyGrid data_len=%u\n", m.data_len);
    });
    node.subscribe<GridCells>("/relink/nav/grid_cells", [](const GridCells& m) {
        std::printf("nav_msgs/GridCells count=%u\n", m.count);
    });

    node.advertise<Odometry>("/relink/nav/odometry");
    node.advertise<MapMetaData>("/relink/nav/map_meta");
    node.advertise<Path>("/relink/nav/path");
    node.advertise<OccupancyGrid>("/relink/nav/occupancy_grid");
    node.advertise<GridCells>("/relink/nav/grid_cells");

    int i = 0;
    while (true) {
        node.spin_once();   // services discovery -- call this every loop
        float t = static_cast<float>(i) * 0.1f;

        {
            Odometry odom{};
            odom.header.set_frame_id("odom");
            odom.set_child_frame_id("base_link");
            odom.pose.position = { t, 0, 0 };
            node.publish<Odometry>("/relink/nav/odometry", odom);
        }
        {
            MapMetaData meta{};
            meta.map_load_time = std_msgs::Time::now();
            meta.resolution = 0.05f;
            meta.width = 100;
            meta.height = 100;
            node.publish<MapMetaData>("/relink/nav/map_meta", meta);
        }
        {
            Path path{};
            path.header.set_frame_id("map");
            path.count = 2;
            path.poses[0].pose.position = { 0, 0, 0 };
            path.poses[1].pose.position = { t, 0, 0 };
            node.publish<Path>("/relink/nav/path", path);
        }
        {
            OccupancyGrid grid{};
            grid.header.set_frame_id("map");
            grid.info.width = 10;
            grid.info.height = 10;
            grid.data_len = 100;
            node.publish<OccupancyGrid>("/relink/nav/occupancy_grid", grid);
        }
        {
            GridCells cells{};
            cells.header.set_frame_id("map");
            cells.cell_width = 0.1f;
            cells.cell_height = 0.1f;
            cells.count = 2;
            cells.cells[0] = { 0, 0, 0 };
            cells.cells[1] = { t, 0, 0 };
            node.publish<GridCells>("/relink/nav/grid_cells", cells);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ++i;
    }
}
