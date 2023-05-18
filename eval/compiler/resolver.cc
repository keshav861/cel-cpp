#include "eval/compiler/resolver.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "base/values/enum_value.h"
#include "eval/internal/interop.h"
#include "eval/public/cel_type_registry.h"
#include "runtime/function_registry.h"

namespace google::api::expr::runtime {

using ::cel::EnumType;
using ::cel::Handle;
using ::cel::MemoryManager;
using ::cel::Value;
using ::cel::interop_internal::CreateIntValue;

Resolver::Resolver(absl::string_view container,
                   const cel::FunctionRegistry& function_registry,
                   const CelTypeRegistry* type_registry,
                   bool resolve_qualified_type_identifiers)
    : namespace_prefixes_(),
      enum_value_map_(),
      function_registry_(function_registry),
      type_registry_(type_registry),
      resolve_qualified_type_identifiers_(resolve_qualified_type_identifiers) {
  // The constructor for the registry determines the set of possible namespace
  // prefixes which may appear within the given expression container, and also
  // eagerly maps possible enum names to enum values.

  auto container_elements = absl::StrSplit(container, '.');
  std::string prefix = "";
  namespace_prefixes_.push_back(prefix);
  for (const auto& elem : container_elements) {
    // Tolerate trailing / leading '.'.
    if (elem.empty()) {
      continue;
    }
    absl::StrAppend(&prefix, elem, ".");
    namespace_prefixes_.insert(namespace_prefixes_.begin(), prefix);
  }

  for (const auto& prefix : namespace_prefixes_) {
    for (auto iter = type_registry->resolveable_enums().begin();
         iter != type_registry->resolveable_enums().end(); ++iter) {
      absl::string_view enum_name = iter->first;
      if (!absl::StartsWith(enum_name, prefix)) {
        continue;
      }

      auto remainder = absl::StripPrefix(enum_name, prefix);
      const Handle<EnumType>& enum_type = iter->second;

      absl::StatusOr<cel::UniqueRef<cel::EnumType::ConstantIterator>>
          enum_value_iter_or =
              enum_type->NewConstantIterator(MemoryManager::Global());

      // Errors are not expected from the implementation in the type registry,
      // but we need to swallow the error case to avoid compiler/lint warnings.
      if (!enum_value_iter_or.ok()) {
        continue;
      }
      auto enum_value_iter = *std::move(enum_value_iter_or);
      while (enum_value_iter->HasNext()) {
        absl::StatusOr<EnumType::Constant> constant = enum_value_iter->Next();
        if (!constant.ok()) {
          break;
        }
        // "prefixes" container is ascending-ordered. As such, we will be
        // assigning enum reference to the deepest available.
        // E.g. if both a.b.c.Name and a.b.Name are available, and
        // we try to reference "Name" with the scope of "a.b.c",
        // it will be resolved to "a.b.c.Name".
        auto key = absl::StrCat(remainder, !remainder.empty() ? "." : "",
                                constant->name);
        enum_value_map_[key] = CreateIntValue(constant->number);
      }
    }
  }
}

std::vector<std::string> Resolver::FullyQualifiedNames(absl::string_view name,
                                                       int64_t expr_id) const {
  // TODO(issues/105): refactor the reference resolution into this method.
  // and handle the case where this id is in the reference map as either a
  // function name or identifier name.
  std::vector<std::string> names;
  // Handle the case where the name contains a leading '.' indicating it is
  // already fully-qualified.
  if (absl::StartsWith(name, ".")) {
    std::string fully_qualified_name = std::string(name.substr(1));
    names.push_back(fully_qualified_name);
    return names;
  }

  // namespace prefixes is guaranteed to contain at least empty string, so this
  // function will always produce at least one result.
  for (const auto& prefix : namespace_prefixes_) {
    std::string fully_qualified_name = absl::StrCat(prefix, name);
    names.push_back(fully_qualified_name);
  }
  return names;
}

Handle<Value> Resolver::FindConstant(absl::string_view name,
                                     int64_t expr_id) const {
  auto names = FullyQualifiedNames(name, expr_id);
  for (const auto& name : names) {
    // Attempt to resolve the fully qualified name to a known enum.
    auto enum_entry = enum_value_map_.find(name);
    if (enum_entry != enum_value_map_.end()) {
      return enum_entry->second;
    }
    // Conditionally resolve fully qualified names as type values if the option
    // to do so is configured in the expression builder. If the type name is
    // not qualified, then it too may be returned as a constant value.
    if (resolve_qualified_type_identifiers_ || !absl::StrContains(name, ".")) {
      auto type_value = type_registry_->FindType(name);
      if (type_value) {
        return type_value;
      }
    }
  }

  return Handle<Value>();
}

std::vector<cel::FunctionOverloadReference> Resolver::FindOverloads(
    absl::string_view name, bool receiver_style,
    const std::vector<cel::Kind>& types, int64_t expr_id) const {
  // Resolve the fully qualified names and then search the function registry
  // for possible matches.
  std::vector<cel::FunctionOverloadReference> funcs;
  auto names = FullyQualifiedNames(name, expr_id);
  for (auto it = names.begin(); it != names.end(); it++) {
    // Only one set of overloads is returned along the namespace hierarchy as
    // the function name resolution follows the same behavior as variable name
    // resolution, meaning the most specific definition wins. This is different
    // from how C++ namespaces work, as they will accumulate the overload set
    // over the namespace hierarchy.
    funcs = function_registry_.FindStaticOverloads(*it, receiver_style, types);
    if (!funcs.empty()) {
      return funcs;
    }
  }
  return funcs;
}

std::vector<cel::FunctionRegistry::LazyOverload> Resolver::FindLazyOverloads(
    absl::string_view name, bool receiver_style,
    const std::vector<cel::Kind>& types, int64_t expr_id) const {
  // Resolve the fully qualified names and then search the function registry
  // for possible matches.
  std::vector<cel::FunctionRegistry::LazyOverload> funcs;
  auto names = FullyQualifiedNames(name, expr_id);
  for (const auto& name : names) {
    funcs = function_registry_.FindLazyOverloads(name, receiver_style, types);
    if (!funcs.empty()) {
      return funcs;
    }
  }
  return funcs;
}

absl::optional<LegacyTypeAdapter> Resolver::FindTypeAdapter(
    absl::string_view name, int64_t expr_id) const {
  // Resolve the fully qualified names and then defer to the type registry
  // for possible matches.
  auto names = FullyQualifiedNames(name, expr_id);
  for (const auto& name : names) {
    auto maybe_adapter = type_registry_->FindTypeAdapter(name);
    if (maybe_adapter.has_value()) {
      return maybe_adapter;
    }
  }
  return absl::nullopt;
}

}  // namespace google::api::expr::runtime
