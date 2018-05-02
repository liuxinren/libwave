#include "wave/odometry/LaserOdom.hpp"

namespace wave {

namespace {

double l2length(const double *const vec, uint16_t length) {
    double retval = 0;
    for (uint16_t i = 0; i < length; i++) {
        retval += vec[i] * vec[i];
    }
    return retval;
}

double norm(const std::vector<double> &vec) {
    double retval = 0;
    for (auto elem : vec) {
        retval += elem * elem;
    }
    return std::sqrt(retval);
}

}

LaserOdom::LaserOdom(const LaserOdomParams params, const FeatureExtractorParams feat_params) : param(params) {
    this->CSVFormat = new Eigen::IOFormat(Eigen::FullPrecision, Eigen::DontAlignCols, ", ", ", ");

    auto n_ring = static_cast<size_t>(param.n_ring);
    this->feature_extractor.setParams(feat_params, n_ring);
    this->counters.resize(n_ring);
    std::fill(this->counters.begin(), this->counters.end(), 0);

    this->cur_scan.resize(n_ring);
    this->signals.resize(n_ring);

    for (uint32_t i = 0; i < n_ring; i++) {
        this->cur_scan.at(i) = Eigen::Tensor<float, 2>(5, this->MAX_POINTS);
        this->signals.at(i) = Eigen::Tensor<float, 2>(this->N_SIGNALS, this->MAX_POINTS);
    }

    this->range_sensor = std::make_shared<RangeSensor>(param.sensor_params);

    this->feature_points.resize(this->N_FEATURES);
    this->prv_feature_points.resize(this->N_FEATURES);
    this->feature_corrs.resize(this->N_FEATURES);
    this->output_corrs.resize(this->N_FEATURES);
    this->feature_idx.resize(this->N_FEATURES);
    this->feature_association.resize(this->N_FEATURES);
    this->undis_features.resize(this->N_FEATURES);
    this->map_features.resize(this->N_FEATURES);

    for (uint32_t i = 0; i < this->N_FEATURES; i++) {
        this->feature_idx.at(i) =
          std::make_shared<kd_tree_t>(3, this->prv_feature_points.at(i), nanoflann::KDTreeSingleIndexAdaptorParams(20));
        this->feature_corrs.at(i).resize(n_ring);
    }

    if (this->param.num_trajectory_states < 2) {
        throw std::out_of_range("Number of parameter states must be at least 2");
    }

    this->param_blocks.resize(this->param.num_trajectory_states);
    this->cur_difference.resize(this->param.num_trajectory_states - 1);
    for (auto &block : this->param_blocks) {
        block.setZero();
    }

    // todo(ben) automatically get the scan perithis->trajectory_stamps.reserve(this->param.num_trajectory_states);

    double step_size = 0.1 / (double) (this->param.num_trajectory_states - 1);
    for (uint32_t i = 0; i < this->param.num_trajectory_states; i++) {
        Trajectory unit;
        Trajectory unit2;
        unit2.pose.setIdentity();
        unit2.vel.setZero();
        unit.pose.setIdentity();
        unit.vel.setZero();
        this->current_twist.setZero();
        this->prior_twist.setZero();
        this->cur_trajectory.emplace_back(unit);
        this->prev_trajectory.emplace_back(unit2);
        this->trajectory_stamps.emplace_back(i * step_size);
        if (i > 0) {
            this->cv_vector.emplace_back(this->trajectory_stamps.at(i - 1),
                                         this->trajectory_stamps.at(i),
                                         nullptr,
                                         this->param.Qc,
                                         this->param.inv_Qc);
            this->cur_difference.at(i - 1).hat_multiplier.setZero();
            this->cur_difference.at(i - 1).candle_multiplier.setZero();
        }
    }
    this->sqrtinfo.setIdentity();

    if (params.visualize) {
        this->display = new PointCloudDisplay("laser odom");
        this->display->startSpin();
        // Allocate space for visualization clouds
        this->prev_viz = boost::shared_ptr<pcl::PointCloud<pcl::PointXYZI>>(new pcl::PointCloud<pcl::PointXYZI>);
        this->cur_viz = boost::shared_ptr<pcl::PointCloud<pcl::PointXYZI>>(new pcl::PointCloud<pcl::PointXYZI>);
    }

    if (params.output_trajectory) {
        long timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        this->file.open(std::to_string(timestamp) + "laser_odom_traj.txt");
    }

    this->output_eigen.resize(6 * (1 + this->param.num_trajectory_states));

    // Initial size of pre-allocated memory for jacobians
    this->PtLMem.resize(1000);
    this->PtPMem.resize(3000);
}

/// tau is the time of the point
void LaserOdom::getTransformIndices(const uint32_t &tick, uint32_t &start, uint32_t &end, double &tau) {
    // This will round down to provide first transform index
    start = ((tick * (param.num_trajectory_states - 1)) / (param.max_ticks * this->param.n_window));
    end = start + 1;
    tau = (tick * this->param.scan_period) / (this->param.max_ticks * this->param.n_window);
}

void LaserOdom::transformToMap(
  const double *const pt, const uint32_t tick, double *output, uint32_t &k, uint32_t &kp1, double &tau) {
    this->getTransformIndices(tick, k, kp1, tau);

    Eigen::Matrix<double, 12, 12> hat, candle;
    this->cv_vector.at(k).tau = &tau;
    this->cv_vector.at(k).calculateStuff(hat, candle);

    T_TYPE T_MAP_LIDAR_i = this->cur_trajectory.at(k).pose;

    Eigen::Map<const Vec3> LIDAR_i_P(pt, 3, 1);
    Eigen::Map<Vec3> MAP_P(output, 3, 1);
    T_MAP_LIDAR_i.manifoldPlus(hat.block<6, 12>(0, 0) * this->cur_difference.at(k).hat_multiplier +
                               candle.block<6, 12>(0, 0) * this->cur_difference.at(k).candle_multiplier);
    T_MAP_LIDAR_i.transform(LIDAR_i_P, MAP_P);
}

void LaserOdom::transformToMap(const double *const pt, const uint32_t tick, double *output) {
    uint32_t k, kp1;
    double tau;

    this->transformToMap(pt, tick, output, k, kp1, tau);
}

void LaserOdom::transformToCurLidar(const double *const pt, const uint32_t tick, double *output) {
    uint32_t k, kp1;
    double tau;
    this->getTransformIndices(tick, k, kp1, tau);

    Eigen::Matrix<double, 12, 12> hat, candle;
    this->cv_vector.at(k).tau = &tau;
    this->cv_vector.at(k).calculateStuff(hat, candle);

    T_TYPE T_MAP_LIDAR_i = this->cur_trajectory.at(k).pose;

    T_MAP_LIDAR_i.manifoldPlus(hat.block<6, 12>(0, 0) * this->cur_difference.at(k).hat_multiplier +
                               candle.block<6, 12>(0, 0) * this->cur_difference.at(k).candle_multiplier);

    Eigen::Map<const Vec3> LIDAR_I_P(pt, 3, 1);
    Eigen::Map<Vec3> LIDAR_END_P(output, 3, 1);

    auto &T_MAP_LIDAR_END = this->cur_trajectory.back().pose;

    (T_MAP_LIDAR_END.transformInverse() * T_MAP_LIDAR_i).transform(LIDAR_I_P, LIDAR_END_P);
}

void LaserOdom::updateParams(const LaserOdomParams new_params) {
    this->param = new_params;
}

LaserOdomParams LaserOdom::getParams() {
    return this->param;
}

LaserOdom::~LaserOdom() {
    if (this->param.visualize) {
        this->display->stopSpin();
        delete this->display;
    }
    if (this->param.output_trajectory) {
        this->file.close();
    }
    if (this->output_thread) {
        this->continue_output = false;
        this->output_condition.notify_one();
        this->output_thread->join();
    }

    delete this->CSVFormat;
}

void LaserOdom::registerOutputFunction(std::function<void()> output_function) {
    this->f_output = std::move(output_function);
    this->output_thread = std::unique_ptr<std::thread>(new std::thread(&LaserOdom::spinOutput, this));
}

void LaserOdom::spinOutput() {
    std::unique_lock<std::mutex> lk(this->output_mutex);
    while (this->continue_output) {
        while (!this->fresh_output) {  // wait can have spurious wakeups
            this->output_condition.wait(lk);
            if (!this->continue_output) {
                break;
            }
        }
        this->f_output();
        this->fresh_output = false;
    }
}

/**
 * This function should transform all points of interest to the frame of the lidar at the end of the last scan
 */
void LaserOdom::undistort() {
//    this->undistorted_cld.clear();
//    for (uint32_t i = 0; i < this->N_FEATURES; i++) {
//        this->undis_features.at(i).clear();
//        this->map_features.at(i).resize(this->prv_feature_points.at(i).points.size());
//        this->output_corrs.at(i).clear();
//    }
//
//    for (uint16_t r_idx = 0; r_idx < this->param.n_ring; r_idx++) {
//        for (PCLPointXYZIT pt : this->cur_scan.at(r_idx)) {
//            double point[3] = {pt.x, pt.y, pt.z};
//            double u_pt[3];
//            this->transformToCurLidar(point, pt.tick, u_pt);
//            pcl::PointXYZI op_pt;
//            op_pt.x = (float) u_pt[0];
//            op_pt.y = (float) u_pt[1];
//            op_pt.z = (float) u_pt[2];
//            op_pt.intensity = pt.intensity;
//            this->undistorted_cld.push_back(op_pt);
//        }
//        for (uint32_t j = 0; j < this->N_FEATURES; j++) {
//            for (uint32_t i = 0; i < this->feature_points.at(j).at(r_idx).size(); i++) {
//                double point[3] = {this->feature_points.at(j).at(r_idx).at(i).pt[0],
//                                   feature_points.at(j).at(r_idx).at(i).pt[1],
//                                   feature_points.at(j).at(r_idx).at(i).pt[2]};
//                double u_pt[3];
//                this->transformToCurLidar(point, feature_points.at(j).at(r_idx).at(i).tick, u_pt);
//                pcl::PointXYZ op_pt;
//                op_pt.x = (float) u_pt[0];
//                op_pt.y = (float) u_pt[1];
//                op_pt.z = (float) u_pt[2];
//                this->undis_features.at(j).push_back(op_pt);
//            }
//
//            for (uint32_t c_idx = 0; c_idx < this->feature_corrs.at(j).at(r_idx).size(); c_idx++) {
//                auto &corr_list = this->feature_corrs.at(j).at(r_idx).at(c_idx);
//                std::vector<double> undis(3 * (corr_list.size() + 1));
//
//                // putting the undistorted point into the end of the vector
//                this->transformToCurLidar(&(this->feature_points.at(j).at(r_idx).at(corr_list.at(0)).pt[0]),
//                                          this->feature_points.at(j).at(r_idx).at(corr_list.at(0)).tick,
//                                          &(undis[undis.size() - 3]));
//
//                // putting uncorrection point into vector
//                memcpy(undis.data(), this->feature_points.at(j).at(r_idx).at(corr_list.at(0)).pt, 24);
//
//                for (uint32_t k = 1; k < corr_list.size(); k++) {
//                    Eigen::Map<const Vec3> map_point(this->prv_feature_points.at(j).points.at(corr_list.at(k)).data());
//                    Eigen::Map<Vec3> transformed_map_point(undis.data() + 3 * k);
//                    this->cur_trajectory.back().pose.inverseTransform(map_point, transformed_map_point);
//                }
//
//                this->output_corrs.at(j).emplace_back(std::vector<double>(undis.begin(), undis.end()));
//            }
//        }
//    }
//
//    if (this->param.output_correspondences) {
//        long timestamp = std::chrono::system_clock::now().time_since_epoch().count();
//        for (uint32_t i = 0; i < this->N_FEATURES; i++) {
//            ofstream cur_file;
//            cur_file.open(std::to_string(timestamp) + "feature_" + std::to_string(i) + "_cor.txt");
//            for (auto vec : this->output_corrs.at(i)) {
//                for (auto val : vec) {
//                    cur_file << std::to_string(val) << " ";
//                }
//                cur_file << std::endl;
//            }
//            cur_file.close();
//        }
//    }
//
//    for (uint32_t j = 0; j < this->N_FEATURES; j++) {
//        for (size_t i = 0; i < this->prv_feature_points.at(j).points.size(); i++) {
//
//            // Publishing in map frame
//            Eigen::Map<Eigen::Vector3f> map_feature(this->map_features.at(j).points.at(i).data);
//
//            // Point is in map frame
//            Eigen::Map<Eigen::Vector3d> prv_feature_point(this->prv_feature_points.at(j).points.at(i).data());
//
//            map_feature = prv_feature_point.cast<float>();
//        }
//    }
}

void LaserOdom::copyTrajectory() {
    for (uint32_t i = 0; i < this->param.num_trajectory_states; i++) {
        this->prev_trajectory.at(i).pose = this->cur_trajectory.at(i).pose;
        this->prev_trajectory.at(i).vel = this->cur_trajectory.at(i).vel;
    }
}

/**
 * Using projection matrix for solution remapping
 */
void LaserOdom::applyRemap() {
    VecX cur_diff;
    uint32_t offset = 0;
    if (this->param.lock_first) {
        cur_diff.resize((this->param.num_trajectory_states - 1) * 12, 1);
        offset = 1;
    } else {
        cur_diff.resize(this->param.num_trajectory_states * 12, 1);
    }

    for (uint32_t i = 0; i + offset < this->param.num_trajectory_states; i++) {
        cur_diff.block<6, 1>(12 * i, 0) =
          this->cur_trajectory.at(i + offset).pose.manifoldMinus(this->prev_trajectory.at(i + offset).pose);
        cur_diff.block<6, 1>(12 * i + 6, 0) =
          this->cur_trajectory.at(i + offset).vel - this->prev_trajectory.at(i + offset).vel;
    }

    if (this->param.plot_stuff) {
        plotMat(this->covar);
        MatX info = this->covar.inverse();
        plotMat(info);
        Eigen::SelfAdjointEigenSolver<MatX> eigs(info);
        plotVec(eigs.eigenvalues(), true);
        plotMat(eigs.eigenvectors());
    }

    MatX AtA = this->covar.inverse();

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eigs(AtA);

    long cnt = 0;
    while (eigs.eigenvalues()(cnt) < this->param.min_eigen) {
        cnt++;
        if (cnt == eigs.eigenvectors().rows())
            break;
    }

    Eigen::MatrixXd Vu = eigs.eigenvectors().transpose();
    Vu.block(0, 0, cnt, Vu.cols()).setZero();
    MatX proj_mat = eigs.eigenvectors().transpose().inverse() * Vu;

    VecX mapped_diff = proj_mat * cur_diff;

    if (this->param.plot_stuff) {
        plotVec(cur_diff, true);
        plotVec(mapped_diff, true);
    }

    for (uint32_t i = 0; i + offset < this->param.num_trajectory_states; i++) {
        this->cur_trajectory.at(i + offset).pose = this->prev_trajectory.at(i + offset).pose;
        this->cur_trajectory.at(i + offset).pose.manifoldPlus(mapped_diff.block<6, 1>(12 * i, 0));

        this->cur_trajectory.at(i + offset).vel =
          this->cur_trajectory.at(i + offset).vel + mapped_diff.block<6, 1>(12 * i + 6, 0);
    }

    // set previous to current trajectory to update operating point
    this->copyTrajectory();
}

// For annoying clamping function
namespace {

inline float clampToRange(const float ip, const float min, const float max) {
    if (ip > max) {
        return max;
    }
    if (ip < min) {
        return min;
    }
    return ip;
}
}

void LaserOdom::addPoints(const std::vector<PointXYZIR> &pts, const int tick, TimeType stamp) {
    static uint32_t n_scan_in_batch = 0;
    bool trigger = false;
    if (tick - this->prv_tick < -200) {
        n_scan_in_batch = (n_scan_in_batch + 1) % this->param.n_window;
        if (n_scan_in_batch == 0) {
            trigger = true;
        }
    }
    if (trigger) {                 // tolerate minor nonlinearity error
        this->feature_extractor.getFeatures(this->cur_scan, this->signals, this->counters, this->indices);
        if (this->initialized) {   // there is a set of previous features from
                                   // last scan
            T_TYPE last_transform;
            auto &ref = this->cur_trajectory.back().pose;

            for (int i = 0; i < this->param.opt_iters; i++) {
                if (i > 0) {
                    memcpy(last_transform.storage.data(), ref.storage.data(), 96);
                }
                if (!this->match()) {
                    return;
                }
                if (i > 0 && ref.isNear(last_transform, this->param.diff_tol)) {
                    break;
                }
            }

            if (this->param.output_trajectory) {
                this->file << this->cur_trajectory.back().pose.storage.format(*(this->CSVFormat)) << std::endl;
            }
            if (this->param.visualize) {
                this->updateViz();
            }
            if (this->output_thread) {
                {
                    std::unique_lock<std::mutex> lk(this->output_mutex);
                    if (this->fresh_output) {
                        // data from last time hasn't been consumed yet
                        LOG_ERROR("Overwriting previous output");
                    }
                    this->undistorted_stamp = this->prv_time;
                    this->undistort_transform = this->cur_trajectory.back().pose;
                    memcpy(this->undistort_velocity.data(), this->cur_trajectory.back().vel.data(), 48);

                    // this->undistort();
                    this->fresh_output = true;
                }
                this->output_condition.notify_one();
            }
        }
        this->rollover(stamp);
        std::fill(this->counters.begin(), this->counters.end(), 0);
    }

    float tick_frac = (float) tick / (float) this->param.max_ticks;
    for (PointXYZIR pt : pts) {
        if (counters.at(pt.ring) >= this->MAX_POINTS) {
            throw std::out_of_range("Rebuild with higher max points");
        }
        this->cur_scan.at(pt.ring)(0, counters.at(pt.ring)) = pt.x;
        this->cur_scan.at(pt.ring)(1, counters.at(pt.ring)) = pt.y;
        this->cur_scan.at(pt.ring)(2, counters.at(pt.ring)) = pt.z;
        this->cur_scan.at(pt.ring)(3, counters.at(pt.ring)) = tick_frac;
        // todo put timestamp here
        this->cur_scan.at(pt.ring)(4, counters.at(pt.ring)) = 0;
        // todo, consider doing this operation using tensors to check for speedup
        this->signals.at(pt.ring)(0, counters.at(pt.ring)) = (float) sqrt(pt.x * pt.x + pt.y * pt.y + pt.z * pt.z);
        this->signals.at(pt.ring)(1, counters.at(pt.ring)) = pt.intensity;

        this->counters.at(pt.ring)++;
    }

    this->prv_tick = tick;
}

void LaserOdom::updateViz() {
    // populate prev with points stored in kd tree
    // These are already transformed to the start of the current scan
//    this->display->removeAll();
//    this->prev_viz->clear();
//    this->cur_viz->clear();
//
//    for (uint32_t i = 0; i < this->N_FEATURES; i++) {
//        for (auto iter = this->prv_feature_points.at(i).points.begin();
//             iter != this->prv_feature_points.at(i).points.end();
//             iter++) {
//            pcl::PointXYZI pt;
//            pt.x = (float) (*iter).at(0);
//            pt.y = (float) (*iter).at(1);
//            pt.z = (float) (*iter).at(2);
//            pt.intensity = 1 + i;
//            this->prev_viz->push_back(pt);
//        }
//
//        for (uint32_t j = 0; j < this->param.n_ring; j++) {
//            for (auto pt : this->feature_points.at(i).at(j)) {
//                double T_pt[3];
//                this->transformToMap(pt.pt, pt.tick, T_pt);
//                pcl::PointXYZI point;
//                point.x = T_pt[0];
//                point.y = T_pt[1];
//                point.z = T_pt[2];
//                point.intensity = 10 + i;
//                this->prev_viz->push_back(point);
//            }
//        }
//    }
//
//    this->display->addPointcloud(this->prev_viz, 0);
}

PCLPointXYZIT LaserOdom::applyIMU(const PCLPointXYZIT &p) {
    // for now don't transform
    PCLPointXYZIT pt;
    pt = p;
    return pt;
}

void LaserOdom::rollover(TimeType stamp) {
    this->prv_time = this->cur_time;
    this->cur_time = stamp;

    this->buildTrees();
    for (unlong i = 0; i < this->param.n_ring; i++) {
        this->counters.at(i) = 0;
    }
    if (!this->initialized) {
        // This check is to avoid initializing against a partial scan.
        if (!this->full_revolution) {
            this->full_revolution = true;
            return;
        }
        size_t feature_count = 0;
        for (uint32_t i = 0; i < this->N_FEATURES; i++) {
            feature_count += this->prv_feature_points.at(i).points.size();
        }
        if (feature_count >= (size_t)(this->param.min_features)) {
            this->initialized = true;
        }
    }
    this->prior_twist = this->cur_trajectory.back().vel;
    this->cur_trajectory.back().pose.transformInverse(this->inv_prior_pose);

    this->cur_trajectory.front().pose = this->cur_trajectory.back().pose;

    for (uint32_t i = 1; i < this->param.num_trajectory_states; i++) {
        this->cur_trajectory.at(i).pose = this->cur_trajectory.at(i - 1).pose;
        this->cur_trajectory.at(i).pose.manifoldPlus(
          (this->param.scan_period / (this->param.num_trajectory_states - 1)) * this->cur_trajectory.back().vel);
    }
    // Now previous trajectory will hold the "motion generated" trajectory
    this->copyTrajectory();
    this->updateDifferences();
}

void LaserOdom::buildTrees() {
    size_t ret_index;
    double out_dist_sqr;
    nanoflann::KNNResultSet<double> resultSet(1);
    resultSet.init(&ret_index, &out_dist_sqr);

    // First step is to check if any existing map features have expired
    for (uint32_t i = 0; i < this->N_FEATURES; i++) {
        for (uint32_t j = 0; j < this->prv_feature_points.at(i).points.size(); j++) {
            if (this->feature_association.at(i).at(j).first > 0) {
                //                Vec3 transformed_pt;
                Eigen::Map<const Vec3> Vecpt(this->prv_feature_points.at(i).points.at(j).data(), 3, 1);
                //                this->cur_trajectory.front().pose.transform(Vecpt, transformed_pt);
                //                this->cur_trajectory.back().pose.inverseTransform(Vecpt, transformed_pt);
                // Check if feature is within range of map
                if (l2length(Vecpt.data(), 3) < this->param.local_map_range) {
                    //                    memcpy(this->prv_feature_points.at(i).points.at(j).data(), Vecpt.data(), 24);
                    if (this->feature_association.at(i).at(j).second == AssociationStatus::CORRESPONDED) {
                        this->feature_association.at(i).at(j).second = AssociationStatus::UNCORRESPONDED;
                        this->feature_association.at(i).at(j).first = this->param.TTL;
                    } else {
                        --(this->feature_association.at(i).at(j).first);
                    }
                    --(this->feature_association.at(i).at(j).first);
                    continue;
                }
            }
            auto loc = this->prv_feature_points.at(i).points.size() - 1;
            memcpy(this->prv_feature_points.at(i).points.at(j).data(),
                   this->prv_feature_points.at(i).points.at(loc).data(),
                   24);
            this->feature_association.at(i).at(j) = this->feature_association.at(i).at(loc);
            this->prv_feature_points.at(i).points.resize(loc);
            this->feature_association.at(i).resize(loc);
        }
        // rebuild kdtree index
        if (!this->prv_feature_points.at(i).points.empty()) {
            this->feature_idx.at(i)->buildIndex();
        }
        for (uint16_t j = 0; j < this->param.n_ring; j++) {
//            for (PointXYZIT pt : this->feature_points.at(i).at(j)) {
//                double transformed_pt[3] = {0};
//                //                this->transformToCurLidar(pt.pt, pt.tick, transformed_pt);
//                this->transformToMap(pt.pt, pt.tick, transformed_pt);
//
//                resultSet.init(&ret_index, &out_dist_sqr);
//
//                this->feature_idx.at(i)->findNeighbors(resultSet, transformed_pt, nanoflann::SearchParams(32, 1));
//
//                float map_density;
//                if (this->feature_definitions.at(i).residual == PointToLine) {
//                    map_density = this->param.edge_map_density;
//                } else {
//                    map_density = this->param.flat_map_density;
//                }
//                if (out_dist_sqr > map_density) {
//                    this->feature_association.at(i).emplace_back(
//                      std::make_pair(this->param.TTL, AssociationStatus::UNCORRESPONDED));
//                    this->prv_feature_points.at(i).points.push_back(
//                      std::array<double, 3>{transformed_pt[0], transformed_pt[1], transformed_pt[2]});
//                }
//            }
        }
        // rebuild kdtree index
        if (!this->prv_feature_points.at(i).points.empty()) {
            this->feature_idx.at(i)->buildIndex();
        }
    }
}

bool LaserOdom::findCorrespondingPoints(const Vec3 &query, const uint32_t &f_idx, std::vector<size_t> *index) {
    using ContType = std::pair<size_t, double>;
    std::vector<ContType> indices_dists;
    nanoflann::RadiusResultSet<double, size_t> resultSet(this->param.max_correspondence_dist, indices_dists);
    uint32_t knn = 0;
    switch (this->feature_residuals.at(f_idx)) {
        case PointToLine: knn = 2; break;
        case PointToPlane: knn = 3; break;
        default: knn = 0;
    }
    this->feature_idx.at(f_idx)->findNeighbors(resultSet, query.data(), nanoflann::SearchParams());
    if (indices_dists.size() < knn) {
        return false;
    }
    std::sort(indices_dists.begin(), indices_dists.end(), [](const ContType &left, const ContType &right) {
        return left.second < right.second;
    });

    index->clear();
    // In order to ensure correspondences are not picked along a scan line,
    // use bins and enforce that the set of correspondences should fall into at
    // least 2 bins
    double offset = 0;
    double current_azimuth = 0;
    uint16_t counter = 0;
    bool non_zero_bin = false;
    double const *point;
    while (counter < indices_dists.size()) {
        point = this->prv_feature_points.at(f_idx).points.at(indices_dists.at(counter).first).data();
        if (counter == 0) {
            offset = std::atan2(point[2], std::sqrt(point[0] * point[0] + point[1] * point[1]));
        } else {
            current_azimuth = std::atan2(point[2], std::sqrt(point[0] * point[0] + point[1] * point[1]));
            double t_bin = (current_azimuth - offset) / this->param.azimuth_tol;
            int cur_bin = t_bin > 0 ? (int) (t_bin + 0.5) : (int) (t_bin - 0.5);
            if (cur_bin != 0) {
                non_zero_bin = true;
            }
        }
        if (index->size() + 1 != knn || non_zero_bin) {
            index->push_back(indices_dists.at(counter).first);
        }
        if (index->size() == knn) {
            return true;
        }
        ++counter;
    }
    return false;
}

bool LaserOdom::outOfBounds(const Vec3 &query, const uint32_t &f_idx, const std::vector<size_t> &index) {
    Eigen::Map<const Vec3> pA(this->prv_feature_points.at(f_idx).points.at(index.at(0)).data());
    Eigen::Map<const Vec3> pB(this->prv_feature_points.at(f_idx).points.at(index.at(1)).data());
    if (this->feature_residuals.at(f_idx) == PointToPlane) {
        return false;
    } else {
        Vec3 AB = pB - pA;
        Vec3 Aq = query - pA;

        double eta = Aq.dot(AB) / AB.dot(AB);

        if (eta < -this->param.max_extrapolation || eta > (1.0 + this->param.max_extrapolation)) {
            return true;
        }
    }
    return false;
}

bool LaserOdom::match() {
    double zero_pt[3] = {0};

    // set up pointer for evaluating residuals
    const double **parameters;
    parameters = new const double *[4];
    std::vector<double> residuals;

    ceres::Problem problem;

    // Add motion residuals
    // The prior for the pose at the end of the previous trajectory, the prior for twist is the twist at the end of the
    // previous
    // trajectory
    //    Transformation<> identity;
    if (this->param.motion_prior) {
        this->cv_vector.at(0).calculateLinInvCovariance();

        Vec12 op_error;

        T_TYPE diff;
        this->cur_trajectory.begin()->pose.compose(this->inv_prior_pose, diff);
        op_error.block<6, 1>(0, 0) = diff.logMap();
        op_error.block<6, 1>(6, 0) = this->cur_trajectory.begin()->vel - this->prior_twist;

        // expect the correction to be the negative of the error
        ceres::CostFunction *prior_cost = new ceres::NormalPrior(this->cv_vector.at(0).inv_covar.sqrt(), -op_error);

        problem.AddResidualBlock(prior_cost, nullptr, this->param_blocks.at(0).data());
    }

    for (uint32_t i = 0; i < this->param.num_trajectory_states; i++) {
        if (i + 1 < this->param.num_trajectory_states) {
            this->cv_vector.at(i).calculateLinInvCovariance();

            Vec12 op_error;

            op_error.block<6, 1>(0, 0) =
              this->cur_trajectory.at(i + 1).pose.manifoldMinus(this->cur_trajectory.at(i).pose) -
              (this->cv_vector.at(i).tkp1 - this->cv_vector.at(i).tk) * this->cur_trajectory.at(i).vel;
            op_error.block<6, 1>(6, 0) = this->cur_trajectory.at(i + 1).vel - this->cur_trajectory.at(i).vel;

            ceres::CostFunction *motion_cost = new wave_optimization::ConstantVelocityPrior(
              this->cv_vector.at(i).inv_covar.sqrt(), op_error, this->cv_vector.at(i).tkp1 - this->cv_vector.at(i).tk);

            problem.AddResidualBlock(
              motion_cost, nullptr, this->param_blocks.at(i).data(), this->param_blocks.at(i + 1).data());
        }
    }

    // now loop over each type of feature, and generate residuals for each
    Eigen::Matrix<double, 12, 12> candle, hat;

    uint32_t k, kp1;
    double tau;

    std::vector<size_t> ret_indices;
    std::vector<double> out_dist_sqr;

    uint64_t cur_PtL_idx = 0, cur_PtP_idx = 0;

//    for (uint16_t i = 0; i < this->N_FEATURES; i++) {
//        for (uint16_t j = 0; j < this->param.n_ring; j++) {
//            this->feature_corrs.at(i).at(j).clear();
//            for (uint64_t pt_cntr = 0; pt_cntr < this->feature_points.at(i).at(j).size(); pt_cntr++) {
//                double transformed[3];
//                this->transformToMap(this->feature_points.at(i).at(j).at(pt_cntr).pt,
//                                     this->feature_points.at(i).at(j).at(pt_cntr).tick,
//                                     transformed,
//                                     k,
//                                     kp1,
//                                     tau);
//                Eigen::Map<const Vec3> query(transformed, 3, 1);
//                ret_indices.clear();
//                out_dist_sqr.clear();
//                Eigen::Matrix3f covZ;
//                this->range_sensor->getEuclideanCovariance(query.data(), j, covZ);
//                if (this->findCorrespondingPoints(query, i, &ret_indices)) {
//                    // check if there is enough memory for next cost function
//                    if (cur_PtL_idx == this->PtLMem.size()) {
//                        LOG_ERROR("Pre allocated point to line memory block is too small, reseting");
//                        return false;
//                    }
//                    if (cur_PtP_idx == this->PtPMem.size()) {
//                        LOG_ERROR("Pre allocated point to plane memory block is too small, reseting");
//                        return false;
//                    }
//                    if (this->param.no_extrapolation && this->outOfBounds(query, i, ret_indices)) {
//                        break;
//                    }
//                    this->cv_vector.at(k).tau = &tau;
//                    this->cv_vector.at(k).calculateStuff(hat, candle);
//                    ceres::CostFunction *cost_function;
//                    wave_optimization::SE3PointToLineGP *pTl_cost_function = nullptr;
//                    wave_optimization::SE3PointToPlaneGP *pTPl_cost_function = nullptr;
//                    double rescale;
//                    switch (this->feature_definitions.at(i).residual) {
//                        case PointToLine:
//                            if (this->param.treat_lines_as_planes) {
//                                this->PtPMem.at(cur_PtP_idx).hat = hat.block<6, 12>(0, 0);
//                                this->PtPMem.at(cur_PtP_idx).candle = candle.block<6, 12>(0, 0);
//                                this->PtPMem.at(cur_PtP_idx).T0_pt = query;
//                                pTPl_cost_function = new wave_optimization::SE3PointToPlaneGP(
//                                  this->prv_feature_points.at(i).points.at(ret_indices.at(0)).data(),
//                                  this->prv_feature_points.at(i).points.at(ret_indices.at(1)).data(),
//                                  zero_pt,
//                                  this->PtPMem.at(cur_PtP_idx),
//                                  covZ.cast<double>(),
//                                  this->param.use_weighting);
//                                residuals.resize(1);
//                                rescale = pTPl_cost_function->weight;
//                                cost_function = pTPl_cost_function;
//                            } else {
//                                this->PtLMem.at(cur_PtL_idx).hat = hat.block<6, 12>(0, 0);
//                                this->PtLMem.at(cur_PtL_idx).candle = candle.block<6, 12>(0, 0);
//                                this->PtLMem.at(cur_PtP_idx).T0_pt = query;
//                                pTl_cost_function = new wave_optimization::SE3PointToLineGP(
//                                  this->prv_feature_points.at(i).points.at(ret_indices.at(0)).data(),
//                                  this->prv_feature_points.at(i).points.at(ret_indices.at(1)).data(),
//                                  this->PtLMem.at(cur_PtL_idx),
//                                  covZ.cast<double>(),
//                                  this->param.use_weighting);
//                                residuals.resize(2);
//                                rescale = pTl_cost_function->weight_matrix.trace();
//                                cost_function = pTl_cost_function;
//                            }
//                            break;
//                        case PointToPlane:
//                            this->PtPMem.at(cur_PtP_idx).hat = hat.block<6, 12>(0, 0);
//                            this->PtPMem.at(cur_PtP_idx).candle = candle.block<6, 12>(0, 0);
//                            this->PtPMem.at(cur_PtP_idx).T0_pt = query;
//                            pTPl_cost_function = new wave_optimization::SE3PointToPlaneGP(
//                              this->prv_feature_points.at(i).points.at(ret_indices.at(0)).data(),
//                              this->prv_feature_points.at(i).points.at(ret_indices.at(1)).data(),
//                              this->prv_feature_points.at(i).points.at(ret_indices.at(2)).data(),
//                              this->PtPMem.at(cur_PtP_idx),
//                              covZ.cast<double>(),
//                              this->param.use_weighting);
//                            residuals.resize(1);
//                            rescale = pTPl_cost_function->weight;
//                            cost_function = pTPl_cost_function;
//                            break;
//                        default: continue;
//                    }
//
//                    parameters[0] = this->param_blocks.at(k).data();
//                    parameters[1] = this->param_blocks.at(kp1).data();
//
//                    if (!cost_function->Evaluate(parameters, residuals.data(), nullptr)) {
//                        LOG_ERROR("Cost function did not evaluate");
//                        delete cost_function;
//                        continue;
//                    }
//                    rescale *= rescale;
//
//                    if (norm(residuals) > rescale * this->param.max_residual_val) {
//                        delete cost_function;
//                        continue;
//                    }
//
//                    ceres::LossFunction *p_LossFunction = new BisquareLoss(this->param.robust_param);
//                    std::vector<uint64_t> corr_list;
//                    corr_list.emplace_back(pt_cntr);
//                    for (auto index : ret_indices) {
//                        corr_list.emplace_back(index);
//                        this->feature_association.at(i).at(index).second = AssociationStatus::CORRESPONDED;
//                    }
//                    this->feature_corrs.at(i).at(j).emplace_back(corr_list);
//
//                    problem.AddResidualBlock(cost_function,
//                                             p_LossFunction,
//                                             this->param_blocks.at(k).data(),
//                                             this->param_blocks.at(kp1).data());
//
//                    if (pTl_cost_function) {
//                        cur_PtL_idx++;
//                    } else if (pTPl_cost_function) {
//                        cur_PtP_idx++;
//                    }
//                }
//            }
//        }
//    }
//
//    if (this->param.lock_first) {
//        this->covar.resize(12 * (this->param.num_trajectory_states - 1), 12 * (this->param.num_trajectory_states - 1));
//    } else {
//        this->covar.resize(12 * this->param.num_trajectory_states, 12 * this->param.num_trajectory_states);
//    }
//
//    for (auto &block : this->param_blocks) {
//        problem.AddParameterBlock(block.data(), 12, nullptr);
//    }
//
//    ceres::Solver::Options options;
//    options.linear_solver_type = ceres::DENSE_QR;
//    options.max_num_iterations = this->param.max_inner_iters;
//    options.max_num_consecutive_invalid_steps = 30;
//    options.function_tolerance = 1e-8;
//    options.parameter_tolerance = 1e-7;
//    options.logging_type = ceres::LoggingType::SILENT;
//
//    ceres::Covariance::Options covar_options;
//    covar_options.sparse_linear_algebra_library_type = ceres::SparseLinearAlgebraLibraryType::SUITE_SPARSE;
//    covar_options.algorithm_type = ceres::CovarianceAlgorithmType::SPARSE_QR;
//
//    if (this->param.solver_threads < 1) {
//        options.num_threads = std::thread::hardware_concurrency();
//        options.num_linear_solver_threads = std::thread::hardware_concurrency();
//        covar_options.num_threads = std::thread::hardware_concurrency();
//    } else {
//        options.num_threads = this->param.solver_threads;
//        options.num_linear_solver_threads = this->param.solver_threads;
//        covar_options.num_threads = this->param.solver_threads;
//    }
//
//    if (this->param.lock_first) {
//        problem.SetParameterBlockConstant(this->param_blocks.at(0).data());
//    }
//
//    if (problem.NumResidualBlocks() < this->param.min_residuals) {
//        LOG_ERROR("Less than expected residuals, resetting");
//        LOG_ERROR("%d residuals, threshold is %d", problem.NumResidualBlocks(), this->param.min_residuals);
//        this->resetTrajectory();
//        this->initialized = false;
//        delete[] parameters;
//        return false;
//    } else if (!this->param.only_extract_features) {
//        ceres::Solver::Summary summary;
//        ceres::Solve(options, &problem, &summary);
//        if (this->param.plot_stuff) {
//            LOG_INFO("%s", summary.FullReport().c_str());
//        }
//        //        ceres::Covariance covariance(covar_options);
//        //        if (!covariance.Compute(this->param_blocks, &problem)) {
//        //            LOG_ERROR("covariance did not compute");
//        //        }
//        //        covariance.GetCovarianceMatrixInTangentSpace(this->param_blocks, this->covar.data());
//        if (this->param.solution_remapping) {
//            this->applyRemap();
//        }
//        this->updateOperatingPoint();
//    }
//    delete[] parameters;
    return true;
}

void LaserOdom::updateDifferences() {
    for (uint32_t i = 0; i < this->cur_difference.size(); i++) {
        this->cur_difference.at(i).hat_multiplier.block<6, 1>(6, 0) = this->cur_trajectory.at(i).vel;
        this->cur_difference.at(i).candle_multiplier.block<6, 1>(0, 0) =
          this->cur_trajectory.at(i + 1).pose.manifoldMinus(this->cur_trajectory.at(i).pose);
        this->cur_difference.at(i).candle_multiplier.block<6, 1>(6, 0) =
          T_TYPE::SE3ApproxInvLeftJacobian(this->cur_difference.at(i).candle_multiplier.block<6, 1>(0, 0)) *
          this->cur_trajectory.at(i + 1).vel;
    }
}

void LaserOdom::updateOperatingPoint() {
    // update trajectory with current twist vectors
    for (uint32_t i = 0; i < this->param_blocks.size(); i++) {
        this->cur_trajectory.at(i).pose.manifoldPlus(this->param_blocks.at(i).block<6, 1>(0, 0));
        this->cur_trajectory.at(i).vel += this->param_blocks.at(i).block<6, 1>(6, 0);
        this->param_blocks.at(i).setZero();
    }
    this->updateDifferences();
}

void LaserOdom::resetTrajectory() {
    for (auto tra : this->cur_trajectory) {
        tra.pose.setIdentity();
        tra.vel.setZero();
    }
    this->prior_twist.setZero();
    this->current_twist.setZero();
}

}  // namespace wave
