////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_UTILS_TRANSACTION_CONTEXT_H
#define ARANGOD_UTILS_TRANSACTION_CONTEXT_H 1

#include "Basics/Common.h"
#include "Utils/CollectionNameResolver.h"

#include <velocypack/Options.h>
#include <velocypack/velocypack-aliases.h>

struct TRI_transaction_s;
struct TRI_vocbase_t;

namespace arangodb {

class TransactionContext {
 public:
  TransactionContext(TransactionContext const&) = delete;
  TransactionContext& operator=(TransactionContext const&) = delete;

 protected:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief create the context
  //////////////////////////////////////////////////////////////////////////////

  TransactionContext(TRI_vocbase_t* vocbase) : _vocbase(vocbase), _resolver(nullptr) {}

 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief destroy the context
  //////////////////////////////////////////////////////////////////////////////

  virtual ~TransactionContext() = default;

 public:
  
  //////////////////////////////////////////////////////////////////////////////
  /// @brief return the vocbase
  //////////////////////////////////////////////////////////////////////////////

  TRI_vocbase_t* vocbase() const { return _vocbase; }

  //////////////////////////////////////////////////////////////////////////////
  /// @brief return the resolver
  //////////////////////////////////////////////////////////////////////////////

  virtual CollectionNameResolver const* getResolver() const = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief get parent transaction (if any)
  //////////////////////////////////////////////////////////////////////////////

  virtual struct TRI_transaction_s* getParentTransaction() const = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not the transaction is embeddable
  //////////////////////////////////////////////////////////////////////////////

  virtual bool isEmbeddable() const = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief register the transaction in the context
  //////////////////////////////////////////////////////////////////////////////

  virtual int registerTransaction(struct TRI_transaction_s*) = 0;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief unregister the transaction from the context
  //////////////////////////////////////////////////////////////////////////////

  virtual void unregisterTransaction() = 0;
 
 protected:
  
  TRI_vocbase_t* _vocbase; 
  
  arangodb::CollectionNameResolver const* _resolver;
};
}

#endif
