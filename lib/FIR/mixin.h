// Copyright (c) 2019, NVIDIA CORPORATION.  All rights reserved.
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

#ifndef FORTRAN_FIR_MIXIN_H_
#define FORTRAN_FIR_MIXIN_H_

// Mixin classes are "partial" classes (not used standalone) that can be used to
// add a repetitive (ad hoc) interface (and implementation) to a class.  It's
// better to think of these as "included in" a class, rather than as an
// "inherited from" base class.

#include "llvm/ADT/ilist.h"
#include <optional>
#include <tuple>
#include <type_traits>
#include <variant>

namespace Fortran::FIR {

// implementation of a (moveable) sum type (variant)
template<typename... Ts> struct SumTypeMixin {
  template<typename A> SumTypeMixin(A &&x) : u{std::move(x)} {}
  using SumTypeTrait = std::true_type;
  SumTypeMixin(SumTypeMixin &&) = default;
  SumTypeMixin &operator=(SumTypeMixin &&) = default;
  SumTypeMixin(const SumTypeMixin &) = delete;
  SumTypeMixin &operator=(const SumTypeMixin &) = delete;
  SumTypeMixin() = delete;
  std::variant<Ts...> u;
};

// implementation of a copyable sum type
template<typename... Ts> struct SumTypeCopyMixin {
  template<typename A> SumTypeCopyMixin(A &&x) : u{std::move(x)} {}
  template<typename A> SumTypeCopyMixin(const A &x) : u{x} {}
  using CopyableSumTypeTrait = std::true_type;
  SumTypeCopyMixin(SumTypeCopyMixin &&) = default;
  SumTypeCopyMixin &operator=(SumTypeCopyMixin &&) = default;
  SumTypeCopyMixin(const SumTypeCopyMixin &) = default;
  SumTypeCopyMixin &operator=(const SumTypeCopyMixin &) = default;
  SumTypeCopyMixin() = delete;
  std::variant<Ts...> u;
};
#define SUM_TYPE_COPY_MIXIN(DT) \
  DT(const DT &derived) : SumTypeCopyMixin(derived.u) {} \
  DT &operator=(const DT &derived) { \
    SumTypeCopyMixin::operator=(derived.u); \
    return *this; \
  }

// implementation of a (moveable) product type (tuple)
template<typename... Ts> struct ProductTypeMixin {
  template<typename A> ProductTypeMixin(A &&x) : t{std::move(x)} {}
  using ProductTypeTrait = std::true_type;
  ProductTypeMixin(ProductTypeMixin &&) = default;
  ProductTypeMixin &operator=(ProductTypeMixin &&) = default;
  ProductTypeMixin(const ProductTypeMixin &) = delete;
  ProductTypeMixin &operator=(const ProductTypeMixin &) = delete;
  ProductTypeMixin() = delete;
  std::tuple<Ts...> t;
};

// implementation of a (moveable) maybe type
template<typename T>
struct MaybeMixin {
  MaybeMixin(T &&x) : o{std::move(x)} {}
  using MaybeTrait = std::true_type;
  MaybeMixin(MaybeMixin &&) = default;
  MaybeMixin &operator=(MaybeMixin &&) = default;
  MaybeMixin(const MaybeMixin &) = delete;
  MaybeMixin &operator=(const MaybeMixin &) = delete;
  MaybeMixin() = delete;
  std::optional<T> o;
};

// implementation of a child type (composable hierarchy)
template<typename T, typename P> struct ChildMixin {
protected:
  P *parent;

public:
  ChildMixin(P *p) : parent{p} {}
  inline const P *getParent() const { return parent; }
  inline P *getParent() { return parent; }
  llvm::iplist<T> &getList() { return parent->getSublist(this); }
};

// zip :: ([a],[b]) -> [(a,b)]
template<typename A, typename B, typename C>
C Zip(C out, A first, A last, B other) {
  std::transform(first, last, other, out,
      [](auto &&a, auto &&b) -> std::pair<decltype(a), decltype(b)> {
        return {a, b};
      });
  return out;
}

// unzip :: [(a,b)] -> ([a],[b])
template<typename A, typename B> B &Unzip(B &out, A first, A last) {
  std::transform(first, last, std::back_inserter(out.first),
      [](auto &&a) -> decltype(a.first) { return a.first; });
  std::transform(first, last, std::back_inserter(out.second),
      [](auto &&a) -> decltype(a.second) { return a.second; });
  return out;
}

template<typename A, typename B> B &UnzipSnd(B &out, A first, A last) {
  std::transform(first, last, std::back_inserter(out.second),
      [](auto &&a) -> decltype(a.second) { return a.second; });
  return out;
}
}

#endif  // FORTRAN_FIR_COMMON_H_
