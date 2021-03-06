// Copyright (c) 2018-2019, NVIDIA CORPORATION.  All rights reserved.
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

#include "mod-file.h"
#include "scope.h"
#include "semantics.h"
#include "symbol.h"
#include "../parser/parsing.h"
#include <algorithm>
#include <cerrno>
#include <fstream>
#include <ostream>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

namespace Fortran::semantics {

using namespace parser::literals;

// The extension used for module files.
static constexpr auto extension{".mod"};
// The initial characters of a file that identify it as a .mod file.
static constexpr auto magic{"!mod$ v1 sum:"};

static const SourceName *GetSubmoduleParent(const parser::Program &);
static std::string ModFilePath(
    const std::string &, const SourceName &, const std::string &);
static std::vector<const Symbol *> CollectSymbols(const Scope &);
static void PutEntity(std::ostream &, const Symbol &);
static void PutObjectEntity(std::ostream &, const Symbol &);
static void PutProcEntity(std::ostream &, const Symbol &);
static void PutPassName(std::ostream &, const std::optional<SourceName> &);
static void PutTypeParam(std::ostream &, const Symbol &);
static void PutEntity(std::ostream &, const Symbol &, std::function<void()>);
static void PutInit(std::ostream &, const MaybeExpr &);
static void PutInit(std::ostream &, const MaybeIntExpr &);
static void PutBound(std::ostream &, const Bound &);
static std::ostream &PutAttrs(std::ostream &, Attrs,
    const MaybeExpr & = std::nullopt, std::string before = ","s,
    std::string after = ""s);
static std::ostream &PutLower(std::ostream &, const Symbol &);
static std::ostream &PutLower(std::ostream &, const DeclTypeSpec &);
static std::ostream &PutLower(std::ostream &, const std::string &);
static bool WriteFile(const std::string &, std::string &&);
static bool FileContentsMatch(
    std::fstream &, const std::string &, const std::string &);
static std::string GetHeader(const std::string &);
static std::size_t GetFileSize(const std::string &);

bool ModFileWriter::WriteAll() {
  WriteAll(context_.globalScope());
  return !context_.AnyFatalError();
}

void ModFileWriter::WriteAll(const Scope &scope) {
  for (const auto &child : scope.children()) {
    WriteOne(child);
  }
}

void ModFileWriter::WriteOne(const Scope &scope) {
  if (scope.kind() == Scope::Kind::Module) {
    auto *symbol{scope.symbol()};
    if (!symbol->test(Symbol::Flag::ModFile)) {
      Write(*symbol);
    }
    WriteAll(scope);  // write out submodules
  }
}

// Write the module file for symbol, which must be a module or submodule.
void ModFileWriter::Write(const Symbol &symbol) {
  auto *ancestor{symbol.get<ModuleDetails>().ancestor()};
  auto ancestorName{ancestor ? ancestor->name().ToString() : ""s};
  auto path{
      ModFilePath(context_.moduleDirectory(), symbol.name(), ancestorName)};
  PutSymbols(*symbol.scope());
  if (!WriteFile(path, GetAsString(symbol))) {
    context_.Say(symbol.name(), "Error writing %s: %s"_err_en_US, path.c_str(),
        std::strerror(errno));
  }
}

// Return the entire body of the module file
// and clear saved uses, decls, and contains.
std::string ModFileWriter::GetAsString(const Symbol &symbol) {
  std::stringstream all;
  auto &details{symbol.get<ModuleDetails>()};
  if (!details.isSubmodule()) {
    PutLower(all << "module ", symbol);
  } else {
    auto *parent{details.parent()->symbol()};
    auto *ancestor{details.ancestor()->symbol()};
    PutLower(all << "submodule(", *ancestor);
    if (parent != ancestor) {
      PutLower(all << ':', *parent);
    }
    PutLower(all << ") ", symbol);
  }
  all << '\n' << uses_.str();
  uses_.str(""s);
  all << useExtraAttrs_.str();
  useExtraAttrs_.str(""s);
  all << decls_.str();
  decls_.str(""s);
  auto str{contains_.str()};
  contains_.str(""s);
  if (!str.empty()) {
    all << "contains\n" << str;
  }
  all << "end\n";
  return all.str();
}

// Put out the visible symbols from scope.
void ModFileWriter::PutSymbols(const Scope &scope) {
  std::stringstream typeBindings;  // stuff after CONTAINS in derived type
  for (const auto *symbol : CollectSymbols(scope)) {
    PutSymbol(typeBindings, symbol);
  }
  if (auto str{typeBindings.str()}; !str.empty()) {
    decls_ << "contains\n" << str;
  }
}

// Emit a symbol to decls_, except for bindings in a derived type (type-bound
// procedures, type-bound generics, final procedures) which go to typeBindings.
void ModFileWriter::PutSymbol(
    std::stringstream &typeBindings, const Symbol *symbol) {
  if (symbol == nullptr) {
    return;
  }
  std::visit(
      common::visitors{
          [&](const ModuleDetails &) { /* should be current module */ },
          [&](const DerivedTypeDetails &) { PutDerivedType(*symbol); },
          [&](const SubprogramDetails &) { PutSubprogram(*symbol); },
          [&](const GenericDetails &x) {
            PutGeneric(*symbol);
            PutSymbol(typeBindings, x.specific());
            PutSymbol(typeBindings, x.derivedType());
          },
          [&](const UseDetails &) { PutUse(*symbol); },
          [](const UseErrorDetails &) {},
          [&](const ProcBindingDetails &x) {
            bool deferred{symbol->attrs().test(Attr::DEFERRED)};
            typeBindings << "procedure";
            if (deferred) {
              PutLower(typeBindings << '(', x.symbol()) << ')';
            }
            PutPassName(typeBindings, x.passName());
            PutAttrs(typeBindings, symbol->attrs());
            PutLower(typeBindings << "::", *symbol);
            if (!deferred && x.symbol().name() != symbol->name()) {
              PutLower(typeBindings << "=>", x.symbol());
            }
            typeBindings << '\n';
          },
          [&](const GenericBindingDetails &x) {
            for (const auto *proc : x.specificProcs()) {
              PutLower(typeBindings << "generic::", *symbol);
              PutLower(typeBindings << "=>", *proc) << '\n';
            }
          },
          [&](const NamelistDetails &x) {
            PutLower(decls_ << "namelist/", *symbol);
            char sep{'/'};
            for (const auto *object : x.objects()) {
              PutLower(decls_ << sep, *object);
              sep = ',';
            }
            decls_ << '\n';
          },
          [&](const CommonBlockDetails &x) {
            PutLower(decls_ << "common/", *symbol);
            char sep = '/';
            for (const auto *object : x.objects()) {
              PutLower(decls_ << sep, *object);
              sep = ',';
            }
            decls_ << '\n';
            if (symbol->attrs().test(Attr::BIND_C)) {
              PutAttrs(decls_, symbol->attrs(), x.bindName(), ""s);
              PutLower(decls_ << "::/", *symbol) << "/\n";
            }
          },
          [&](const FinalProcDetails &) {
            PutLower(typeBindings << "final::", *symbol) << '\n';
          },
          [](const HostAssocDetails &) {},
          [](const MiscDetails &) {},
          [&](const auto &) { PutEntity(decls_, *symbol); },
      },
      symbol->details());
}

void ModFileWriter::PutDerivedType(const Symbol &typeSymbol) {
  auto &details{typeSymbol.get<DerivedTypeDetails>()};
  PutAttrs(decls_ << "type", typeSymbol.attrs());
  if (const DerivedTypeSpec * extends{typeSymbol.GetParentTypeSpec()}) {
    PutLower(decls_ << ",extends(", extends->typeSymbol()) << ')';
  }
  PutLower(decls_ << "::", typeSymbol);
  auto &typeScope{*typeSymbol.scope()};
  if (!details.paramNames().empty()) {
    bool first{true};
    decls_ << '(';
    for (const auto &name : details.paramNames()) {
      PutLower(first ? decls_ : decls_ << ',', name.ToString());
      first = false;
    }
    decls_ << ')';
  }
  decls_ << '\n';
  if (details.sequence()) {
    decls_ << "sequence\n";
  }
  PutSymbols(typeScope);
  decls_ << "end type\n";
}

void ModFileWriter::PutSubprogram(const Symbol &symbol) {
  auto attrs{symbol.attrs()};
  auto &details{symbol.get<SubprogramDetails>()};
  Attrs bindAttrs{};
  if (attrs.test(Attr::BIND_C)) {
    // bind(c) is a suffix, not prefix
    bindAttrs.set(Attr::BIND_C, true);
    attrs.set(Attr::BIND_C, false);
  }
  bool isInterface{details.isInterface()};
  std::ostream &os{isInterface ? decls_ : contains_};
  if (isInterface) {
    os << "interface\n";
  }
  PutAttrs(os, attrs, std::nullopt, ""s, " "s);
  os << (details.isFunction() ? "function " : "subroutine ");
  PutLower(os, symbol) << '(';
  int n = 0;
  for (const auto &dummy : details.dummyArgs()) {
    if (n++ > 0) os << ',';
    PutLower(os, *dummy);
  }
  os << ')';
  PutAttrs(os, bindAttrs, details.bindName(), " "s, ""s);
  if (details.isFunction()) {
    const Symbol &result{details.result()};
    if (result.name() != symbol.name()) {
      PutLower(os << " result(", result) << ')';
    }
    os << '\n';
    PutEntity(os, details.result());
  } else {
    os << '\n';
  }
  for (const auto &dummy : details.dummyArgs()) {
    PutEntity(os, *dummy);
  }
  os << "end\n";
  if (isInterface) {
    os << "end interface\n";
  }
}

void ModFileWriter::PutGeneric(const Symbol &symbol) {
  auto &details{symbol.get<GenericDetails>()};
  decls_ << "generic";
  PutAttrs(decls_, symbol.attrs()) << "::";
  if (details.kind() == GenericKind::DefinedOp) {
    PutLower(decls_ << "operator(", symbol) << ')';
  } else {
    PutLower(decls_, symbol);
  }
  decls_ << "=>";
  int n = 0;
  for (auto *specific : details.specificProcs()) {
    if (n++ > 0) decls_ << ',';
    PutLower(decls_, *specific);
  }
  decls_ << '\n';
}

void ModFileWriter::PutUse(const Symbol &symbol) {
  auto &details{symbol.get<UseDetails>()};
  auto &use{details.symbol()};
  PutLower(uses_ << "use ", details.module());
  PutLower(uses_ << ",only:", symbol);
  if (use.name() != symbol.name()) {
    PutLower(uses_ << "=>", use);
  }
  uses_ << '\n';
  PutUseExtraAttr(Attr::VOLATILE, symbol, use);
  PutUseExtraAttr(Attr::ASYNCHRONOUS, symbol, use);
}

// We have "USE local => use" in this module. If attr was added locally
// (i.e. on local but not on use), also write it out in the mod file.
void ModFileWriter::PutUseExtraAttr(
    Attr attr, const Symbol &local, const Symbol &use) {
  if (local.attrs().test(attr) && !use.attrs().test(attr)) {
    PutLower(useExtraAttrs_, AttrToString(attr)) << "::";
    PutLower(useExtraAttrs_, local) << '\n';
  }
}

// Collect the symbols of this scope sorted by their original order, not name.
// Namelists are an exception: they are sorted after other symbols.
std::vector<const Symbol *> CollectSymbols(const Scope &scope) {
  std::set<const Symbol *> symbols;  // to prevent duplicates
  std::vector<const Symbol *> sorted;
  sorted.reserve(scope.size());
  for (const auto &pair : scope) {
    auto *symbol{pair.second};
    if (!symbol->test(Symbol::Flag::ParentComp)) {
      if (symbols.insert(symbol).second) {
        sorted.push_back(symbol);
      }
    }
  }
  for (const auto &pair : scope.commonBlocks()) {
    auto *symbol{pair.second};
    if (symbols.insert(symbol).second) {
      sorted.push_back(symbol);
    }
  }
  std::sort(sorted.begin(), sorted.end(), [](const Symbol *x, const Symbol *y) {
    bool xIsNml{x->has<NamelistDetails>()};
    bool yIsNml{y->has<NamelistDetails>()};
    if (xIsNml != yIsNml) {
      return xIsNml < yIsNml;
    } else {
      return x->name().begin() < y->name().begin();
    }
  });
  return sorted;
}

void PutEntity(std::ostream &os, const Symbol &symbol) {
  std::visit(
      common::visitors{
          [&](const ObjectEntityDetails &) { PutObjectEntity(os, symbol); },
          [&](const ProcEntityDetails &) { PutProcEntity(os, symbol); },
          [&](const TypeParamDetails &) { PutTypeParam(os, symbol); },
          [&](const auto &) {
            common::die("PutEntity: unexpected details: %s",
                DetailsToString(symbol.details()).c_str());
          },
      },
      symbol.details());
  os << '\n';
}

void PutShapeSpec(std::ostream &os, const ShapeSpec &x) {
  if (x.ubound().isAssumed()) {
    CHECK(x.ubound().isAssumed());
    os << "..";
  } else {
    if (!x.lbound().isDeferred()) {
      PutBound(os, x.lbound());
    }
    os << ':';
    if (!x.ubound().isDeferred()) {
      PutBound(os, x.ubound());
    }
  }
}
void PutShape(std::ostream &os, const ArraySpec &shape) {
  if (!shape.empty()) {
    os << '(';
    bool first{true};
    for (const auto &shapeSpec : shape) {
      if (first) {
        first = false;
      } else {
        os << ',';
      }
      PutShapeSpec(os, shapeSpec);
    }
    os << ')';
  }
}

void PutObjectEntity(std::ostream &os, const Symbol &symbol) {
  auto &details{symbol.get<ObjectEntityDetails>()};
  PutEntity(os, symbol, [&]() {
    auto *type{symbol.GetType()};
    CHECK(type);
    PutLower(os, *type);
  });
  PutShape(os, details.shape());
  PutInit(os, details.init());
}

void PutProcEntity(std::ostream &os, const Symbol &symbol) {
  const auto &details{symbol.get<ProcEntityDetails>()};
  const ProcInterface &interface{details.interface()};
  PutEntity(os, symbol, [&]() {
    os << "procedure(";
    if (interface.symbol()) {
      PutLower(os, *interface.symbol());
    } else if (interface.type()) {
      PutLower(os, *interface.type());
    }
    os << ')';
    PutPassName(os, details.passName());
  });
}

void PutPassName(std::ostream &os, const std::optional<SourceName> &passName) {
  if (passName) {
    PutLower(os << ",pass(", passName->ToString()) << ')';
  }
}

void PutTypeParam(std::ostream &os, const Symbol &symbol) {
  auto &details{symbol.get<TypeParamDetails>()};
  PutEntity(os, symbol, [&]() {
    auto *type{symbol.GetType()};
    CHECK(type);
    PutLower(os, *type);
    PutLower(os << ',', common::EnumToString(details.attr()));
  });
  PutInit(os, details.init());
}

void PutInit(std::ostream &os, const MaybeExpr &init) {
  if (init) {
    init->AsFortran(os << '=');
  }
}

void PutInit(std::ostream &os, const MaybeIntExpr &init) {
  if (init) {
    init->AsFortran(os << '=');
  }
}

void PutBound(std::ostream &os, const Bound &x) {
  if (x.isAssumed()) {
    os << '*';
  } else if (x.isDeferred()) {
    os << ':';
  } else {
    x.GetExplicit()->AsFortran(os);
  }
}

// Write an entity (object or procedure) declaration.
// writeType is called to write out the type.
void PutEntity(
    std::ostream &os, const Symbol &symbol, std::function<void()> writeType) {
  writeType();
  MaybeExpr bindName;
  std::visit(
      common::visitors{
          [&](const SubprogramDetails &x) { bindName = x.bindName(); },
          [&](const ObjectEntityDetails &x) { bindName = x.bindName(); },
          [&](const ProcEntityDetails &x) { bindName = x.bindName(); },
          [&](const auto &) {},
      },
      symbol.details());
  PutAttrs(os, symbol.attrs(), bindName);
  PutLower(os << "::", symbol);
}

// Put out each attribute to os, surrounded by `before` and `after` and
// mapped to lower case.
std::ostream &PutAttrs(std::ostream &os, Attrs attrs, const MaybeExpr &bindName,
    std::string before, std::string after) {
  attrs.set(Attr::PUBLIC, false);  // no need to write PUBLIC
  attrs.set(Attr::EXTERNAL, false);  // no need to write EXTERNAL
  if (bindName) {
    bindName->AsFortran(os << before << "bind(c, name=") << ')' << after;
    attrs.set(Attr::BIND_C, false);
  }
  for (std::size_t i{0}; i < Attr_enumSize; ++i) {
    Attr attr{static_cast<Attr>(i)};
    if (attrs.test(attr)) {
      PutLower(os << before, AttrToString(attr)) << after;
    }
  }
  return os;
}

std::ostream &PutLower(std::ostream &os, const Symbol &symbol) {
  return PutLower(os, symbol.name().ToString());
}

std::ostream &PutLower(std::ostream &os, const DeclTypeSpec &type) {
  std::stringstream s;
  s << type;
  return PutLower(os, s.str());
}

std::ostream &PutLower(std::ostream &os, const std::string &str) {
  for (char c : str) {
    os << parser::ToLowerCaseLetter(c);
  }
  return os;
}

// Write the module file at path, prepending header. Return false on error.
static bool WriteFile(const std::string &path, std::string &&contents) {
  std::fstream stream;
  auto header{GetHeader(contents)};
  auto size{GetFileSize(path)};
  if (size == header.size() + 1 + contents.size()) {
    // file exists and has the right size, check the contents
    stream.open(path, std::ios::in | std::ios::out);
    if (FileContentsMatch(stream, header, contents)) {
      return true;
    }
    stream.seekp(0);
  } else {
    stream.open(path, std::ios::out);
  }
  stream << header << '\n' << contents;
  stream.close();
  return !stream.fail();
}

// Return true if the stream matches what we would write for the mod file.
static bool FileContentsMatch(std::fstream &stream, const std::string &header,
    const std::string &contents) {
  char c;
  for (std::size_t i{0}; i < header.size(); ++i) {
    if (!stream.get(c) || c != header[i]) {
      return false;
    }
  }
  if (!stream.get(c) || c != '\n') {
    return false;
  }
  for (std::size_t i{0}; i < contents.size(); ++i) {
    if (!stream.get(c) || c != contents[i]) {
      return false;
    }
  }
  return !stream.get(c);
}

// Compute a simple hash of the contents of a module file and
// return it as a string of hex digits.
// This uses the Fowler-Noll-Vo hash function.
template<typename Iter> static std::string CheckSum(Iter begin, Iter end) {
  std::uint64_t hash{0xcbf29ce484222325ull};
  for (auto it{begin}; it != end; ++it) {
    char c{*it};
    hash ^= c & 0xff;
    hash *= 0x100000001b3;
  }
  static const char *digits = "0123456789abcdef";
  std::string result(16, '0');
  for (size_t i{16}; hash != 0; hash >>= 4) {
    result[--i] = digits[hash & 0xf];
  }
  return result;
}

static bool VerifyHeader(const std::string &path) {
  std::fstream stream{path};
  std::string header;
  std::getline(stream, header);
  auto magicLen{strlen(magic)};
  if (header.compare(0, magicLen, magic) != 0) {
    return false;
  }
  std::string expectSum{header.substr(magicLen, 16)};
  std::string actualSum{CheckSum(std::istreambuf_iterator<char>(stream),
      std::istreambuf_iterator<char>())};
  return expectSum == actualSum;
}

static std::string GetHeader(const std::string &all) {
  std::stringstream ss;
  ss << magic << CheckSum(all.begin(), all.end());
  return ss.str();
}

static std::size_t GetFileSize(const std::string &path) {
  struct stat statbuf;
  if (stat(path.c_str(), &statbuf) == 0) {
    return static_cast<std::size_t>(statbuf.st_size);
  } else {
    return 0;
  }
}

Scope *ModFileReader::Read(const SourceName &name, Scope *ancestor) {
  std::string ancestorName;  // empty for module
  if (ancestor) {
    if (auto *scope{ancestor->FindSubmodule(name)}) {
      return scope;
    }
    ancestorName = ancestor->name().ToString();
  } else {
    auto it{context_.globalScope().find(name)};
    if (it != context_.globalScope().end()) {
      return it->second->scope();
    }
  }
  auto path{FindModFile(name, ancestorName)};
  if (!path.has_value()) {
    return nullptr;
  }
  // TODO: We are reading the file once to verify the checksum and then again
  // to parse. Do it only reading the file once.
  if (!VerifyHeader(*path)) {
    context_.Say(name,
        "Module file for '%s' has invalid checksum: %s"_err_en_US,
        name.ToString().data(), path->data());
    return nullptr;
  }
  // TODO: Construct parsing with an AllSources reference to share provenance
  parser::Parsing parsing;
  parser::Options options;
  options.isModuleFile = true;
  parsing.Prescan(*path, options);
  parsing.Parse(nullptr);
  auto &parseTree{parsing.parseTree()};
  if (!parsing.messages().empty() || !parsing.consumedWholeFile() ||
      !parseTree.has_value()) {
    context_.Say(name, "Module file for '%s' is corrupt: %s"_err_en_US,
        name.ToString().data(), path->data());
    return nullptr;
  }
  Scope *parentScope;  // the scope this module/submodule goes into
  if (!ancestor) {
    parentScope = &context_.globalScope();
  } else if (auto *parent{GetSubmoduleParent(*parseTree)}) {
    parentScope = Read(*parent, ancestor);
  } else {
    parentScope = ancestor;
  }
  // TODO: Check that default kinds of intrinsic types match?
  ResolveNames(context_, *parseTree);
  const auto &it{parentScope->find(name)};
  if (it == parentScope->end()) {
    return nullptr;
  }
  auto &modSymbol{*it->second};
  // TODO: Preserve the CookedSource rather than acquiring its string.
  modSymbol.scope()->set_chars(std::string{parsing.cooked().AcquireData()});
  modSymbol.set(Symbol::Flag::ModFile);
  return modSymbol.scope();
}

std::optional<std::string> ModFileReader::FindModFile(
    const SourceName &name, const std::string &ancestor) {
  parser::Messages attachments;
  for (auto &dir : context_.searchDirectories()) {
    std::string path{ModFilePath(dir, name, ancestor)};
    std::ifstream ifstream{path};
    if (!ifstream.good()) {
      attachments.Say(name, "%s: %s"_en_US, path.data(), std::strerror(errno));
    } else {
      std::string line;
      std::getline(ifstream, line);
      if (line.compare(0, strlen(magic), magic) == 0) {
        return path;
      }
      attachments.Say(name, "%s: Not a valid module file"_en_US, path.data());
    }
  }
  auto error{parser::Message{name,
      ancestor.empty()
          ? "Cannot find module file for '%s'"_err_en_US
          : "Cannot find module file for submodule '%s' of module '%s'"_err_en_US,
      name.ToString().data(), ancestor.data()}};
  attachments.AttachTo(error);
  context_.Say(error);
  return std::nullopt;
}

// program was read from a .mod file for a submodule; return the name of the
// submodule's parent submodule, nullptr if none.
static const SourceName *GetSubmoduleParent(const parser::Program &program) {
  CHECK(program.v.size() == 1);
  auto &unit{program.v.front()};
  auto &submod{std::get<common::Indirection<parser::Submodule>>(unit.u)};
  auto &stmt{
      std::get<parser::Statement<parser::SubmoduleStmt>>(submod.value().t)};
  auto &parentId{std::get<parser::ParentIdentifier>(stmt.statement.t)};
  if (auto &parent{std::get<std::optional<parser::Name>>(parentId.t)}) {
    return &parent->source;
  } else {
    return nullptr;
  }
}

// Construct the path to a module file. ancestorName not empty means submodule.
static std::string ModFilePath(const std::string &dir, const SourceName &name,
    const std::string &ancestorName) {
  std::stringstream path;
  if (dir != "."s) {
    path << dir << '/';
  }
  if (!ancestorName.empty()) {
    PutLower(path, ancestorName) << '-';
  }
  PutLower(path, name.ToString()) << extension;
  return path.str();
}
}
