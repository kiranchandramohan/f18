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

#include "basicblock.h"
#include "statements.h"

namespace Fortran::FIR {

BasicBlock::BasicBlock(Region *parentRegion, BasicBlock *insertBefore)
  : ChildMixin{parentRegion} {
  parent->insertBefore(this, insertBefore);
}

BasicBlock::~BasicBlock() { statementList_.clear(); }

void BasicBlock::insertBefore(Statement *stmt, Statement *before) {
  if (before) {
    statementList_.insert(before->getIterator(), stmt);
  } else {
    statementList_.push_back(stmt);
  }
}

void BasicBlock::addPred(BasicBlock *bb) {
  for (auto *p : preds_) {
    if (p == bb) {
      return;
    }
  }
  preds_.push_back(bb);
}

const Statement *BasicBlock::terminator() const {
  if (statementList_.empty()) {
    return nullptr;
  }
  const auto &lastStmt{statementList_.back()};
  return std::visit(
      [&lastStmt](auto stmt) -> const Statement * {
        if constexpr (std::is_base_of_v<TerminatorStmt_impl,
                          std::decay_t<decltype(stmt)>>) {
          return &lastStmt;
        }
        return nullptr;
      },
      lastStmt.u);
}
}
