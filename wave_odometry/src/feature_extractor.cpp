#include "wave/odometry/feature_extractor.hpp"

namespace wave {

void FeatureExtractor::setup() {
    this->threadpool = std::unique_ptr<Eigen::ThreadPool>(new Eigen::ThreadPool(this->param.eigen_threads));
    this->thrddev = std::unique_ptr<Eigen::ThreadPoolDevice>(
            new Eigen::ThreadPoolDevice(this->threadpool.get(), this->param.eigen_threads));

    this->valid_pts.resize(this->n_ring);
    this->scores.resize(this->n_ring);
    this->kernels.resize(this->param.N_SCORES);
    this->filtered_scores.resize(this->param.N_FEATURES);
    for(uint32_t i = 0; i < this->param.N_FEATURES; i++) {
        this->filtered_scores.at(i).resize(this->n_ring);
    }

    this->kernels.at(0) = Eigen::TensorMap<Eigen::Tensor<float, 1>>(loam_kernel, 11);
    this->kernels.at(1) = Eigen::TensorMap<Eigen::Tensor<float, 1>>(LoG_kernel, 11);
    this->kernels.at(2) = Eigen::TensorMap<Eigen::Tensor<float, 1>>(FoG_kernel, 11);
    this->kernels.at(3).setConstant(1.0);
    this->kernels.at(4).setConstant(1.0);

    this->ready = true;
}

FeatureExtractor::FeatureExtractor(FeatureExtractorParams params, unlong n_ring) : param(params), n_ring(n_ring) {
    this->setup();
}

void FeatureExtractor::setParams(FeatureExtractorParams params, unlong n_ring) {
    this->param = std::move(params);
    this->n_ring = n_ring;

    if(!this->ready) {
        this->setup();
    }
}

void FeatureExtractor::computeScores(const Tensorf &signals, const Vec<int> &range) {
    if (!this->ready) {
        throw std::length_error("Must set feature parameters before using");
    }
    Eigen::array<ptrdiff_t, 1> dims({1});
    Eigen::Tensor<float, 1> sum_kernel(this->param.variance_window);
    sum_kernel.setConstant(1.0);

    for (uint32_t i = 0; i < this->n_ring; i++) {
        auto max = (int) range.at(i);
        //resize
        if (max < 11) {
            continue;
        }
        this->scores.at(i).resize(this->param.N_SCORES, max - 10);

        for (ulong j = 0; j < this->param.N_SCORES; j++) {
            int s_idx = static_cast<int>(this->param.feature_definitions.at(j).criteria.front().signal);

            // todo have flexibility for different kernel sizes
            if (j < 3) {
                this->scores.at(i).slice(ar2({j, 0}), ar2({1, max - 10})).device(*(this->thrddev)) =
                        signals.at(i).slice(ar2({s_idx, 0}), ar2({1, max})).convolve(this->kernels.at(j), dims);
                // or if sample variance
            } else {
                auto &N = this->param.variance_window;
                Eigen::Tensor<float, 1> Ninv(1);
                Ninv.setConstant(1.0 / (float) N);
                Eigen::Tensor<float, 1> Nm1inv(1);
                Nm1inv.setConstant(1.0 / (float) (N - 1));

                // so called computational formula for sample variance
                this->scores.at(i).slice(ar2({j, 0}), ar2({1, max - 10}))
                  .device(*(this->thrddev)) =
                  (signals.at(i).slice(ar2({s_idx, 0}), ar2({1, max})).square().convolve(sum_kernel, dims) -
                   signals.at(i)
                     .slice(ar2({s_idx, 0}), ar2({1, max}))
                     .convolve(sum_kernel, dims)
                     .square()
                     .convolve(Ninv, dims))
                    .convolve(Nm1inv, dims);
            }

        }
    }
}

void FeatureExtractor::preFilter(const Tensorf &scan, const Tensorf &signals, const Vec<int> &range) {
    for (uint32_t i = 0; i < this->n_ring; i++) {
        if(range.at(i) < 11) {
            this->valid_pts.at(i).resize(0);
            continue;
        }
        this->valid_pts.at(i).resize(range.at(i));
        // Assume all points are valid until proven otherwise
        this->valid_pts.at(i).setConstant(true);

        Eigen::array<ptrdiff_t, 1> dims({1});
        Eigen::Tensor<float, 1> diff_kernel(2);
        diff_kernel.setValues({1.f, -1.f});

        Eigen::Tensor<float, 1> rng_diff;
        rng_diff.resize(range.at(i) - 1);
        rng_diff.device(*(this->thrddev)) = signals.at(i).slice(ar2({0, 0}), ar2({1, range.at(i)})).convolve(diff_kernel, dims).chip(0,0);

        Eigen::Tensor<bool, 1> oc_tol2_cond = rng_diff.abs() > this->param.occlusion_tol_2;
        Eigen::Tensor<bool, 1> ang_diff_cond =
                scan.at(i).slice(ar2({4, 0}), ar2({1, range.at(i)})).convolve(diff_kernel, dims).chip(0,0) < this->param.occlusion_tol;

        Eigen::Tensor<bool, 1> branch_1_cond = oc_tol2_cond && ang_diff_cond && (rng_diff > 0.0f);
        Eigen::Tensor<bool, 1> branch_2_cond = oc_tol2_cond && ang_diff_cond && (rng_diff < 0.0f);

        // This section excludes any points whose nearby surface is
        // near to parallel to the laser beam
        Eigen::array<ptrdiff_t, 2> dims2({0, 1});
        Eigen::Tensor<float, 2> ex_diff_K(1, 2);
        ex_diff_K.setValues({{1.f, -1.f}});

        Eigen::Tensor<float, 1> delforback;

        delforback.resize(range.at(i) - 1);
        delforback.device(*(this->thrddev)) = scan.at(i)
                .slice(ar2({0, 0}), ar2({3, range.at(i)})).convolve(ex_diff_K, dims2).square().sum(Earr<1>({0}));

        Eigen::Tensor<float, 1> sqr_rng = signals.at(i).slice(ar2({0, 1}), ar2({1, range.at(i) - 2})).square().chip(0,0);

        Eigen::Tensor<bool, 1> low_side_cond = delforback.slice(ar1({0}), ar1({range.at(i) - 2})) > this->param.parallel_tol * sqr_rng;
        Eigen::Tensor<bool, 1> high_side_cond = delforback.slice(ar1({1}), ar1({range.at(i) - 2})) > this->param.parallel_tol * sqr_rng;

        Eigen::Tensor<bool, 1> branch_3_cond = low_side_cond && high_side_cond;

        Eigen::Tensor<bool, 1> false_tensor(range.at(i) - 2);
        false_tensor.setConstant(false);
        this->valid_pts.at(i).slice(ar1({1}), ar1({range.at(i) - 2})) =
                branch_3_cond.select(false_tensor, this->valid_pts.at(i).slice(ar1({1}), ar1({range.at(i) - 2})));

        for (int j = 1; j + 1 < range.at(i); j++) {
            if (branch_1_cond(j)) {
                int start;
                (j - 5) >= 0 ? start = j - 5 : start = 0;

                ar2 starts({start});
                ar2 extents({j - start});

                this->valid_pts.at(i).slice(starts, extents).setConstant(false);
            }
            if (branch_2_cond(j)) {
                int end;
                (j + 5) >= range.at(i) ? end = range.at(i) - 1 : end = j + 5;

                ar2 starts({j});
                ar2 extents({end - j});

                this->valid_pts.at(i).slice(starts, extents).setConstant(false);
            }
        }
    }
}

namespace {

Eigen::Tensor<bool, 1> near_zero_score(const Eigen::Tensor<float, 1> &score, float threshold) {
    return score.abs() < threshold;
}

Eigen::Tensor<bool, 1> high_pos_score(const Eigen::Tensor<float, 1> &score, float threshold) {
    return score > threshold;
}

Eigen::Tensor<bool, 1> high_neg_score(const Eigen::Tensor<float, 1> &score, float threshold) {
    return score < -threshold;
}
}

void FeatureExtractor::buildFilteredScore(const Vec<int> &range) {
    for (uint32_t k = 0; k < this->param.N_FEATURES; k++) {
        auto &def = this->param.feature_definitions.at(k);
        // get primary score index by kernel type
        std::vector<std::function<Eigen::Tensor<bool, 1>(const Eigen::Tensor<float, 1> &, double)>> compfuns;
        std::vector<uint32_t> k_idx;
        std::vector<uint32_t> k_offsets;
        int offset = 0;
        compfuns.resize(def.criteria.size());
        k_idx.resize(def.criteria.size());
        for (uint32_t i = 0; i < def.criteria.size(); i++) {
            switch (def.criteria.at(i).sel_pol) {
                case FeatureExtractorParams::SelectionPolicy::NEAR_ZERO: compfuns.at(i) = near_zero_score; break;
                case FeatureExtractorParams::SelectionPolicy::HIGH_POS: compfuns.at(i) = high_pos_score; break;
                case FeatureExtractorParams::SelectionPolicy::HIGH_NEG: compfuns.at(i) = high_neg_score; break;
                default: throw std::out_of_range("Invalid Comparison Function");
            }
            k_idx.at(i) = static_cast<uint32_t>(def.criteria.at(i).kernel);
            // todo something about this
            k_offsets.emplace_back(5);
            offset = 5;
        }

        for (uint32_t i = 0; i < this->n_ring; i++) {
            this->filtered_scores.at(k).at(i).clear();
            if (range.at(i) < 11) {
                continue;
            }
            Eigen::Tensor<bool, 1> condition = this->valid_pts.at(i).slice(ar1({offset}), ar1({range.at(i) - 2 * offset}));
            for (uint32_t l = 0; l < def.criteria.size(); l++) {
                condition = condition && compfuns.at(l)(this->scores.at(i).chip(k_idx.at(l), 0), *(def.criteria.at(l).threshold));
            }

            for (int j = offset; j + offset < condition.dimension(0); j++) {
                if (condition(j)) {
                    this->filtered_scores[k].at(i).emplace_back(j, this->scores.at(i)(k_idx.at(0), j - offset));
                }
            }
        }
    }
}

void FeatureExtractor::flagNearbyPoints(const uint32_t p_idx, Eigen::Tensor<bool, 1> &valid) {
    for (uint32_t j = 0; j < this->param.key_radius; j++) {
        if (p_idx + j + 1 >= valid.dimension(0)) {
            break;
        }
        valid(p_idx + j + 1) = false;
    }
    for (uint32_t j = 0; j < this->param.key_radius; j++) {
        if (p_idx < j + 1) {
            break;
        }
        valid(p_idx - j - 1) = false;
    }
}

void FeatureExtractor::sortAndBin(const Tensorf &scan, TensorIdx &feature_indices) {
    std::vector<unlong> cnt_in_bins;
    cnt_in_bins.resize(this->param.angular_bins);
    for (uint32_t i = 0; i < this->param.N_FEATURES; i++) {
        for (unlong j = 0; j < this->n_ring; j++) {
            auto &def = this->param.feature_definitions.at(i);
            auto &pol = def.criteria.at(0).sel_pol;
            auto &filt_scores = this->filtered_scores.at(i).at(j);

            feature_indices.at(i).at(j) = Eigen::Tensor<int, 1>(*(def.n_limit));
            uint64_t feat_cnt = 0;

            Eigen::Tensor<bool, 1> valid_pts_copy;
            valid_pts_copy = this->valid_pts.at(j);

            unlong max_bin = *(def.n_limit) / this->param.angular_bins;
            std::fill(cnt_in_bins.begin(), cnt_in_bins.end(), 0);

            if (pol != FeatureExtractorParams::SelectionPolicy::HIGH_POS) {
                std::sort(filt_scores.begin(),
                          filt_scores.end(),
                          [](const std::pair<unlong, double> lhs, const std::pair<unlong, double> rhs) {
                              return lhs.second < rhs.second;
                          });
            } else {
                std::sort(filt_scores.begin(),
                          filt_scores.end(),
                          [](const std::pair<unlong, double> lhs, const std::pair<unlong, double> rhs) {
                              return lhs.second > rhs.second;
                          });
            }

            for (auto score : filt_scores) {
                // Using data conversion to floor result
                unlong bin = (unlong) scan.at(j)(3, score.first) * this->param.angular_bins;
                if (cnt_in_bins.at(bin) >= max_bin) {
                    continue;
                }
                if (valid_pts_copy(score.first)) {
                    feature_indices.at(i).at(j)(feat_cnt) = (int) score.first;
                    feat_cnt++;

                    this->flagNearbyPoints(score.first, valid_pts_copy);
                    cnt_in_bins.at(bin)++;
                }
            }
            feature_indices.at(i).at(j) = feature_indices.at(i).at(j).slice(ar1({0}), ar1({feat_cnt}));
        }
    }
}

void FeatureExtractor::getFeatures(const Tensorf &scan, const Tensorf &signals,
                                   const std::vector<int> &range, TensorIdx &indices) {
    if (scan.size() != this->n_ring || signals.size() != this->n_ring) {
        throw std::length_error("mismatch between configured ring count and input scan/signals size");
    }

    this->computeScores(signals, range);
    this->preFilter(scan, signals, range);
    this->buildFilteredScore(range);
    this->sortAndBin(scan, indices);
}

}
