// Copyright 2021 Google LLC
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

#include "internal/names.h"

#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "internal/lexis.h"

namespace cel::internal {

bool IsValidRelativeName(absl::string_view name) {
  if (name.empty()) {
    return false;
  }
  for (const auto& id : absl::StrSplit(name, '.')) {
    if (!LexisIsIdentifier(id)) {
      return false;
    }
  }
  return true;
}

}  // namespace cel::internal
