// モーダル解析（固有振動数）。

#include "physics/ModalAnalysis.h"
#include "physics/ChronoCompat.h"
#include "physics/PhysicsSystem.h"

#include <chrono/physics/ChSystem.h>
#ifdef WIZ_HAVE_MODAL
#include <chrono_modal/ChEigenvalueSolver.h>
#endif

#include <algorithm>
#include <cmath>
#include <complex>
#include <exception>
#include <string>
#include <vector>

using namespace chrono;
using namespace wizengine::physics::compat;

namespace wizengine {
namespace physics {

// ---- モーダル解析（Chrono::Modal。B の 20）--------------------------------
// 系の質量行列 M・剛性行列 K・拘束ヤコビアン Cq を取り出して、非減衰の
// 一般化固有値問題を Lanczos で解く。接触は入らない（K に無い）。剛体だけの
// 系では拘束のモードしか出ないので、ケーブル（FEA）がある系で意味を持つ。

bool modalFrequencies(PhysicsSystem& system, int count, std::vector<double>& hz,
                      std::string& why) {
    hz.clear();
#ifdef WIZ_HAVE_MODAL
    if (system.backend() == PhysicsBackend::Multicore) {
        why = "modal analysis needs the core backend";
        return false;
    }
    if (count < 1) {
        why = "count must be >= 1";
        return false;
    }
    try {
        system.sys()->Setup();
        system.sys()->Update();
        ChSparseMatrix M, K, Cq;
        if (!systemMatrices(system.sys().get(), M, K, Cq, 0)) {
            why = "this Chrono has no system matrix accessors";
            return false;
        }
        if (M.rows() == 0) {
            why = "the system is empty";
            return false;
        }
        modal::ChModalSolveUndamped solver(count, 1e-5, 500, 1e-10, false,
                                           modal::ChGeneralizedEigenvalueSolverLanczos());
        ChMatrixDynamic<std::complex<double>> V;
        ChVectorDynamic<std::complex<double>> eig;
        ChVectorDynamic<double> freq;
        solver.Solve(M, K, Cq, V, eig, freq);
        for (int i = 0; i < freq.size(); ++i) {
            if (std::isfinite(freq(i))) hz.push_back(freq(i));
        }
        std::sort(hz.begin(), hz.end());
        if (int(hz.size()) > count) hz.resize(std::size_t(count));
        return true;
    } catch (const std::exception& e) {
        why = e.what();
        return false;
    }
#else
    (void)system;
    (void)count;
    why = "Chrono::Modal is not built in (configure with -DWIZ_WITH_CHRONO_MODAL=ON)";
    return false;
#endif
}

}  // namespace physics
}  // namespace wizengine
