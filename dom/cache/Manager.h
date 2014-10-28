/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_Manager_h
#define mozilla_dom_cache_Manager_h

#include "mozilla/dom/cache/Context.h"
#include "mozilla/dom/cache/PCacheStreamControlParent.h"
#include "mozilla/dom/cache/Types.h"
#include "nsCOMPtr.h"
#include "nsISupportsImpl.h"
#include "nsString.h"
#include "nsTArray.h"

class nsIInputStream;
class nsIOutputStream;
class nsIThread;

namespace mozilla {
namespace dom {
namespace cache {

class PCacheQueryParams;
class PCacheRequest;
class PCacheRequestOrVoid;
class PCacheResponse;
struct SavedRequest;
struct SavedResponse;

class Manager MOZ_FINAL : public Context::Listener
{
public:
  class StreamList;

  class StreamControl : public PCacheStreamControlParent
  {
  public:
    virtual ~StreamControl() { }

    virtual void SetStreamList(StreamList* aStreamList)=0;

    virtual void Close(const nsID& aId)=0;
    virtual void CloseAll()=0;
    virtual void Shutdown()=0;
  };

  class StreamList
  {
  public:
    StreamList(Manager* aManager, Context* aContext);

    void SetStreamControl(StreamControl* aStreamControl);
    void RemoveStreamControl(StreamControl* aStreamControl);

    void Activate(CacheId aCacheId);

    void Add(const nsID& aId, nsIInputStream* aStream);
    already_AddRefed<nsIInputStream> Extract(const nsID& aId);

    void NoteClosed(const nsID& aId);
    void Close(const nsID& aId);
    void CloseAll();

  private:
    ~StreamList();
    struct Entry
    {
      nsID mId;
      nsCOMPtr<nsIInputStream> mStream;
    };
    nsRefPtr<Manager> mManager;
    nsRefPtr<Context> mContext;
    CacheId mCacheId;
    StreamControl* mStreamControl;
    nsTArray<Entry> mList;
    bool mActivated;

  public:
    NS_INLINE_DECL_REFCOUNTING(mozilla::dom::cache::Manager::StreamList)
  };

  class Listener
  {
  public:
    virtual ~Listener() { }

    virtual void OnCacheMatch(RequestId aRequestId, nsresult aRv,
                              const SavedResponse* aResponse,
                              StreamList* aStreamList) { }
    virtual void OnCacheMatchAll(RequestId aRequestId, nsresult aRv,
                                 const nsTArray<SavedResponse>& aSavedResponses,
                                 StreamList* aStreamList) { }
    virtual void OnCachePut(RequestId aRequestId, nsresult aRv) { }
    virtual void OnCacheDelete(RequestId aRequestId, nsresult aRv,
                               bool aSuccess) { }
    virtual void OnCacheKeys(RequestId aRequestId, nsresult aRv,
                             const nsTArray<SavedRequest>& aSavedRequests,
                             StreamList* aStreamList) { }

    virtual void OnStorageMatch(RequestId aRequestId, nsresult aRv,
                                const SavedResponse* aResponse,
                                StreamList* aStreamList) { }
    virtual void OnStorageHas(RequestId aRequestId, nsresult aRv,
                              bool aCacheFound) { }
    virtual void OnStorageOpen(RequestId aRequestId, nsresult aRv,
                               CacheId aCacheId) { }
    virtual void OnStorageDelete(RequestId aRequestId, nsresult aRv,
                                 bool aCacheDeleted) { }
    virtual void OnStorageKeys(RequestId aRequestId, nsresult aRv,
                               const nsTArray<nsString>& aKeys) { }
  };

  static already_AddRefed<Manager> ForOrigin(const nsACString& aOrigin,
                                             const nsACString& aBaseDomain);
  static already_AddRefed<Manager> ForExistingOrigin(const nsACString& aOrigin);

  void RemoveListener(Listener* aListener);
  void AddRefCacheId(CacheId aCacheId);
  void ReleaseCacheId(CacheId aCacheId);
  uint32_t GetCacheIdRefCount(CacheId aCacheId);
  void Shutdown();

  // TODO: consider moving CacheId up in the argument lists below
  void CacheMatch(Listener* aListener, RequestId aRequestId, CacheId aCacheId,
                  const PCacheRequest& aRequest,
                  const PCacheQueryParams& aParams);
  void CacheMatchAll(Listener* aListener, RequestId aRequestId,
                     CacheId aCacheId, const PCacheRequestOrVoid& aRequestOrVoid,
                     const PCacheQueryParams& aParams);
  void CachePut(Listener* aListener, RequestId aRequestId, CacheId aCacheId,
                const PCacheRequest& aRequest,
                nsIInputStream* aRequestBodyStream,
                const PCacheResponse& aResponse,
                nsIInputStream* aResponseBodyStream);
  void CacheDelete(Listener* aListener, RequestId aRequestId,
                   CacheId aCacheId, const PCacheRequest& aRequest,
                   const PCacheQueryParams& aParams);
  void CacheKeys(Listener* aListener, RequestId aRequestId,
                 CacheId aCacheId, const PCacheRequestOrVoid& aRequestOrVoid,
                 const PCacheQueryParams& aParams);

  void StorageMatch(Listener* aListener, RequestId aRequestId,
                    Namespace aNamespace, const PCacheRequest& aRequest,
                    const PCacheQueryParams& aParams);
  void StorageHas(Listener* aListener, RequestId aRequestId,
                  Namespace aNamespace, const nsAString& aKey);
  void StorageOpen(Listener* aListener, RequestId aRequestId,
                   Namespace aNamespace, const nsAString& aKey);
  void StorageDelete(Listener* aListener, RequestId aRequestId,
                     Namespace aNamespace, const nsAString& aKey);
  void StorageKeys(Listener* aListener, RequestId aRequestId,
                   Namespace aNamespace);

  const nsCString& Origin() const { return mOrigin; }
  const nsCString& BaseDomain() const { return mBaseDomain; }

  // Context::Listener methods
  virtual void RemoveContext(Context* aContext) MOZ_OVERRIDE;

private:
  class Factory;
  class BaseAction;
  class CheckCacheOrphanedAction;
  class DeleteOrphanedCacheAction;

  class CacheMatchAction;
  class CacheMatchAllAction;
  class CachePutAction;
  class CacheDeleteAction;
  class CacheKeysAction;

  class StorageMatchAction;
  class StorageHasAction;
  class StorageOpenAction;
  class StorageDeleteAction;
  class StorageKeysAction;

  typedef uintptr_t ListenerId;

  Manager(const nsACString& aOrigin, const nsACString& aBaseDomain);
  ~Manager();
  Context* CurrentContext();

  ListenerId SaveListener(Listener* aListener);
  Listener* GetListener(ListenerId aListenerId) const;

  void AddStreamList(StreamList* aStreamList);
  void RemoveStreamList(StreamList* aStreamList);

  const nsCString mOrigin;
  const nsCString mBaseDomain;
  nsCOMPtr<nsIThread> mIOThread;
  nsTArray<Listener*> mListeners;
  nsTArray<StreamList*> mStreamLists;

  struct CacheIdRefCounter
  {
    CacheId mCacheId;
    uint32_t mCount;
  };
  nsTArray<CacheIdRefCounter> mCacheIdRefs;

  // weak ref as Context destructor clears this pointer
  Context* mContext;

  bool mShuttingDown;
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_Manager_h
