#ifndef OMEGA_H_LOOP_HPP
#define OMEGA_H_LOOP_HPP

#include <Omega_h_defines.hpp>
#include <Omega_h_stack.hpp>

#ifdef OMEGA_H_USE_KOKKOSCORE
#include <Omega_h_kokkos.hpp>
#endif

#include <Omega_h_reduce.hpp>
#if defined(OMEGA_H_USE_CUDA)
#include <thrust/transform_scan.h>
#include <thrust/execution_policy.h>
#elif defined(OMEGA_H_USE_OPENMP)
#include <omp.h>
#endif

namespace Omega_h {

template <typename T>
void parallel_scan(LO n, T f, char const* name = "") {
#ifdef OMEGA_H_USE_KOKKOSCORE
  if (n > 0) Kokkos::parallel_scan(name, policy(n), f);
#else
  using VT = typename T::value_type;
  begin_code(name);
  VT update;
  f.init(update);
  for (LO i = 0; i < n; ++i) f(i, update, true);
  end_code();
#endif
}

#if defined(OMEGA_H_USE_CUDA)

template <typename InputIterator, typename OutputIterator, typename BinaryOp, typename UnaryOp>
OutputIterator transform_inclusive_scan(
    InputIterator first,
    InputIterator last,
    OutputIterator result,
    BinaryOp op,
    UnaryOp&& transform)
{
  return thrust::transform_inclusive_scan(
      thrust::device, first, last, result, native_op(transform), native_op(op));
}

#elif defined(OMEGA_H_USE_OPENMP)

template <typename InputIterator, typename OutputIterator, typename Transform, typename Op>
OutputIterator transform_inclusive_scan(
    InputIterator first,
    InputIterator last,
    OutputIterator result,
    Op op,
    Transform&& transform)
{
  auto const n = last - first;
  if (n <= 0) return result;
  Omega_h::entering_parallel = true;
  auto const transform_local = std::move(transform);
  Omega_h::entering_parallel = false;
  constexpr int max_num_threads = 512;
  using T_const_ref = decltype(transform_local(*first));
  using T_const = typename std::remove_reference<T_const_ref>::type;
  using T = typename std::remove_const<T_const>::type;
  T thread_sums[max_num_threads];
#pragma omp parallel
  {
    int const num_threads = omp_get_num_threads();
    int const thread_num = omp_get_thread_num();
    auto const quotient = n / num_threads;
    auto const remainder = n % num_threads;
    auto const begin_i = (thread_num > remainder) ?
      (quotient * thread_num + remainder) :
      ((quotient + 1) * thread_num);
    auto const end_i = (thread_num >= remainder) ?
      (begin_i + quotient) : (begin_i + quotient + 1);
    T thread_sum;
    if (begin_i < end_i) {
      thread_sum = transform_local(first[begin_i]);
      for (auto i = begin_i + 1; i < end_i; ++i) {
        thread_sum = op(std::move(thread_sum), transform_local(first[i]));
      }
      thread_sums[thread_num] = std::move(thread_sum);
    }
#pragma omp barrier
    if (begin_i < end_i) {
      if (thread_num) {
        thread_sum = thread_sums[0];
        for (int i = 1; i < thread_num; ++i) {
          thread_sum = op(std::move(thread_sum), thread_sums[i]);
        }
        thread_sum = op(std::move(thread_sum), transform_local(first[begin_i]));
      } else {
        thread_sum = transform_local(first[begin_i]);
      }
      result[begin_i] = thread_sum;
      for (auto i = begin_i + 1; i < end_i; ++i) {
        thread_sum = op(std::move(thread_sum), transform_local(first[i]));
        result[i] = thread_sum;
      }
    }
  }
  return result + n;
}

#else

template <typename InputIterator, typename OutputIterator, typename BinaryOp, typename UnaryOp>
OutputIterator transform_inclusive_scan(
    InputIterator first,
    InputIterator last,
    OutputIterator result,
    BinaryOp op,
    UnaryOp&& transform)
{
  auto const n = last - first;
  if (n <= 0) return result;
  Omega_h::entering_parallel = true;
  auto const transform_local = std::move(transform);
  Omega_h::entering_parallel = false;
  auto value = transform_local(first[0]);
  result[0] = value;
  using d_t = typename std::remove_const<decltype(n)>::type;
  for (d_t i = 1; i < n; ++i) {
    value = op(std::move(value), transform_local(first[i]));
    result[i] = value;
  }
  return result + n;
}

#endif

}

#endif
