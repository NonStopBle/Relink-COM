cmake_minimum_required(VERSION 3.8)
project(relink_bench_compare)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# find dependencies
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(OpenCV REQUIRED)

add_executable(bench_pub src/bench_pub.cpp)
ament_target_dependencies(bench_pub rclcpp std_msgs)

add_executable(bench_sub src/bench_sub.cpp)
ament_target_dependencies(bench_sub rclcpp std_msgs)

add_executable(cam_pub src/cam_pub.cpp)
ament_target_dependencies(cam_pub rclcpp sensor_msgs)
target_include_directories(cam_pub PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(cam_pub opencv_core opencv_videoio opencv_imgcodecs opencv_highgui)
target_link_options(cam_pub PRIVATE -Wl,--allow-shlib-undefined)

add_executable(cam_sub src/cam_sub.cpp)
ament_target_dependencies(cam_sub rclcpp sensor_msgs)
target_include_directories(cam_sub PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(cam_sub opencv_core opencv_videoio opencv_imgcodecs opencv_highgui)
target_link_options(cam_sub PRIVATE -Wl,--allow-shlib-undefined)

install(TARGETS bench_pub bench_sub cam_pub cam_sub DESTINATION lib/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  # the following line skips the linter which checks for copyrights
  # comment the line when a copyright and license is added to all source files
  set(ament_cmake_copyright_FOUND TRUE)
  # the following line skips cpplint (only works in a git repo)
  # comment the line when this package is in a git repo and when
  # a copyright and license is added to all source files
  set(ament_cmake_cpplint_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
