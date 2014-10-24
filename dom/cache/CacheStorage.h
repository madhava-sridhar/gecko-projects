/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_CacheStorage_h
#define mozilla_dom_cache_CacheStorage_h

#include "mozilla/dom/CacheBinding.h"
#include "mozilla/dom/cache/CacheStorageChildListener.h"
#include "mozilla/dom/cache/Types.h"
#include "nsAutoPtr.h"
#include "nsCOMPtr.h"
#include "nsISupportsImpl.h"
#include "nsTArray.h"
#include "nsWrapperCache.h"
#include "nsIIPCBackgroundChildCreateCallback.h"

class nsIGlobalObject;

namespace mozilla {

class ErrorResult;

namespace ipc {
  class IProtocol;
}

namespace dom {

class Promise;
//struct QueryParams;
//class RequestOrScalarValueString;

namespace cache {

class CacheStorageChild;
class PCacheRequest;

class CacheStorage MOZ_FINAL : public nsIIPCBackgroundChildCreateCallback
                             , public nsWrapperCache
                             , public CacheStorageChildListener
{
  typedef mozilla::ipc::PBackgroundChild PBackgroundChild;

public:
  CacheStorage(Namespace aNamespace, nsISupports* aOwner,
               nsIGlobalObject* aGlobal, const nsACString& aOrigin,
               const nsACString& aBaseDomain);

  // webidl interface methods
  already_AddRefed<Promise> Match(const RequestOrScalarValueString& aRequest,
                                  const QueryParams& aParams, ErrorResult& aRv);
  already_AddRefed<Promise> Has(const nsAString& aKey, ErrorResult& aRv);
  already_AddRefed<Promise> Open(const nsAString& aKey, ErrorResult& aRv);
  already_AddRefed<Promise> Delete(const nsAString& aKey, ErrorResult& aRv);
  already_AddRefed<Promise> Keys(ErrorResult& aRv);

  // binding methods
  static bool PrefEnabled(JSContext* aCx, JSObject* aObj);

  virtual nsISupports* GetParentObject() const;
  virtual JSObject* WrapObject(JSContext* aContext) MOZ_OVERRIDE;

  // nsIIPCbackgroundChildCreateCallback methods
  virtual void ActorCreated(PBackgroundChild* aActor) MOZ_OVERRIDE;
  virtual void ActorFailed() MOZ_OVERRIDE;

  // CacheStorageChildListener methods
  virtual void ActorDestroy(mozilla::ipc::IProtocol& aActor) MOZ_OVERRIDE;
  virtual void RecvMatchResponse(RequestId aRequestId, nsresult aRv,
                           const PCacheResponseOrVoid& aResponse,
                           PCacheStreamControlChild* aStreamControl) MOZ_OVERRIDE;
  virtual void RecvHasResponse(RequestId aRequestId, nsresult aRv,
                               bool aSuccess) MOZ_OVERRIDE;
  virtual void RecvOpenResponse(RequestId aRequestId, nsresult aRv,
                                PCacheChild* aActor) MOZ_OVERRIDE;
  virtual void RecvDeleteResponse(RequestId aRequestId, nsresult aRv,
                                  bool aSuccess) MOZ_OVERRIDE;
  virtual void RecvKeysResponse(RequestId aRequestId, nsresult aRv,
                                const nsTArray<nsString>& aKeys) MOZ_OVERRIDE;

private:
  virtual ~CacheStorage();

  RequestId AddRequestPromise(Promise* aPromise, ErrorResult& aRv);
  already_AddRefed<Promise> RemoveRequestPromise(RequestId aRequestId);

  nsresult
  ToPCacheRequest(PCacheRequest& aOut, const RequestOrScalarValueString& aIn);

  const Namespace mNamespace;
  nsCOMPtr<nsISupports> mOwner;
  nsCOMPtr<nsIGlobalObject> mGlobal;
  const nsCString mOrigin;
  const nsCString mBaseDomain;
  CacheStorageChild* mActor;
  nsTArray<nsRefPtr<Promise>> mRequestPromises;

  enum Op
  {
    OP_MATCH,
    OP_HAS,
    OP_OPEN,
    OP_DELETE,
    OP_KEYS
  };

  struct Entry
  {
    Entry() { }
    ~Entry() { }
    RequestId mRequestId;
    Op mOp;
    // Would prefer to use PCacheRequest/PCacheQueryParams, but can't
    // because they introduce a header dependency on windows.h which
    // breaks the bindings build.
    RequestOrScalarValueString mRequest;
    QueryParams mParams;
    // It would also be nice to union the key with the match args above,
    // but VS2013 doesn't like these types in unions because of copy
    // constructors.
    nsString mKey;
  };

  nsTArray<Entry> mPendingRequests;
  bool mFailedActor;

public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(CacheStorage)
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_CacheStorage_h
