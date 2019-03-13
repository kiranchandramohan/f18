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

#include "characteristics.h"
#include "type.h"
#include "../common/indirection.h"
#include "../semantics/symbol.h"
#include <ostream>
#include <sstream>
#include <string>

using namespace std::literals::string_literals;

namespace Fortran::evaluate::characteristics {

bool DummyDataObject::operator==(const DummyDataObject &that) const {
  return attrs == that.attrs && intent == that.intent && type == that.type &&
      shape == that.shape && coshape == that.coshape;
}

std::ostream &DummyDataObject::Dump(std::ostream &o) const {
  attrs.Dump(o, EnumToString);
  if (intent != common::Intent::Default) {
    o << "INTENT(" << common::EnumToString(intent) << ')';
  }
  o << type.AsFortran();
  if (!shape.empty()) {
    char sep{'('};
    for (const auto &expr : shape) {
      o << sep;
      sep = ',';
      if (expr.has_value()) {
        expr->AsFortran(o);
      } else {
        o << ':';
      }
    }
    o << ')';
  }
  if (!coshape.empty()) {
    char sep{'['};
    for (const auto &expr : coshape) {
      expr.AsFortran(o << sep);
      sep = ',';
    }
  }
  return o;
}

bool DummyProcedure::operator==(const DummyProcedure &that) const {
  return attrs == that.attrs && explicitProcedure == that.explicitProcedure;
}

std::ostream &DummyProcedure::Dump(std::ostream &o) const {
  attrs.Dump(o, EnumToString);
  if (explicitProcedure.has_value()) {
    explicitProcedure.value().Dump(o);
  }
  return o;
}

std::ostream &AlternateReturn::Dump(std::ostream &o) const { return o << '*'; }

bool FunctionResult::operator==(const FunctionResult &that) const {
  if (attrs == that.attrs && type == that.type && rank == that.rank) {
    if (procedurePointer.has_value()) {
      return that.procedurePointer.has_value() &&
          procedurePointer.value() == that.procedurePointer.value();
    } else {
      return !that.procedurePointer.has_value();
    }
  }
  return false;
}

std::ostream &FunctionResult::Dump(std::ostream &o) const {
  attrs.Dump(o, EnumToString);
  o << type.AsFortran() << " rank " << rank;
  if (procedurePointer.has_value()) {
    procedurePointer.value().Dump(o << " procedure(") << ')';
  }
  return o;
}

bool Procedure::operator==(const Procedure &that) const {
  return attrs == that.attrs && dummyArguments == that.dummyArguments &&
      functionResult == that.functionResult;
}

std::ostream &Procedure::Dump(std::ostream &o) const {
  attrs.Dump(o, EnumToString);
  if (functionResult.has_value()) {
    functionResult->Dump(o << "TYPE(") << ") FUNCTION";
  } else {
    o << "SUBROUTINE";
  }
  char sep{'('};
  for (const auto &dummy : dummyArguments) {
    o << sep;
    sep = ',';
    std::visit([&](const auto &x) { x.Dump(o); }, dummy);
  }
  return o << (sep == '(' ? "()" : ")");
}

template<>
std::optional<DummyDataObject> Characterize<DummyDataObject>(
    const semantics::Symbol &symbol) {
  if (symbol.IsDummy()) {
    if (const auto *obj{symbol.detailsIf<semantics::ObjectEntityDetails>()}) {
      if (auto type{GetSymbolType(symbol)}) {
        DummyDataObject result{type.value()};
        if (obj->IsAssumedRank()) {
          result.attrs.set(DummyDataObject::Attr::AssumedRank);
        } else {
          // shape
        }
        if (symbol.attrs().test(semantics::Attr::OPTIONAL)) {
          result.attrs.set(DummyDataObject::Attr::Optional);
        }
        if (symbol.attrs().test(semantics::Attr::ALLOCATABLE)) {
          result.attrs.set(DummyDataObject::Attr::Allocatable);
        }
        if (symbol.attrs().test(semantics::Attr::ASYNCHRONOUS)) {
          result.attrs.set(DummyDataObject::Attr::Asynchronous);
        }
        if (symbol.attrs().test(semantics::Attr::CONTIGUOUS)) {
          result.attrs.set(DummyDataObject::Attr::Contiguous);
        }
        if (symbol.attrs().test(semantics::Attr::VALUE)) {
          result.attrs.set(DummyDataObject::Attr::Value);
        }
        if (symbol.attrs().test(semantics::Attr::VOLATILE)) {
          result.attrs.set(DummyDataObject::Attr::Volatile);
        }
        // Polymorphic?
        if (symbol.attrs().test(semantics::Attr::POINTER)) {
          result.attrs.set(DummyDataObject::Attr::Pointer);
        }
        if (symbol.attrs().test(semantics::Attr::TARGET)) {
          result.attrs.set(DummyDataObject::Attr::Target);
        }
        if (symbol.attrs().test(semantics::Attr::INTENT_IN)) {
          result.intent = common::Intent::In;
        }
        if (symbol.attrs().test(semantics::Attr::INTENT_OUT)) {
          CHECK(result.intent == common::Intent::Default);
          result.intent = common::Intent::Out;
        }
        if (symbol.attrs().test(semantics::Attr::INTENT_INOUT)) {
          CHECK(result.intent == common::Intent::Default);
          result.intent = common::Intent::InOut;
        }
        // coshape
      }
    }
  }
  return std::nullopt;
}

template<>
std::optional<DummyProcedure> Characterize<DummyProcedure>(
    const semantics::Symbol &symbol) {}

template<>
std::optional<DummyArgument> Characterize<DummyArgument>(
    const semantics::Symbol &symbol) {
  if (auto objCharacteristics{Characterize<DummyDataObject>(symbol)}) {
    return std::move(objCharacteristics.value());
  } else if (auto procCharacteristics{Characterize<DummyProcedure>(symbol)}) {
    return std::move(procCharacteristics.value());
  } else {
    return std::nullopt;
  }
}

template<>
std::optional<Procedure> Characterize<Procedure>(
    const semantics::Symbol &symbol) {
  if (const auto *subp{symbol.detailsIf<semantics::SubprogramDetails>()}) {
    Procedure result;
    if (symbol.attrs().test(semantics::Attr::PURE)) {
      result.attrs.set(Procedure::Attr::Pure);
    }
    if (symbol.attrs().test(semantics::Attr::ELEMENTAL)) {
      result.attrs.set(Procedure::Attr::Elemental);
    }
    if (symbol.attrs().test(semantics::Attr::BIND_C)) {
      result.attrs.set(Procedure::Attr::BindC);
    }
    for (const semantics::Symbol *arg : subp->dummyArgs()) {
      if (arg == nullptr) {
        result.dummyArguments.emplace_back(AlternateReturn{});
      } else if (auto argCharacteristics{Characterize<DummyArgument>(*arg)}) {
        result.dummyArguments.emplace_back(
            std::move(argCharacteristics.value()));
      } else {
        return std::nullopt;
      }
    }
    return std::move(result);
  } else if (const auto *proc{
                 symbol.detailsIf<semantics::ProcEntityDetails>()}) {
  }
  return std::nullopt;
}
}

// Define OwningPointer special member functions
DEFINE_OWNING_SPECIAL_FUNCTIONS(
    OwningPointer, evaluate::characteristics::Procedure)
