#include <iostream>
#include <spdlog/spdlog.h>
#include <glk/io/ply_io.hpp>
#include <guik/viewer/light_viewer.hpp>

#include <gtsam_points/util/read_points.hpp>
#include <gtsam_points/util/easy_profiler.hpp>
#include <gtsam_points/ann/kdtree2.hpp>
#include <gtsam_points/ann/kdtreex.hpp>
#include <gtsam_points/ann/fast_occupancy_grid.hpp>
#include <gtsam_points/ann/locality_sensitive_hashing.hpp>
#include <gtsam_points/features/normal_estimation.hpp>
#include <gtsam_points/features/fpfh_estimation.hpp>
#include <gtsam_points/registration/ransac.hpp>
#include <gtsam_points/registration/graduated_non_convexity.hpp>
#include <gtsam_points/types/point_cloud_cpu.hpp>

int main(int argc, char** argv) {
  gtsam_points::EasyProfiler prof("prof", true, true);

  prof.push("read");

  spdlog::info("load map1 and map2");
  const std::string map1_path = "/home/koide/map1.ply";
  const std::string map2_path = "/home/koide/map2.ply";
  const auto target_raw = glk::load_ply(map1_path)->vertices;
  const auto source_raw = glk::load_ply(map2_path)->vertices;

  // const auto target_raw = gtsam_points::read_points("/home/koide/workspace/gtsam_points/data/kitti_00/000000.bin");
  // const auto source_raw = gtsam_points::read_points("/home/koide/workspace/gtsam_points/data/kitti_00/000001.bin");

  const int num_threads = 10;

  prof.push("preprocess");
  auto target = std::make_shared<gtsam_points::PointCloudCPU>(target_raw);
  target = gtsam_points::voxelgrid_sampling(target, 0.5, num_threads);
  target->add_normals(gtsam_points::estimate_normals(target->points, target->size(), 10, num_threads));

  auto source = std::make_shared<gtsam_points::PointCloudCPU>(source_raw);
  source = gtsam_points::voxelgrid_sampling(source, 0.5, num_threads);
  source->add_normals(gtsam_points::estimate_normals(source->points, source->size(), 10, num_threads));

  auto target_tree = std::make_shared<gtsam_points::KdTree2<gtsam_points::PointCloud>>(target, num_threads);
  auto source_tree = std::make_shared<gtsam_points::KdTree2<gtsam_points::PointCloud>>(source, num_threads);

  prof.push("fpfh");
  gtsam_points::FPFHEstimationParams fpfh_params;
  fpfh_params.search_radius = 5.0;
  fpfh_params.num_threads = num_threads;
  auto target_fpfh = gtsam_points::estimate_fpfh(target->points, target->normals, target->size(), *target_tree, fpfh_params);
  auto source_fpfh = gtsam_points::estimate_fpfh(source->points, source->normals, source->size(), *source_tree, fpfh_params);

  prof.push("kdtree");
  auto target_fpfh_tree = std::make_shared<gtsam_points::KdTreeX<gtsam_points::FPFH_DIM>>(target_fpfh.data(), target_fpfh.size());
  auto source_fpfh_tree = std::make_shared<gtsam_points::KdTreeX<gtsam_points::FPFH_DIM>>(source_fpfh.data(), source_fpfh.size());

  prof.push("lsh");
  std::mt19937 mt;
  // Eigen::ArrayXd offset = Eigen::ArrayXd::Zero(gtsam_points::FPFH_DIM);
  // Eigen::ArrayXd scale = Eigen::ArrayXd::Ones(gtsam_points::FPFH_DIM) / 10.0;
  // gtsam_points::LocalitySensitiveHashing<gtsam_points::FPFHSignature> lsh(target_fpfh, offset, scale, 8192 * 16, 128, mt);
  gtsam_points::MultiProbeLocalitySensitiveHashing lsh;
  lsh.create_tables(target_fpfh, mt);

  prof.push("search");
  for (int i = 0; i < source_fpfh.size(); i++) {
    size_t index = 0;
    double sq_dist = 0.0;
    size_t num_found = lsh.knn_search(source_fpfh[i].data(), 1, &index, &sq_dist);

    size_t gt_index = 0;
    double gt_sq_dist = 0.0;
    target_fpfh_tree->knn_search(source_fpfh[i].data(), 1, &gt_index, &gt_sq_dist);

    if (num_found == 0) {
      continue;
    }
    std::cout << i << " : " << index << " : " << sq_dist << " / " << gt_index << " : " << gt_sq_dist << std::endl;
  }

  prof.push("done");
  return 0;

  prof.push("gnc");
  gtsam_points::GNCParams gnc_params;
  gnc_params.verbose = true;
  gnc_params.max_init_samples = 5000;
  gnc_params.reciprocal_check = false;
  gnc_params.num_threads = num_threads;
  auto result = gtsam_points::estimate_pose_gnc<gtsam_points::PointCloud>(
    *target,
    *source,
    target_fpfh,
    source_fpfh,
    *target_tree,
    *target_fpfh_tree,
    *source_fpfh_tree,
    gnc_params);

  // gtsam_points::RegistrationResult result;
  // for (int i = 0; i < 5; i++) {
  //   prof.push("ransac");
  //   gtsam_points::RANSACParams ransac_params;
  //   ransac_params.num_threads = num_threads;
  //   ransac_params.inlier_voxel_resolution = 0.5;
  //   result = gtsam_points::estimate_pose_ransac<gtsam_points::PointCloud>(
  //     *target,
  //     *source,
  //     target_fpfh,
  //     source_fpfh,
  //     *target_tree,
  //     *target_fpfh_tree,
  //     ransac_params);
  // }

  prof.push("done");

  std::cout << "inlier rate: " << result.inlier_rate << std::endl;
  std::cout << "T_target_source: " << std::endl << result.T_target_source.matrix() << std::endl;

  auto viewer = guik::viewer();
  viewer->update_points("target", target->points, target->size(), guik::FlatBlue());
  viewer->update_points("source", source->points, source->size(), guik::FlatRed(result.T_target_source));
  viewer->spin();

  return 0;
}