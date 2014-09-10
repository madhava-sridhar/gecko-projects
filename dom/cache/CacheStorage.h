/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CacheStorage_h
#define mozilla_dom_CacheStorage_h

#include "mozilla/dom/CacheStorageChildListener.h"
#include "mozilla/dom/CacheTypes.h"
#include "nsISupportsImpl.h"
#include "nsWrapperCache.h"
#include "nsIIPCBackgroundChildCreateCallback.h"

class nsIGlobalObject;

namespace mozilla {

class ErrorResult;

namespace ipc {
  class IProtocol;
}

namespace dom {

class CacheStorageChild;
class Promise;
struct QueryParams;
class RequestOrScalarValueString;

class CacheStorage MOZ_FINAL : public nsIIPCBackgroundChildCreateCallback
                             , public nsWrapperCache
                             , public CacheStorageChildListener
{
  typedef mozilla::ipc::PBackgroundChild PBackgroundChild;

public:
  CacheStorage(cache::Namespace aNamespace, nsISupports* aOwner,
               nsIGlobalObject* aGlobal, const nsACString& aOrigin,
               const nsACString& aBaseDomain);

  // webidl interface methods
  already_AddRefed<Promise> Match(const RequestOrScalarValueString& aRequest,
                                  const QueryParams& aParams, ErrorResult& aRv);
  already_AddRefed<Promise> Get(const nsAString& aKey, ErrorResult& aRv);
  already_AddRefed<Promise> Has(const nsAString& aKey, ErrorResult& aRv);
  already_AddRefed<Promise> Create(const nsAString& aKey, ErrorResult& aRv);
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
  virtual void RecvGetResponse(cache::RequestId aRequestId, nsresult aRv,
                               PCacheChild* aActor) MOZ_OVERRIDE;
  virtual void RecvHasResponse(cache::RequestId aRequestId, nsresult aRv,
                               bool aSuccess) MOZ_OVERRIDE;
  virtual void RecvCreateResponse(cache::RequestId aRequestId, nsresult aRv,
                                  PCacheChild* aActor) MOZ_OVERRIDE;
  virtual void RecvDeleteResponse(cache::RequestId aRequestId, nsresult aRv,
                                  bool aSuccess) MOZ_OVERRIDE;
  virtual void RecvKeysResponse(cache::RequestId aRequestId, nsresult aRv,
                                const nsTArray<nsString>& aKeys) MOZ_OVERRIDE;

private:
  virtual ~CacheStorage();

  cache::RequestId AddRequestPromise(Promise* aPromise, ErrorResult& aRv);
  already_AddRefed<Promise> RemoveRequestPromise(cache::RequestId aRequestId);

  const cache::Namespace mNamespace;
  nsCOMPtr<nsISupports> mOwner;
  nsCOMPtr<nsIGlobalObject> mGlobal;
  const nsCString mOrigin;
  const nsCString mBaseDomain;
  CacheStorageChild* mActor;
  nsTArray<nsRefPtr<Promise>> mRequestPromises;

public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(CacheStorage)
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_CacheStorage_h
