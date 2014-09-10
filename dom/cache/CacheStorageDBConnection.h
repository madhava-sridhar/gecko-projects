/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_CacheStorageDBConnection_h
#define mozilla_dom_cache_CacheStorageDBConnection_h

#include "mozilla/dom/CacheTypes.h"
#include "nsCOMPtr.h"
#include "nsISupportsImpl.h"

class mozIStorageConnection;
struct nsID;

namespace mozilla {
namespace dom {

class CacheStorageDBListener;

class CacheStorageDBConnection MOZ_FINAL
{
public:
  CacheStorageDBConnection(CacheStorageDBListener* aListener,
                           cache::Namespace aNamespace,
                           const nsACString& aOrigin,
                           const nsACString& aBaseDomain);

  void ClearListener();

  void Get(cache::RequestId aRequestId, const nsAString& aKey);
  void Has(cache::RequestId aRequestId, const nsAString& aKey);
  void Put(cache::RequestId aRequestId, const nsAString& aKey,
           const nsID& aCacheId);
  void Delete(cache::RequestId aRequestId, const nsAString& aKey);
  void Keys(cache::RequestId aRequestId);

private:
  class OpenRunnable;
  class GetRunnable;
  class HasRunnable;
  class PutRunnable;
  class DeleteRunnable;
  class KeysRunnable;

  ~CacheStorageDBConnection();
  void OnGetComplete(cache::RequestId aRequestId, nsresult aRv, nsID* aCacheId);
  void OnHasComplete(cache::RequestId aRequestId, nsresult aRv, bool aSuccess);
  void OnPutComplete(cache::RequestId aRequestId, nsresult aRv, bool aSuccess);
  void OnDeleteComplete(cache::RequestId aRequestId, nsresult aRv, bool aSuccess);
  void OnKeysComplete(cache::RequestId aRequestId, nsresult aRv,
                      const nsTArray<nsString>& aKeys);

  CacheStorageDBListener* mListener;
  const cache::Namespace mNamespace;
  const nsCString mOrigin;
  const nsCString mBaseDomain;

public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(CacheStorageDBConnection)
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_CacheStorageDBConnection_h
