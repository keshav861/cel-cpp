// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "eval/compiler/qualified_reference_resolver.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "base/ast.h"
#include "base/ast_internal/ast_impl.h"
#include "base/ast_internal/expr.h"
#include "base/builtins.h"
#include "base/kind.h"
#include "eval/compiler/flat_expr_builder_extensions.h"
#include "eval/compiler/resolver.h"
#include "eval/eval/expression_build_warning.h"
#include "eval/public/ast_rewrite_native.h"
#include "eval/public/source_position_native.h"

namespace google::api::expr::runtime {

namespace {

using ::cel::ast_internal::Expr;
using ::cel::ast_internal::Reference;
using ::cel::ast_internal::SourcePosition;

// Determines if function is implemented with custom evaluation step instead of
// registered.
bool IsSpecialFunction(absl::string_view function_name) {
  return function_name == cel::builtin::kAnd ||
         function_name == cel::builtin::kOr ||
         function_name == cel::builtin::kIndex ||
         function_name == cel::builtin::kTernary;
}

bool OverloadExists(const Resolver& resolver, absl::string_view name,
                    const std::vector<cel::Kind>& arguments_matcher,
                    bool receiver_style = false) {
  return !resolver.FindOverloads(name, receiver_style, arguments_matcher)
              .empty() ||
         !resolver.FindLazyOverloads(name, receiver_style, arguments_matcher)
              .empty();
}

// Return the qualified name of the most qualified matching overload, or
// nullopt if no matches are found.
absl::optional<std::string> BestOverloadMatch(const Resolver& resolver,
                                              absl::string_view base_name,
                                              int argument_count) {
  if (IsSpecialFunction(base_name)) {
    return std::string(base_name);
  }
  auto arguments_matcher = ArgumentsMatcher(argument_count);
  // Check from most qualified to least qualified for a matching overload.
  auto names = resolver.FullyQualifiedNames(base_name);
  for (auto name = names.begin(); name != names.end(); ++name) {
    if (OverloadExists(resolver, *name, arguments_matcher)) {
      if (base_name[0] == '.') {
        // Preserve leading '.' to prevent re-resolving at plan time.
        return std::string(base_name);
      }
      return *name;
    }
  }
  return absl::nullopt;
}

// Rewriter visitor for resolving references.
//
// On previsit pass, replace (possibly qualified) identifier branches with the
// canonical name in the reference map (most qualified references considered
// first).
//
// On post visit pass, update function calls to determine whether the function
// target is a namespace for the function or a receiver for the call.
class ReferenceResolver : public cel::ast_internal::AstRewriterBase {
 public:
  ReferenceResolver(
      const absl::flat_hash_map<int64_t, Reference>& reference_map,
      const Resolver& resolver, BuilderWarnings& warnings)
      : reference_map_(reference_map),
        resolver_(resolver),
        warnings_(warnings) {}

  // Attempt to resolve references in expr. Return true if part of the
  // expression was rewritten.
  // TODO(issues/95): If possible, it would be nice to write a general utility
  // for running the preprocess steps when traversing the AST instead of having
  // one pass per transform.
  bool PreVisitRewrite(Expr* expr, const SourcePosition* position) override {
    const Reference* reference = GetReferenceForId(expr->id());

    // Fold compile time constant (e.g. enum values)
    if (reference != nullptr && reference->has_value()) {
      if (reference->value().has_int64_value()) {
        // Replace enum idents with const reference value.
        expr->mutable_const_expr().set_int64_value(
            reference->value().int64_value());
        return true;
      } else {
        // No update if the constant reference isn't an int (an enum value).
        return false;
      }
    }

    if (reference != nullptr) {
      if (expr->has_ident_expr()) {
        return MaybeUpdateIdentNode(expr, *reference);
      } else if (expr->has_select_expr()) {
        return MaybeUpdateSelectNode(expr, *reference);
      } else {
        // Call nodes are updated on post visit so they will see any select
        // path rewrites.
        return false;
      }
    }
    return false;
  }

  bool PostVisitRewrite(Expr* expr,
                        const SourcePosition* source_position) override {
    const Reference* reference = GetReferenceForId(expr->id());
    if (expr->has_call_expr()) {
      return MaybeUpdateCallNode(expr, reference);
    }
    return false;
  }

 private:
  // Attempt to update a function call node. This disambiguates
  // receiver call verses namespaced names in parse if possible.
  //
  // TODO(issues/95): This duplicates some of the overload matching behavior
  // for parsed expressions. We should refactor to consolidate the code.
  bool MaybeUpdateCallNode(Expr* out, const Reference* reference) {
    auto& call_expr = out->mutable_call_expr();
    if (reference != nullptr && reference->overload_id().empty()) {
      warnings_
          .AddWarning(absl::InvalidArgumentError(
              absl::StrCat("Reference map doesn't provide overloads for ",
                           out->call_expr().function())))
          .IgnoreError();
    }
    bool receiver_style = call_expr.has_target();
    int arg_num = call_expr.args().size();
    if (receiver_style) {
      auto maybe_namespace = ToNamespace(call_expr.target());
      if (maybe_namespace.has_value()) {
        std::string resolved_name =
            absl::StrCat(*maybe_namespace, ".", call_expr.function());
        auto resolved_function =
            BestOverloadMatch(resolver_, resolved_name, arg_num);
        if (resolved_function.has_value()) {
          call_expr.set_function(*resolved_function);
          call_expr.set_target(nullptr);
          return true;
        }
      }
    } else {
      // Not a receiver style function call. Check to see if it is a namespaced
      // function using a shorthand inside the expression container.
      auto maybe_resolved_function =
          BestOverloadMatch(resolver_, call_expr.function(), arg_num);
      if (!maybe_resolved_function.has_value()) {
        warnings_
            .AddWarning(absl::InvalidArgumentError(
                absl::StrCat("No overload found in reference resolve step for ",
                             call_expr.function())))
            .IgnoreError();
      } else if (maybe_resolved_function.value() != call_expr.function()) {
        call_expr.set_function(maybe_resolved_function.value());
        return true;
      }
    }
    // For parity, if we didn't rewrite the receiver call style function,
    // check that an overload is provided in the builder.
    if (call_expr.has_target() &&
        !OverloadExists(resolver_, call_expr.function(),
                        ArgumentsMatcher(arg_num + 1),
                        /* receiver_style= */ true)) {
      warnings_
          .AddWarning(absl::InvalidArgumentError(
              absl::StrCat("No overload found in reference resolve step for ",
                           call_expr.function())))
          .IgnoreError();
    }
    return false;
  }

  // Attempt to resolve a select node. If reference is valid,
  // replace the select node with the fully qualified ident node.
  bool MaybeUpdateSelectNode(Expr* out, const Reference& reference) {
    if (out->select_expr().test_only()) {
      warnings_
          .AddWarning(
              absl::InvalidArgumentError("Reference map points to a presence "
                                         "test -- has(container.attr)"))
          .IgnoreError();
    } else if (!reference.name().empty()) {
      out->mutable_ident_expr().set_name(reference.name());
      rewritten_reference_.insert(out->id());
      return true;
    }
    return false;
  }

  // Attempt to resolve an ident node. If reference is valid,
  // replace the node with the fully qualified ident node.
  bool MaybeUpdateIdentNode(Expr* out, const Reference& reference) {
    if (!reference.name().empty() &&
        reference.name() != out->ident_expr().name()) {
      out->mutable_ident_expr().set_name(reference.name());
      rewritten_reference_.insert(out->id());
      return true;
    }
    return false;
  }

  // Convert a select expr sub tree into a namespace name if possible.
  // If any operand of the top element is a not a select or an ident node,
  // return nullopt.
  absl::optional<std::string> ToNamespace(const Expr& expr) {
    absl::optional<std::string> maybe_parent_namespace;
    if (rewritten_reference_.find(expr.id()) != rewritten_reference_.end()) {
      // The target expr matches a reference (resolved to an ident decl).
      // This should not be treated as a function qualifier.
      return absl::nullopt;
    }
    if (expr.has_ident_expr()) {
      return expr.ident_expr().name();
    } else if (expr.has_select_expr()) {
      if (expr.select_expr().test_only()) {
        return absl::nullopt;
      }
      maybe_parent_namespace = ToNamespace(expr.select_expr().operand());
      if (!maybe_parent_namespace.has_value()) {
        return absl::nullopt;
      }
      return absl::StrCat(*maybe_parent_namespace, ".",
                          expr.select_expr().field());
    } else {
      return absl::nullopt;
    }
  }

  // Find a reference for the given expr id.
  //
  // Returns nullptr if no reference is available.
  const Reference* GetReferenceForId(int64_t expr_id) {
    auto iter = reference_map_.find(expr_id);
    if (iter == reference_map_.end()) {
      return nullptr;
    }
    if (expr_id == 0) {
      warnings_
          .AddWarning(absl::InvalidArgumentError(
              "reference map entries for expression id 0 are not supported"))
          .IgnoreError();
      return nullptr;
    }
    return &iter->second;
  }

  const absl::flat_hash_map<int64_t, Reference>& reference_map_;
  const Resolver& resolver_;
  BuilderWarnings& warnings_;
  absl::flat_hash_set<int64_t> rewritten_reference_;
};

class ReferenceResolverExtension : public AstTransform {
 public:
  explicit ReferenceResolverExtension(ReferenceResolverOption opt)
      : opt_(opt) {}
  absl::Status UpdateAst(PlannerContext& context,
                         cel::ast_internal::AstImpl& ast) const override {
    if (opt_ == ReferenceResolverOption::kCheckedOnly &&
        ast.reference_map().empty()) {
      return absl::OkStatus();
    }
    return ResolveReferences(context.resolver(), context.builder_warnings(),
                             ast)
        .status();
  }

 private:
  ReferenceResolverOption opt_;
};

}  // namespace

absl::StatusOr<bool> ResolveReferences(const Resolver& resolver,
                                       BuilderWarnings& warnings,
                                       cel::ast_internal::AstImpl& ast) {
  ReferenceResolver ref_resolver(ast.reference_map(), resolver, warnings);

  // Rewriting interface doesn't support failing mid traverse propagate first
  // error encountered if fail fast enabled.
  bool was_rewritten = cel::ast_internal::AstRewrite(
      &ast.root_expr(), &ast.source_info(), &ref_resolver);
  if (warnings.fail_immediately() && !warnings.warnings().empty()) {
    return warnings.warnings().front();
  }
  return was_rewritten;
}

std::unique_ptr<AstTransform> NewReferenceResolverExtension(
    ReferenceResolverOption option) {
  return std::make_unique<ReferenceResolverExtension>(option);
}

}  // namespace google::api::expr::runtime
