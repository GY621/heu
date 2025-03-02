// Copyright 2022 Ant Group Co., Ltd.
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

#pragma once
#include <variant>

#include "heu/library/phe/base/plaintext.h"
#include "heu/library/phe/base/schema.h"
#include "heu/library/phe/base/serializable_types.h"
#include "heu/library/phe/base/variant_helper.h"

namespace heu::lib::phe {

typedef std::variant<HE_NAMESPACE_LIST(Decryptor)> DecryptorType;

class Decryptor {
 public:
  explicit Decryptor(SchemaType schema_type, DecryptorType decryptor_ptr)
      : schema_type_(schema_type), decryptor_ptr_(std::move(decryptor_ptr)) {}

  void Decrypt(const Ciphertext& ct, Plaintext* out) const;
  [[nodiscard]] Plaintext Decrypt(const Ciphertext& ct) const;

  SchemaType GetSchemaType() const;

 protected:
  SchemaType schema_type_;
  DecryptorType decryptor_ptr_;
};

}  // namespace heu::lib::phe
