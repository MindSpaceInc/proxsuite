#include <qp/settings.hpp>
#include <qp/status.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/eigen.h>

namespace proxsuite {
namespace qp {
namespace python {
template <typename T>
void exposeSettings(pybind11::module_ m) {

	::pybind11::enum_<InitialGuessStatus>(m, "initial_guess")
		.value("NO_INITIAL_GUESS", InitialGuessStatus::NO_INITIAL_GUESS)
		.value("UNCONSTRAINED_INITIAL_GUESS", InitialGuessStatus::UNCONSTRAINED_INITIAL_GUESS)
		.value("EQUALITY_CONSTRAINED_INITIAL_GUESS", InitialGuessStatus::EQUALITY_CONSTRAINED_INITIAL_GUESS)
		.value("WARM_START_WITH_PREVIOUS_RESULT", InitialGuessStatus::WARM_START_WITH_PREVIOUS_RESULT)
		.value("WARM_START", InitialGuessStatus::WARM_START)
		.value("COLD_START", InitialGuessStatus::COLD_START)
		.export_values();

	::pybind11::class_<Settings<T>>(m, "Settings")
			.def(::pybind11::init()) // constructor
			.def_readwrite("alpha_bcl", &Settings<T>::alpha_bcl)
			.def_readwrite("beta_bcl", &Settings<T>::beta_bcl)
			.def_readwrite(
					"refactor_dual_feasibility_threshold",
					&Settings<T>::refactor_dual_feasibility_threshold)
			.def_readwrite(
					"refactor_rho_threshold", &Settings<T>::refactor_rho_threshold)
			.def_readwrite("mu_max_eq", &Settings<T>::mu_max_eq)
			.def_readwrite("mu_max_in", &Settings<T>::mu_max_in)
			.def_readwrite("mu_update_factor", &Settings<T>::mu_update_factor)
			.def_readwrite("cold_reset_mu_eq", &Settings<T>::cold_reset_mu_eq)
			.def_readwrite("cold_reset_mu_in", &Settings<T>::cold_reset_mu_in)
			.def_readwrite("max_iter", &Settings<T>::max_iter)
			.def_readwrite("max_iter_in", &Settings<T>::max_iter_in)
			.def_readwrite("eps_abs", &Settings<T>::eps_abs)
			.def_readwrite("eps_rel", &Settings<T>::eps_rel)
			.def_readwrite("eps_primal_inf", &Settings<T>::eps_primal_inf)
			.def_readwrite("eps_dual_inf", &Settings<T>::eps_dual_inf)
			.def_readwrite("nb_iterative_refinement", &Settings<T>::nb_iterative_refinement)
			.def_readwrite("initial_guess", &Settings<T>::initial_guess)
			.def_readwrite("verbose", &Settings<T>::verbose);
}
} // namespace python
} // namespace qp
} // namespace proxsuite
