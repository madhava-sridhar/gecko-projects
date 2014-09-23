/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/Manager.h"

#include "mozilla/dom/cache/DBAction.h"
#include "mozilla/dom/cache/DBSchema.h"
#include "mozilla/dom/cache/PCacheQueryParams.h"
#include "mozilla/dom/cache/PCacheRequest.h"
#include "mozilla/dom/cache/PCacheResponse.h"
#include "mozilla/dom/cache/Types.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozStorageHelper.h"
#include "nsAutoPtr.h"
#include "nsIThread.h"
#include "nsThreadUtils.h"

namespace {

using mozilla::dom::cache::DBSchema;
using mozilla::dom::cache::SyncDBAction;

class SetupAction MOZ_FINAL : public SyncDBAction
{
public:
  SetupAction(const nsACString& aOrigin, const nsACString& aBaseDomain)
    : SyncDBAction(DBAction::Create, aOrigin, aBaseDomain)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    // TODO: create body directory structure
    // TODO: init maintainance marker
    // TODO: perform maintainance if necessary
    // TODO: find orphaned caches in database

    mozStorageTransaction trans(aConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    nsresult rv = DBSchema::CreateSchema(aConn);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = trans.Commit();
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    return rv;
  }

private:
  virtual ~SetupAction() { }
};

} // anonymous namespace

namespace mozilla {
namespace dom {
namespace cache {

class Manager::Factory
{
private:
  static Factory* sFactory;
  nsTArray<Manager*> mManagerList;

public:
  static Factory& Instance()
  {
    mozilla::ipc::AssertIsOnBackgroundThread();

    if (!sFactory) {
      sFactory = new Factory();
    }
    return *sFactory;
  }

  already_AddRefed<Manager> GetOrCreate(const nsACString& aOrigin,
                                        const nsACString& aBaseDomain)
  {
    mozilla::ipc::AssertIsOnBackgroundThread();

    for (uint32_t i = 0; i < mManagerList.Length(); ++i) {
      if (mManagerList[i]->Origin() == aOrigin) {
        nsRefPtr<Manager> ref = mManagerList[i];
        return ref.forget();
      }
    }

    nsRefPtr<Manager> ref = new Manager(aOrigin, aBaseDomain);

    mManagerList.AppendElement(ref);

    return ref.forget();
  }

  void Remove(Manager* aManager)
  {
    mozilla::ipc::AssertIsOnBackgroundThread();
    MOZ_ASSERT(aManager);

    for (uint32_t i = 0; i < mManagerList.Length(); ++i) {
      if (mManagerList[i] == aManager) {
        mManagerList.RemoveElementAt(i);

        if (mManagerList.Length() < 1) {
          delete sFactory;
          sFactory = nullptr;
        }
        return;
      }
    }
  }
};

// static
Manager::Factory* Manager::Factory::sFactory = nullptr;

class Manager::BaseAction : public SyncDBAction
{
protected:
  BaseAction(Manager* aManager, ListenerId aListenerId, RequestId aRequestId)
    : SyncDBAction(DBAction::Existing, aManager->Origin(),
                   aManager->BaseDomain())
    , mManager(aManager)
    , mListenerId(aListenerId)
    , mRequestId (aRequestId)
  { }

  virtual void
  Complete(Listener* aListener, nsresult aRv)=0;

  virtual void
  CompleteOnInitiatingThread(nsresult aRv) MOZ_OVERRIDE
  {
    Listener* listener = mManager->GetListener(mListenerId);
    if (!listener) {
      return;
    }
    Complete(listener, aRv);
  }

  virtual ~BaseAction() { }
  nsRefPtr<Manager> mManager;
  const ListenerId mListenerId;
  const RequestId mRequestId;
};

class Manager::DeleteOrphanedCacheAction MOZ_FINAL : public SyncDBAction
{
public:
  DeleteOrphanedCacheAction(Manager* aManager, CacheId aCacheId)
    : SyncDBAction(DBAction::Existing, aManager->Origin(),
                   aManager->BaseDomain())
    , mManager(aManager)
    , mCacheId(aCacheId)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    mozStorageTransaction trans(aConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    // TODO: get body data file names

    nsresult rv =  DBSchema::DeleteCache(aConn, mCacheId);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = trans.Commit();
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    // TODO: delete body data files

    return rv;
  }

  virtual void
  CompleteOnInitiatingThread(nsresult aRv) MOZ_OVERRIDE { }

private:
  virtual ~DeleteOrphanedCacheAction() { }
  nsRefPtr<Manager> mManager;
  const CacheId mCacheId;
};

class Manager::CheckCacheOrphanedAction MOZ_FINAL : public SyncDBAction
{
public:
  CheckCacheOrphanedAction(Manager* aManager, CacheId aCacheId)
    : SyncDBAction(DBAction::Existing, aManager->Origin(),
                   aManager->BaseDomain())
    , mManager(aManager)
    , mCacheId(aCacheId)
    , mOrphaned(false)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    // Note: We need to do the check separately from the delete so we have the
    //       opportunity to cancel pending IO actions in
    //       CompleteOnInitiatingThread().
    return DBSchema::IsCacheOrphaned(aConn, mCacheId, &mOrphaned);
  }

  virtual void
  CompleteOnInitiatingThread(nsresult aRv) MOZ_OVERRIDE
  {
    if (!mOrphaned) {
      return;
    }

    mManager->CurrentContext()->CancelForCacheId(mCacheId);

    nsRefPtr<Action> action = new DeleteOrphanedCacheAction(mManager, mCacheId);
    mManager->CurrentContext()->Dispatch(mManager->mIOThread, action);
  }

private:
  virtual ~CheckCacheOrphanedAction() { }
  nsRefPtr<Manager> mManager;
  const CacheId mCacheId;
  bool mOrphaned;
};

class Manager::CacheMatchAction : public Manager::BaseAction
{
public:
  CacheMatchAction(Manager* aManager, ListenerId aListenerId,
                   RequestId aRequestId, CacheId aCacheId,
                   const PCacheRequest& aRequest,
                   const PCacheQueryParams& aParams)
    : BaseAction(aManager, aListenerId, aRequestId)
    , mCacheId(aCacheId)
    , mRequest(aRequest)
    , mParams(aParams)
    , mResponseOrVoid(void_t())
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    return DBSchema::CacheMatch(aConn, mCacheId, mRequest, mParams,
                                &mResponseOrVoid);
  }

  virtual void
  Complete(Listener* aListener, nsresult aRv) MOZ_OVERRIDE
  {
    aListener->OnCacheMatch(mRequestId, aRv, mResponseOrVoid);
  }

protected:
  virtual ~CacheMatchAction() { }
  const CacheId mCacheId;
  const PCacheRequest mRequest;
  const PCacheQueryParams mParams;
  PCacheResponseOrVoid mResponseOrVoid;
};

class Manager::CacheMatchAllAction : public Manager::BaseAction
{
public:
  CacheMatchAllAction(Manager* aManager, ListenerId aListenerId,
                      RequestId aRequestId, CacheId aCacheId,
                      const PCacheRequestOrVoid& aRequestOrVoid,
                      const PCacheQueryParams& aParams)
    : BaseAction(aManager, aListenerId, aRequestId)
    , mCacheId(aCacheId)
    , mRequestOrVoid(aRequestOrVoid)
    , mParams(aParams)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    return DBSchema::CacheMatchAll(aConn, mCacheId, mRequestOrVoid, mParams,
                                   mResponses);
  }

  virtual void
  Complete(Listener* aListener, nsresult aRv) MOZ_OVERRIDE
  {
    aListener->OnCacheMatchAll(mRequestId, aRv, mResponses);
  }

protected:
  virtual ~CacheMatchAllAction() { }
  const CacheId mCacheId;
  const PCacheRequestOrVoid mRequestOrVoid;
  const PCacheQueryParams mParams;
  nsTArray<PCacheResponse> mResponses;
};

class Manager::CachePutAction : public Manager::BaseAction
{
public:
  CachePutAction(Manager* aManager, ListenerId aListenerId,
                 RequestId aRequestId, CacheId aCacheId,
                 const PCacheRequest& aRequest, const PCacheResponse& aResponse)
    : BaseAction(aManager, aListenerId, aRequestId)
    , mCacheId(aCacheId)
    , mRequest(aRequest)
    , mResponse(aResponse)
    , mResponseOrVoid(void_t())
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    mozStorageTransaction trans(aConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    nsresult rv = DBSchema::CachePut(aConn, mCacheId, mRequest, mResponse);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = trans.Commit();
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    // TODO: replace this with a read stream of the written files
    mResponseOrVoid = mResponse;

    return rv;
  }

  virtual void
  Complete(Listener* aListener, nsresult aRv) MOZ_OVERRIDE
  {
    aListener->OnCachePut(mRequestId, aRv, mResponseOrVoid);
  }

protected:
  virtual ~CachePutAction() { }
  const CacheId mCacheId;
  const PCacheRequest mRequest;
  const PCacheResponse mResponse;
  PCacheResponseOrVoid mResponseOrVoid;
};

class Manager::CacheDeleteAction : public Manager::BaseAction
{
public:
  CacheDeleteAction(Manager* aManager, ListenerId aListenerId,
                    RequestId aRequestId, CacheId aCacheId,
                    const PCacheRequest& aRequest,
                    const PCacheQueryParams& aParams)
    : BaseAction(aManager, aListenerId, aRequestId)
    , mCacheId(aCacheId)
    , mRequest(aRequest)
    , mParams(aParams)
    , mSuccess(false)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    mozStorageTransaction trans(aConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    nsresult rv = DBSchema::CacheDelete(aConn, mCacheId, mRequest, mParams,
                                        &mSuccess);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = trans.Commit();
    if (NS_WARN_IF(NS_FAILED(rv))) {
      mSuccess = false;
      return rv;
    }

    return rv;
  }

  virtual void
  Complete(Listener* aListener, nsresult aRv) MOZ_OVERRIDE
  {
    aListener->OnCacheDelete(mRequestId, aRv, mSuccess);
  }

protected:
  virtual ~CacheDeleteAction() { }
  const CacheId mCacheId;
  const PCacheRequest mRequest;
  const PCacheQueryParams mParams;
  bool mSuccess;
};

class Manager::StorageGetAction : public Manager::BaseAction
{
public:
  StorageGetAction(Manager* aManager, ListenerId aListenerId,
                   RequestId aRequestId, Namespace aNamespace,
                   const nsAString& aKey)
    : BaseAction(aManager, aListenerId, aRequestId)
    , mNamespace(aNamespace)
    , mKey(aKey)
    , mCacheFound(false)
    , mCacheId(0)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    return DBSchema::StorageGetCacheId(aConn, mNamespace, mKey,
                                       &mCacheFound, &mCacheId);
  }

  virtual void
  Complete(Listener* aListener, nsresult aRv) MOZ_OVERRIDE
  {
    aListener->OnStorageGet(mRequestId, aRv, mCacheFound, mCacheId);
  }

protected:
  virtual ~StorageGetAction() { }
  const Namespace mNamespace;
  const nsString mKey;
  bool mCacheFound;
  CacheId mCacheId;
};

class Manager::StorageHasAction MOZ_FINAL : public Manager::StorageGetAction
{
public:
  StorageHasAction(Manager* aManager, ListenerId aListenerId,
                   RequestId aRequestId, Namespace aNamespace,
                   const nsAString& aKey)
    : StorageGetAction(aManager, aListenerId, aRequestId, aNamespace, aKey)
  { }

  virtual void
  Complete(Listener* aListener, nsresult aRv) MOZ_OVERRIDE
  {
    aListener->OnStorageHas(mRequestId, aRv, mCacheFound);
  }

private:
  virtual ~StorageHasAction() { }
};

class Manager::StorageCreateAction MOZ_FINAL : public Manager::BaseAction
{
public:
  StorageCreateAction(Manager* aManager, ListenerId aListenerId,
                      RequestId aRequestId, Namespace aNamespace,
                      const nsAString& aKey)
    : BaseAction(aManager, aListenerId, aRequestId)
    , mNamespace(aNamespace)
    , mKey(aKey)
    , mCacheId(0)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    mozStorageTransaction trans(aConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    nsresult rv = DBSchema::CreateCache(aConn, &mCacheId);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = DBSchema::StoragePutCache(aConn, mNamespace, mKey, mCacheId);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = trans.Commit();
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    return rv;
  }

  virtual void
  Complete(Listener* aListener, nsresult aRv) MOZ_OVERRIDE
  {
    aListener->OnStorageCreate(mRequestId, aRv, mCacheId);
  }

private:
  virtual ~StorageCreateAction() { }
  const Namespace mNamespace;
  const nsString mKey;
  CacheId mCacheId;
};

class Manager::StorageDeleteAction MOZ_FINAL : public Manager::BaseAction
{
public:
  StorageDeleteAction(Manager* aManager, ListenerId aListenerId,
                      RequestId aRequestId, Namespace aNamespace,
                      const nsAString& aKey)
    : BaseAction(aManager, aListenerId, aRequestId)
    , mNamespace(aNamespace)
    , mKey(aKey)
    , mCacheDeleted(false)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    mozStorageTransaction trans(aConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    bool exists;
    CacheId cacheId;
    nsresult rv = DBSchema::StorageGetCacheId(aConn, mNamespace, mKey, &exists,
                                              &cacheId);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    if (!exists) {
      mCacheDeleted = false;
      return NS_OK;
    }

    rv = DBSchema::StorageForgetCache(aConn, mNamespace, mKey);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = trans.Commit();
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    mCacheDeleted = true;
    return rv;
  }

  virtual void
  Complete(Listener* aListener, nsresult aRv) MOZ_OVERRIDE
  {
    // TODO: check for Cache ref and remove orphan
    aListener->OnStorageDelete(mRequestId, aRv, mCacheDeleted);
  }

private:
  virtual ~StorageDeleteAction() { }
  const Namespace mNamespace;
  const nsString mKey;
  bool mCacheDeleted;
};

class Manager::StorageKeysAction MOZ_FINAL : public Manager::BaseAction
{
public:
  StorageKeysAction(Manager* aManager, ListenerId aListenerId,
                      RequestId aRequestId, Namespace aNamespace)
    : BaseAction(aManager, aListenerId, aRequestId)
    , mNamespace(aNamespace)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    return DBSchema::StorageGetKeys(aConn, mNamespace, mKeys);
  }

  virtual void
  Complete(Listener* aListener, nsresult aRv) MOZ_OVERRIDE
  {
    if (NS_FAILED(aRv)) {
      mKeys.Clear();
    }
    aListener->OnStorageKeys(mRequestId, aRv, mKeys);
  }

private:
  virtual ~StorageKeysAction() { }
  const Namespace mNamespace;
  nsTArray<nsString> mKeys;
};

// static
already_AddRefed<Manager>
Manager::ForOrigin(const nsACString& aOrigin, const nsACString& aBaseDomain)
{
  mozilla::ipc::AssertIsOnBackgroundThread();
  return Factory::Instance().GetOrCreate(aOrigin, aBaseDomain);
}

void
Manager::RemoveListener(Listener* aListener)
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  mListeners.RemoveElement(aListener);
}

void
Manager::AddRefCacheId(CacheId aCacheId)
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  for (uint32_t i = 0; i < mCacheIdRefs.Length(); ++i) {
    if (mCacheIdRefs[i].mCacheId == aCacheId) {
      mCacheIdRefs[i].mCount += 1;
      return;
    }
  }
  CacheIdRefCounter* entry = mCacheIdRefs.AppendElement();
  entry->mCacheId = aCacheId;
  entry->mCount = 1;
}

void
Manager::ReleaseCacheId(CacheId aCacheId)
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  for (uint32_t i = 0; i < mCacheIdRefs.Length(); ++i) {
    if (mCacheIdRefs[i].mCacheId == aCacheId) {
      DebugOnly<uint32_t> oldRef = mCacheIdRefs[i].mCount;
      mCacheIdRefs[i].mCount -= 1;
      MOZ_ASSERT(mCacheIdRefs[i].mCount < oldRef);
      if (mCacheIdRefs[i].mCount < 1) {
        mCacheIdRefs.RemoveElementAt(i);
        nsRefPtr<Action> action = new CheckCacheOrphanedAction(this, aCacheId);
        CurrentContext()->Dispatch(mIOThread, action);
      }
      return;
    }
  }
  MOZ_ASSERT_UNREACHABLE("Attempt to release CacheId that is not referenced!");
}

void
Manager::CacheMatch(Listener* aListener, RequestId aRequestId, CacheId aCacheId,
                    const PCacheRequest& aRequest,
                    const PCacheQueryParams& aParams)
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_ASSERT(aListener);
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new CacheMatchAction(this, listenerId, aRequestId,
                                                 aCacheId, aRequest, aParams);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::CacheMatchAll(Listener* aListener, RequestId aRequestId,
                       CacheId aCacheId, const PCacheRequestOrVoid& aRequest,
                       const PCacheQueryParams& aParams)
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_ASSERT(aListener);
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new CacheMatchAllAction(this, listenerId, aRequestId,
                                                    aCacheId, aRequest, aParams);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::CachePut(Listener* aListener, RequestId aRequestId, CacheId aCacheId,
                  const PCacheRequest& aRequest,
                  const PCacheResponse& aResponse)
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_ASSERT(aListener);
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new CachePutAction(this, listenerId, aRequestId,
                                               aCacheId, aRequest, aResponse);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::CacheDelete(Listener* aListener, const RequestId& aRequestId,
                     CacheId aCacheId, const PCacheRequest& aRequest,
                     const PCacheQueryParams& aParams)
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_ASSERT(aListener);
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new CacheDeleteAction(this, listenerId, aRequestId,
                                                  aCacheId, aRequest, aParams);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::StorageGet(Listener* aListener, RequestId aRequestId,
                    Namespace aNamespace, const nsAString& aKey)
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_ASSERT(aListener);
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new StorageGetAction(this, listenerId, aRequestId,
                                                 aNamespace, aKey);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::StorageHas(Listener* aListener, RequestId aRequestId,
                    Namespace aNamespace, const nsAString& aKey)
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_ASSERT(aListener);
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new StorageHasAction(this, listenerId, aRequestId,
                                                 aNamespace, aKey);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::StorageCreate(Listener* aListener, RequestId aRequestId,
                       Namespace aNamespace, const nsAString& aKey)
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_ASSERT(aListener);
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new StorageCreateAction(this, listenerId, aRequestId,
                                                    aNamespace, aKey);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::StorageDelete(Listener* aListener, RequestId aRequestId,
                       Namespace aNamespace, const nsAString& aKey)
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_ASSERT(aListener);
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new StorageDeleteAction(this, listenerId, aRequestId,
                                                    aNamespace, aKey);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::StorageKeys(Listener* aListener, RequestId aRequestId,
                     Namespace aNamespace)
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_ASSERT(aListener);
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new StorageKeysAction(this, listenerId, aRequestId,
                                                  aNamespace);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::RemoveContext(Context* aContext)
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  MOZ_ASSERT(mContext);
  MOZ_ASSERT(mContext == aContext);
  mContext = nullptr;
}

Manager::Manager(const nsACString& aOrigin, const nsACString& aBaseDomain)
  : mOrigin(aOrigin)
  , mBaseDomain(aBaseDomain)
  , mContext(nullptr)
{
  nsresult rv = NS_NewNamedThread("DOMCacheThread",
                                  getter_AddRefs(mIOThread));
  if (NS_FAILED(rv)) {
    MOZ_CRASH("Failed to spawn cache manager IO thread.");
  }
}

Manager::~Manager()
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  Factory::Instance().Remove(this);
  if (mContext) {
    mContext->CancelAll();
    mContext->ClearListener();
  }
  mIOThread->Shutdown();
}

Context*
Manager::CurrentContext()
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  if (!mContext) {
    nsRefPtr<Action> setupAction = new SetupAction(mOrigin, mBaseDomain);
    mContext = new Context(this, mOrigin, mBaseDomain, setupAction);
  }
  return mContext;
}

Manager::ListenerId
Manager::SaveListener(Listener* aListener)
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  for (uint32_t i = 0; i < mListeners.Length(); ++i) {
    if (mListeners[i] == aListener) {
      return reinterpret_cast<ListenerId>(aListener);
    }
  }
  mListeners.AppendElement(aListener);
  return reinterpret_cast<ListenerId>(aListener);
}

Manager::Listener*
Manager::GetListener(ListenerId aListenerId) const
{
  NS_ASSERT_OWNINGTHREAD(Manager);
  for (uint32_t i = 0; i < mListeners.Length(); ++i) {
    if (reinterpret_cast<ListenerId>(mListeners[i]) == aListenerId) {
      return mListeners[i];
    }
  }
  return nullptr;
}

} // namespace cache
} // namespace dom
} // namespace mozilla
