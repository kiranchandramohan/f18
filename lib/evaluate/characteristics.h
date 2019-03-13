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

// Defines data structures to represent "characteristics" of Fortran
// procedures and other entities as they are specified in section 15.3
// of Fortran 2018.

#ifndef FORTRAN_EVALUATE_CHARACTERISTICS_H_
#define FORTRAN_EVALUATE_CHARACTERISTICS_H_

#include "common.h"
#include "expression.h"
#include "type.h"
#include "../common/Fortran.h"
#include "../common/enum-set.h"
#include "../common/idioms.h"
#include "../common/indirection.h"
#include "../semantics/symbol.h"
#include <memory>
#include <optional>
#include <ostream>
#include <variant>
#include <vector>

namespace Fortran::evaluate::characteristics {

// Forward declare Procedure so dummy procedures and procedure pointers
// can use it indirectly.
struct Procedure;

// 15.3.2.2
struct DummyDataObject {
  ENUM_CLASS(Attr, AssumedRank, Optional, Allocatable, Asynchronous, Contiguous,
      Value, Volatile, Polymorphic, Pointer, Target)
  DEFAULT_CONSTRUCTORS_AND_ASSIGNMENTS(DummyDataObject)
  explicit DummyDataObject(DynamicType t) : type{t} {}
  DynamicType type;
  std::vector<std::optional<Expr<SubscriptInteger>>> shape;
  std::vector<Expr<SubscriptInteger>> coshape;
  common::Intent intent{common::Intent::Default};
  common::EnumSet<Attr, 32> attrs;
  bool operator==(const DummyDataObject &) const;
  std::ostream &Dump(std::ostream &) const;
};

// 15.3.2.3
struct DummyProcedure {
  ENUM_CLASS(Attr, Pointer, Optional)
  DEFAULT_CONSTRUCTORS_AND_ASSIGNMENTS(DummyProcedure)
  DummyProcedure() {}
  common::OwningPointer<Procedure> explicitProcedure;
  common::EnumSet<Attr, 32> attrs;
  bool operator==(const DummyProcedure &) const;
  std::ostream &Dump(std::ostream &) const;
};

// 15.3.2.4
struct AlternateReturn {
  bool operator==(const AlternateReturn &) const { return true; }
  std::ostream &Dump(std::ostream &) const;
};

// 15.3.2.1
using DummyArgument =
    std::variant<DummyDataObject, DummyProcedure, AlternateReturn>;

bool IsOptional(const DummyArgument &da) {
  return std::visit(
      common::visitors{
          [](const DummyDataObject &data) {
            return data.attrs.test(DummyDataObject::Attr::Optional);
          },
          [](const DummyProcedure &proc) {
            return proc.attrs.test(DummyProcedure::Attr::Optional);
          },
          [](const auto &) { return false; },
      },
      da);
}

// 15.3.3
struct FunctionResult {
  ENUM_CLASS(Attr, Polymorphic, Allocatable, Pointer, Contiguous)
  DEFAULT_CONSTRUCTORS_AND_ASSIGNMENTS(FunctionResult)
  explicit FunctionResult(DynamicType t) : type{t} {}
  DynamicType type;
  int rank{0};
  common::EnumSet<Attr, 32> attrs;
  common::OwningPointer<Procedure> procedurePointer;
  bool operator==(const FunctionResult &) const;
  std::ostream &Dump(std::ostream &) const;
};

// 15.3.1
struct Procedure {
  ENUM_CLASS(Attr, Pure, Elemental, BindC)
  Procedure() {}
  DEFAULT_CONSTRUCTORS_AND_ASSIGNMENTS(Procedure)

  bool IsFunction() const { return functionResult.has_value(); }
  bool IsSubroutine() const { return !IsFunction(); }
  bool IsPure() const { return attrs.test(Attr::Pure); }
  bool IsElemental() const { return attrs.test(Attr::Elemental); }
  bool IsBindC() const { return attrs.test(Attr::BindC); }

  std::optional<FunctionResult> functionResult;
  std::vector<DummyArgument> dummyArguments;
  common::EnumSet<Attr, 32> attrs;
  bool operator==(const Procedure &) const;
  std::ostream &Dump(std::ostream &) const;
};

template<typename A> std::optional<A> Characterize(const semantics::Symbol &);
template<>
std::optional<DummyDataObject> Characterize<DummyDataObject>(
    const semantics::Symbol &);
template<>
std::optional<DummyProcedure> Characterize<DummyProcedure>(
    const semantics::Symbol &);
template<>
std::optional<DummyArgument> Characterize<DummyArgument>(
    const semantics::Symbol &);
template<>
std::optional<Procedure> Characterize<Procedure>(const semantics::Symbol &);
}
#endif  // FORTRAN_EVALUATE_CHARACTERISTICS_H_
