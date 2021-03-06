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

#ifndef FORTRAN_EVALUATE_DESCENDER_H_
#define FORTRAN_EVALUATE_DESCENDER_H_

// Helper friend class templates for Visitor::Visit() and Rewriter::Traverse().

#include "expression.h"

namespace Fortran::evaluate {

template<typename VISITOR> class Descender {
public:
  explicit Descender(VISITOR &v) : visitor_{v} {}

  // Default cases
  template<typename X> void Descend(const X &) {}
  template<typename X> void Descend(X &x) {}

  template<typename X> void Descend(const X *p) {
    if (p != nullptr) {
      Visit(*p);
    }
  }
  template<typename X> void Descend(X *p) {
    if (p != nullptr) {
      Visit(*p);
    }
  }

  template<typename X> void Descend(const std::optional<X> &o) {
    if (o.has_value()) {
      Visit(*o);
    }
  }
  template<typename X> void Descend(std::optional<X> &o) {
    if (o.has_value()) {
      Visit(*o);
    }
  }

  template<typename X> void Descend(const CopyableIndirection<X> &p) {
    Visit(p.value());
  }
  template<typename X> void Descend(CopyableIndirection<X> &p) {
    Visit(p.value());
  }

  template<typename... X> void Descend(const std::variant<X...> &u) {
    std::visit([&](const auto &x) { Visit(x); }, u);
  }
  template<typename... X> void Descend(std::variant<X...> &u) {
    std::visit([&](auto &x) { Visit(x); }, u);
  }

  template<typename X> void Descend(const std::vector<X> &xs) {
    for (const auto &x : xs) {
      Visit(x);
    }
  }
  template<typename X> void Descend(std::vector<X> &xs) {
    for (auto &x : xs) {
      Visit(x);
    }
  }

  template<typename T> void Descend(const Expr<T> &expr) { Visit(expr.u); }
  template<typename T> void Descend(Expr<T> &expr) { Visit(expr.u); }

  template<typename D, typename R, typename... O>
  void Descend(const Operation<D, R, O...> &op) {
    Visit(op.left());
    if constexpr (op.operands > 1) {
      Visit(op.right());
    }
  }
  template<typename D, typename R, typename... O>
  void Descend(Operation<D, R, O...> &op) {
    Visit(op.left());
    if constexpr (op.operands > 1) {
      Visit(op.right());
    }
  }

  template<typename R> void Descend(const ImpliedDo<R> &ido) {
    Visit(ido.lower());
    Visit(ido.upper());
    Visit(ido.stride());
    Visit(ido.values());
  }
  template<typename R> void Descend(ImpliedDo<R> &ido) {
    Visit(ido.lower());
    Visit(ido.upper());
    Visit(ido.stride());
    Visit(ido.values());
  }

  template<typename R> void Descend(const ArrayConstructorValue<R> &av) {
    Visit(av.u);
  }
  template<typename R> void Descend(ArrayConstructorValue<R> &av) {
    Visit(av.u);
  }

  template<typename R> void Descend(const ArrayConstructorValues<R> &avs) {
    Visit(avs.values());
  }
  template<typename R> void Descend(ArrayConstructorValues<R> &avs) {
    Visit(avs.values());
  }

  template<int KIND>
  void Descend(
      const ArrayConstructor<Type<TypeCategory::Character, KIND>> &ac) {
    const ArrayConstructorValues<Type<TypeCategory::Character, KIND>> &base{ac};
    Visit(base);
    Visit(ac.LEN());
  }
  template<int KIND>
  void Descend(ArrayConstructor<Type<TypeCategory::Character, KIND>> &ac) {
    ArrayConstructorValues<Type<TypeCategory::Character, KIND>> &base{ac};
    Visit(base);
    Visit(ac.LEN());
  }

  void Descend(const semantics::ParamValue &param) {
    Visit(param.GetExplicit());
  }
  void Descend(semantics::ParamValue &param) { Visit(param.GetExplicit()); }

  void Descend(const semantics::DerivedTypeSpec &derived) {
    for (const auto &pair : derived.parameters()) {
      Visit(pair.second);
    }
  }
  void Descend(semantics::DerivedTypeSpec &derived) {
    for (const auto &pair : derived.parameters()) {
      Visit(pair.second);
    }
  }

  void Descend(const StructureConstructor &sc) {
    Visit(sc.derivedTypeSpec());
    for (const auto &pair : sc.values()) {
      Visit(pair.second);
    }
  }
  void Descend(StructureConstructor &sc) {
    Visit(sc.derivedTypeSpec());
    for (const auto &pair : sc.values()) {
      Visit(pair.second);
    }
  }

  void Descend(const BaseObject &object) { Visit(object.u); }
  void Descend(BaseObject &object) { Visit(object.u); }

  void Descend(const Component &component) {
    Visit(component.base());
    Visit(component.GetLastSymbol());
  }
  void Descend(Component &component) {
    Visit(component.base());
    Visit(component.GetLastSymbol());
  }

  template<int KIND> void Descend(const TypeParamInquiry<KIND> &inq) {
    Visit(inq.base());
    Visit(inq.parameter());
  }
  template<int KIND> void Descend(TypeParamInquiry<KIND> &inq) {
    Visit(inq.base());
    Visit(inq.parameter());
  }

  void Descend(const Triplet &triplet) {
    Visit(triplet.lower());
    Visit(triplet.upper());
    Visit(triplet.stride());
  }
  void Descend(Triplet &triplet) {
    Visit(triplet.lower());
    Visit(triplet.upper());
    Visit(triplet.stride());
  }

  void Descend(const Subscript &sscript) { Visit(sscript.u); }
  void Descend(Subscript &sscript) { Visit(sscript.u); }

  void Descend(const ArrayRef &aref) {
    Visit(aref.base());
    Visit(aref.subscript());
  }
  void Descend(ArrayRef &aref) {
    Visit(aref.base());
    Visit(aref.subscript());
  }

  void Descend(const CoarrayRef &caref) {
    Visit(caref.base());
    Visit(caref.subscript());
    Visit(caref.cosubscript());
    Visit(caref.stat());
    Visit(caref.team());
  }
  void Descend(CoarrayRef &caref) {
    Visit(caref.base());
    Visit(caref.subscript());
    Visit(caref.cosubscript());
    Visit(caref.stat());
    Visit(caref.team());
  }

  void Descend(const DataRef &data) { Visit(data.u); }
  void Descend(DataRef &data) { Visit(data.u); }

  void Descend(const ComplexPart &z) { Visit(z.complex()); }
  void Descend(ComplexPart &z) { Visit(z.complex()); }

  template<typename T> void Descend(const Designator<T> &designator) {
    Visit(designator.u);
  }
  template<typename T> void Descend(Designator<T> &designator) {
    Visit(designator.u);
  }

  template<typename T> void Descend(const Variable<T> &var) { Visit(var.u); }
  template<typename T> void Descend(Variable<T> &var) { Visit(var.u); }

  void Descend(const ActualArgument &arg) { Visit(arg.value()); }
  void Descend(ActualArgument &arg) { Visit(arg.value()); }

  void Descend(const ProcedureDesignator &p) { Visit(p.u); }
  void Descend(ProcedureDesignator &p) { Visit(p.u); }

  void Descend(const ProcedureRef &call) {
    Visit(call.proc());
    Visit(call.arguments());
  }
  void Descend(ProcedureRef &call) {
    Visit(call.proc());
    Visit(call.arguments());
  }

private:
  template<typename T> void Visit(const T &x) { return visitor_.Visit(x); }
  template<typename T> void Visit(T &x) { x = visitor_.Traverse(std::move(x)); }

  VISITOR &visitor_;
};
}
#endif  // FORTRAN_EVALUATE_DESCENDER_H_
