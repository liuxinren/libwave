/**
 * This residual penalizes the distance of a number of points to a line fit to those points
 *
 * The derivative of the points wrt the transform parameters is calculated using ceres EvaluationCallback API. This
 * abstracts the transformation interpolation technique out of the residual
 */

#ifndef WAVE_IMPLICIT_LINE_HPP
#define WAVE_IMPLICIT_LINE_HPP

#include <Eigen/Core>
#include <ceres/ceres.h>
#include "wave/geometry/transformation.hpp"
#include "wave/odometry/feature_track.hpp"

namespace wave {

template <int cnt, int state_dim, int... num>
class ImplicitLineResidual : public ceres::SizedCostFunction<3*cnt, 3, num...> {
 public:
    virtual ~ImplicitLineResidual() {}

    ImplicitLineResidual(const FeatureTrack<state_dim> &tracks) : tracks(tracks) {}

    virtual bool Evaluate(double const *const *parameters, double *residuals, double **jacobians) const;

 private:
    const FeatureTrack<state_dim> &tracks;
    const double *ptdata;
    const double *avgdata;
};
}

#include "impl/implicit_line_impl.hpp"

#endif  // WAVE_IMPLICIT_LINE_HPP