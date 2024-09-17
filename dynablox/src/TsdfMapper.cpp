/**
 * Copyright (C) 2023-now, RPL, KTH Royal Institute of Technology
 * MIT License
 * @author Qingwen Zhang (https://kin-zhang.github.io/)
 * @date: 2023-05-02 21:23
 * @details: No ROS version, speed up the process, check our benchmark in
 * dufomap
 */
#include "common/TsdfMapper.h"

#include <future>
#include <mutex>

namespace voxblox {
TsdfMapper::TsdfMapper(common::Config::VoxbloxCfg& config) : config_(config) {
  TsdfMap::Config TsdfMap_config;
  TsdfMap_config.tsdf_voxel_size = config.tsdf_voxel_size_;
  TsdfMap_config.tsdf_voxels_per_side = config.tsdf_voxels_per_side_;

  // Initialize TSDF Map and integrator.
  tsdf_map_.reset(new TsdfMap(TsdfMap_config));

  TsdfIntegratorBase::Config integrator_config;
  integrator_config.max_ray_length_m = *config.max_range_m;
  integrator_config.min_ray_length_m = *config.min_range_m;
  integrator_config.default_truncation_distance = config.truncation_distance_;
  integrator_config.max_weight = config.max_weight_;
  integrator_config.sensor_horizontal_resolution = config.sensor_horizontal_resolution_;
  integrator_config.sensor_vertical_resolution = config.sensor_vertical_resolution_;
  integrator_config.sensor_vertical_field_of_view_degrees = config.sensor_vertical_field_of_view_degrees_;
  integrator_config.use_const_weight = config.use_const_weight_;

  tsdf_integrator_ =
      TsdfIntegratorFactory::create(config.tsdf_methods, integrator_config, tsdf_map_->getTsdfLayerPtr());

  setColor();
}

void TsdfMapper::processPointCloudAndInsert(dynablox::Cloud& cloud, voxblox::Transformation& T_G_C,
                                            ufo::Timing& timing_) {
  // T_G_C can be refined
  Pointcloud points_C;
  Colors colors;
  timing_[6][0].start("Convert PointCloud");
  convertPointcloud(cloud, color_map_, &points_C, &colors);
  timing_[6][0].stop();

  timing_[6][1].start("Integrate PointCloud");
  tsdf_integrator_->integratePointCloud(T_G_C, points_C, colors, false);
  timing_[6][1].stop();

  LOG_IF(INFO, *config_.verbose_) << "have " << tsdf_map_->getTsdfLayer().getNumberOfAllocatedBlocks() << " blocks.";

  timing_[6][2].start("Remove Distant Blocks");
  tsdf_map_->getTsdfLayerPtr()->removeDistantBlocks(T_G_C.getPosition(), max_block_distance_from_body_);
  timing_[6][2].stop();
}
void TsdfMapper::setColor() {
  // Color map for intensity pointclouds.
  std::string intensity_colormap("rainbow");
  // Default set in constructor.
  if (intensity_colormap == "rainbow") {
    color_map_.reset(new RainbowColorMap());
  } else if (intensity_colormap == "inverse_rainbow") {
    color_map_.reset(new InverseRainbowColorMap());
  } else if (intensity_colormap == "grayscale") {
    color_map_.reset(new GrayscaleColorMap());
  } else if (intensity_colormap == "inverse_grayscale") {
    color_map_.reset(new InverseGrayscaleColorMap());
  } else if (intensity_colormap == "ironbow") {
    color_map_.reset(new IronbowColorMap());
  } else {
    LOG(WARNING) << "Invalid color map: " << intensity_colormap;
  }
  color_map_->setMaxValue(kDefaultMaxIntensity);
}
}  // namespace voxblox

namespace dynablox {
EverFreeIntegrator::EverFreeIntegrator(const common::Config::EverFreeCfg& config, TsdfLayer::Ptr tsdf_layer)
    : config_(config),
      tsdf_layer_(std::move(tsdf_layer)),
      neighborhood_search_(config_.neighbor_connectivity),
      voxel_size_(tsdf_layer_->voxel_size()),
      voxels_per_side_(tsdf_layer_->voxels_per_side()),
      voxels_per_block_(voxels_per_side_ * voxels_per_side_ * voxels_per_side_) {
  CHECK_NOTNULL(tsdf_layer_.get());
}

void EverFreeIntegrator::updateEverFreeVoxels(const int frame_counter, ufo::Timing& timing_) const {
  // Get all updated blocks. NOTE: we highjack the kESDF flag here for ever-free
  // tracking.
  voxblox::BlockIndexList updated_blocks;
  tsdf_layer_->getAllUpdatedBlocks(voxblox::Update::kEsdf, &updated_blocks);
  std::vector<BlockIndex> indices(updated_blocks.size());
  for (size_t i = 0; i < indices.size(); ++i) {
    indices[i] = updated_blocks[i];
  }

  // Update occupancy counter and calls removeEverFree if warranted in parallel
  // by block.
  voxblox::AlignedVector<voxblox::VoxelKey> voxels_to_remove;
  std::mutex result_aggregation_mutex;
  IndexGetter<BlockIndex> index_getter(indices);
  std::vector<std::future<void>> threads;
  timing_[5][1].start("remove_occupied");
  for (int i = 0; i < *config_.num_threads; ++i) {
    threads.emplace_back(std::async(std::launch::async, [&]() {
      BlockIndex index;
      voxblox::AlignedVector<voxblox::VoxelKey> local_voxels_to_remove;

      // Process all blocks.
      while (index_getter.getNextIndex(&index)) {
        voxblox::AlignedVector<voxblox::VoxelKey> voxels;
        if (blockWiseUpdateEverFree(index, frame_counter, voxels)) {
          local_voxels_to_remove.insert(local_voxels_to_remove.end(), voxels.begin(), voxels.end());
        }
      }

      // Aggregate results.
      std::lock_guard lock(result_aggregation_mutex);
      voxels_to_remove.insert(voxels_to_remove.end(), local_voxels_to_remove.begin(), local_voxels_to_remove.end());
    }));
  }
  for (auto& thread : threads) {
    thread.get();
  }

  // Remove the remaining voxels single threaded.
  for (const auto& voxel_key : voxels_to_remove) {
    TsdfBlock::Ptr tsdf_block = tsdf_layer_->getBlockPtrByIndex(voxel_key.first);
    if (!tsdf_block) {
      continue;
    }
    TsdfVoxel& tsdf_voxel = tsdf_block->getVoxelByVoxelIndex(voxel_key.second);
    tsdf_voxel.ever_free = false;
    tsdf_voxel.dynamic = false;
  }
  timing_[5][1].stop();

  // Labels tsdf-updated voxels as ever-free if they satisfy the criteria.
  // Performed blockwise in parallel.
  index_getter.reset();
  threads.clear();
  timing_[5][2].start("label_free");
  for (int i = 0; i < *config_.num_threads; ++i) {
    threads.emplace_back(std::async(std::launch::async, [&]() {
      BlockIndex index;
      while (index_getter.getNextIndex(&index)) {
        blockWiseMakeEverFree(index, frame_counter);
      }
    }));
  }
  for (auto& thread : threads) {
    thread.get();
  }
  timing_[5][2].stop();
}

bool EverFreeIntegrator::blockWiseUpdateEverFree(const BlockIndex& block_index, const int frame_counter,
                                                 voxblox::AlignedVector<voxblox::VoxelKey>& voxels_to_remove) const {
  TsdfBlock::Ptr tsdf_block = tsdf_layer_->getBlockPtrByIndex(block_index);
  if (!tsdf_block) {
    return false;
  }

  for (size_t index = 0; index < voxels_per_block_; ++index) {
    TsdfVoxel& tsdf_voxel = tsdf_block->getVoxelByLinearIndex(index);

    // Updating the occupancy counter.
    if (tsdf_voxel.distance < config_.tsdf_occupancy_threshold || tsdf_voxel.last_lidar_occupied == frame_counter) {
      updateOccupancyCounter(tsdf_voxel, frame_counter);
    }
    if (tsdf_voxel.last_lidar_occupied < frame_counter - config_.temporal_buffer) {
      tsdf_voxel.dynamic = false;
    }

    // Call to remove ever-free if warranted.
    if (tsdf_voxel.occ_counter >= config_.counter_to_reset && tsdf_voxel.ever_free) {
      const VoxelIndex voxel_index = tsdf_block->computeVoxelIndexFromLinearIndex(index);
      voxblox::AlignedVector<voxblox::VoxelKey> voxels =
          removeEverFree(*tsdf_block, tsdf_voxel, block_index, voxel_index);
      voxels_to_remove.insert(voxels_to_remove.end(), voxels.begin(), voxels.end());
    }
  }

  return !voxels_to_remove.empty();
}

void EverFreeIntegrator::blockWiseMakeEverFree(const BlockIndex& block_index, const int frame_counter) const {
  TsdfBlock::Ptr tsdf_block = tsdf_layer_->getBlockPtrByIndex(block_index);
  if (!tsdf_block) {
    return;
  }

  // Check all voxels.
  for (size_t index = 0; index < voxels_per_block_; ++index) {
    TsdfVoxel& tsdf_voxel = tsdf_block->getVoxelByLinearIndex(index);

    // If already ever-free we can save the cost of checking the neighbourhood.
    // Only observed voxels (with weight) can be set to ever free.
    // Voxel must be unoccupied for the last burn_in_period frames and
    // TSDF-value must be larger than 3/2 voxel_size
    if (tsdf_voxel.ever_free || tsdf_voxel.weight <= 1e-6 ||
        tsdf_voxel.last_occupied > frame_counter - config_.burn_in_period) {
      continue;
    }

    // Check the neighbourhood for unobserved or occupied voxels.
    const VoxelIndex voxel_index = tsdf_block->computeVoxelIndexFromLinearIndex(index);

    voxblox::AlignedVector<voxblox::VoxelKey> neighbors =
        neighborhood_search_.search(block_index, voxel_index, voxels_per_side_);

    bool neighbor_occupied_or_unobserved = false;

    for (const voxblox::VoxelKey& neighbor_key : neighbors) {
      const TsdfBlock* neighbor_block;
      if (neighbor_key.first == block_index) {
        // Often will be the same block.
        neighbor_block = tsdf_block.get();
      } else {
        neighbor_block = tsdf_layer_->getBlockPtrByIndex(neighbor_key.first).get();
        if (neighbor_block == nullptr) {
          // Block does not exist.
          neighbor_occupied_or_unobserved = true;
          break;
        }
      }

      // Check the voxel if it is unobserved or static.
      const TsdfVoxel& neighbor_voxel = neighbor_block->getVoxelByVoxelIndex(neighbor_key.second);
      if (neighbor_voxel.weight < 1e-6 || neighbor_voxel.last_occupied > frame_counter - config_.burn_in_period) {
        neighbor_occupied_or_unobserved = true;
        break;
      }
    }

    // Only observed free space, can be labeled as ever-free.
    if (!neighbor_occupied_or_unobserved) {
      tsdf_voxel.ever_free = true;
    }
  }
  tsdf_block->updated().reset(voxblox::Update::kEsdf);
}

voxblox::AlignedVector<voxblox::VoxelKey> EverFreeIntegrator::removeEverFree(TsdfBlock& block, TsdfVoxel& voxel,
                                                                             const BlockIndex& block_index,
                                                                             const VoxelIndex& voxel_index) const {
  // Remove ever-free attributes.
  voxel.ever_free = false;
  voxel.dynamic = false;

  // Remove ever-free attribute also from neighbouring voxels.
  voxblox::AlignedVector<voxblox::VoxelKey> neighbors =
      neighborhood_search_.search(block_index, voxel_index, voxels_per_side_);
  voxblox::AlignedVector<voxblox::VoxelKey> voxels_to_remove;

  for (const voxblox::VoxelKey& neighbor_key : neighbors) {
    if (neighbor_key.first == block_index) {
      // Since this is executed in parallel only modify this block.
      TsdfVoxel& neighbor_voxel = block.getVoxelByVoxelIndex(neighbor_key.second);
      neighbor_voxel.ever_free = false;
      neighbor_voxel.dynamic = false;
    } else {
      // Otherwise mark the voxel for later clean-up.
      voxels_to_remove.push_back(neighbor_key);
    }
  }

  return voxels_to_remove;
}

void EverFreeIntegrator::updateOccupancyCounter(TsdfVoxel& voxel, const int frame_counter) const {
  // Allow for breaks of temporal_buffer between occupied observations to
  // compensate for lidar sparsity.
  if (voxel.last_occupied >= frame_counter - config_.temporal_buffer) {
    voxel.occ_counter++;
  } else {
    voxel.occ_counter = 1;
  }
  voxel.last_occupied = frame_counter;
}
}  // namespace dynablox