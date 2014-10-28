/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/Manager.h"

#include "mozilla/dom/cache/DBAction.h"
#include "mozilla/dom/cache/DBSchema.h"
#include "mozilla/dom/cache/FileUtils.h"
#include "mozilla/dom/cache/PCacheTypes.h"
#include "mozilla/dom/cache/SavedTypes.h"
#include "mozilla/dom/cache/ShutdownObserver.h"
#include "mozilla/dom/cache/Types.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozStorageHelper.h"
#include "nsAutoPtr.h"
#include "nsIInputStream.h"
#include "nsID.h"
#include "nsIFile.h"
#include "nsIThread.h"

namespace {

using mozilla::dom::cache::DBSchema;
using mozilla::dom::cache::FileUtils;
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
    // TODO: init maintainance marker
    // TODO: perform maintainance if necessary
    // TODO: find orphaned caches in database
    // TODO: have Context create/delete marker files in constructor/destructor
    //       and only do expensive maintenance if that marker is present

    nsresult rv = FileUtils::BodyCreateDir(aDBDir);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    mozStorageTransaction trans(aConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    rv = DBSchema::CreateSchema(aConn);
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

    nsRefPtr<Manager> ref = Get(aOrigin);
    if (!ref) {
      ref = new Manager(aOrigin, aBaseDomain);
      mManagerList.AppendElement(ref);
    }

    return ref.forget();
  }

  already_AddRefed<Manager> Get(const nsACString& aOrigin)
  {
    mozilla::ipc::AssertIsOnBackgroundThread();

    for (uint32_t i = 0; i < mManagerList.Length(); ++i) {
      if (mManagerList[i]->Origin() == aOrigin) {
        nsRefPtr<Manager> ref = mManagerList[i];
        return ref.forget();
      }
    }

    return nullptr;
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
    mManager = nullptr;
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

    nsresult rv =  DBSchema::DeleteCache(aConn, mCacheId);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = FileUtils::BodyDeleteCacheDir(aDBDir, mCacheId);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = trans.Commit();
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    return rv;
  }

  virtual void
  CompleteOnInitiatingThread(nsresult aRv) MOZ_OVERRIDE
  {
    mManager = nullptr;
  }

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
      mManager = nullptr;
      return;
    }

    mManager->CurrentContext()->CancelForCacheId(mCacheId);

    nsRefPtr<Action> action = new DeleteOrphanedCacheAction(mManager, mCacheId);
    mManager->CurrentContext()->Dispatch(mManager->mIOThread, action);

    mManager = nullptr;
  }

private:
  virtual ~CheckCacheOrphanedAction() { }
  nsRefPtr<Manager> mManager;
  const CacheId mCacheId;
  bool mOrphaned;
};

class Manager::CacheMatchAction MOZ_FINAL : public Manager::BaseAction
{
public:
  CacheMatchAction(Manager* aManager, ListenerId aListenerId,
                   RequestId aRequestId, CacheId aCacheId,
                   const PCacheRequest& aRequest,
                   const PCacheQueryParams& aParams,
                   StreamList* aStreamList)
    : BaseAction(aManager, aListenerId, aRequestId)
    , mCacheId(aCacheId)
    , mRequest(aRequest)
    , mParams(aParams)
    , mStreamList(aStreamList)
    , mFoundResponse(false)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    nsresult rv = DBSchema::CacheMatch(aConn, mCacheId, mRequest, mParams,
                                       &mFoundResponse, &mResponse);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    if (!mFoundResponse || !mResponse.mHasBodyId) {
      return rv;
    }

    nsCOMPtr<nsIInputStream> stream;
    rv = FileUtils::BodyOpen(mManager->Origin(), mManager->BaseDomain(),
                             aDBDir, mCacheId, mResponse.mBodyId,
                             getter_AddRefs(stream));
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
    if (NS_WARN_IF(!stream)) { return NS_ERROR_FILE_NOT_FOUND; }

    mStreamList->Add(mResponse.mBodyId, stream);

    return rv;
  }

  virtual void
  Complete(Listener* aListener, nsresult aRv) MOZ_OVERRIDE
  {
    if (!mFoundResponse) {
      aListener->OnCacheMatch(mRequestId, aRv, nullptr, nullptr);
    } else {
      mStreamList->Activate(mCacheId);
      aListener->OnCacheMatch(mRequestId, aRv, &mResponse, mStreamList);
    }
    mStreamList = nullptr;
  }

  virtual bool MatchesCacheId(CacheId aCacheId) MOZ_OVERRIDE
  {
    return aCacheId == mCacheId;
  }

protected:
  virtual ~CacheMatchAction() { }
  const CacheId mCacheId;
  const PCacheRequest mRequest;
  const PCacheQueryParams mParams;
  nsRefPtr<StreamList> mStreamList;
  bool mFoundResponse;
  SavedResponse mResponse;
};

class Manager::CacheMatchAllAction MOZ_FINAL : public Manager::BaseAction
{
public:
  CacheMatchAllAction(Manager* aManager, ListenerId aListenerId,
                      RequestId aRequestId, CacheId aCacheId,
                      const PCacheRequestOrVoid& aRequestOrVoid,
                      const PCacheQueryParams& aParams,
                      StreamList* aStreamList)
    : BaseAction(aManager, aListenerId, aRequestId)
    , mCacheId(aCacheId)
    , mRequestOrVoid(aRequestOrVoid)
    , mParams(aParams)
    , mStreamList(aStreamList)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    nsresult rv = DBSchema::CacheMatchAll(aConn, mCacheId, mRequestOrVoid,
                                          mParams, mSavedResponses);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    for (uint32_t i = 0; i < mSavedResponses.Length(); ++i) {
      if (!mSavedResponses[i].mHasBodyId) {
        continue;
      }

      nsCOMPtr<nsIInputStream> stream;
      rv = FileUtils::BodyOpen(mManager->Origin(), mManager->BaseDomain(),
                               aDBDir, mCacheId, mSavedResponses[i].mBodyId,
                               getter_AddRefs(stream));
      if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
      if (NS_WARN_IF(!stream)) { return NS_ERROR_FILE_NOT_FOUND; }

      mStreamList->Add(mSavedResponses[i].mBodyId, stream);
    }

    return rv;
  }

  virtual void
  Complete(Listener* aListener, nsresult aRv) MOZ_OVERRIDE
  {
    mStreamList->Activate(mCacheId);
    aListener->OnCacheMatchAll(mRequestId, aRv, mSavedResponses, mStreamList);
    mStreamList = nullptr;
  }

  virtual bool MatchesCacheId(CacheId aCacheId) MOZ_OVERRIDE
  {
    return aCacheId == mCacheId;
  }

protected:
  virtual ~CacheMatchAllAction() { }
  const CacheId mCacheId;
  const PCacheRequestOrVoid mRequestOrVoid;
  const PCacheQueryParams mParams;
  nsRefPtr<StreamList> mStreamList;
  nsTArray<SavedResponse> mSavedResponses;
};

class Manager::CachePutAction MOZ_FINAL : public DBAction
{
public:
  CachePutAction(Manager* aManager, ListenerId aListenerId,
                 RequestId aRequestId, CacheId aCacheId,
                 const PCacheRequest& aRequest,
                 nsIInputStream* aRequestBodyStream,
                 const PCacheResponse& aResponse,
                 nsIInputStream* aResponseBodyStream,
                 StreamList* aStreamList)
    : DBAction(DBAction::Existing, aManager->Origin(), aManager->BaseDomain())
    , mManager(aManager)
    , mListenerId(aListenerId)
    , mRequestId(aRequestId)
    , mCacheId(aCacheId)
    , mRequest(aRequest)
    , mRequestBodyStream(aRequestBodyStream)
    , mResponse(aResponse)
    , mResponseBodyStream(aResponseBodyStream)
    , mStreamList(aStreamList)
    , mExpectedAsyncCopyCompletions(0)
  { }

  virtual void
  RunWithDBOnTarget(Resolver* aResolver, nsIFile* aDBDir,
                    mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    MOZ_ASSERT(aResolver);
    MOZ_ASSERT(aDBDir);
    MOZ_ASSERT(aConn);
    MOZ_ASSERT(!mResolver);
    MOZ_ASSERT(!mDBDir);
    MOZ_ASSERT(!mConn);

    mResolver = aResolver;
    mDBDir = aDBDir;
    mConn = aConn;

    mExpectedAsyncCopyCompletions = mRequestBodyStream ? 1 : 0;
    mExpectedAsyncCopyCompletions += mResponseBodyStream ? 1 : 0;

    if (mExpectedAsyncCopyCompletions < 1) {
      mExpectedAsyncCopyCompletions = 1;
      OnAsyncCopyComplete(NS_OK);
      return;
    }

    nsresult rv = StartStreamCopy(mRequestBodyStream, &mRequestBodyId,
                                  getter_AddRefs(mRequestBodyCopyContext));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      DoResolve(rv);
      return;
    }
    MOZ_ASSERT(mRequestBodyCopyContext);

    rv = StartStreamCopy(mResponseBodyStream, &mResponseBodyId,
                         getter_AddRefs(mResponseBodyCopyContext));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      CancelStreamCopy(mRequestBodyStream, mRequestBodyCopyContext,
                       mRequestBodyId);
      mRequestBodyCopyContext = nullptr;
      DoResolve(rv);
      return;
    }
  }

  void
  OnAsyncCopyComplete(nsresult aRv)
  {
    MOZ_ASSERT(mConn);
    MOZ_ASSERT(mResolver);
    MOZ_ASSERT(mExpectedAsyncCopyCompletions > 0);

    // When DoResolve() is called below the "this" object can get destructed
    // out from under us on the initiating thread.  Ensure that we cleanly
    // run to completion in this scope before destruction.
    nsRefPtr<Action> kungFuDeathGrip = this;

    if (NS_FAILED(aRv)) {
      DoResolve(aRv);
      return;
    }

    mExpectedAsyncCopyCompletions -= 1;
    if (mExpectedAsyncCopyCompletions > 0) {
      return;
    }

    mRequestBodyCopyContext = nullptr;
    mResponseBodyCopyContext = nullptr;

    nsresult rv = NS_OK;

    if (mRequestBodyStream) {
      rv = FileUtils::BodyFinalizeWrite(mDBDir, mCacheId, mRequestBodyId);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        DoResolve(rv);
        return;
      }
    }

    if (mResponseBodyStream) {
      rv = FileUtils::BodyFinalizeWrite(mDBDir, mCacheId, mResponseBodyId);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        DoResolve(rv);
        return;
      }
    }

    mozStorageTransaction trans(mConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    nsTArray<nsID> deletedBodyIdList;
    rv = DBSchema::CachePut(mConn, mCacheId, mRequest,
                            mRequestBodyStream ? &mRequestBodyId : nullptr,
                            mResponse,
                            mResponseBodyStream ? &mResponseBodyId : nullptr,
                            deletedBodyIdList);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      DoResolve(rv);
      return;
    }

    rv = FileUtils::BodyDeleteFiles(mDBDir, mCacheId, deletedBodyIdList);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      DoResolve(rv);
      return;
    }

    rv = trans.Commit();
    if (NS_WARN_IF(NS_FAILED(rv))) {
      DoResolve(rv);
      return;
    }

    DoResolve(rv);
  }

  virtual void
  CompleteOnInitiatingThread(nsresult aRv) MOZ_OVERRIDE
  {
    NS_ASSERT_OWNINGTHREAD(Action);
    Listener* listener = mManager->GetListener(mListenerId);
    mManager = nullptr;
    if (!listener) {
      return;
    }
    if (NS_FAILED(aRv)) {
      listener->OnCachePut(mRequestId, aRv);
    } else {
      mStreamList->Activate(mCacheId);
      listener->OnCachePut(mRequestId, aRv);
    }
    mStreamList = nullptr;
  }

  virtual void
  CancelOnTarget() MOZ_OVERRIDE
  {
    CancelStreamCopy(mRequestBodyStream, mRequestBodyCopyContext,
                     mRequestBodyId);
    mRequestBodyCopyContext = nullptr;
    CancelStreamCopy(mResponseBodyStream, mResponseBodyCopyContext,
                     mResponseBodyId);
    mResponseBodyCopyContext = nullptr;
    mConn = nullptr;
    mResolver = nullptr;
  }

  virtual bool MatchesCacheId(CacheId aCacheId) MOZ_OVERRIDE
  {
    return aCacheId == mCacheId;
  }

private:
  virtual ~CachePutAction() { }

  nsresult
  StartStreamCopy(nsIInputStream* aSource, nsID* aIdOut,
                  nsISupports** aCopyContextOut)
  {
    MOZ_ASSERT(aIdOut);
    MOZ_ASSERT(aCopyContextOut);
    MOZ_ASSERT(mDBDir);

    if (!aSource) {
      return NS_OK;
    }

    nsresult rv = FileUtils::BodyStartWriteStream(mManager->Origin(),
                                                  mManager->BaseDomain(),
                                                  mDBDir,
                                                  mCacheId,
                                                  aSource,
                                                  this,
                                                  AsyncCopyCompleteFunc,
                                                  aIdOut,
                                                  aCopyContextOut);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    return rv;
  }

  void
  CancelStreamCopy(nsIInputStream* aSource, nsISupports* aCopyContext,
                   const nsID& aId)
  {
    if (!aSource || !aCopyContext) {
      return;
    }
    FileUtils::BodyCancelWrite(mDBDir, mCacheId, aId, aCopyContext);
  }

  static void
  AsyncCopyCompleteFunc(void* aClosure, nsresult aRv)
  {
    MOZ_ASSERT(aClosure);
    CachePutAction* action = static_cast<CachePutAction*>(aClosure);
    action->OnAsyncCopyComplete(aRv);
  }

  void
  DoResolve(nsresult aRv)
  {
    if (NS_FAILED(aRv)) {
      CancelStreamCopy(mRequestBodyStream, mRequestBodyCopyContext,
                       mRequestBodyId);
      CancelStreamCopy(mResponseBodyStream, mResponseBodyCopyContext,
                       mResponseBodyId);
    }

    mConn = nullptr;
    mRequestBodyCopyContext = nullptr;
    mResponseBodyCopyContext = nullptr;

    nsRefPtr<Resolver> resolver;
    mResolver.swap(resolver);

    if (resolver) {
      // This can trigger self desctruction if a self-ref is not held by the
      // caller.
      resolver->Resolve(aRv);
    }
  }

  nsRefPtr<Manager> mManager;
  const ListenerId mListenerId;
  const RequestId mRequestId;
  const CacheId mCacheId;
  const PCacheRequest mRequest;
  nsCOMPtr<nsIInputStream> mRequestBodyStream;
  const PCacheResponse mResponse;
  nsCOMPtr<nsIInputStream> mResponseBodyStream;
  nsRefPtr<StreamList> mStreamList;
  nsRefPtr<Resolver> mResolver;
  nsCOMPtr<nsIFile> mDBDir;
  nsCOMPtr<mozIStorageConnection> mConn;
  uint32_t mExpectedAsyncCopyCompletions;
  nsID mRequestBodyId;
  nsCOMPtr<nsISupports> mRequestBodyCopyContext;
  nsID mResponseBodyId;
  nsCOMPtr<nsISupports> mResponseBodyCopyContext;
};

class Manager::CacheDeleteAction MOZ_FINAL : public Manager::BaseAction
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

    nsTArray<nsID> deletedBodyIdList;
    nsresult rv = DBSchema::CacheDelete(aConn, mCacheId, mRequest, mParams,
                                        deletedBodyIdList, &mSuccess);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = FileUtils::BodyDeleteFiles(aDBDir, mCacheId, deletedBodyIdList);
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

  virtual bool MatchesCacheId(CacheId aCacheId) MOZ_OVERRIDE
  {
    return aCacheId == mCacheId;
  }

protected:
  virtual ~CacheDeleteAction() { }
  const CacheId mCacheId;
  const PCacheRequest mRequest;
  const PCacheQueryParams mParams;
  bool mSuccess;
};

class Manager::CacheKeysAction MOZ_FINAL : public Manager::BaseAction
{
public:
  CacheKeysAction(Manager* aManager, ListenerId aListenerId,
                    RequestId aRequestId, CacheId aCacheId,
                    const PCacheRequestOrVoid& aRequestOrVoid,
                    const PCacheQueryParams& aParams,
                    StreamList* aStreamList)
    : BaseAction(aManager, aListenerId, aRequestId)
    , mCacheId(aCacheId)
    , mRequestOrVoid(aRequestOrVoid)
    , mParams(aParams)
    , mStreamList(aStreamList)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    nsresult rv = DBSchema::CacheKeys(aConn, mCacheId, mRequestOrVoid, mParams,
                                      mSavedRequests);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    for (uint32_t i = 0; i < mSavedRequests.Length(); ++i) {
      if (!mSavedRequests[i].mHasBodyId) {
        continue;
      }

      nsCOMPtr<nsIInputStream> stream;
      rv = FileUtils::BodyOpen(mManager->Origin(), mManager->BaseDomain(),
                               aDBDir, mCacheId, mSavedRequests[i].mBodyId,
                               getter_AddRefs(stream));
      if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
      if (NS_WARN_IF(!stream)) { return NS_ERROR_FILE_NOT_FOUND; }

      mStreamList->Add(mSavedRequests[i].mBodyId, stream);
    }

    return rv;
  }

  virtual void
  Complete(Listener* aListener, nsresult aRv) MOZ_OVERRIDE
  {
    mStreamList->Activate(mCacheId);
    aListener->OnCacheKeys(mRequestId, aRv, mSavedRequests, mStreamList);
    mStreamList = nullptr;
  }

  virtual bool MatchesCacheId(CacheId aCacheId) MOZ_OVERRIDE
  {
    return aCacheId == mCacheId;
  }

protected:
  virtual ~CacheKeysAction() { }
  const CacheId mCacheId;
  const PCacheRequestOrVoid mRequestOrVoid;
  const PCacheQueryParams mParams;
  nsRefPtr<StreamList> mStreamList;
  nsTArray<SavedRequest> mSavedRequests;
};

class Manager::StorageMatchAction MOZ_FINAL : public Manager::BaseAction
{
public:
  StorageMatchAction(Manager* aManager, ListenerId aListenerId,
                     RequestId aRequestId, Namespace aNamespace,
                     const PCacheRequest& aRequest,
                     const PCacheQueryParams& aParams,
                     StreamList* aStreamList)
    : BaseAction(aManager, aListenerId, aRequestId)
    , mNamespace(aNamespace)
    , mRequest(aRequest)
    , mParams(aParams)
    , mStreamList(aStreamList)
    , mFoundResponse(false)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    nsresult rv = DBSchema::StorageMatch(aConn, mNamespace, mRequest, mParams,
                                         &mFoundResponse, &mSavedResponse);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    if (!mFoundResponse || !mSavedResponse.mHasBodyId) {
      return rv;
    }

    nsCOMPtr<nsIInputStream> stream;
    rv = FileUtils::BodyOpen(mManager->Origin(), mManager->BaseDomain(),
                             aDBDir, mSavedResponse.mCacheId,
                             mSavedResponse.mBodyId, getter_AddRefs(stream));
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
    if (NS_WARN_IF(!stream)) { return NS_ERROR_FILE_NOT_FOUND; }

    mStreamList->Add(mSavedResponse.mBodyId, stream);

    return rv;
  }

  virtual void
  Complete(Listener* aListener, nsresult aRv) MOZ_OVERRIDE
  {
    if (!mFoundResponse) {
      aListener->OnStorageMatch(mRequestId, aRv, nullptr, nullptr);
    } else {
      mStreamList->Activate(mSavedResponse.mCacheId);
      aListener->OnStorageMatch(mRequestId, aRv, &mSavedResponse, mStreamList);
    }
    mStreamList = nullptr;
  }

protected:
  virtual ~StorageMatchAction() { }
  const Namespace mNamespace;
  const PCacheRequest mRequest;
  const PCacheQueryParams mParams;
  nsRefPtr<StreamList> mStreamList;
  bool mFoundResponse;
  SavedResponse mSavedResponse;
};

class Manager::StorageHasAction : public Manager::BaseAction
{
public:
  StorageHasAction(Manager* aManager, ListenerId aListenerId,
                   RequestId aRequestId, Namespace aNamespace,
                   const nsAString& aKey)
    : BaseAction(aManager, aListenerId, aRequestId)
    , mNamespace(aNamespace)
    , mKey(aKey)
    , mCacheFound(false)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    CacheId cacheId;
    return DBSchema::StorageGetCacheId(aConn, mNamespace, mKey,
                                       &mCacheFound, &cacheId);
  }

  virtual void
  Complete(Listener* aListener, nsresult aRv) MOZ_OVERRIDE
  {
    aListener->OnStorageHas(mRequestId, aRv, mCacheFound);
  }

protected:
  virtual ~StorageHasAction() { }
  const Namespace mNamespace;
  const nsString mKey;
  bool mCacheFound;
};

class Manager::StorageOpenAction MOZ_FINAL : public Manager::BaseAction
{
public:
  StorageOpenAction(Manager* aManager, ListenerId aListenerId,
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
    // Look for existing cache
    bool cacheFound;
    nsresult rv = DBSchema::StorageGetCacheId(aConn, mNamespace, mKey,
                                              &cacheFound, &mCacheId);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
    if (cacheFound) {
      return rv;
    }

    // Cache does not exist, create it instead
    mozStorageTransaction trans(aConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    rv = DBSchema::CreateCache(aConn, &mCacheId);
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
    aListener->OnStorageOpen(mRequestId, aRv, mCacheId);
  }

private:
  virtual ~StorageOpenAction() { }
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
    , mCacheId(0)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    mozStorageTransaction trans(aConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    bool exists;
    nsresult rv = DBSchema::StorageGetCacheId(aConn, mNamespace, mKey, &exists,
                                              &mCacheId);
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
    if (mCacheDeleted) {
      // If nothing is actively referencing this cache, then delete it
      // completely from the database and filesystem.
      uint32_t cacheRefCount = mManager->GetCacheIdRefCount(mCacheId);
      if (cacheRefCount < 1) {
        mManager->CurrentContext()->CancelForCacheId(mCacheId);
        nsRefPtr<Action> action =
          new DeleteOrphanedCacheAction(mManager, mCacheId);
        mManager->CurrentContext()->Dispatch(mManager->mIOThread, action);
      }
    }

    aListener->OnStorageDelete(mRequestId, aRv, mCacheDeleted);
  }

private:
  virtual ~StorageDeleteAction() { }
  const Namespace mNamespace;
  const nsString mKey;
  bool mCacheDeleted;
  CacheId mCacheId;
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

Manager::StreamList::StreamList(Manager* aManager, Context* aContext)
  : mManager(aManager)
  , mContext(aContext)
  , mCacheId(0)
  , mStreamControl(nullptr)
  , mActivated(false)
{
  MOZ_ASSERT(mManager);
  MOZ_ASSERT(mContext);
}

void
Manager::StreamList::SetStreamControl(StreamControl* aStreamControl)
{
  NS_ASSERT_OWNINGTHREAD(Manager::StreamList);
  MOZ_ASSERT(!mStreamControl);
  MOZ_ASSERT(aStreamControl);
  mStreamControl = aStreamControl;
  mStreamControl->SetStreamList(this);
}

void
Manager::StreamList::RemoveStreamControl(StreamControl* aStreamControl)
{
  NS_ASSERT_OWNINGTHREAD(Manager::StreamList);
  MOZ_ASSERT(mStreamControl);
  mStreamControl = nullptr;
}

void
Manager::StreamList::Activate(CacheId aCacheId)
{
  NS_ASSERT_OWNINGTHREAD(Manager::StreamList);
  MOZ_ASSERT(!mActivated);
  MOZ_ASSERT(!mCacheId);
  mActivated = true;
  mCacheId = aCacheId;
  mManager->AddRefCacheId(mCacheId);
  mManager->AddStreamList(this);
}

void
Manager::StreamList::Add(const nsID& aId, nsIInputStream* aStream)
{
  // All streams should be added on IO thread before we set the stream
  // control on the owning IPC thread.
  MOZ_ASSERT(!mStreamControl);
  MOZ_ASSERT(aStream);
  Entry* entry = mList.AppendElement();
  entry->mId = aId;
  entry->mStream = aStream;
}

already_AddRefed<nsIInputStream>
Manager::StreamList::Extract(const nsID& aId)
{
  NS_ASSERT_OWNINGTHREAD(Manager::StreamList);
  for (uint32_t i = 0; i < mList.Length(); ++i) {
    if (mList[i].mId == aId) {
      return mList[i].mStream.forget();
    }
  }
  return nullptr;
}

void
Manager::StreamList::NoteClosed(const nsID& aId)
{
  NS_ASSERT_OWNINGTHREAD(Manager::StreamList);
  for (uint32_t i = 0; i < mList.Length(); ++i) {
    if (mList[i].mId == aId) {
      mList.RemoveElementAt(i);
      break;
    }
  }

  if (mList.IsEmpty() && mStreamControl) {
    mStreamControl->Shutdown();
  }
}

void
Manager::StreamList::Close(const nsID& aId)
{
  NS_ASSERT_OWNINGTHREAD(Manager::StreamList);
  if (mStreamControl) {
    mStreamControl->Close(aId);
  }
}

void
Manager::StreamList::CloseAll()
{
  NS_ASSERT_OWNINGTHREAD(Manager::StreamList);
  if (mStreamControl) {
    mStreamControl->CloseAll();
  }
}

Manager::StreamList::~StreamList()
{
  NS_ASSERT_OWNINGTHREAD(Manager::StreamList);
  MOZ_ASSERT(!mStreamControl);
  if (mActivated) {
    mManager->RemoveStreamList(this);
    mManager->ReleaseCacheId(mCacheId);
  }
}

// static
already_AddRefed<Manager>
Manager::ForOrigin(const nsACString& aOrigin, const nsACString& aBaseDomain)
{
  mozilla::ipc::AssertIsOnBackgroundThread();
  return Factory::Instance().GetOrCreate(aOrigin, aBaseDomain);
}

// static
already_AddRefed<Manager>
Manager::ForExistingOrigin(const nsACString& aOrigin)
{
  mozilla::ipc::AssertIsOnBackgroundThread();
  return Factory::Instance().Get(aOrigin);
}

void
Manager::RemoveListener(Listener* aListener)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  mListeners.RemoveElement(aListener);
}

void
Manager::AddRefCacheId(CacheId aCacheId)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
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
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  for (uint32_t i = 0; i < mCacheIdRefs.Length(); ++i) {
    if (mCacheIdRefs[i].mCacheId == aCacheId) {
      DebugOnly<uint32_t> oldRef = mCacheIdRefs[i].mCount;
      mCacheIdRefs[i].mCount -= 1;
      MOZ_ASSERT(mCacheIdRefs[i].mCount < oldRef);
      if (mCacheIdRefs[i].mCount < 1) {
        mCacheIdRefs.RemoveElementAt(i);
        // TODO: note that we need to check this cache for staleness on startup
        if (!mShuttingDown) {
          nsRefPtr<Action> action = new CheckCacheOrphanedAction(this, aCacheId);
          CurrentContext()->Dispatch(mIOThread, action);
        }
      }
      return;
    }
  }
  MOZ_ASSERT_UNREACHABLE("Attempt to release CacheId that is not referenced!");
}

uint32_t
Manager::GetCacheIdRefCount(CacheId aCacheId)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  for (uint32_t i = 0; i < mCacheIdRefs.Length(); ++i) {
    if (mCacheIdRefs[i].mCacheId == aCacheId) {
      MOZ_ASSERT(mCacheIdRefs[i].mCount > 0);
      return mCacheIdRefs[i].mCount;
    }
  }
  return 0;
}

void
Manager::Shutdown()
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  mShuttingDown = true;
  for (uint32_t i = 0; i < mStreamLists.Length(); ++i) {
    mStreamLists[i]->CloseAll();
  }

  // If there is no context, then note that we're done shutting down
  if (!mContext) {
    nsRefPtr<ShutdownObserver> so = ShutdownObserver::Instance();
    if (so) {
      so->RemoveOrigin(mOrigin);
    }

  // Otherwise, cancel the context and note complete when it cleans up
  } else {
    mContext->CancelAll();
  }
}

void
Manager::CacheMatch(Listener* aListener, RequestId aRequestId, CacheId aCacheId,
                    const PCacheRequest& aRequest,
                    const PCacheQueryParams& aParams)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  MOZ_ASSERT(aListener);
  if (mShuttingDown) {
    aListener->OnCacheMatch(aRequestId, NS_ERROR_ILLEGAL_DURING_SHUTDOWN,
                            nullptr, nullptr);
    return;
  }
  nsRefPtr<StreamList> streamList = new StreamList(this, CurrentContext());
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new CacheMatchAction(this, listenerId, aRequestId,
                                                 aCacheId, aRequest, aParams,
                                                 streamList);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::CacheMatchAll(Listener* aListener, RequestId aRequestId,
                       CacheId aCacheId, const PCacheRequestOrVoid& aRequest,
                       const PCacheQueryParams& aParams)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  MOZ_ASSERT(aListener);
  if (mShuttingDown) {
    aListener->OnCacheMatchAll(aRequestId, NS_ERROR_ILLEGAL_DURING_SHUTDOWN,
                               nsTArray<SavedResponse>(), nullptr);
    return;
  }
  nsRefPtr<StreamList> streamList = new StreamList(this, CurrentContext());
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new CacheMatchAllAction(this, listenerId, aRequestId,
                                                    aCacheId, aRequest, aParams,
                                                    streamList);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::CachePut(Listener* aListener, RequestId aRequestId, CacheId aCacheId,
                  const PCacheRequest& aRequest,
                  nsIInputStream* aRequestBodyStream,
                  const PCacheResponse& aResponse,
                  nsIInputStream* aResponseBodyStream)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  MOZ_ASSERT(aListener);
  if (mShuttingDown) {
    aListener->OnCachePut(aRequestId, NS_ERROR_ILLEGAL_DURING_SHUTDOWN);
    return;
  }
  nsRefPtr<StreamList> streamList = new StreamList(this, CurrentContext());
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new CachePutAction(this, listenerId, aRequestId,
                                               aCacheId,
                                               aRequest, aRequestBodyStream,
                                               aResponse, aResponseBodyStream,
                                               streamList);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::CacheDelete(Listener* aListener, RequestId aRequestId,
                     CacheId aCacheId, const PCacheRequest& aRequest,
                     const PCacheQueryParams& aParams)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  MOZ_ASSERT(aListener);
  if (mShuttingDown) {
    aListener->OnCacheDelete(aRequestId, NS_ERROR_ILLEGAL_DURING_SHUTDOWN, false);
    return;
  }
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new CacheDeleteAction(this, listenerId, aRequestId,
                                                  aCacheId, aRequest, aParams);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::CacheKeys(Listener* aListener, RequestId aRequestId,
                   CacheId aCacheId, const PCacheRequestOrVoid& aRequestOrVoid,
                   const PCacheQueryParams& aParams)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  MOZ_ASSERT(aListener);
  if (mShuttingDown) {
    aListener->OnCacheKeys(aRequestId, NS_ERROR_ILLEGAL_DURING_SHUTDOWN,
                           nsTArray<SavedRequest>(), nullptr);
    return;
  }
  nsRefPtr<StreamList> streamList = new StreamList(this, CurrentContext());
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new CacheKeysAction(this, listenerId, aRequestId,
                                                aCacheId, aRequestOrVoid,
                                                aParams, streamList);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::StorageMatch(Listener* aListener, RequestId aRequestId,
                      Namespace aNamespace, const PCacheRequest& aRequest,
                      const PCacheQueryParams& aParams)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  MOZ_ASSERT(aListener);
  if (mShuttingDown) {
    aListener->OnStorageMatch(aRequestId, NS_ERROR_ILLEGAL_DURING_SHUTDOWN,
                              nullptr, nullptr);
    return;
  }
  nsRefPtr<StreamList> streamList = new StreamList(this, CurrentContext());
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new StorageMatchAction(this, listenerId, aRequestId,
                                                   aNamespace, aRequest,
                                                   aParams, streamList);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::StorageHas(Listener* aListener, RequestId aRequestId,
                    Namespace aNamespace, const nsAString& aKey)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  MOZ_ASSERT(aListener);
  if (mShuttingDown) {
    aListener->OnStorageHas(aRequestId, NS_ERROR_ILLEGAL_DURING_SHUTDOWN,
                            false);
    return;
  }
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new StorageHasAction(this, listenerId, aRequestId,
                                                 aNamespace, aKey);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::StorageOpen(Listener* aListener, RequestId aRequestId,
                     Namespace aNamespace, const nsAString& aKey)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  MOZ_ASSERT(aListener);
  if (mShuttingDown) {
    aListener->OnStorageOpen(aRequestId, NS_ERROR_ILLEGAL_DURING_SHUTDOWN, 0);
    return;
  }
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new StorageOpenAction(this, listenerId, aRequestId,
                                                  aNamespace, aKey);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::StorageDelete(Listener* aListener, RequestId aRequestId,
                       Namespace aNamespace, const nsAString& aKey)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  MOZ_ASSERT(aListener);
  if (mShuttingDown) {
    aListener->OnStorageDelete(aRequestId, NS_ERROR_ILLEGAL_DURING_SHUTDOWN,
                               false);
    return;
  }
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new StorageDeleteAction(this, listenerId, aRequestId,
                                                    aNamespace, aKey);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::StorageKeys(Listener* aListener, RequestId aRequestId,
                     Namespace aNamespace)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  MOZ_ASSERT(aListener);
  if (mShuttingDown) {
    aListener->OnStorageKeys(aRequestId, NS_ERROR_ILLEGAL_DURING_SHUTDOWN,
                             nsTArray<nsString>());
    return;
  }
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new StorageKeysAction(this, listenerId, aRequestId,
                                                  aNamespace);
  CurrentContext()->Dispatch(mIOThread, action);
}

void
Manager::RemoveContext(Context* aContext)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  MOZ_ASSERT(mContext);
  MOZ_ASSERT(mContext == aContext);
  mContext = nullptr;

  if (mShuttingDown) {
    nsRefPtr<ShutdownObserver> so = ShutdownObserver::Instance();
    if (so) {
      so->RemoveOrigin(mOrigin);
    }
  }
}

Manager::Manager(const nsACString& aOrigin, const nsACString& aBaseDomain)
  : mOrigin(aOrigin)
  , mBaseDomain(aBaseDomain)
  , mContext(nullptr)
  , mShuttingDown(false)
{
  nsresult rv = NS_NewNamedThread("DOMCacheThread",
                                  getter_AddRefs(mIOThread));
  if (NS_FAILED(rv)) {
    MOZ_CRASH("Failed to spawn cache manager IO thread.");
  }

  nsRefPtr<ShutdownObserver> so = ShutdownObserver::Instance();
  if (so) {
    so->AddOrigin(mOrigin);
  } else {
    Shutdown();
  }
}

Manager::~Manager()
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  Shutdown();
  Factory::Instance().Remove(this);
  mIOThread->Shutdown();
}

Context*
Manager::CurrentContext()
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  if (!mContext) {
    MOZ_ASSERT(!mShuttingDown);
    nsRefPtr<Action> setupAction = new SetupAction(mOrigin, mBaseDomain);
    mContext = new Context(this, mOrigin, mBaseDomain, setupAction);
  }
  return mContext;
}

Manager::ListenerId
Manager::SaveListener(Listener* aListener)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
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
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  for (uint32_t i = 0; i < mListeners.Length(); ++i) {
    if (reinterpret_cast<ListenerId>(mListeners[i]) == aListenerId) {
      return mListeners[i];
    }
  }
  return nullptr;
}

void
Manager::AddStreamList(StreamList* aStreamList)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  MOZ_ASSERT(aStreamList);
  mStreamLists.AppendElement(aStreamList);
}

void
Manager::RemoveStreamList(StreamList* aStreamList)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  MOZ_ASSERT(aStreamList);
  mStreamLists.RemoveElement(aStreamList);
}

} // namespace cache
} // namespace dom
} // namespace mozilla
