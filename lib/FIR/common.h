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

#ifndef FORTRAN_FIR_COMMON_H_
#define FORTRAN_FIR_COMMON_H_

#include "../common/idioms.h"
#include "../common/indirection.h"
#include "../evaluate/expression.h"
#include "../evaluate/type.h"
#include "../evaluate/variable.h"
#include "../parser/parse-tree.h"
#include "../semantics/symbol.h"

// Some useful, self-documenting macros for failure modes
#define STRINGIFY(X) #X
#define LINE2STRING(X) STRINGIFY(X)
#define AT_HERE " at " __FILE__ "(" LINE2STRING(__LINE__) ")"
#define DIE Fortran::common::die
#define SEMANTICS_FAILED(STRING) DIE("semantics bug: " STRING AT_HERE)
#define SEMANTICS_CHECK(CONDITION, STRING) \
  if (CONDITION) { \
  } else { \
    DIE("semantics bug: " STRING AT_HERE); \
  }
#define WRONG_PATH() DIE("control should not reach here" AT_HERE)

namespace Fortran::FIR {

CLASS_TRAIT(ValueTrait)

class Value_impl {
public:
  using ValueTrait = std::true_type;

  std::string dump() const { return {}; }
};

struct Nothing {};
constexpr Nothing NOTHING{};

class Value;
class Statement;
class BasicBlock;
class Region;
class Procedure;
class Program;
class GraphWriter;
class DataObject;

struct Attribute {
  enum { IntentIn, IntentOut, IntentInOut, Value } attribute;
  unsigned short position;
};
using FunctionType = evaluate::SomeType;  // TODO: what should this be?
using AttributeList = std::vector<Attribute>;
enum struct LinkageTypes { Public, Hidden, External };
using Expression = evaluate::GenericExprWrapper;
using Variable = const semantics::Symbol *;
using PathVariable = const parser::Variable;
using Scope = const semantics::Scope;
using PHIPair = std::pair<Value, BasicBlock *>;
using CallArguments = std::vector<Expression>;
using Type = const semantics::DeclTypeSpec *;  // FIXME

enum InputOutputCallType {
  InputOutputCallBackspace = 11,
  InputOutputCallClose,
  InputOutputCallEndfile,
  InputOutputCallFlush,
  InputOutputCallInquire,
  InputOutputCallOpen,
  InputOutputCallPrint,
  InputOutputCallRead,
  InputOutputCallRewind,
  InputOutputCallWait,
  InputOutputCallWrite,
  InputOutputCallSIZE = InputOutputCallWrite - InputOutputCallBackspace + 1
};

using IOCallArguments = CallArguments;

enum RuntimeCallType {
  RuntimeCallFailImage = 31,
  RuntimeCallStop,
  RuntimeCallPause,
  RuntimeCallFormTeam,
  RuntimeCallEventPost,
  RuntimeCallEventWait,
  RuntimeCallSyncAll,
  RuntimeCallSyncImages,
  RuntimeCallSyncMemory,
  RuntimeCallSyncTeam,
  RuntimeCallLock,
  RuntimeCallUnlock,
  RuntimeCallSIZE = RuntimeCallUnlock - RuntimeCallFailImage + 1
};

using RuntimeCallArguments = CallArguments;
}

#endif  // FORTRAN_FIR_COMMON_H_
