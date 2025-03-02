// Copyright 2022 Ant Group Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
#include "yacl/base/exception.h"
#define eigen_assert(X) YACL_ENFORCE((X))

#include <experimental/type_traits>

#include "Eigen/Core"
#include "msgpack.hpp"
#include "yacl/utils/parallel.h"

#include "heu/library/numpy/eigen_traits.h"
#include "heu/library/numpy/shape.h"
#include "heu/library/phe/phe.h"

namespace heu::lib::numpy {

// Check if T has a member function .Serialize()
template <typename T>
using kHasSerializeMethod = decltype(std::declval<T&>().Serialize());

// Vector is cheated as an n*1 matrix
template <typename T>
class DenseMatrix {
 public:
  typedef T value_type;

  DenseMatrix(Eigen::Index rows, Eigen::Index cols, int64_t ndim = 2)
      : m_(rows, cols), ndim_(ndim) {
    YACL_ENFORCE(ndim <= 2, "HEU tensor dimension cannot exceed 2");
    if (ndim == 1) {
      YACL_ENFORCE(cols == 1, "vector's cols must be 1");
    } else if (ndim == 0) {
      YACL_ENFORCE(rows == 1 && cols == 1, "scalar's shape must be 1x1");
    }
  }

  explicit DenseMatrix(const Shape& s)
      : DenseMatrix(s.RowsAlloc(), s.ColsAlloc(), s.Ndim()) {}

  explicit DenseMatrix(Eigen::Index rows) : m_(rows, 1), ndim_(1) {}

  T& operator()(Eigen::Index rows, Eigen::Index cols) { return m_(rows, cols); }
  const T& operator()(Eigen::Index rows, Eigen::Index cols) const {
    return m_(rows, cols);
  }

  T& operator()(Eigen::Index rows) {
    YACL_ENFORCE(ndim_ == 1, "tensor is {}-dim, but index is 1-dim", ndim_);
    return m_(rows, 0);
  }
  const T& operator()(Eigen::Index rows) const {
    YACL_ENFORCE(ndim_ == 1, "tensor is {}-dim, but index is 1-dim", ndim_);
    return m_(rows, 0);
  }

  // squeeze: something like np.squeeze(), see below for detail
  // https://numpy.org/doc/stable/reference/generated/numpy.squeeze.html
  template <typename RowIndices, typename ColIndices>
  DenseMatrix<T> GetItem(const RowIndices& row_indices,
                         const ColIndices& col_indices,
                         bool squeeze_row = false,
                         bool squeeze_col = false) const {
    const auto& view = m_(row_indices, col_indices);

    if (ndim_ == 1) {
      YACL_ENFORCE(!squeeze_col,
                   "axis not exist, you cannot squeeze shape[1] of a vector");
    } else if (ndim_ == 0) {
      YACL_ENFORCE(
          !squeeze_row && !squeeze_col,
          "axis not exist, tensor is 0-d, but you want to squeeze dim 1 and 2");
    }

    int64_t min_dim = view.rows() > 1 ? 1 : 0 + view.cols() > 1 ? 1 : 0;
    if ((!squeeze_row && !squeeze_col) or ndim_ == min_dim) {
      // no squeeze or nothing to squeeze
      return {view, ndim_};
    }

    int64_t new_dim = ndim_;
    if (squeeze_col && view.cols() <= 1) {
      // vertical vector or scalar
      new_dim -= 1;
      if (squeeze_row && view.rows() <= 1) {
        new_dim -= 1;  // scalar
      }

      YACL_ENFORCE(new_dim >= min_dim,
                   "internal error: a bug occurred during squeeze");
      return {view, new_dim};
    } else if (squeeze_row && view.rows() <= 1) {
      // horizontal vector or scalar
      new_dim -= 1;

      YACL_ENFORCE(new_dim >= min_dim,
                   "internal error: a bug occurred during squeeze");
      return {view.transpose(), new_dim};
    }

    YACL_THROW_LOGIC_ERROR("GetItem should not reach here");
  }

  template <typename RowIndices, typename ColIndices>
  auto SetItem(const RowIndices& rowIndices, const ColIndices& colIndices,
               const DenseMatrix<T>& v, bool transpose = false) {
    m_(rowIndices, colIndices) = (transpose ? v.m_.transpose() : v.m_);
  }

  template <typename RowIndices, typename ColIndices>
  auto SetItem(const RowIndices& rowIndices, const ColIndices& colIndices,
               const T& v) {
    m_(rowIndices, colIndices) = Eigen::Matrix<T, 1, 1>{v};
  }

  [[nodiscard]] int64_t ndim() const { return ndim_; }
  [[nodiscard]] Eigen::Index rows() const { return m_.rows(); }
  [[nodiscard]] Eigen::Index cols() const { return m_.cols(); }
  [[nodiscard]] Eigen::Index size() const { return m_.size(); }
  [[nodiscard]] Shape shape() const {
    std::vector<int64_t> res = {m_.rows(), m_.cols()};
    res.resize(ndim_);
    return Shape(res);
  }

  T* data() { return m_.data(); }
  const T* data() const { return m_.data(); }

  // if parallel = true, please make sure update() is thread safe
  void ForEach(
      const std::function<void(int64_t row, int64_t col, T* element)>& update,
      bool parallel = true) {
    // Why not use static_assert: m_.IsRowMajor is not a constexpr value
    // Why not use assert: C++ assert do not support custom message
    YACL_ENFORCE(!m_.IsRowMajor,
                 "Internal bug: tensor must be stored in ColMajor way.");

    T* buf = m_.data();
    int64_t row = rows();
    auto func = [&](int64_t beg, int64_t end) {
      for (int64_t i = beg; i < end; ++i) {
        update(i % row, i / row, buf + i);
      }
    };

    if (parallel) {
      yacl::parallel_for(0, size(), 1, func);
    } else {
      func(0, size());
    }
  }

  // if parallel = true, be make sure visit() is thread safe
  void ForEach(const std::function<void(int64_t row, int64_t col,
                                        const T& element)>& visit,
               bool parallel = true) const {
    YACL_ENFORCE(!m_.IsRowMajor,
                 "Internal bug: tensor must be stored in ColMajor way.");

    const T* buf = m_.data();
    int64_t row = rows();
    auto func = [&](int64_t beg, int64_t end) {
      for (int64_t i = beg; i < end; ++i) {
        visit(i % row, i / row, *(buf + i));
      }
    };

    if (parallel) {
      yacl::parallel_for(0, size(), 1, func);
    } else {
      func(0, size());
    }
  }

  // there is compiling error in TransposeInplace(), so we only provide
  // Transpose()
  DenseMatrix<T> Transpose() {
    YACL_ENFORCE(ndim_ == 2, "you cannot transpose a {}d-tensor", ndim_);
    return {m_.transpose(), ndim_};
  }

  [[nodiscard]] std::string ToString() const {
    std::stringstream ss;
    ss << m_.format(Eigen::IOFormat(Eigen::StreamPrecision, 0, " ", "\n", "[",
                                    "]", "[", "]"));
    return ss.str();
  }

  friend std::ostream& operator<<(std::ostream& os, const DenseMatrix& m) {
    return os << m.ToString();
  }

  const auto& EigenMatrix() const { return m_; }

  [[nodiscard]] yacl::Buffer Serialize() const {
    msgpack::sbuffer buffer;
    msgpack::packer<msgpack::sbuffer> o(buffer);

    // Since element T is not POD, so we cannot pack underlying buffer
    // directly
    o.pack_array(4);
    Eigen::Index rows = this->rows();
    Eigen::Index cols = this->cols();
    int64_t ndim = this->ndim();

    o.pack(rows);
    o.pack(cols);
    o.pack(ndim);
    o.pack_array(this->size());

    for (Eigen::Index j = 0; j < cols; j++) {
      for (Eigen::Index i = 0; i < rows; i++) {
        if constexpr (std::experimental::is_detected_v<kHasSerializeMethod,
                                                       T>) {
          o.pack(std::string_view(m_(i, j).Serialize()));
        } else {
          o.pack(m_(i, j));
        }
      }
    }

    auto sz = buffer.size();
    return {buffer.release(), sz, [](void* ptr) { free(ptr); }};
  }

  static DenseMatrix<T> LoadFrom(yacl::ByteContainerView in) {
    auto msg =
        msgpack::unpack(reinterpret_cast<const char*>(in.data()), in.size());
    msgpack::object o = msg.get();

    if (o.type != msgpack::type::ARRAY) {
      throw msgpack::type_error();
    }
    if (o.via.array.size != 4) {
      throw msgpack::type_error();
    }

    Eigen::Index rows = o.via.array.ptr[0].as<Eigen::Index>();
    Eigen::Index cols = o.via.array.ptr[1].as<Eigen::Index>();
    auto dim = o.via.array.ptr[2].as<int64_t>();
    heu::lib::numpy::DenseMatrix<T> res(rows, cols, dim);

    auto inner_obj = o.via.array.ptr[3];
    if (inner_obj.type != msgpack::type::ARRAY ||
        inner_obj.via.array.size != res.size()) {
      throw msgpack::type_error();
    }

    msgpack::object* p = inner_obj.via.array.ptr;
    for (Eigen::Index j = 0; j < cols; j++) {
      for (Eigen::Index i = 0; i < rows; i++) {
        if constexpr (std::experimental::is_detected_v<kHasSerializeMethod,
                                                       T>) {
          res(i, j).Deserialize(p++->template as<std::string_view>());
        } else {
          res(i, j) = p++->template as<T>();
        }
      }
    }

    return res;
  }

 private:
  DenseMatrix(Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic> m, int64_t ndim)
      : m_(std::move(m)), ndim_(ndim) {
    YACL_ENFORCE(ndim <= 2, "HEU tensor dimension cannot exceed 2");
    if (ndim == 1) {
      YACL_ENFORCE(m_.cols() == 1, "vector's cols must be 1");
    } else if (ndim == 0) {
      YACL_ENFORCE(m_.rows() == 1 && m_.cols() == 1,
                   "scalar's shape must be 1x1");
    }
  }

  Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic> m_;
  int64_t ndim_;
};

using PMatrix = DenseMatrix<phe::Plaintext>;
using CMatrix = DenseMatrix<phe::Ciphertext>;

}  // namespace heu::lib::numpy
