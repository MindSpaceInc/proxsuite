#ifndef DENSE_LDLT_LDLT_HPP_6MGYLBRCS
#define DENSE_LDLT_LDLT_HPP_6MGYLBRCS

#include "dense-ldlt/factorize.hpp"
#include "dense-ldlt/update.hpp"
#include "dense-ldlt/modify.hpp"
#include "dense-ldlt/solve.hpp"
#include <veg/vec.hpp>

namespace dense_ldlt {
namespace _detail {
struct SimdAlignedSystemAlloc {
	friend auto operator==(
			SimdAlignedSystemAlloc /*unused*/,
			SimdAlignedSystemAlloc /*unused*/) noexcept -> bool {
		return true;
	}
};
} // namespace _detail
} // namespace dense_ldlt

template <>
struct veg::mem::Alloc<dense_ldlt::_detail::SimdAlignedSystemAlloc> {
	static constexpr usize min_align = SIMDE_NATURAL_VECTOR_SIZE / 8;

	using RefMut = veg::RefMut<dense_ldlt::_detail::SimdAlignedSystemAlloc>;

	VEG_INLINE static auto adjusted_layout(Layout l) noexcept -> Layout {
		if (l.align < min_align) {
			l.align = min_align;
		}
		return l;
	}

	VEG_INLINE static void
	dealloc(RefMut /*alloc*/, void* ptr, Layout l) noexcept {
		return Alloc<SystemAlloc>::dealloc(
				mut(SystemAlloc{}), ptr, adjusted_layout(l));
	}

	VEG_NODISCARD VEG_INLINE static auto
	alloc(RefMut /*alloc*/, Layout l) noexcept -> mem::AllocBlock {
		return Alloc<SystemAlloc>::alloc(mut(SystemAlloc{}), adjusted_layout(l));
	}

	VEG_NODISCARD VEG_INLINE static auto grow(
			RefMut /*alloc*/,
			void* ptr,
			Layout l,
			usize new_size,
			RelocFn reloc) noexcept -> mem::AllocBlock {
		return Alloc<SystemAlloc>::grow(
				mut(SystemAlloc{}), ptr, adjusted_layout(l), new_size, reloc);
	}
	VEG_NODISCARD VEG_INLINE static auto shrink(
			RefMut /*alloc*/,
			void* ptr,
			Layout l,
			usize new_size,
			RelocFn reloc) noexcept -> mem::AllocBlock {
		return Alloc<SystemAlloc>::shrink(
				mut(SystemAlloc{}), ptr, adjusted_layout(l), new_size, reloc);
	}
};

namespace dense_ldlt {

template <typename T>
struct Ldlt {
private:
	static constexpr auto DYN = Eigen::Dynamic;
	using ColMat = Eigen::Matrix<T, DYN, DYN, Eigen::ColMajor>;
	using RowMat = Eigen::Matrix<T, DYN, DYN, Eigen::RowMajor>;
	using Vec = Eigen::Matrix<T, DYN, 1>;

	using LView = Eigen::TriangularView<
			Eigen::Map< //
					ColMat const,
					Eigen::Unaligned,
					Eigen::OuterStride<DYN>>,
			Eigen::UnitLower>;
	using LViewMut = Eigen::TriangularView<
			Eigen::Map< //
					ColMat,
					Eigen::Unaligned,
					Eigen::OuterStride<DYN>>,
			Eigen::UnitLower>;

	using LTView = Eigen::TriangularView<
			Eigen::Map< //
					RowMat const,
					Eigen::Unaligned,
					Eigen::OuterStride<DYN>>,
			Eigen::UnitUpper>;
	using LTViewMut = Eigen::TriangularView<
			Eigen::Map< //
					RowMat,
					Eigen::Unaligned,
					Eigen::OuterStride<DYN>>,
			Eigen::UnitUpper>;

	using DView =
			Eigen::Map<Vec const, Eigen::Unaligned, Eigen::InnerStride<DYN>>;
	using DViewMut = Eigen::Map<Vec, Eigen::Unaligned, Eigen::InnerStride<DYN>>;

	using VecMapISize = Eigen::Map<Eigen::Matrix<isize, DYN, 1> const>;
	using Perm = Eigen::PermutationWrapper<VecMapISize>;

	using StorageSimdVec = veg::Vec<
			T,
			veg::meta::if_t<
					_detail::should_vectorize<T>::value,
					_detail::SimdAlignedSystemAlloc,
					veg::mem::SystemAlloc>>;

	StorageSimdVec ld_storage;
	isize stride{};
	veg::Vec<isize> perm;
	veg::Vec<isize> perm_inv;

	// sorted on a best effort basis
	veg::Vec<T> maybe_sorted_diag;

	VEG_REFLECT(Ldlt, ld_storage, stride, perm, perm_inv, maybe_sorted_diag);

	static auto adjusted_stride(isize n) noexcept -> isize {
		return _detail::adjusted_stride<T>(n);
	}

	// soft invariants:
	// - perm.len() == perm_inv.len() == dim
	// - dim < stride
	// - ld_storage.len() >= dim * stride
public:
	Ldlt() = default;

	void reserve_uninit(isize cap) noexcept {
		static_assert(VEG_CONCEPT(nothrow_constructible<T>), ".");

		auto new_stride = adjusted_stride(cap);
		if (cap <= stride && cap * new_stride <= ld_storage.len()) {
			return;
		}

		ld_storage.reserve_exact(cap * new_stride);
		perm.reserve_exact(cap);
		perm_inv.reserve_exact(cap);
		maybe_sorted_diag.reserve_exact(cap);

		ld_storage.resize_for_overwrite(cap * new_stride);
		stride = new_stride;
	}

	void reserve(isize cap) noexcept {
		auto new_stride = adjusted_stride(cap);
		if (cap <= stride && cap * new_stride <= ld_storage.len()) {
			return;
		}
		auto n = dim();

		ld_storage.reserve_exact(cap * new_stride);
		perm.reserve_exact(cap);
		perm_inv.reserve_exact(cap);
		maybe_sorted_diag.reserve_exact(cap);

		ld_storage.resize_for_overwrite(cap * new_stride);

		for (isize i = 0; i < n; ++i) {
			auto col = n - i - 1;
			T* ptr = ld_col_mut().data();
			std::move_backward( //
					ptr + col * stride,
					ptr + col * stride + n,
					ptr + col * new_stride + n);
		}
		stride = new_stride;
	}

	static auto rank_r_update_req(isize n, isize r) noexcept
			-> veg::dynstack::StackReq {
		auto w_req = veg::dynstack::StackReq{
				_detail::adjusted_stride<T>(n) * r * isize{sizeof(T)},
				_detail::align<T>(),
		};
		auto alpha_req = veg::dynstack::StackReq{
				r * isize{sizeof(T)},
				alignof(T),
		};
		return w_req & alpha_req;
	}

	static auto delete_at_req(isize n, isize r) noexcept
			-> veg::dynstack::StackReq {
		return veg::dynstack::StackReq{
							 r * isize{sizeof(isize)},
							 alignof(isize),
					 } &
		       dense_ldlt::ldlt_delete_rows_and_cols_req(veg::Tag<T>{}, n, r);
	}

	void
	delete_at(isize const* indices, isize r, veg::dynstack::DynStackMut stack) {
		if (r == 0) {
			return;
		}

		VEG_ASSERT(std::is_sorted(indices, indices + r));

		isize n = dim();

		auto _indices_actual =
				stack.make_new_for_overwrite(veg::Tag<isize>{}, r).unwrap();
		auto indices_actual = _indices_actual.ptr_mut();

		for (isize k = 0; k < r; ++k) {
			indices_actual[k] = perm_inv[indices[k]];
		}

		dense_ldlt::ldlt_delete_rows_and_cols_sort_indices( //
				ld_col_mut(),
				indices_actual,
				r,
				stack);

		// PERF: do this in one pass
		for (isize k = 0; k < r; ++k) {
			auto i_actual = indices_actual[r - 1 - k];
			auto i = indices[r - 1 - k];

			perm.pop_mid(i_actual);
			perm_inv.pop_mid(i);
			maybe_sorted_diag.pop_mid(i_actual);

			for (isize j = 0; j < n - 1 - k; ++j) {
				auto& p_j = perm[j];
				auto& pinv_j = perm_inv[j];

				if (p_j > i) {
					--p_j;
				}
				if (pinv_j > i_actual) {
					--pinv_j;
				}
			}
		}
	}

	auto choose_insertion_position(isize i, Eigen::Ref<Vec const> a) -> isize {
		isize n = dim();
		auto diag_elem = a[i];

		isize pos = 0;
		for (; pos < n; ++pos) {
			if (diag_elem >= maybe_sorted_diag[pos]) {
				break;
			}
		}
		return pos;
	}

	static auto insert_block_at_req(isize n, isize r) noexcept
			-> veg::dynstack::StackReq {
		using veg::dynstack::StackReq;
		return StackReq{
							 isize{sizeof(T)} * (adjusted_stride(n + r) * r),
							 _detail::align<T>(),
					 } &
		       dense_ldlt::ldlt_insert_rows_and_cols_req(veg::Tag<T>{}, n, r);
	}

	void insert_block_at(
			isize i, Eigen::Ref<ColMat const> a, veg::dynstack::DynStackMut stack) {

		isize n = dim();
		isize r = a.cols();

		if (r == 0) {
			return;
		}

		reserve(n + r);

		isize i_actual = choose_insertion_position(i, a.col(0));

		for (isize j = 0; j < n; ++j) {
			auto& p_j = perm[j];
			auto& pinv_j = perm_inv[j];

			if (p_j >= i) {
				p_j += r;
			}
			if (pinv_j >= i_actual) {
				pinv_j += r;
			}
		}

		for (isize k = 0; k < r; ++k) {
			perm.push_mid(i + k, i_actual + k);
			perm_inv.push_mid(i_actual + k, i + k);
			maybe_sorted_diag.push_mid(a(i + k, k), i_actual + k);
		}

		LDLT_TEMP_MAT_UNINIT(T, permuted_a, n + r, r, stack);

		for (isize k = 0; k < r; ++k) {
			for (isize j = 0; j < n + r; ++j) {
				permuted_a(j, k) = a(perm[j], k);
			}
		}

		dense_ldlt::ldlt_insert_rows_and_cols(
				ld_col_mut(), i_actual, permuted_a, stack);
	}

	static auto diagonal_update_req(isize n, isize r) noexcept
			-> veg::dynstack::StackReq {
		using veg::dynstack::StackReq;
		auto algo_req = StackReq{
				2 * r * isize{sizeof(isize)},
				alignof(isize),
		};
		auto w_req = StackReq{
				_detail::adjusted_stride<T>(n) * r * isize{sizeof(T)},
				_detail::align<T>(),
		};
		auto alpha_req = StackReq{
				r * isize{sizeof(T)},
				alignof(T),
		};
		return algo_req & w_req & alpha_req;
	}

	void diagonal_update_clobber_indices( //
			isize* indices,
			isize r,
			Eigen::Ref<Vec const> alpha,
			veg::dynstack::DynStackMut stack) {

		if (r == 0) {
			return;
		}

		auto _positions =
				stack.make_new_for_overwrite(veg::Tag<isize>{}, r).unwrap();
		auto _sorted_indices =
				stack.make_new_for_overwrite(veg::Tag<isize>{}, r).unwrap();
		auto positions = _positions.ptr_mut();
		auto sorted_indices = _sorted_indices.ptr_mut();

		for (isize k = 0; k < r; ++k) {
			indices[k] = perm_inv[indices[k]];
			positions[k] = k;
		}

		std::sort(
				positions, positions + r, [indices](isize i, isize j) noexcept -> bool {
					return indices[i] < indices[j];
				});

		for (isize k = 0; k < r; ++k) {
			sorted_indices[k] = indices[positions[k]];
		}

		auto first = sorted_indices[0];
		auto n = dim() - first;

		LDLT_TEMP_MAT(T, _w, n, r, stack);
		LDLT_TEMP_VEC_UNINIT(T, _alpha, r, stack);

		for (isize k = 0; k < r; ++k) {
			_alpha(k) = alpha(positions[k]);
			_w(sorted_indices[k] - first, k) = 1;
		}

		dense_ldlt::_detail::rank_r_update_clobber_w_impl(
				util::submatrix(ld_col_mut(), first, first, n, n),
				_w.data(),
				_w.outerStride(),
				_alpha.data(),
				_detail::IndicesR{
						first,
						0,
						r,
						sorted_indices,
				});
	}

	void rank_r_update( //
			Eigen::Ref<ColMat const> w,
			Eigen::Ref<Vec const> alpha,
			veg::dynstack::DynStackMut stack) {

		auto n = dim();
		auto r = w.cols();
		if (r == 0) {
			return;
		}

		VEG_ASSERT(w.rows() == n);

		LDLT_TEMP_MAT_UNINIT(T, _w, n, r, stack);
		LDLT_TEMP_VEC_UNINIT(T, _alpha, r, stack);

		for (isize k = 0; k < r; ++k) {
			auto alpha_tmp = alpha(k);
			_alpha(k) = alpha_tmp;
			for (isize i = 0; i < n; ++i) {
				auto w_tmp = w(perm[i], k);
				_w(i, k) = w_tmp;
				maybe_sorted_diag[i] += alpha_tmp * (w_tmp * w_tmp);
			}
		}

		dense_ldlt::rank_r_update_clobber_inputs(ld_col_mut(), _w, _alpha);
	}

	auto dim() const noexcept -> isize { return perm.len(); }

	auto ld_col() const noexcept -> Eigen::Map< //
			ColMat const,
			Eigen::Unaligned,
			Eigen::OuterStride<DYN>> {
		return {ld_storage.ptr(), dim(), dim(), stride};
	}
	auto ld_col_mut() noexcept -> Eigen::Map< //
			ColMat,
			Eigen::Unaligned,
			Eigen::OuterStride<DYN>> {
		return {ld_storage.ptr_mut(), dim(), dim(), stride};
	}
	auto ld_row() const noexcept -> Eigen::Map< //
			RowMat const,
			Eigen::Unaligned,
			Eigen::OuterStride<DYN>> {
		return {
				ld_storage.ptr(),
				dim(),
				dim(),
				Eigen::OuterStride<DYN>{stride},
		};
	}
	auto ld_row_mut() noexcept -> Eigen::Map< //
			RowMat,
			Eigen::Unaligned,
			Eigen::OuterStride<DYN>> {
		return {
				ld_storage.ptr_mut(),
				dim(),
				dim(),
				Eigen::OuterStride<DYN>{stride},
		};
	}

	auto l() const noexcept -> LView {
		return ld_col().template triangularView<Eigen::UnitLower>();
	}
	auto l_mut() noexcept -> LViewMut {
		return ld_col_mut().template triangularView<Eigen::UnitLower>();
	}
	auto lt() const noexcept -> LTView {
		return ld_row().template triangularView<Eigen::UnitUpper>();
	}
	auto lt_mut() noexcept -> LTViewMut {
		return ld_row_mut().template triangularView<Eigen::UnitUpper>();
	}

	auto d() const noexcept -> DView {
		return {
				ld_storage.ptr(),
				dim(),
				1,
				Eigen::InnerStride<DYN>{stride + 1},
		};
	}
	auto d_mut() noexcept -> DView {
		return {
				ld_storage.ptr_mut(),
				dim(),
				1,
				Eigen::InnerStride<DYN>{stride + 1},
		};
	}
	auto p() -> Perm { return {VecMapISize(perm.ptr(), dim())}; }
	auto pt() -> Perm { return {VecMapISize(perm_inv.ptr(), dim())}; }

	static auto factorize_req(isize n) -> veg::dynstack::StackReq {
		return veg::dynstack::StackReq{
							 n * adjusted_stride(n) * isize{sizeof(T)},
							 _detail::align<T>(),
					 } |
		       dense_ldlt::factorize_req(veg::Tag<T>{}, n);
	}
	void factorize(
			Eigen::Ref<ColMat const> mat /* NOLINT */,
			veg::dynstack::DynStackMut stack) {
		VEG_ASSERT(mat.rows() == mat.cols());
		isize n = mat.rows();
		reserve_uninit(n);

		perm.resize_for_overwrite(n);
		perm_inv.resize_for_overwrite(n);
		maybe_sorted_diag.resize_for_overwrite(n);

		dense_ldlt::_detail::compute_permutation( //
				perm.ptr_mut(),
				perm_inv.ptr_mut(),
				util::diagonal(mat));

		{
			LDLT_TEMP_MAT_UNINIT(T, work, n, n, stack);
			ld_col_mut() = mat;
			dense_ldlt::_detail::apply_permutation_tri_lower(
					ld_col_mut(), work, perm.ptr());
		}

		for (isize i = 0; i < n; ++i) {
			maybe_sorted_diag[i] = ld_col()(i, i);
		}

		dense_ldlt::factorize(ld_col_mut(), stack);
	}

	static auto solve_in_place_req(isize n) -> veg::dynstack::StackReq {
		return {
				n * isize{sizeof(T)},
				_detail::align<T>(),
		};
	}
	void
	solve_in_place(Eigen::Ref<Vec> rhs, veg::dynstack::DynStackMut stack) const {
		isize n = rhs.rows();
		LDLT_TEMP_VEC_UNINIT(T, work, n, stack);

		for (isize i = 0; i < n; ++i) {
			work[i] = rhs[perm[i]];
		}

		dense_ldlt::solve(ld_col(), work);

		for (isize i = 0; i < n; ++i) {
			rhs[i] = work[perm_inv[i]];
		}
	}

	auto dbg_reconstructed_matrix_internal() const -> ColMat {
		veg::dbg(maybe_sorted_diag);
		isize n = dim();
		auto tmp = ColMat(n, n);
		tmp = l();
		tmp = tmp * d().asDiagonal();
		auto A = ColMat(tmp * lt());
		return A;
	}

	auto dbg_reconstructed_matrix() const -> ColMat {
		isize n = dim();
		auto tmp = ColMat(n, n);
		tmp = l();
		tmp = tmp * d().asDiagonal();
		auto A = ColMat(tmp * lt());

		for (isize i = 0; i < n; i++) {
			tmp.row(i) = A.row(perm_inv[i]);
		}
		for (isize i = 0; i < n; i++) {
			A.col(i) = tmp.col(perm_inv[i]);
		}
		return A;
	}
};
} // namespace dense_ldlt

#endif /* end of include guard DENSE_LDLT_LDLT_HPP_6MGYLBRCS */
