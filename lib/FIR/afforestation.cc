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

#include "afforestation.h"
#include "builder.h"
#include "mixin.h"
#include "../evaluate/fold.h"
#include "../evaluate/tools.h"
#include "../parser/parse-tree-visitor.h"
#include "../semantics/expression.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

namespace Fortran::FIR {
namespace {
Expression *ExprRef(const parser::Expr &a) { return &a.typedExpr.value(); }
Expression *ExprRef(const common::Indirection<parser::Expr> &a) {
  return &a.value().typedExpr.value();
}

struct LinearOp;

using LinearLabelRef = unsigned;
constexpr LinearLabelRef unspecifiedLabel{~0u};

llvm::raw_ostream *debugChannel;
llvm::raw_ostream &DebugChannel() {
  return debugChannel ? *debugChannel : llvm::errs();
}
void SetDebugChannel(llvm::raw_ostream *output) { debugChannel = output; }

struct LinearLabelBuilder {
  LinearLabelBuilder() : referenced(32), counter{0u} {}
  LinearLabelRef getNext() {
    LinearLabelRef next{counter++};
    auto cap{referenced.capacity()};
    if (cap < counter) {
      referenced.reserve(2 * cap);
    }
    referenced[next] = false;
    return next;
  }
  void setReferenced(LinearLabelRef label) { referenced[label] = true; }
  bool isReferenced(LinearLabelRef label) const { return referenced[label]; }
  std::vector<bool> referenced;
  unsigned counter;
};

struct LinearLabel {
  explicit LinearLabel(LinearLabelBuilder &builder)
    : builder_{builder}, label_{builder.getNext()} {}
  LinearLabel(const LinearLabel &that)
    : builder_{that.builder_}, label_{that.label_} {}
  LinearLabel &operator=(const LinearLabel &that) {
    CHECK(&builder_ == &that.builder_);
    label_ = that.label_;
    return *this;
  }
  void setReferenced() const { builder_.setReferenced(label_); }
  bool isReferenced() const { return builder_.isReferenced(label_); }
  LinearLabelRef get() const { return label_; }
  operator LinearLabelRef() const { return get(); }

private:
  LinearLabelBuilder &builder_;
  LinearLabelRef label_;
};

struct LinearGoto {
  struct LinearArtificial {};
  LinearGoto(LinearLabelRef dest) : u{LinearArtificial{}}, target{dest} {}
  template<typename T>
  LinearGoto(const T &stmt, LinearLabelRef dest) : u{&stmt}, target{dest} {}
  std::variant<const parser::CycleStmt *, const parser::ExitStmt *,
      const parser::GotoStmt *, LinearArtificial>
      u;
  LinearLabelRef target;
};

struct LinearReturn
  : public SumTypeCopyMixin<const parser::FailImageStmt *,
        const parser::ReturnStmt *, const parser::StopStmt *> {
  SUM_TYPE_COPY_MIXIN(LinearReturn)
  template<typename T> LinearReturn(const T &stmt) : SumTypeCopyMixin{&stmt} {}
};

struct LinearConditionalGoto {
  template<typename T>
  LinearConditionalGoto(const T &cond, LinearLabelRef tb, LinearLabelRef fb)
    : u{&cond}, trueLabel{tb}, falseLabel{fb} {}
  std::variant<const parser::Statement<parser::IfThenStmt> *,
      const parser::Statement<parser::ElseIfStmt> *, const parser::IfStmt *,
      const parser::Statement<parser::NonLabelDoStmt> *>
      u;
  LinearLabelRef trueLabel;
  LinearLabelRef falseLabel;
};

struct LinearIndirectGoto {
  LinearIndirectGoto(
      const semantics::Symbol *symbol, std::vector<LinearLabelRef> &&labelRefs)
    : symbol{symbol}, labelRefs{labelRefs} {}
  const semantics::Symbol *symbol;
  std::vector<LinearLabelRef> labelRefs;
};

struct LinearSwitchingIO {
  template<typename T>
  LinearSwitchingIO(const T &io, LinearLabelRef next,
      std::optional<LinearLabelRef> errLab,
      std::optional<LinearLabelRef> eorLab = std::nullopt,
      std::optional<LinearLabelRef> endLab = std::nullopt)
    : u{&io}, next{next}, errLabel{errLab}, eorLabel{eorLab}, endLabel{endLab} {
  }
  std::variant<const parser::ReadStmt *, const parser::WriteStmt *,
      const parser::WaitStmt *, const parser::OpenStmt *,
      const parser::CloseStmt *, const parser::BackspaceStmt *,
      const parser::EndfileStmt *, const parser::RewindStmt *,
      const parser::FlushStmt *, const parser::InquireStmt *>
      u;
  LinearLabelRef next;
  std::optional<LinearLabelRef> errLabel;
  std::optional<LinearLabelRef> eorLabel;
  std::optional<LinearLabelRef> endLabel;
};

struct LinearSwitch {
  template<typename T>
  LinearSwitch(const T &sw, const std::vector<LinearLabelRef> &refs)
    : u{&sw}, refs{refs} {}
  std::variant<const parser::CallStmt *, const parser::ComputedGotoStmt *,
      const parser::ArithmeticIfStmt *, const parser::CaseConstruct *,
      const parser::SelectRankConstruct *, const parser::SelectTypeConstruct *>
      u;
  const std::vector<LinearLabelRef> refs;
};

struct LinearAction {
  LinearAction(const parser::Statement<parser::ActionStmt> &stmt) : v{&stmt} {}
  parser::CharBlock getSource() const { return v->source; }

  const parser::Statement<parser::ActionStmt> *v;
};

#define WRAP(T) const parser::T *
#define CONSTRUCT_TYPES \
  WRAP(AssociateConstruct), WRAP(BlockConstruct), WRAP(CaseConstruct), \
      WRAP(ChangeTeamConstruct), WRAP(CriticalConstruct), WRAP(DoConstruct), \
      WRAP(IfConstruct), WRAP(SelectRankConstruct), WRAP(SelectTypeConstruct), \
      WRAP(WhereConstruct), WRAP(ForallConstruct), WRAP(CompilerDirective), \
      WRAP(OpenMPConstruct), WRAP(OpenMPEndLoopDirective)

struct LinearBeginConstruct : public SumTypeCopyMixin<CONSTRUCT_TYPES> {
  SUM_TYPE_COPY_MIXIN(LinearBeginConstruct)
  template<typename T>
  LinearBeginConstruct(const T &c) : SumTypeCopyMixin{&c} {}
};
struct LinearEndConstruct : public SumTypeCopyMixin<CONSTRUCT_TYPES> {
  SUM_TYPE_COPY_MIXIN(LinearEndConstruct)
  template<typename T> LinearEndConstruct(const T &c) : SumTypeCopyMixin{&c} {}
};

struct LinearDoIncrement {
  LinearDoIncrement(const parser::DoConstruct &stmt) : v{&stmt} {}
  const parser::DoConstruct *v;
};
struct LinearDoCompare {
  LinearDoCompare(const parser::DoConstruct &stmt) : v{&stmt} {}
  const parser::DoConstruct *v;
};

template<typename CONSTRUCT>
const char *GetConstructName(const CONSTRUCT &construct) {
  return std::visit(
      common::visitors{
          [](const parser::AssociateConstruct *) { return "ASSOCIATE"; },
          [](const parser::BlockConstruct *) { return "BLOCK"; },
          [](const parser::CaseConstruct *) { return "SELECT CASE"; },
          [](const parser::ChangeTeamConstruct *) { return "CHANGE TEAM"; },
          [](const parser::CriticalConstruct *) { return "CRITICAL"; },
          [](const parser::DoConstruct *) { return "DO"; },
          [](const parser::IfConstruct *) { return "IF"; },
          [](const parser::SelectRankConstruct *) { return "SELECT RANK"; },
          [](const parser::SelectTypeConstruct *) { return "SELECT TYPE"; },
          [](const parser::WhereConstruct *) { return "WHERE"; },
          [](const parser::ForallConstruct *) { return "FORALL"; },
          [](const parser::CompilerDirective *) { return "<directive>"; },
          [](const parser::OpenMPConstruct *) { return "<open-mp>"; },
          [](const parser::OpenMPEndLoopDirective *) {
            return "<open-mp-end-loop>";
          }},
      construct.u);
}

struct AnalysisData {
  std::map<parser::Label, LinearLabel> labelMap;
  std::vector<std::tuple<const parser::Name *, LinearLabelRef, LinearLabelRef>>
      nameStack;
  LinearLabelBuilder labelBuilder;
  std::map<const semantics::Symbol *, std::set<parser::Label>> assignMap;
};

void AddAssign(AnalysisData &ad, const semantics::Symbol *symbol,
    const parser::Label &label) {
  ad.assignMap[symbol].insert(label);
}
std::vector<LinearLabelRef> GetAssign(
    AnalysisData &ad, const semantics::Symbol *symbol) {
  std::vector<LinearLabelRef> result;
  for (auto lab : ad.assignMap[symbol]) {
    result.emplace_back(lab);
  }
  return result;
}
LinearLabel BuildNewLabel(AnalysisData &ad) {
  return LinearLabel{ad.labelBuilder};
}
LinearLabel FetchLabel(AnalysisData &ad, const parser::Label &label) {
  auto iter{ad.labelMap.find(label)};
  if (iter == ad.labelMap.end()) {
    LinearLabel ll{ad.labelBuilder};
    ll.setReferenced();
    ad.labelMap.insert({label, ll});
    return ll;
  }
  return iter->second;
}
std::tuple<const parser::Name *, LinearLabelRef, LinearLabelRef> FindStack(
    const std::vector<std::tuple<const parser::Name *, LinearLabelRef,
        LinearLabelRef>> &stack,
    const parser::Name *key) {
  for (auto iter{stack.rbegin()}, iend{stack.rend()}; iter != iend; ++iter) {
    if (std::get<0>(*iter) == key) return *iter;
  }
  SEMANTICS_FAILED("construct name not on stack");
  return {};
}

template<typename T> parser::Label GetErr(const T &stmt) {
  if constexpr (std::is_same_v<T, parser::ReadStmt> ||
      std::is_same_v<T, parser::WriteStmt>) {
    for (const auto &control : stmt.controls) {
      if (std::holds_alternative<parser::ErrLabel>(control.u)) {
        return std::get<parser::ErrLabel>(control.u).v;
      }
    }
  }
  if constexpr (std::is_same_v<T, parser::WaitStmt> ||
      std::is_same_v<T, parser::OpenStmt> ||
      std::is_same_v<T, parser::CloseStmt> ||
      std::is_same_v<T, parser::BackspaceStmt> ||
      std::is_same_v<T, parser::EndfileStmt> ||
      std::is_same_v<T, parser::RewindStmt> ||
      std::is_same_v<T, parser::FlushStmt>) {
    for (const auto &spec : stmt.v) {
      if (std::holds_alternative<parser::ErrLabel>(spec.u)) {
        return std::get<parser::ErrLabel>(spec.u).v;
      }
    }
  }
  if constexpr (std::is_same_v<T, parser::InquireStmt>) {
    for (const auto &spec : std::get<std::list<parser::InquireSpec>>(stmt.u)) {
      if (std::holds_alternative<parser::ErrLabel>(spec.u)) {
        return std::get<parser::ErrLabel>(spec.u).v;
      }
    }
  }
  return 0;
}

template<typename T> parser::Label GetEor(const T &stmt) {
  if constexpr (std::is_same_v<T, parser::ReadStmt> ||
      std::is_same_v<T, parser::WriteStmt>) {
    for (const auto &control : stmt.controls) {
      if (std::holds_alternative<parser::EorLabel>(control.u)) {
        return std::get<parser::EorLabel>(control.u).v;
      }
    }
  }
  if constexpr (std::is_same_v<T, parser::WaitStmt>) {
    for (const auto &waitSpec : stmt.v) {
      if (std::holds_alternative<parser::EorLabel>(waitSpec.u)) {
        return std::get<parser::EorLabel>(waitSpec.u).v;
      }
    }
  }
  return 0;
}

template<typename T> parser::Label GetEnd(const T &stmt) {
  if constexpr (std::is_same_v<T, parser::ReadStmt> ||
      std::is_same_v<T, parser::WriteStmt>) {
    for (const auto &control : stmt.controls) {
      if (std::holds_alternative<parser::EndLabel>(control.u)) {
        return std::get<parser::EndLabel>(control.u).v;
      }
    }
  }
  if constexpr (std::is_same_v<T, parser::WaitStmt>) {
    for (const auto &waitSpec : stmt.v) {
      if (std::holds_alternative<parser::EndLabel>(waitSpec.u)) {
        return std::get<parser::EndLabel>(waitSpec.u).v;
      }
    }
  }
  return 0;
}

template<typename T>
void errLabelSpec(const T &s, std::list<LinearOp> &ops,
    const parser::Statement<parser::ActionStmt> &ec, AnalysisData &ad) {
  if (auto errLab{GetErr(s)}) {
    std::optional<LinearLabelRef> errRef{FetchLabel(ad, errLab).get()};
    LinearLabel next{BuildNewLabel(ad)};
    ops.emplace_back(LinearSwitchingIO{s, next, errRef});
    ops.emplace_back(next);
  } else {
    ops.emplace_back(LinearAction{ec});
  }
}

template<typename T>
void threeLabelSpec(const T &s, std::list<LinearOp> &ops,
    const parser::Statement<parser::ActionStmt> &ec, AnalysisData &ad) {
  auto errLab{GetErr(s)};
  auto eorLab{GetEor(s)};
  auto endLab{GetEnd(s)};
  if (errLab || eorLab || endLab) {
    std::optional<LinearLabelRef> errRef{std::nullopt};
    if (errLab) errRef = FetchLabel(ad, errLab).get();
    std::optional<LinearLabelRef> eorRef{std::nullopt};
    if (eorLab) eorRef = FetchLabel(ad, eorLab).get();
    std::optional<LinearLabelRef> endRef{std::nullopt};
    if (endLab) endRef = FetchLabel(ad, endLab).get();
    LinearLabel next{BuildNewLabel(ad)};
    ops.emplace_back(LinearSwitchingIO{s, next, errRef, eorRef, endRef});
    ops.emplace_back(next);
  } else {
    ops.emplace_back(LinearAction{ec});
  }
}

template<typename T>
std::vector<LinearLabelRef> toLabelRef(AnalysisData &ad, const T &labels) {
  std::vector<LinearLabelRef> result;
  for (auto label : labels) {
    result.emplace_back(FetchLabel(ad, label).get());
  }
  CHECK(result.size() == labels.size());
  return result;
}

bool hasAltReturns(const parser::CallStmt &callStmt) {
  const auto &args{std::get<std::list<parser::ActualArgSpec>>(callStmt.v.t)};
  for (const auto &arg : args) {
    const auto &actual{std::get<parser::ActualArg>(arg.t)};
    if (std::holds_alternative<parser::AltReturnSpec>(actual.u)) {
      return true;
    }
  }
  return false;
}
std::list<parser::Label> getAltReturnLabels(const parser::Call &call) {
  std::list<parser::Label> result;
  const auto &args{std::get<std::list<parser::ActualArgSpec>>(call.t)};
  for (const auto &arg : args) {
    const auto &actual{std::get<parser::ActualArg>(arg.t)};
    if (const auto *p{std::get_if<parser::AltReturnSpec>(&actual.u)}) {
      result.push_back(p->v);
    }
  }
  return result;
}
LinearLabelRef NearestEnclosingDoConstruct(AnalysisData &ad) {
  for (auto iterator{ad.nameStack.rbegin()}, endIterator{ad.nameStack.rend()};
       iterator != endIterator; ++iterator) {
    auto labelReference{std::get<2>(*iterator)};
    if (labelReference != unspecifiedLabel) {
      return labelReference;
    }
  }
  SEMANTICS_FAILED("CYCLE|EXIT not in loop");
  return unspecifiedLabel;
}

struct LinearOp : public SumTypeMixin<LinearLabel, LinearGoto, LinearReturn,
                      LinearConditionalGoto, LinearSwitchingIO, LinearSwitch,
                      LinearAction, LinearBeginConstruct, LinearEndConstruct,
                      LinearIndirectGoto, LinearDoIncrement, LinearDoCompare> {
  template<typename T> LinearOp(const T &thing) : SumTypeMixin{thing} {}
  void dump() const;

  static void Build(std::list<LinearOp> &ops,
      const parser::Statement<parser::ActionStmt> &ec, AnalysisData &ad) {
    std::visit(
        common::visitors{
            [&](const auto &s) { ops.emplace_back(LinearAction{ec}); },
            [&](const common::Indirection<parser::CallStmt> &s) {
              if (hasAltReturns(s.value())) {
                auto next{BuildNewLabel(ad)};
                auto labels{toLabelRef(ad, getAltReturnLabels(s.value().v))};
                labels.push_back(next);
                ops.emplace_back(LinearSwitch{s.value(), std::move(labels)});
                ops.emplace_back(next);
              } else {
                ops.emplace_back(LinearAction{ec});
              }
            },
            [&](const common::Indirection<parser::AssignStmt> &s) {
              AddAssign(ad, std::get<parser::Name>(s.value().t).symbol,
                  std::get<parser::Label>(s.value().t));
              ops.emplace_back(LinearAction{ec});
            },
            [&](const common::Indirection<parser::CycleStmt> &s) {
              ops.emplace_back(LinearGoto{s.value(),
                  s.value().v ? std::get<2>(FindStack(
                                    ad.nameStack, &s.value().v.value()))
                              : NearestEnclosingDoConstruct(ad)});
            },
            [&](const common::Indirection<parser::ExitStmt> &s) {
              ops.emplace_back(LinearGoto{s.value(),
                  s.value().v ? std::get<1>(FindStack(
                                    ad.nameStack, &s.value().v.value()))
                              : NearestEnclosingDoConstruct(ad)});
            },
            [&](const common::Indirection<parser::GotoStmt> &s) {
              ops.emplace_back(
                  LinearGoto{s.value(), FetchLabel(ad, s.value().v).get()});
            },
            [&](const parser::FailImageStmt &s) {
              ops.emplace_back(LinearReturn{s});
            },
            [&](const common::Indirection<parser::ReturnStmt> &s) {
              ops.emplace_back(LinearReturn{s.value()});
            },
            [&](const common::Indirection<parser::StopStmt> &s) {
              ops.emplace_back(LinearAction{ec});
              ops.emplace_back(LinearReturn{s.value()});
            },
            [&](const common::Indirection<const parser::ReadStmt> &s) {
              threeLabelSpec(s.value(), ops, ec, ad);
            },
            [&](const common::Indirection<const parser::WriteStmt> &s) {
              threeLabelSpec(s.value(), ops, ec, ad);
            },
            [&](const common::Indirection<const parser::WaitStmt> &s) {
              threeLabelSpec(s.value(), ops, ec, ad);
            },
            [&](const common::Indirection<const parser::OpenStmt> &s) {
              errLabelSpec(s.value(), ops, ec, ad);
            },
            [&](const common::Indirection<const parser::CloseStmt> &s) {
              errLabelSpec(s.value(), ops, ec, ad);
            },
            [&](const common::Indirection<const parser::BackspaceStmt> &s) {
              errLabelSpec(s.value(), ops, ec, ad);
            },
            [&](const common::Indirection<const parser::EndfileStmt> &s) {
              errLabelSpec(s.value(), ops, ec, ad);
            },
            [&](const common::Indirection<const parser::RewindStmt> &s) {
              errLabelSpec(s.value(), ops, ec, ad);
            },
            [&](const common::Indirection<const parser::FlushStmt> &s) {
              errLabelSpec(s.value(), ops, ec, ad);
            },
            [&](const common::Indirection<const parser::InquireStmt> &s) {
              errLabelSpec(s.value(), ops, ec, ad);
            },
            [&](const common::Indirection<parser::ComputedGotoStmt> &s) {
              auto next{BuildNewLabel(ad)};
              auto labels{toLabelRef(
                  ad, std::get<std::list<parser::Label>>(s.value().t))};
              labels.push_back(next);
              ops.emplace_back(LinearSwitch{s.value(), std::move(labels)});
              ops.emplace_back(next);
            },
            [&](const common::Indirection<parser::ArithmeticIfStmt> &s) {
              ops.emplace_back(LinearSwitch{s.value(),
                  toLabelRef(ad,
                      std::list{std::get<1>(s.value().t),
                          std::get<2>(s.value().t),
                          std::get<3>(s.value().t)})});
            },
            [&](const common::Indirection<parser::AssignedGotoStmt> &s) {
              ops.emplace_back(
                  LinearIndirectGoto{std::get<parser::Name>(s.value().t).symbol,
                      toLabelRef(ad,
                          std::get<std::list<parser::Label>>(s.value().t))});
            },
            [&](const common::Indirection<parser::IfStmt> &s) {
              auto then{BuildNewLabel(ad)};
              auto endif{BuildNewLabel(ad)};
              ops.emplace_back(LinearConditionalGoto{s.value(), then, endif});
              ops.emplace_back(then);
              ops.emplace_back(LinearAction{ec});
              ops.emplace_back(endif);
            },
        },
        ec.statement.u);
  }
};

template<typename STMTTYPE, typename CT>
const std::optional<parser::Name> &GetSwitchAssociateName(
    const CT *selectConstruct) {
  return std::get<1>(
      std::get<parser::Statement<STMTTYPE>>(selectConstruct->t).statement.t);
}

template<typename CONSTRUCT>
void DumpSwitchWithSelector(
    const CONSTRUCT *construct, char const *const name) {
  /// auto selector{getSelector(construct)};
  DebugChannel() << name << "(";  // << selector.dump()
}

void LinearOp::dump() const {
  std::visit(
      common::visitors{
          [](const LinearLabel &t) {
            DebugChannel() << "label: " << t.get() << '\n';
          },
          [](const LinearGoto &t) {
            DebugChannel() << "goto " << t.target << '\n';
          },
          [](const LinearReturn &) { DebugChannel() << "return\n"; },
          [](const LinearConditionalGoto &t) {
            DebugChannel() << "cbranch (?) " << t.trueLabel << ' '
                           << t.falseLabel << '\n';
          },
          [](const LinearSwitchingIO &t) {
            DebugChannel() << "io-op";
            if (t.errLabel) DebugChannel() << " ERR=" << t.errLabel.value();
            if (t.eorLabel) DebugChannel() << " EOR=" << t.eorLabel.value();
            if (t.endLabel) DebugChannel() << " END=" << t.endLabel.value();
            DebugChannel() << '\n';
          },
          [](const LinearSwitch &lswitch) {
            DebugChannel() << "switch-";
            std::visit(
                common::visitors{
                    [](const parser::CaseConstruct *caseConstruct) {
                      DumpSwitchWithSelector(caseConstruct, "case");
                    },
                    [](const parser::SelectRankConstruct *selectRankConstruct) {
                      DumpSwitchWithSelector(selectRankConstruct, "rank");
                    },
                    [](const parser::SelectTypeConstruct *selectTypeConstruct) {
                      DumpSwitchWithSelector(selectTypeConstruct, "type");
                    },
                    [](const parser::ComputedGotoStmt *computedGotoStmt) {
                      DebugChannel() << "igoto(?";
                    },
                    [](const parser::ArithmeticIfStmt *arithmeticIfStmt) {
                      DebugChannel() << "<=>(?";
                    },
                    [](const parser::CallStmt *callStmt) {
                      DebugChannel() << "alt-return(?";
                    },
                },
                lswitch.u);
            DebugChannel() << ") [...]\n";
          },
          [](const LinearAction &t) {
            DebugChannel() << "action: " << t.getSource().ToString() << '\n';
          },
          [](const LinearBeginConstruct &construct) {
            DebugChannel() << "construct-" << GetConstructName(construct)
                           << " {\n";
          },
          [](const LinearDoIncrement &) { DebugChannel() << "do increment\n"; },
          [](const LinearDoCompare &) { DebugChannel() << "do compare\n"; },
          [](const LinearEndConstruct &construct) {
            DebugChannel() << "} construct-" << GetConstructName(construct)
                           << "\n";
          },
          [](const LinearIndirectGoto &) { DebugChannel() << "igoto\n"; },
      },
      u);
}
}  // end namespace

struct ControlFlowAnalyzer {
  explicit ControlFlowAnalyzer(std::list<LinearOp> &ops, AnalysisData &ad)
    : linearOps{ops}, ad{ad} {}

  LinearLabel buildNewLabel() { return BuildNewLabel(ad); }
  LinearOp findLabel(const parser::Label &lab) {
    auto iter{ad.labelMap.find(lab)};
    if (iter == ad.labelMap.end()) {
      LinearLabel ll{ad.labelBuilder};
      ad.labelMap.insert({lab, ll});
      return {ll};
    }
    return {iter->second};
  }
  template<typename A> constexpr bool Pre(const A &) { return true; }
  template<typename A> constexpr void Post(const A &) {}
  template<typename A> bool Pre(const parser::Statement<A> &stmt) {
    if (stmt.label) {
      linearOps.emplace_back(findLabel(*stmt.label));
    }
    if constexpr (std::is_same_v<A, parser::ActionStmt>) {
      LinearOp::Build(linearOps, stmt, ad);
    }
    return true;
  }

  // named constructs
  template<typename T> bool linearConstruct(const T &construct) {
    std::list<LinearOp> ops;
    LinearLabel label{buildNewLabel()};
    const parser::Name *name{getName(construct)};
    ad.nameStack.emplace_back(name, GetLabelRef(label), unspecifiedLabel);
    ops.emplace_back(LinearBeginConstruct{construct});
    ControlFlowAnalyzer cfa{ops, ad};
    Walk(std::get<parser::Block>(construct.t), cfa);
    ops.emplace_back(label);
    ops.emplace_back(LinearEndConstruct{construct});
    linearOps.splice(linearOps.end(), ops);
    ad.nameStack.pop_back();
    return false;
  }
  bool Pre(const parser::AssociateConstruct &c) { return linearConstruct(c); }
  bool Pre(const parser::ChangeTeamConstruct &c) { return linearConstruct(c); }
  bool Pre(const parser::CriticalConstruct &c) { return linearConstruct(c); }
  bool Pre(const parser::BlockConstruct &construct) {
    std::list<LinearOp> ops;
    LinearLabel label{buildNewLabel()};
    const auto &optName{
        std::get<parser::Statement<parser::BlockStmt>>(construct.t)
            .statement.v};
    const parser::Name *name{optName ? &*optName : nullptr};
    ad.nameStack.emplace_back(name, GetLabelRef(label), unspecifiedLabel);
    ops.emplace_back(LinearBeginConstruct{construct});
    ControlFlowAnalyzer cfa{ops, ad};
    Walk(std::get<parser::Block>(construct.t), cfa);
    ops.emplace_back(LinearEndConstruct{construct});
    ops.emplace_back(label);
    linearOps.splice(linearOps.end(), ops);
    ad.nameStack.pop_back();
    return false;
  }

  bool Pre(const parser::DoConstruct &construct) {
    std::list<LinearOp> ops;
    LinearLabel backedgeLab{buildNewLabel()};
    LinearLabel incrementLab{buildNewLabel()};
    LinearLabel entryLab{buildNewLabel()};
    LinearLabel exitLab{buildNewLabel()};
    const parser::Name *name{getName(construct)};
    LinearLabelRef exitOpRef{GetLabelRef(exitLab)};
    ad.nameStack.emplace_back(name, exitOpRef, GetLabelRef(incrementLab));
    ops.emplace_back(LinearBeginConstruct{construct});
    ops.emplace_back(LinearGoto{GetLabelRef(backedgeLab)});
    ops.emplace_back(incrementLab);
    ops.emplace_back(LinearDoIncrement{construct});
    ops.emplace_back(backedgeLab);
    ops.emplace_back(LinearDoCompare{construct});
    ops.emplace_back(LinearConditionalGoto{
        std::get<parser::Statement<parser::NonLabelDoStmt>>(construct.t),
        GetLabelRef(entryLab), exitOpRef});
    ops.push_back(entryLab);
    ControlFlowAnalyzer cfa{ops, ad};
    Walk(std::get<parser::Block>(construct.t), cfa);
    ops.emplace_back(LinearGoto{GetLabelRef(incrementLab)});
    ops.emplace_back(LinearEndConstruct{construct});
    ops.emplace_back(exitLab);
    linearOps.splice(linearOps.end(), ops);
    ad.nameStack.pop_back();
    return false;
  }

  bool Pre(const parser::IfConstruct &construct) {
    std::list<LinearOp> ops;
    LinearLabel thenLab{buildNewLabel()};
    LinearLabel elseLab{buildNewLabel()};
    LinearLabel exitLab{buildNewLabel()};
    const parser::Name *name{getName(construct)};
    ad.nameStack.emplace_back(name, GetLabelRef(exitLab), unspecifiedLabel);
    ops.emplace_back(LinearBeginConstruct{construct});
    ops.emplace_back(LinearConditionalGoto{
        std::get<parser::Statement<parser::IfThenStmt>>(construct.t),
        GetLabelRef(thenLab), GetLabelRef(elseLab)});
    ops.emplace_back(thenLab);
    ControlFlowAnalyzer cfa{ops, ad};
    Walk(std::get<parser::Block>(construct.t), cfa);
    LinearLabelRef exitOpRef{GetLabelRef(exitLab)};
    ops.emplace_back(LinearGoto{exitOpRef});
    for (const auto &elseIfBlock :
        std::get<std::list<parser::IfConstruct::ElseIfBlock>>(construct.t)) {
      ops.emplace_back(elseLab);
      LinearLabel newThenLab{buildNewLabel()};
      LinearLabel newElseLab{buildNewLabel()};
      ops.emplace_back(LinearConditionalGoto{
          std::get<parser::Statement<parser::ElseIfStmt>>(elseIfBlock.t),
          GetLabelRef(newThenLab), GetLabelRef(newElseLab)});
      ops.emplace_back(newThenLab);
      Walk(std::get<parser::Block>(elseIfBlock.t), cfa);
      ops.emplace_back(LinearGoto{exitOpRef});
      elseLab = newElseLab;
    }
    ops.emplace_back(elseLab);
    if (const auto &optElseBlock{
            std::get<std::optional<parser::IfConstruct::ElseBlock>>(
                construct.t)}) {
      Walk(std::get<parser::Block>(optElseBlock->t), cfa);
    }
    ops.emplace_back(LinearGoto{exitOpRef});
    ops.emplace_back(exitLab);
    ops.emplace_back(LinearEndConstruct{construct});
    linearOps.splice(linearOps.end(), ops);
    ad.nameStack.pop_back();
    return false;
  }
  template<typename A,
      typename B = std::conditional_t<std::is_same_v<A, parser::CaseConstruct>,
          parser::CaseConstruct::Case,
          std::conditional_t<std::is_same_v<A, parser::SelectRankConstruct>,
              parser::SelectRankConstruct::RankCase,
              std::conditional_t<std::is_same_v<A, parser::SelectTypeConstruct>,
                  parser::SelectTypeConstruct::TypeCase, void>>>>
  bool Multiway(const A &construct) {
    std::list<LinearOp> ops;
    LinearLabel exitLab{buildNewLabel()};
    const parser::Name *name{getName(construct)};
    ad.nameStack.emplace_back(name, GetLabelRef(exitLab), unspecifiedLabel);
    ops.emplace_back(LinearBeginConstruct{construct});
    const auto N{std::get<std::list<B>>(construct.t).size()};
    LinearLabelRef exitOpRef{GetLabelRef(exitLab)};
    if (N > 0) {
      typename std::list<B>::size_type i;
      std::vector<LinearLabel> toLabels;
      for (i = 0; i != N; ++i) {
        toLabels.emplace_back(buildNewLabel());
      }
      std::vector<LinearLabelRef> targets;
      for (i = 0; i != N; ++i) {
        targets.emplace_back(GetLabelRef(toLabels[i]));
      }
      ops.emplace_back(LinearSwitch{construct, targets});
      ControlFlowAnalyzer cfa{ops, ad};
      i = 0;
      for (const auto &caseBlock : std::get<std::list<B>>(construct.t)) {
        ops.emplace_back(toLabels[i++]);
        Walk(std::get<parser::Block>(caseBlock.t), cfa);
        ops.emplace_back(LinearGoto{exitOpRef});
      }
    }
    ops.emplace_back(exitLab);
    ops.emplace_back(LinearEndConstruct{construct});
    linearOps.splice(linearOps.end(), ops);
    ad.nameStack.pop_back();
    return false;
  }
  bool Pre(const parser::CaseConstruct &c) { return Multiway(c); }
  bool Pre(const parser::SelectRankConstruct &c) { return Multiway(c); }
  bool Pre(const parser::SelectTypeConstruct &c) { return Multiway(c); }
  bool Pre(const parser::WhereConstruct &c) {
    std::list<LinearOp> ops;
    LinearLabel label{buildNewLabel()};
    const parser::Name *name{getName(c)};
    ad.nameStack.emplace_back(name, GetLabelRef(label), unspecifiedLabel);
    ops.emplace_back(LinearBeginConstruct{c});
    ControlFlowAnalyzer cfa{ops, ad};
    Walk(std::get<std::list<parser::WhereBodyConstruct>>(c.t), cfa);
    Walk(
        std::get<std::list<parser::WhereConstruct::MaskedElsewhere>>(c.t), cfa);
    Walk(std::get<std::optional<parser::WhereConstruct::Elsewhere>>(c.t), cfa);
    ops.emplace_back(label);
    ops.emplace_back(LinearEndConstruct{c});
    linearOps.splice(linearOps.end(), ops);
    ad.nameStack.pop_back();
    return false;
  }
  bool Pre(const parser::ForallConstruct &construct) {
    std::list<LinearOp> ops;
    LinearLabel label{buildNewLabel()};
    const parser::Name *name{getName(construct)};
    ad.nameStack.emplace_back(name, GetLabelRef(label), unspecifiedLabel);
    ops.emplace_back(LinearBeginConstruct{construct});
    ControlFlowAnalyzer cfa{ops, ad};
    Walk(std::get<std::list<parser::ForallBodyConstruct>>(construct.t), cfa);
    ops.emplace_back(label);
    ops.emplace_back(LinearEndConstruct{construct});
    linearOps.splice(linearOps.end(), ops);
    ad.nameStack.pop_back();
    return false;
  }
  template<typename A> const parser::Name *getName(const A &a) {
    const auto &optName{std::get<0>(std::get<0>(a.t).statement.t)};
    return optName ? &*optName : nullptr;
  }
  LinearLabelRef GetLabelRef(const LinearLabel &label) {
    label.setReferenced();
    return label;
  }
  LinearLabelRef GetLabelRef(const parser::Label &label) {
    return FetchLabel(ad, label);
  }

  std::list<LinearOp> &linearOps;
  AnalysisData &ad;
};

template<typename T> struct SwitchArgs {
  Value exp;
  LinearLabelRef defLab;
  std::vector<T> values;
  std::vector<LinearLabelRef> labels;
};
using SwitchArguments = SwitchArgs<SwitchStmt::ValueType>;
using SwitchCaseArguments = SwitchArgs<SwitchCaseStmt::ValueType>;
using SwitchRankArguments = SwitchArgs<SwitchRankStmt::ValueType>;
using SwitchTypeArguments = SwitchArgs<SwitchTypeStmt::ValueType>;

template<typename T> bool IsDefault(const typename T::ValueType &valueType) {
  return std::holds_alternative<typename T::Default>(valueType);
}

template<typename T>
void cleanupSwitchPairs(LinearLabelRef &defLab,
    std::vector<typename T::ValueType> &values,
    std::vector<LinearLabelRef> &labels) {
  CHECK(values.size() == labels.size());
  for (std::size_t i{0}, len{values.size()}; i < len; ++i) {
    if (IsDefault<T>(values[i])) {
      defLab = labels[i];
      for (std::size_t j{i}; j < len - 1; ++j) {
        values[j] = values[j + 1];
        labels[j] = labels[j + 1];
      }
      values.pop_back();
      labels.pop_back();
      break;
    }
  }
}

static std::vector<SwitchCaseStmt::ValueType> populateSwitchValues(
    FIRBuilder *builder, const std::list<parser::CaseConstruct::Case> &list) {
  std::vector<SwitchCaseStmt::ValueType> result;
  for (auto &v : list) {
    auto &caseSelector{std::get<parser::CaseSelector>(
        std::get<parser::Statement<parser::CaseStmt>>(v.t).statement.t)};
    if (std::holds_alternative<parser::Default>(caseSelector.u)) {
      result.emplace_back(SwitchCaseStmt::Default{});
    } else {
      std::vector<SwitchCaseStmt::RangeAlternative> valueList;
      for (auto &r :
          std::get<std::list<parser::CaseValueRange>>(caseSelector.u)) {
        std::visit(
            common::visitors{
                [&](const parser::CaseValue &caseValue) {
                  const auto &e{caseValue.thing.thing.value()};
                  auto *app{builder->MakeAsExpr(ExprRef(e))};
                  valueList.emplace_back(SwitchCaseStmt::Exactly{app});
                },
                [&](const parser::CaseValueRange::Range &range) {
                  if (range.lower.has_value()) {
                    if (range.upper.has_value()) {
                      auto *appl{builder->MakeAsExpr(
                          ExprRef(range.lower->thing.thing))};
                      auto *apph{builder->MakeAsExpr(
                          ExprRef(range.upper->thing.thing))};
                      valueList.emplace_back(
                          SwitchCaseStmt::InclusiveRange{appl, apph});
                    } else {
                      auto *app{builder->MakeAsExpr(
                          ExprRef(range.lower->thing.thing))};
                      valueList.emplace_back(
                          SwitchCaseStmt::InclusiveAbove{app});
                    }
                  } else {
                    auto *app{
                        builder->MakeAsExpr(ExprRef(range.upper->thing.thing))};
                    valueList.emplace_back(SwitchCaseStmt::InclusiveBelow{app});
                  }
                },
            },
            r.u);
      }
      result.emplace_back(valueList);
    }
  }
  return result;
}

static std::vector<SwitchRankStmt::ValueType> populateSwitchValues(
    const std::list<parser::SelectRankConstruct::RankCase> &list) {
  std::vector<SwitchRankStmt::ValueType> result;
  for (auto &v : list) {
    auto &rank{std::get<parser::SelectRankCaseStmt::Rank>(
        std::get<parser::Statement<parser::SelectRankCaseStmt>>(v.t)
            .statement.t)};
    std::visit(
        common::visitors{
            [&](const parser::ScalarIntConstantExpr &exp) {
              const auto &e{exp.thing.thing.thing.value()};
              result.emplace_back(SwitchRankStmt::Exactly{ExprRef(e)});
            },
            [&](const parser::Star &) {
              result.emplace_back(SwitchRankStmt::AssumedSize{});
            },
            [&](const parser::Default &) {
              result.emplace_back(SwitchRankStmt::Default{});
            },
        },
        rank.u);
  }
  return result;
}

static std::vector<SwitchTypeStmt::ValueType> populateSwitchValues(
    const std::list<parser::SelectTypeConstruct::TypeCase> &list) {
  std::vector<SwitchTypeStmt::ValueType> result;
  for (auto &v : list) {
    auto &guard{std::get<parser::TypeGuardStmt::Guard>(
        std::get<parser::Statement<parser::TypeGuardStmt>>(v.t).statement.t)};
    std::visit(
        common::visitors{
            [&](const parser::TypeSpec &typeSpec) {
              result.emplace_back(
                  SwitchTypeStmt::TypeSpec{typeSpec.declTypeSpec});
            },
            [&](const parser::DerivedTypeSpec &derivedTypeSpec) {
              result.emplace_back(
                  SwitchTypeStmt::DerivedTypeSpec{nullptr /*FIXME*/});
            },
            [&](const parser::Default &) {
              result.emplace_back(SwitchTypeStmt::Default{});
            },
        },
        guard.u);
  }
  return result;
}

static void buildMultiwayDefaultNext(SwitchArguments &result) {
  result.defLab = result.labels.back();
  result.labels.pop_back();
}

template<typename T>
const T *FindReadWriteSpecifier(
    const std::list<parser::IoControlSpec> &specifiers) {
  for (const auto &specifier : specifiers) {
    if (auto *result{std::get_if<T>(&specifier.u)}) {
      return result;
    }
  }
  return nullptr;
}

const parser::IoUnit *FindReadWriteIoUnit(
    const std::optional<parser::IoUnit> &ioUnit,
    const std::list<parser::IoControlSpec> &specifiers) {
  if (ioUnit.has_value()) {
    return &ioUnit.value();
  }
  if (const auto *result{FindReadWriteSpecifier<parser::IoUnit>(specifiers)}) {
    return result;
  }
  SEMANTICS_FAILED("no UNIT spec");
  return {};
}

const parser::Format *FindReadWriteFormat(
    const std::optional<parser::Format> &format,
    const std::list<parser::IoControlSpec> &specifiers) {
  if (format.has_value()) {
    return &format.value();
  }
  return FindReadWriteSpecifier<parser::Format>(specifiers);
}

static Expression AlwaysTrueExpression() {
  using T = evaluate::Type<evaluate::TypeCategory::Logical, 1>;
  return {evaluate::AsGenericExpr(evaluate::Constant<T>{true})};
}

// create an integer constant as an expression
static Expression CreateConstant(int64_t value) {
  using T = evaluate::SubscriptInteger;
  return {evaluate::AsGenericExpr(evaluate::Constant<T>{value})};
}

static void CreateSwitchHelper(FIRBuilder *builder, Value condition,
    BasicBlock *defaultCase, const SwitchStmt::ValueSuccPairListType &rest) {
  builder->CreateSwitch(condition, defaultCase, rest);
}
static void CreateSwitchCaseHelper(FIRBuilder *builder, Value condition,
    BasicBlock *defaultCase,
    const SwitchCaseStmt::ValueSuccPairListType &rest) {
  builder->CreateSwitchCase(condition, defaultCase, rest);
}
static void CreateSwitchRankHelper(FIRBuilder *builder, Value condition,
    BasicBlock *defaultCase,
    const SwitchRankStmt::ValueSuccPairListType &rest) {
  builder->CreateSwitchRank(condition, defaultCase, rest);
}
static void CreateSwitchTypeHelper(FIRBuilder *builder, Value condition,
    BasicBlock *defaultCase,
    const SwitchTypeStmt::ValueSuccPairListType &rest) {
  builder->CreateSwitchType(condition, defaultCase, rest);
}

class FortranIRLowering {
public:
  using LabelMapType = std::map<LinearLabelRef, BasicBlock *>;
  using Closure = std::function<void(const LabelMapType &)>;

  FortranIRLowering(semantics::SemanticsContext &sc, bool debugLinearIR)
    : fir_{new Program("program_name")}, semanticsContext_{sc},
      debugLinearFIR_{debugLinearIR} {}
  ~FortranIRLowering() { CHECK(!builder_); }

  template<typename A> constexpr bool Pre(const A &) { return true; }
  template<typename A> constexpr void Post(const A &) {}

  void Post(const parser::MainProgram &mainp) {
    std::string mainName{"_MAIN"s};
    if (auto &ps{
            std::get<std::optional<parser::Statement<parser::ProgramStmt>>>(
                mainp.t)}) {
      mainName = ps->statement.v.ToString();
    }
    ProcessRoutine(mainp, mainName);
  }
  void Post(const parser::FunctionSubprogram &subp) {
    ProcessRoutine(subp,
        std::get<parser::Name>(
            std::get<parser::Statement<parser::FunctionStmt>>(subp.t)
                .statement.t)
            .ToString());
  }
  void Post(const parser::SubroutineSubprogram &subp) {
    ProcessRoutine(subp,
        std::get<parser::Name>(
            std::get<parser::Statement<parser::SubroutineStmt>>(subp.t)
                .statement.t)
            .ToString());
  }

  Program *program() { return fir_; }

  template<typename T>
  void ProcessRoutine(const T &here, const std::string &name) {
    CHECK(!fir_->containsProcedure(name));
    auto *subp{fir_->getOrInsertProcedure(name, nullptr, {})};
    builder_ = new FIRBuilder(*CreateBlock(subp->getLastRegion()));
    AnalysisData ad;
    ControlFlowAnalyzer linearize{linearOperations_, ad};
    Walk(here, linearize);
    if (debugLinearFIR_) {
      dumpLinearRepresentation();
    }
    ConstructFIR(ad);
    DrawRemainingArcs();
    Cleanup();
  }
  void dumpLinearRepresentation() const {
    for (const auto &op : linearOperations_) {
      op.dump();
    }
    DebugChannel() << "--- END ---\n";
  }

  template<typename A>
  Statement *BindArrayWithBoundSpecifier(
      const parser::DataRef &dataRef, const std::list<A> &bl) {
    // TODO
    return nullptr;
  }

  Statement *CreatePointerValue(const parser::PointerAssignmentStmt &stmt) {
    auto &dataRef{std::get<parser::DataRef>(stmt.t)};
    auto &bounds{std::get<parser::PointerAssignmentStmt::Bounds>(stmt.t)};
    auto *remap{std::visit(
        common::visitors{
            [&](const std::list<parser::BoundsRemapping> &bl) -> Statement * {
              if (bl.empty()) {
                return nullptr;
              }
              return BindArrayWithBoundSpecifier(dataRef, bl);
            },
            [&](const std::list<parser::BoundsSpec> &bl) -> Statement * {
              if (bl.empty()) {
                return nullptr;
              }
              return BindArrayWithBoundSpecifier(dataRef, bl);
            },
        },
        bounds.u)};
    if (remap) {
      return remap;
    }
    return builder_->CreateAddr(DataRefToExpression(dataRef));
  }
  Type CreateAllocationValue(const parser::Allocation *allocation,
      const parser::AllocateStmt *statement) {
    auto &obj{std::get<parser::AllocateObject>(allocation->t)};
    (void)obj;
    // TODO: build an expression for the allocation
    return nullptr;
  }
  AllocateInsn *CreateDeallocationValue(
      const parser::AllocateObject *allocateObject,
      const parser::DeallocateStmt *statement) {
    // TODO: build an expression for the deallocation
    return nullptr;
  }

  // IO argument translations ...
  IOCallArguments CreateBackspaceArguments(
      const std::list<parser::PositionOrFlushSpec> &specifiers) {
    return IOCallArguments{};
  }
  IOCallArguments CreateCloseArguments(
      const std::list<parser::CloseStmt::CloseSpec> &specifiers) {
    return IOCallArguments{};
  }
  IOCallArguments CreateEndfileArguments(
      const std::list<parser::PositionOrFlushSpec> &specifiers) {
    return IOCallArguments{};
  }
  IOCallArguments CreateFlushArguments(
      const std::list<parser::PositionOrFlushSpec> &specifiers) {
    return IOCallArguments{};
  }
  IOCallArguments CreateRewindArguments(
      const std::list<parser::PositionOrFlushSpec> &specifiers) {
    return IOCallArguments{};
  }
  IOCallArguments CreateInquireArguments(
      const std::list<parser::InquireSpec> &specifiers) {
    return IOCallArguments{};
  }
  IOCallArguments CreateInquireArguments(
      const parser::InquireStmt::Iolength &iolength) {
    return IOCallArguments{};
  }
  IOCallArguments CreateOpenArguments(
      const std::list<parser::ConnectSpec> &specifiers) {
    return IOCallArguments{};
  }
  IOCallArguments CreateWaitArguments(
      const std::list<parser::WaitSpec> &specifiers) {
    return IOCallArguments{};
  }
  IOCallArguments CreatePrintArguments(const parser::Format &format,
      const std::list<parser::OutputItem> &outputs) {
    return IOCallArguments{};
  }
  IOCallArguments CreateReadArguments(
      const std::optional<parser::IoUnit> &ioUnit,
      const std::optional<parser::Format> &format,
      const std::list<parser::IoControlSpec> &controls,
      const std::list<parser::InputItem> &inputs) {
    return IOCallArguments{};
  }
  IOCallArguments CreateWriteArguments(
      const std::optional<parser::IoUnit> &ioUnit,
      const std::optional<parser::Format> &format,
      const std::list<parser::IoControlSpec> &controls,
      const std::list<parser::OutputItem> &outputs) {
    return IOCallArguments{};
  }

  // Runtime argument translations ...
  RuntimeCallArguments CreateEventPostArguments(
      const parser::EventPostStmt &eventPostStatement) {
    return RuntimeCallArguments{};
  }
  RuntimeCallArguments CreateEventWaitArguments(
      const parser::EventWaitStmt &eventWaitStatement) {
    return RuntimeCallArguments{};
  }
  RuntimeCallArguments CreateFailImageArguments(
      const parser::FailImageStmt &failImageStatement) {
    return RuntimeCallArguments{};
  }
  RuntimeCallArguments CreateFormTeamArguments(
      const parser::FormTeamStmt &formTeamStatement) {
    return RuntimeCallArguments{};
  }
  RuntimeCallArguments CreateLockArguments(
      const parser::LockStmt &lockStatement) {
    return RuntimeCallArguments{};
  }
  RuntimeCallArguments CreatePauseArguments(
      const parser::PauseStmt &pauseStatement) {
    return RuntimeCallArguments{};
  }
  RuntimeCallArguments CreateStopArguments(
      const parser::StopStmt &stopStatement) {
    return RuntimeCallArguments{};
  }
  RuntimeCallArguments CreateSyncAllArguments(
      const parser::SyncAllStmt &syncAllStatement) {
    return RuntimeCallArguments{};
  }
  RuntimeCallArguments CreateSyncImagesArguments(
      const parser::SyncImagesStmt &syncImagesStatement) {
    return RuntimeCallArguments{};
  }
  RuntimeCallArguments CreateSyncMemoryArguments(
      const parser::SyncMemoryStmt &syncMemoryStatement) {
    return RuntimeCallArguments{};
  }
  RuntimeCallArguments CreateSyncTeamArguments(
      const parser::SyncTeamStmt &syncTeamStatement) {
    return RuntimeCallArguments{};
  }
  RuntimeCallArguments CreateUnlockArguments(
      const parser::UnlockStmt &unlockStatement) {
    return RuntimeCallArguments{};
  }

  // CALL translations ...
  const Value CreateCalleeValue(const parser::ProcedureDesignator &designator) {
    return NOTHING;
  }
  CallArguments CreateCallArguments(
      const std::list<parser::ActualArgSpec> &arguments) {
    return CallArguments{};
  }

  template<typename STMTTYPE, typename CT>
  Statement *GetSwitchSelector(const CT *selectConstruct) {
    return std::visit(
        common::visitors{
            [&](const parser::Expr &e) {
              return builder_->CreateExpr(ExprRef(e));
            },
            [&](const parser::Variable &v) {
              return builder_->CreateExpr(VariableToExpression(v));
            },
        },
        std::get<parser::Selector>(
            std::get<parser::Statement<STMTTYPE>>(selectConstruct->t)
                .statement.t)
            .u);
  }
  Statement *GetSwitchRankSelector(
      const parser::SelectRankConstruct *selectRankConstruct) {
    return GetSwitchSelector<parser::SelectRankStmt>(selectRankConstruct);
  }
  Statement *GetSwitchTypeSelector(
      const parser::SelectTypeConstruct *selectTypeConstruct) {
    return GetSwitchSelector<parser::SelectTypeStmt>(selectTypeConstruct);
  }
  Statement *GetSwitchCaseSelector(const parser::CaseConstruct *construct) {
    const auto &x{std::get<parser::Scalar<parser::Expr>>(
        std::get<parser::Statement<parser::SelectCaseStmt>>(construct->t)
            .statement.t)};
    return builder_->CreateExpr(ExprRef(x.thing));
  }
  SwitchArguments ComposeSwitchArgs(const LinearSwitch &op) {
    SwitchArguments result{NOTHING, unspecifiedLabel, {}, op.refs};
    std::visit(
        common::visitors{
            [&](const parser::ComputedGotoStmt *c) {
              const auto &e{std::get<parser::ScalarIntExpr>(c->t)};
              result.exp = builder_->CreateExpr(ExprRef(e.thing.thing));
              buildMultiwayDefaultNext(result);
            },
            [&](const parser::ArithmeticIfStmt *c) {
              result.exp =
                  builder_->CreateExpr(ExprRef(std::get<parser::Expr>(c->t)));
            },
            [&](const parser::CallStmt *c) {
              result.exp = NOTHING;  // fixme - result of call
              buildMultiwayDefaultNext(result);
            },
            [](const auto *) { WRONG_PATH(); },
        },
        op.u);
    return result;
  }
  SwitchCaseArguments ComposeSwitchCaseArguments(
      const parser::CaseConstruct *caseConstruct,
      const std::vector<LinearLabelRef> &refs) {
    auto &cases{
        std::get<std::list<parser::CaseConstruct::Case>>(caseConstruct->t)};
    SwitchCaseArguments result{
        GetSwitchCaseSelector(caseConstruct), unspecifiedLabel,
            populateSwitchValues(builder_, cases), std::move(refs)};
    cleanupSwitchPairs<SwitchCaseStmt>(
        result.defLab, result.values, result.labels);
    return result;
  }
  SwitchRankArguments ComposeSwitchRankArguments(
      const parser::SelectRankConstruct *selectRankConstruct,
      const std::vector<LinearLabelRef> &refs) {
    auto &ranks{std::get<std::list<parser::SelectRankConstruct::RankCase>>(
        selectRankConstruct->t)};
    SwitchRankArguments result{GetSwitchRankSelector(selectRankConstruct),
        unspecifiedLabel, populateSwitchValues(ranks), std::move(refs)};
    if (auto &name{GetSwitchAssociateName<parser::SelectRankStmt>(
            selectRankConstruct)}) {
      (void)name;  // get rid of warning
      // TODO: handle associate-name -> Add an assignment stmt?
    }
    cleanupSwitchPairs<SwitchRankStmt>(
        result.defLab, result.values, result.labels);
    return result;
  }
  SwitchTypeArguments ComposeSwitchTypeArguments(
      const parser::SelectTypeConstruct *selectTypeConstruct,
      const std::vector<LinearLabelRef> &refs) {
    auto &types{std::get<std::list<parser::SelectTypeConstruct::TypeCase>>(
        selectTypeConstruct->t)};
    SwitchTypeArguments result{GetSwitchTypeSelector(selectTypeConstruct),
        unspecifiedLabel, populateSwitchValues(types), std::move(refs)};
    if (auto &name{GetSwitchAssociateName<parser::SelectTypeStmt>(
            selectTypeConstruct)}) {
      (void)name;  // get rid of warning
      // TODO: handle associate-name -> Add an assignment stmt?
    }
    cleanupSwitchPairs<SwitchTypeStmt>(
        result.defLab, result.values, result.labels);
    return result;
  }

  Expression VariableToExpression(const parser::Variable &var) {
    evaluate::ExpressionAnalyzer analyzer{semanticsContext_};
    return {std::move(analyzer.Analyze(var).value())};
  }
  Expression DataRefToExpression(const parser::DataRef &dr) {
    evaluate::ExpressionAnalyzer analyzer{semanticsContext_};
    return {std::move(analyzer.Analyze(dr).value())};
  }
  Expression NameToExpression(const parser::Name &name) {
    evaluate::ExpressionAnalyzer analyzer{semanticsContext_};
    return {std::move(analyzer.Analyze(name).value())};
  }
  Expression StructureComponentToExpression(
      const parser::StructureComponent &sc) {
    evaluate::ExpressionAnalyzer analyzer{semanticsContext_};
    return {std::move(analyzer.Analyze(sc).value())};
  }

  void handleIntrinsicAssignmentStmt(const parser::AssignmentStmt &stmt) {
    // TODO: check if allocation or reallocation should happen, etc.
    auto *value{builder_->CreateExpr(ExprRef(std::get<parser::Expr>(stmt.t)))};
    auto *addr{builder_->CreateAddr(
        VariableToExpression(std::get<parser::Variable>(stmt.t)))};
    builder_->CreateStore(addr, value);
  }
  void handleDefinedAssignmentStmt(const parser::AssignmentStmt &stmt) {
    CHECK(false && "TODO defined assignment");
  }
  void handleAssignmentStmt(const parser::AssignmentStmt &stmt) {
    // TODO: is this an intrinsic assignment or a defined assignment?
    if (true) {
      handleIntrinsicAssignmentStmt(stmt);
    } else {
      handleDefinedAssignmentStmt(stmt);
    }
  }

  struct AllocOpts {
    std::optional<Expression> mold;
    std::optional<Expression> source;
    std::optional<Expression> stat;
    std::optional<Expression> errmsg;
  };
  void handleAllocateStmt(const parser::AllocateStmt &stmt) {
    // extract options from list -> opts
    AllocOpts opts;
    for (auto &allocOpt : std::get<std::list<parser::AllocOpt>>(stmt.t)) {
      std::visit(
          common::visitors{
              [&](const parser::AllocOpt::Mold &m) {
                opts.mold = *ExprRef(m.v);
              },
              [&](const parser::AllocOpt::Source &s) {
                opts.source = *ExprRef(s.v);
              },
              [&](const parser::StatOrErrmsg &var) {
                std::visit(
                    common::visitors{
                        [&](const parser::StatVariable &sv) {
                          opts.stat = VariableToExpression(sv.v.thing.thing);
                        },
                        [&](const parser::MsgVariable &mv) {
                          opts.errmsg = VariableToExpression(mv.v.thing.thing);
                        },
                    },
                    var.u);
              },
          },
          allocOpt.u);
    }
    // process the list of allocations
    for (auto &allocation : std::get<std::list<parser::Allocation>>(stmt.t)) {
      // TODO: add more arguments to builder as needed
      builder_->CreateAlloc(CreateAllocationValue(&allocation, &stmt));
    }
  }

  void handleActionStatement(
      AnalysisData &ad, const parser::Statement<parser::ActionStmt> &stmt) {
    std::visit(
        common::visitors{
            [&](const common::Indirection<parser::AllocateStmt> &s) {
              handleAllocateStmt(s.value());
            },
            [&](const common::Indirection<parser::AssignmentStmt> &s) {
              handleAssignmentStmt(s.value());
            },
            [&](const common::Indirection<parser::BackspaceStmt> &s) {
              builder_->CreateIOCall(InputOutputCallBackspace,
                  CreateBackspaceArguments(s.value().v));
            },
            [&](const common::Indirection<parser::CallStmt> &s) {
              builder_->CreateCall(nullptr,
                  CreateCalleeValue(
                      std::get<parser::ProcedureDesignator>(s.value().v.t)),
                  CreateCallArguments(
                      std::get<std::list<parser::ActualArgSpec>>(
                          s.value().v.t)));
            },
            [&](const common::Indirection<parser::CloseStmt> &s) {
              builder_->CreateIOCall(
                  InputOutputCallClose, CreateCloseArguments(s.value().v));
            },
            [](const parser::ContinueStmt &) { WRONG_PATH(); },
            [](const common::Indirection<parser::CycleStmt> &) {
              WRONG_PATH();
            },
            [&](const common::Indirection<parser::DeallocateStmt> &s) {
              for (auto &alloc :
                  std::get<std::list<parser::AllocateObject>>(s.value().t)) {
                builder_->CreateDealloc(
                    CreateDeallocationValue(&alloc, &s.value()));
              }
            },
            [&](const common::Indirection<parser::EndfileStmt> &s) {
              builder_->CreateIOCall(
                  InputOutputCallEndfile, CreateEndfileArguments(s.value().v));
            },
            [&](const common::Indirection<parser::EventPostStmt> &s) {
              builder_->CreateRuntimeCall(
                  RuntimeCallEventPost, CreateEventPostArguments(s.value()));
            },
            [&](const common::Indirection<parser::EventWaitStmt> &s) {
              builder_->CreateRuntimeCall(
                  RuntimeCallEventWait, CreateEventWaitArguments(s.value()));
            },
            [](const common::Indirection<parser::ExitStmt> &) { WRONG_PATH(); },
            [&](const parser::FailImageStmt &s) {
              builder_->CreateRuntimeCall(
                  RuntimeCallFailImage, CreateFailImageArguments(s));
            },
            [&](const common::Indirection<parser::FlushStmt> &s) {
              builder_->CreateIOCall(
                  InputOutputCallFlush, CreateFlushArguments(s.value().v));
            },
            [&](const common::Indirection<parser::FormTeamStmt> &s) {
              builder_->CreateRuntimeCall(
                  RuntimeCallFormTeam, CreateFormTeamArguments(s.value()));
            },
            [](const common::Indirection<parser::GotoStmt> &) { WRONG_PATH(); },
            [](const common::Indirection<parser::IfStmt> &) { WRONG_PATH(); },
            [&](const common::Indirection<parser::InquireStmt> &s) {
              std::visit(
                  common::visitors{
                      [&](const std::list<parser::InquireSpec> &specifiers) {
                        builder_->CreateIOCall(InputOutputCallInquire,
                            CreateInquireArguments(specifiers));
                      },
                      [&](const parser::InquireStmt::Iolength &iolength) {
                        builder_->CreateIOCall(InputOutputCallInquire,
                            CreateInquireArguments(iolength));
                      },
                  },
                  s.value().u);
            },
            [&](const common::Indirection<parser::LockStmt> &s) {
              builder_->CreateRuntimeCall(
                  RuntimeCallLock, CreateLockArguments(s.value()));
            },
            [&](const common::Indirection<parser::NullifyStmt> &s) {
              for (auto &obj : s.value().v) {
                std::visit(
                    common::visitors{
                        [&](const parser::Name &n) {
                          auto *s{builder_->CreateAddr(NameToExpression(n))};
                          builder_->CreateNullify(s);
                        },
                        [&](const parser::StructureComponent &sc) {
                          auto *s{builder_->CreateAddr(
                              StructureComponentToExpression(sc))};
                          builder_->CreateNullify(s);
                        },
                    },
                    obj.u);
              }
            },
            [&](const common::Indirection<parser::OpenStmt> &s) {
              builder_->CreateIOCall(
                  InputOutputCallOpen, CreateOpenArguments(s.value().v));
            },
            [&](const common::Indirection<parser::PointerAssignmentStmt> &s) {
              auto *value{CreatePointerValue(s.value())};
              auto *addr{builder_->CreateAddr(
                  ExprRef(std::get<parser::Expr>(s.value().t)))};
              builder_->CreateStore(addr, value);
            },
            [&](const common::Indirection<parser::PrintStmt> &s) {
              builder_->CreateIOCall(InputOutputCallPrint,
                  CreatePrintArguments(std::get<parser::Format>(s.value().t),
                      std::get<std::list<parser::OutputItem>>(s.value().t)));
            },
            [&](const common::Indirection<parser::ReadStmt> &s) {
              builder_->CreateIOCall(InputOutputCallRead,
                  CreateReadArguments(s.value().iounit, s.value().format,
                      s.value().controls, s.value().items));
            },
            [](const common::Indirection<parser::ReturnStmt> &) {
              WRONG_PATH();
            },
            [&](const common::Indirection<parser::RewindStmt> &s) {
              builder_->CreateIOCall(
                  InputOutputCallRewind, CreateRewindArguments(s.value().v));
            },
            [&](const common::Indirection<parser::StopStmt> &s) {
              builder_->CreateRuntimeCall(
                  RuntimeCallStop, CreateStopArguments(s.value()));
            },
            [&](const common::Indirection<parser::SyncAllStmt> &s) {
              builder_->CreateRuntimeCall(
                  RuntimeCallSyncAll, CreateSyncAllArguments(s.value()));
            },
            [&](const common::Indirection<parser::SyncImagesStmt> &s) {
              builder_->CreateRuntimeCall(
                  RuntimeCallSyncImages, CreateSyncImagesArguments(s.value()));
            },
            [&](const common::Indirection<parser::SyncMemoryStmt> &s) {
              builder_->CreateRuntimeCall(
                  RuntimeCallSyncMemory, CreateSyncMemoryArguments(s.value()));
            },
            [&](const common::Indirection<parser::SyncTeamStmt> &s) {
              builder_->CreateRuntimeCall(
                  RuntimeCallSyncTeam, CreateSyncTeamArguments(s.value()));
            },
            [&](const common::Indirection<parser::UnlockStmt> &s) {
              builder_->CreateRuntimeCall(
                  RuntimeCallUnlock, CreateUnlockArguments(s.value()));
            },
            [&](const common::Indirection<parser::WaitStmt> &s) {
              builder_->CreateIOCall(
                  InputOutputCallWait, CreateWaitArguments(s.value().v));
            },
            [](const common::Indirection<parser::WhereStmt> &) { /*fixme*/ },
            [&](const common::Indirection<parser::WriteStmt> &s) {
              builder_->CreateIOCall(InputOutputCallWrite,
                  CreateWriteArguments(s.value().iounit, s.value().format,
                      s.value().controls, s.value().items));
            },
            [](const common::Indirection<parser::ComputedGotoStmt> &) {
              WRONG_PATH();
            },
            [](const common::Indirection<parser::ForallStmt> &) { /*fixme*/ },
            [](const common::Indirection<parser::ArithmeticIfStmt> &) {
              WRONG_PATH();
            },
            [&](const common::Indirection<parser::AssignStmt> &s) {
              auto *addr{builder_->CreateAddr(
                  NameToExpression(std::get<parser::Name>(s.value().t)))};
              auto *block{
                  blockMap_
                      .find(FetchLabel(ad, std::get<parser::Label>(s.value().t))
                                .get())
                      ->second};
              builder_->CreateStore(addr, block);
            },
            [](const common::Indirection<parser::AssignedGotoStmt> &) {
              WRONG_PATH();
            },
            [&](const common::Indirection<parser::PauseStmt> &s) {
              builder_->CreateRuntimeCall(
                  RuntimeCallPause, CreatePauseArguments(s.value()));
            },
        },
        stmt.statement.u);
  }
  void handleLinearAction(const LinearAction &action, AnalysisData &ad) {
    handleActionStatement(ad, *action.v);
  }

  // DO loop handlers
  struct DoBoundsInfo {
    Statement *doVariable;
    Statement *lowerBound;
    Statement *upperBound;
    Statement *stepExpr;
    Statement *condition;
  };
  void PushDoContext(const parser::NonLabelDoStmt *doStmt, Statement *doVar,
      Statement *lowBound, Statement *upBound, Statement *stepExp) {
    doMap_.emplace(doStmt, DoBoundsInfo{doVar, lowBound, upBound, stepExp});
  }
  void PopDoContext(const parser::NonLabelDoStmt *doStmt) {
    doMap_.erase(doStmt);
  }
  template<typename T> DoBoundsInfo *GetBoundsInfo(const T &linearOp) {
    auto *s{&std::get<parser::Statement<parser::NonLabelDoStmt>>(linearOp.v->t)
                 .statement};
    auto iter{doMap_.find(s)};
    if (iter != doMap_.end()) {
      return &iter->second;
    }
    CHECK(false && "DO context not present");
    return nullptr;
  }

  // do_var = do_var + e3
  void handleLinearDoIncrement(const LinearDoIncrement &inc) {
    auto *info{GetBoundsInfo(inc)};
    auto *var{builder_->CreateLoad(info->doVariable)};
    builder_->CreateIncrement(var, info->stepExpr);
  }

  // (e3 > 0 && do_var <= e2) || (e3 < 0 && do_var >= e2)
  void handleLinearDoCompare(const LinearDoCompare &cmp) {
    auto *info{GetBoundsInfo(cmp)};
    auto *var{builder_->CreateLoad(info->doVariable)};
    auto *cond{
        builder_->CreateDoCondition(info->stepExpr, var, info->upperBound)};
    info->condition = cond;
  }

  // InitiateConstruct - many constructs require some initial setup
  void InitiateConstruct(const parser::AssociateStmt *stmt) {
    for (auto &assoc : std::get<std::list<parser::Association>>(stmt->t)) {
      auto &selector{std::get<parser::Selector>(assoc.t)};
      auto *expr{builder_->CreateExpr(std::visit(
          common::visitors{
              [&](const parser::Variable &v) {
                return VariableToExpression(v);
              },
              [](const parser::Expr &e) { return *ExprRef(e); },
          },
          selector.u))};
      auto *name{builder_->CreateAddr(
          NameToExpression(std::get<parser::Name>(assoc.t)))};
      builder_->CreateStore(name, expr);
    }
  }
  void InitiateConstruct(const parser::SelectCaseStmt *stmt) {
    builder_->CreateExpr(
        ExprRef(std::get<parser::Scalar<parser::Expr>>(stmt->t).thing));
  }
  void InitiateConstruct(const parser::ChangeTeamStmt *changeTeamStmt) {
    // FIXME
  }
  void InitiateConstruct(const parser::IfThenStmt *ifThenStmt) {
    const auto &e{std::get<parser::ScalarLogicalExpr>(ifThenStmt->t).thing};
    builder_->CreateExpr(ExprRef(e.thing));
  }
  void InitiateConstruct(const parser::WhereConstructStmt *whereConstructStmt) {
    const auto &e{std::get<parser::LogicalExpr>(whereConstructStmt->t)};
    builder_->CreateExpr(ExprRef(e.thing));
  }
  void InitiateConstruct(
      const parser::ForallConstructStmt *forallConstructStmt) {
    // FIXME
  }

  void InitiateConstruct(const parser::NonLabelDoStmt *stmt) {
    auto &ctrl{std::get<std::optional<parser::LoopControl>>(stmt->t)};
    if (ctrl.has_value()) {
      std::visit(
          common::visitors{
              [&](const parser::LoopBounds<parser::ScalarIntExpr> &bounds) {
                auto *var = builder_->CreateAddr(
                    NameToExpression(bounds.name.thing.thing));
                // evaluate e1, e2 [, e3] ...
                auto *e1{
                    builder_->CreateExpr(ExprRef(bounds.lower.thing.thing))};
                auto *e2{
                    builder_->CreateExpr(ExprRef(bounds.upper.thing.thing))};
                Statement *e3;
                if (bounds.step.has_value()) {
                  e3 = builder_->CreateExpr(ExprRef(bounds.step->thing.thing));
                } else {
                  e3 = builder_->CreateExpr(CreateConstant(1));
                }
                builder_->CreateStore(var, e1);
                PushDoContext(stmt, var, e1, e2, e3);
              },
              [&](const parser::ScalarLogicalExpr &whileExpr) {},
              [&](const parser::LoopControl::Concurrent &cc) {},
          },
          ctrl->u);
    } else {
      // loop forever
    }
  }

  // finish DO construct construction
  void FinishConstruct(const parser::NonLabelDoStmt *stmt) {
    auto &ctrl{std::get<std::optional<parser::LoopControl>>(stmt->t)};
    if (ctrl.has_value()) {
      std::visit(
          common::visitors{
              [&](const parser::LoopBounds<parser::ScalarIntExpr> &) {
                PopDoContext(stmt);
              },
              [&](auto &) {
                // do nothing
              },
          },
          ctrl->u);
    }
  }

  Statement *BuildLoopLatchExpression(const parser::NonLabelDoStmt *stmt) {
    auto &loopCtrl{std::get<std::optional<parser::LoopControl>>(stmt->t)};
    if (loopCtrl.has_value()) {
      return std::visit(
          common::visitors{
              [&](const parser::LoopBounds<parser::ScalarIntExpr> &) {
                return doMap_.find(stmt)->second.condition;
              },
              [&](const parser::ScalarLogicalExpr &sle) {
                auto &exp{sle.thing.thing.value()};
                SEMANTICS_CHECK(ExprRef(exp), "DO WHILE condition missing");
                return builder_->CreateExpr(ExprRef(exp));
              },
              [&](const parser::LoopControl::Concurrent &concurrent) {
                // FIXME - how do we want to lower DO CONCURRENT?
                return builder_->CreateExpr(AlwaysTrueExpression());
              },
          },
          loopCtrl->u);
    }
    return builder_->CreateExpr(AlwaysTrueExpression());
  }

  void ConstructFIR(AnalysisData &ad) {
    for (auto iter{linearOperations_.begin()}, iend{linearOperations_.end()};
         iter != iend; ++iter) {
      const auto &op{*iter};
      std::visit(
          common::visitors{
              [&](const LinearLabel &linearLabel) {
                auto *newBlock{CreateBlock(builder_->GetCurrentRegion())};
                blockMap_.insert({linearLabel.get(), newBlock});
                if (builder_->GetInsertionPoint()) {
                  builder_->CreateBranch(newBlock);
                }
                builder_->SetInsertionPoint(newBlock);
              },
              [&](const LinearGoto &linearGoto) {
                CheckInsertionPoint();
                AddOrQueueBranch(linearGoto.target);
                builder_->ClearInsertionPoint();
              },
              [&](const LinearIndirectGoto &linearIGoto) {
                CheckInsertionPoint();
                AddOrQueueIGoto(ad, linearIGoto.symbol, linearIGoto.labelRefs);
                builder_->ClearInsertionPoint();
              },
              [&](const LinearReturn &linearReturn) {
                CheckInsertionPoint();
                std::visit(
                    common::visitors{
                        [&](const parser::FailImageStmt *s) {
                          builder_->CreateRuntimeCall(RuntimeCallFailImage,
                              CreateFailImageArguments(*s));
                          builder_->CreateUnreachable();
                        },
                        [&](const parser::ReturnStmt *s) {
                          // alt-return
                          if (s->v) {
                            auto *app{builder_->CreateExpr(
                                ExprRef(s->v->thing.thing))};
                            builder_->CreateReturn(app);
                          } else {
                            auto *zero{builder_->CreateExpr(CreateConstant(0))};
                            builder_->CreateReturn(zero);
                          }
                        },
                        [&](const parser::StopStmt *s) {
                          builder_->CreateRuntimeCall(
                              RuntimeCallStop, CreateStopArguments(*s));
                          builder_->CreateUnreachable();
                        },
                    },
                    linearReturn.u);
                builder_->ClearInsertionPoint();
              },
              [&](const LinearConditionalGoto &linearConditionalGoto) {
                CheckInsertionPoint();
                std::visit(
                    common::visitors{
                        [&](const parser::Statement<parser::IfThenStmt> *s) {
                          const auto &exp{std::get<parser::ScalarLogicalExpr>(
                              s->statement.t)
                                              .thing.thing.value()};
                          SEMANTICS_CHECK(ExprRef(exp),
                              "IF THEN condition expression missing");
                          auto *cond{builder_->CreateExpr(ExprRef(exp))};
                          AddOrQueueCGoto(cond, linearConditionalGoto.trueLabel,
                              linearConditionalGoto.falseLabel);
                        },
                        [&](const parser::Statement<parser::ElseIfStmt> *s) {
                          const auto &exp{std::get<parser::ScalarLogicalExpr>(
                              s->statement.t)
                                              .thing.thing.value()};
                          SEMANTICS_CHECK(ExprRef(exp),
                              "ELSE IF condition expression missing");
                          auto *cond{builder_->CreateExpr(ExprRef(exp))};
                          AddOrQueueCGoto(cond, linearConditionalGoto.trueLabel,
                              linearConditionalGoto.falseLabel);
                        },
                        [&](const parser::IfStmt *s) {
                          const auto &exp{
                              std::get<parser::ScalarLogicalExpr>(s->t)
                                  .thing.thing.value()};
                          SEMANTICS_CHECK(
                              ExprRef(exp), "IF condition expression missing");
                          auto *cond{builder_->CreateExpr(ExprRef(exp))};
                          AddOrQueueCGoto(cond, linearConditionalGoto.trueLabel,
                              linearConditionalGoto.falseLabel);
                        },
                        [&](const parser::Statement<parser::NonLabelDoStmt>
                                *s) {
                          AddOrQueueCGoto(
                              BuildLoopLatchExpression(&s->statement),
                              linearConditionalGoto.trueLabel,
                              linearConditionalGoto.falseLabel);
                        }},
                    linearConditionalGoto.u);
                builder_->ClearInsertionPoint();
              },
              [&](const LinearSwitchingIO &linearIO) {
                CheckInsertionPoint();
                AddOrQueueSwitch<SwitchStmt>(
                    NOTHING, linearIO.next, {}, {}, CreateSwitchHelper);
                builder_->ClearInsertionPoint();
              },
              [&](const LinearSwitch &linearSwitch) {
                CheckInsertionPoint();
                std::visit(
                    common::visitors{
                        [&](auto) {
                          auto args{ComposeSwitchArgs(linearSwitch)};
                          AddOrQueueSwitch<SwitchStmt>(args.exp, args.defLab,
                              args.values, args.labels, CreateSwitchHelper);
                        },
                        [&](const parser::CaseConstruct *caseConstruct) {
                          auto args{ComposeSwitchCaseArguments(
                              caseConstruct, linearSwitch.refs)};
                          AddOrQueueSwitch<SwitchCaseStmt>(args.exp,
                              args.defLab, args.values, args.labels,
                              CreateSwitchCaseHelper);
                        },
                        [&](const parser::SelectRankConstruct
                                *selectRankConstruct) {
                          auto args{ComposeSwitchRankArguments(
                              selectRankConstruct, linearSwitch.refs)};
                          AddOrQueueSwitch<SwitchRankStmt>(args.exp,
                              args.defLab, args.values, args.labels,
                              CreateSwitchRankHelper);
                        },
                        [&](const parser::SelectTypeConstruct
                                *selectTypeConstruct) {
                          auto args{ComposeSwitchTypeArguments(
                              selectTypeConstruct, linearSwitch.refs)};
                          AddOrQueueSwitch<SwitchTypeStmt>(args.exp,
                              args.defLab, args.values, args.labels,
                              CreateSwitchTypeHelper);
                        },
                    },
                    linearSwitch.u);
                builder_->ClearInsertionPoint();
              },
              [&](const LinearAction &action) {
                CheckInsertionPoint();
                handleLinearAction(action, ad);
              },
              [&](const LinearDoIncrement &inc) {
                CheckInsertionPoint();
                handleLinearDoIncrement(inc);
              },
              [&](const LinearDoCompare &cmp) {
                CheckInsertionPoint();
                handleLinearDoCompare(cmp);
              },
              [&](const LinearBeginConstruct &linearConstruct) {
                std::visit(
                    common::visitors{
                        [&](const parser::AssociateConstruct *construct) {
                          const auto &statement{std::get<
                              parser::Statement<parser::AssociateStmt>>(
                              construct->t)};
                          const auto &position{statement.source};
                          EnterRegion(position);
                          InitiateConstruct(&statement.statement);
                        },
                        [&](const parser::BlockConstruct *construct) {
                          EnterRegion(
                              std::get<parser::Statement<parser::BlockStmt>>(
                                  construct->t)
                                  .source);
                        },
                        [&](const parser::CaseConstruct *construct) {
                          InitiateConstruct(
                              &std::get<
                                  parser::Statement<parser::SelectCaseStmt>>(
                                  construct->t)
                                   .statement);
                        },
                        [&](const parser::ChangeTeamConstruct *construct) {
                          const auto &statement{std::get<
                              parser::Statement<parser::ChangeTeamStmt>>(
                              construct->t)};
                          EnterRegion(statement.source);
                          InitiateConstruct(&statement.statement);
                        },
                        [&](const parser::DoConstruct *construct) {
                          const auto &statement{std::get<
                              parser::Statement<parser::NonLabelDoStmt>>(
                              construct->t)};
                          EnterRegion(statement.source);
                          InitiateConstruct(&statement.statement);
                        },
                        [&](const parser::IfConstruct *construct) {
                          InitiateConstruct(
                              &std::get<parser::Statement<parser::IfThenStmt>>(
                                  construct->t)
                                   .statement);
                        },
                        [&](const parser::SelectRankConstruct *construct) {
                          const auto &statement{std::get<
                              parser::Statement<parser::SelectRankStmt>>(
                              construct->t)};
                          EnterRegion(statement.source);
                        },
                        [&](const parser::SelectTypeConstruct *construct) {
                          const auto &statement{std::get<
                              parser::Statement<parser::SelectTypeStmt>>(
                              construct->t)};
                          EnterRegion(statement.source);
                        },
                        [&](const parser::WhereConstruct *construct) {
                          InitiateConstruct(
                              &std::get<parser::Statement<
                                   parser::WhereConstructStmt>>(construct->t)
                                   .statement);
                        },
                        [&](const parser::ForallConstruct *construct) {
                          InitiateConstruct(
                              &std::get<parser::Statement<
                                   parser::ForallConstructStmt>>(construct->t)
                                   .statement);
                        },
                        [](const parser::CriticalConstruct *) { /*fixme*/ },
                        [](const parser::CompilerDirective *) { /*fixme*/ },
                        [](const parser::OpenMPConstruct *) { /*fixme*/ },
                        [](const parser::OpenMPEndLoopDirective
                                *) { /*fixme*/ },
                    },
                    linearConstruct.u);
                auto next{iter};
                const auto &nextOp{*(++next)};
                std::visit(
                    common::visitors{
                        [](const auto &) {},
                        [&](const LinearLabel &linearLabel) {
                          blockMap_.insert({linearLabel.get(),
                              builder_->GetInsertionPoint()});
                          ++iter;
                        },
                    },
                    nextOp.u);
              },
              [&](const LinearEndConstruct &linearConstruct) {
                std::visit(
                    common::visitors{
                        [](const auto &) {},
                        [&](const parser::BlockConstruct *) { ExitRegion(); },
                        [&](const parser::DoConstruct *crct) {
                          const auto &statement{std::get<
                              parser::Statement<parser::NonLabelDoStmt>>(
                              crct->t)};
                          FinishConstruct(&statement.statement);
                          ExitRegion();
                        },
                        [&](const parser::AssociateConstruct *) {
                          ExitRegion();
                        },
                        [&](const parser::ChangeTeamConstruct *) {
                          ExitRegion();
                        },
                        [&](const parser::SelectTypeConstruct *) {
                          ExitRegion();
                        },
                    },
                    linearConstruct.u);
              },
          },
          op.u);
    }
  }
  void EnterRegion(const parser::CharBlock &pos) {
    auto *region{builder_->GetCurrentRegion()};
    auto *scope{semanticsContext_.globalScope().FindScope(pos)};
    auto *newRegion{Region::Create(region->getParent(), scope, region)};
    auto *block{CreateBlock(newRegion)};
    CheckInsertionPoint();
    builder_->CreateBranch(block);
    builder_->SetInsertionPoint(block);
  }
  void ExitRegion() {
    builder_->SetCurrentRegion(builder_->GetCurrentRegion()->GetEnclosing());
  }
  void CheckInsertionPoint() {
    if (!builder_->GetInsertionPoint()) {
      builder_->SetInsertionPoint(CreateBlock(builder_->GetCurrentRegion()));
    }
  }
  void AddOrQueueBranch(LinearLabelRef dest) {
    auto iter{blockMap_.find(dest)};
    if (iter != blockMap_.end()) {
      builder_->CreateBranch(iter->second);
    } else {
      using namespace std::placeholders;
      controlFlowEdgesToAdd_.emplace_back(std::bind(
          [](FIRBuilder *builder, BasicBlock *block, LinearLabelRef dest,
              const LabelMapType &map) {
            builder->SetInsertionPoint(block);
            CHECK(map.find(dest) != map.end());
            builder->CreateBranch(map.find(dest)->second);
          },
          builder_, builder_->GetInsertionPoint(), dest, _1));
    }
  }
  void AddOrQueueCGoto(Statement *condition, LinearLabelRef trueBlock,
      LinearLabelRef falseBlock) {
    auto trueIter{blockMap_.find(trueBlock)};
    auto falseIter{blockMap_.find(falseBlock)};
    if (trueIter != blockMap_.end() && falseIter != blockMap_.end()) {
      builder_->CreateConditionalBranch(
          condition, trueIter->second, falseIter->second);
    } else {
      using namespace std::placeholders;
      controlFlowEdgesToAdd_.emplace_back(std::bind(
          [](FIRBuilder *builder, BasicBlock *block, Statement *expr,
              LinearLabelRef trueDest, LinearLabelRef falseDest,
              const LabelMapType &map) {
            builder->SetInsertionPoint(block);
            CHECK(map.find(trueDest) != map.end());
            CHECK(map.find(falseDest) != map.end());
            builder->CreateConditionalBranch(
                expr, map.find(trueDest)->second, map.find(falseDest)->second);
          },
          builder_, builder_->GetInsertionPoint(), condition, trueBlock,
          falseBlock, _1));
    }
  }

  template<typename SWITCHTYPE, typename F>
  void AddOrQueueSwitch(Value condition, LinearLabelRef defaultLabel,
      const std::vector<typename SWITCHTYPE::ValueType> &values,
      const std::vector<LinearLabelRef> &labels, F function) {
    auto defer{false};
    auto defaultIter{blockMap_.find(defaultLabel)};
    typename SWITCHTYPE::ValueSuccPairListType cases;
    if (defaultIter == blockMap_.end()) {
      defer = true;
    } else {
      CHECK(values.size() == labels.size());
      auto valiter{values.begin()};
      for (auto lab : labels) {
        auto labIter{blockMap_.find(lab)};
        if (labIter == blockMap_.end()) {
          defer = true;
          break;
        } else {
          cases.emplace_back(*valiter++, labIter->second);
        }
      }
    }
    if (defer) {
      using namespace std::placeholders;
      controlFlowEdgesToAdd_.emplace_back(std::bind(
          [](FIRBuilder *builder, BasicBlock *block, Value expr,
              LinearLabelRef defaultDest,
              const std::vector<typename SWITCHTYPE::ValueType> &values,
              const std::vector<LinearLabelRef> &labels, F function,
              const LabelMapType &map) {
            builder->SetInsertionPoint(block);
            typename SWITCHTYPE::ValueSuccPairListType cases;
            auto valiter{values.begin()};
            for (auto &lab : labels) {
              cases.emplace_back(*valiter++, map.find(lab)->second);
            }
            function(builder, expr, map.find(defaultDest)->second, cases);
          },
          builder_, builder_->GetInsertionPoint(), condition, defaultLabel,
          values, labels, function, _1));
    } else {
      function(builder_, condition, defaultIter->second, cases);
    }
  }

  Variable *ConvertToVariable(const semantics::Symbol *symbol) {
    // FIXME: how to convert semantics::Symbol to evaluate::Variable?
    return new Variable(symbol);
  }

  void AddOrQueueIGoto(AnalysisData &ad, const semantics::Symbol *symbol,
      const std::vector<LinearLabelRef> &labels) {
    auto useLabels{labels.empty() ? GetAssign(ad, symbol) : labels};
    auto defer{false};
    IndirectBranchStmt::TargetListType blocks;
    for (auto lab : useLabels) {
      auto iter{blockMap_.find(lab)};
      if (iter == blockMap_.end()) {
        defer = true;
        break;
      } else {
        blocks.push_back(iter->second);
      }
    }
    if (defer) {
      using namespace std::placeholders;
      controlFlowEdgesToAdd_.emplace_back(std::bind(
          [](FIRBuilder *builder, BasicBlock *block, Variable *variable,
              const std::vector<LinearLabelRef> &fixme,
              const LabelMapType &map) {
            builder->SetInsertionPoint(block);
            builder->CreateIndirectBr(variable, {});  // FIXME
          },
          builder_, builder_->GetInsertionPoint(), nullptr /*symbol*/,
          useLabels, _1));
    } else {
      builder_->CreateIndirectBr(ConvertToVariable(symbol), blocks);
    }
  }

  void DrawRemainingArcs() {
    for (auto &arc : controlFlowEdgesToAdd_) {
      arc(blockMap_);
    }
  }

  BasicBlock *CreateBlock(Region *region) { return BasicBlock::Create(region); }

  void Cleanup() {
    delete builder_;
    builder_ = nullptr;
    linearOperations_.clear();
    controlFlowEdgesToAdd_.clear();
    blockMap_.clear();
  }

  FIRBuilder *builder_{nullptr};
  Program *fir_;
  std::list<LinearOp> linearOperations_;
  std::list<Closure> controlFlowEdgesToAdd_;
  std::map<const parser::NonLabelDoStmt *, DoBoundsInfo> doMap_;
  LabelMapType blockMap_;
  semantics::SemanticsContext &semanticsContext_;
  bool debugLinearFIR_;
};

Program *CreateFortranIR(const parser::Program &program,
    semantics::SemanticsContext &semanticsContext, bool debugLinearIR) {
  FortranIRLowering converter{semanticsContext, debugLinearIR};
  Walk(program, converter);
  return converter.program();
}

void SetDebugChannel(const std::string &filename) {
  std::error_code ec;
  SetDebugChannel(
      new llvm::raw_fd_ostream(filename, ec, llvm::sys::fs::F_None));
  CHECK(!ec);
}
}
