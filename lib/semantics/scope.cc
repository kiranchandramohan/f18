#include "scope.h"
#include "symbol.h"
#include <memory>

namespace Fortran::semantics {

const Scope Scope::systemScope{Scope::systemScope, Scope::Kind::System};
Scope Scope::globalScope{Scope::systemScope, Scope::Kind::Global};

Scope &Scope::MakeScope(Kind kind) {
  children_.emplace_back(*this, kind);
  return children_.back();
}

std::ostream &operator<<(std::ostream &os, const Scope &scope) {
  os << Scope::EnumToString(scope.kind()) << " scope: " << scope.children_.size()
     << " children\n";
  for (const auto &sym : scope.symbols_) {
    os << "  " << sym.second << "\n";
  }
  return os;
}

}  // namespace Fortran::semantics