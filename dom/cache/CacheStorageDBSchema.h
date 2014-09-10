/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_CacheStorageDBSchema_h
#define mozilla_dom_cache_CacheStorageDBSchema_h

#include "mozilla/dom/CacheTypes.h"

class mozIStorageConnection;
struct nsID;
template<class T> class nsTArray;

namespace mozilla {
namespace dom {

class CacheStorageDBSchema MOZ_FINAL
{
public:
  static nsresult Create(mozIStorageConnection* aConn);

  static nsresult
  Get(mozIStorageConnection* aConn, cache::Namespace aNamespace,
      const nsAString& aKey, bool* aSuccessOut, nsID* aCacheIdOut);

  static nsresult
  Has(mozIStorageConnection* aConn, cache::Namespace aNamespace,
      const nsAString& aKey, bool* aSuccessOut);

  static nsresult
  Put(mozIStorageConnection* aConn, cache::Namespace aNamespace,
      const nsAString& aKey, const nsID& aCacheId, bool* aSuccessOut);

  static nsresult
  Delete(mozIStorageConnection* aConn, cache::Namespace aNamespace,
         const nsAString& aKey, bool* aSuccessOut);

  static nsresult
  Keys(mozIStorageConnection* aConn, cache::Namespace aNamespace,
       nsTArray<nsString>& aKeysOut);

private:
  CacheStorageDBSchema() MOZ_DELETE;
  ~CacheStorageDBSchema() MOZ_DELETE;

  static const int32_t kLatestSchemaVersion = 1;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_CacheStorageDBSchema_h
