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

using mozilla::dom::cache::CacheInitData;
using mozilla::dom::cache::DBSchema;
using mozilla::dom::cache::FileUtils;
using mozilla::dom::cache::SyncDBAction;

class SetupAction MOZ_FINAL : public SyncDBAction
{
public:
  SetupAction(const CacheInitData& aInitData)
    : SyncDBAction(DBAction::Create, aInitData)
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

  already_AddRefed<Manager> GetOrCreate(const CacheInitData& aInitData)
  {
    mozilla::ipc::AssertIsOnBackgroundThread();

    nsRefPtr<Manager> ref = Get(aInitData.origin());
    if (!ref) {
      ref = new Manager(aInitData);
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
    : SyncDBAction(DBAction::Existing, aManager->mInitData)
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

class Manager::DeleteOrphanedBodyAction MOZ_FINAL : public Action
{
public:
  DeleteOrphanedBodyAction(const nsTArray<nsID>& aDeletedBodyIdList)
    : mDeletedBodyIdList(aDeletedBodyIdList)
  { }

  DeleteOrphanedBodyAction(const nsID& aBodyId)
  {
    mDeletedBodyIdList.AppendElement(aBodyId);
  }

  virtual void
  RunOnTarget(Resolver* aResolver, nsIFile* aQuotaDir) MOZ_OVERRIDE
  {
    MOZ_ASSERT(aResolver);
    MOZ_ASSERT(aQuotaDir);

    nsresult rv = aQuotaDir->Append(NS_LITERAL_STRING("cache"));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      aResolver->Resolve(rv);
      return;
    }

    rv = FileUtils::BodyDeleteFiles(aQuotaDir, mDeletedBodyIdList);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      aResolver->Resolve(rv);
      return;
    }

    aResolver->Resolve(rv);
  }

private:
  virtual ~DeleteOrphanedBodyAction() { }
  nsTArray<nsID> mDeletedBodyIdList;
};

class Manager::DeleteOrphanedCacheAction MOZ_FINAL : public SyncDBAction
{
public:
  DeleteOrphanedCacheAction(Manager* aManager, CacheId aCacheId)
    : SyncDBAction(DBAction::Existing, aManager->mInitData)
    , mManager(aManager)
    , mCacheId(aCacheId)
  { }

  virtual nsresult
  RunSyncWithDBOnTarget(nsIFile* aDBDir,
                        mozIStorageConnection* aConn) MOZ_OVERRIDE
  {
    mozStorageTransaction trans(aConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    nsresult rv = DBSchema::DeleteCache(aConn, mCacheId, mDeletedBodyIdList);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = trans.Commit();
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    return rv;
  }

  virtual void
  CompleteOnInitiatingThread(nsresult aRv) MOZ_OVERRIDE
  {
    mManager->NoteOrphanedBodyIdList(mDeletedBodyIdList);
    mManager = nullptr;
  }

private:
  virtual ~DeleteOrphanedCacheAction() { }
  nsRefPtr<Manager> mManager;
  const CacheId mCacheId;
  nsTArray<nsID> mDeletedBodyIdList;
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
    rv = FileUtils::BodyOpen(mManager->mInitData, aDBDir, mResponse.mBodyId,
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
      rv = FileUtils::BodyOpen(mManager->mInitData, aDBDir,
                               mSavedResponses[i].mBodyId,
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

class Manager::CachePutAllAction MOZ_FINAL : public DBAction
{
public:
  CachePutAllAction(Manager* aManager, ListenerId aListenerId,
                    RequestId aRequestId, CacheId aCacheId,
                    const nsTArray<CacheRequestResponse>& aPutList,
                    const nsTArray<nsCOMPtr<nsIInputStream>>& aRequestStreamList,
                    const nsTArray<nsCOMPtr<nsIInputStream>>& aResponseStreamList)
    : DBAction(DBAction::Existing, aManager->mInitData)
    , mManager(aManager)
    , mListenerId(aListenerId)
    , mRequestId(aRequestId)
    , mCacheId(aCacheId)
    , mList(aPutList.Length())
    , mExpectedAsyncCopyCompletions(0)
  {
    MOZ_ASSERT(aPutList.Length() == aRequestStreamList.Length());
    MOZ_ASSERT(aPutList.Length() == aResponseStreamList.Length());

    for (uint32_t i = 0; i < aPutList.Length(); ++i) {
      Entry* entry = mList.AppendElement();
      entry->mRequest = aPutList[i].request();
      entry->mRequestStream = aRequestStreamList[i];
      entry->mResponse = aPutList[i].response();
      entry->mResponseStream = aResponseStreamList[i];

      mExpectedAsyncCopyCompletions += entry->mRequestStream ? 1 : 0;
      mExpectedAsyncCopyCompletions += entry->mResponseStream ? 1 : 0;
    }
  }

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

    if (mExpectedAsyncCopyCompletions < 1) {
      mExpectedAsyncCopyCompletions = 1;
      OnAsyncCopyComplete(NS_OK);
      return;
    }

    nsresult rv = NS_OK;
    for (uint32_t i = 0; i < mList.Length(); ++i) {
      rv = StartStreamCopy(mList[i].mRequestStream,
                           &mList[i].mRequestBodyId,
                           getter_AddRefs(mList[i].mRequestCopyContext));
      if (NS_WARN_IF(NS_FAILED(rv))) {
        CancelAllStreamCopying();
        DoResolve(rv);
        return;
      }

      rv = StartStreamCopy(mList[i].mResponseStream,
                           &mList[i].mResponseBodyId,
                           getter_AddRefs(mList[i].mResponseCopyContext));
      if (NS_WARN_IF(NS_FAILED(rv))) {
        CancelAllStreamCopying();
        DoResolve(rv);
        return;
      }
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

    mozStorageTransaction trans(mConn, false,
                                mozIStorageConnection::TRANSACTION_IMMEDIATE);

    nsresult rv = NS_OK;
    for (uint32_t i = 0; i < mList.Length(); ++i) {
      Entry& e = mList[i];
      if (e.mRequestStream) {
        e.mRequestCopyContext = nullptr;
        rv = FileUtils::BodyFinalizeWrite(mDBDir, e.mRequestBodyId);
        if (NS_WARN_IF(NS_FAILED(rv))) {
          DoResolve(rv);
          return;
        }
      }
      if (e.mResponseStream) {
        e.mResponseCopyContext = nullptr;
        rv = FileUtils::BodyFinalizeWrite(mDBDir, e.mResponseBodyId);
        if (NS_WARN_IF(NS_FAILED(rv))) {
          DoResolve(rv);
          return;
        }
      }

      rv = DBSchema::CachePut(mConn, mCacheId, e.mRequest,
                              e.mRequestStream ? &e.mRequestBodyId : nullptr,
                              e.mResponse,
                              e.mResponseStream ? &e.mResponseBodyId : nullptr,
                              mDeletedBodyIdList);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        DoResolve(rv);
        return;
      }
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

    for (uint32_t i = 0; i < mList.Length(); ++i) {
      mList[i].mRequestStream = nullptr;
      mList[i].mResponseStream = nullptr;
    }

    mManager->NoteOrphanedBodyIdList(mDeletedBodyIdList);

    Listener* listener = mManager->GetListener(mListenerId);
    mManager = nullptr;
    if (!listener) {
      return;
    }
    listener->OnCachePutAll(mRequestId, aRv);
  }

  virtual void
  CancelOnTarget() MOZ_OVERRIDE
  {
    CancelAllStreamCopying();
    mConn = nullptr;
    mResolver = nullptr;
  }

  virtual bool MatchesCacheId(CacheId aCacheId) MOZ_OVERRIDE
  {
    return aCacheId == mCacheId;
  }

private:
  virtual ~CachePutAllAction() { }

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

    nsresult rv = FileUtils::BodyStartWriteStream(mManager->mInitData,
                                                  mDBDir,
                                                  aSource,
                                                  this,
                                                  AsyncCopyCompleteFunc,
                                                  aIdOut,
                                                  aCopyContextOut);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    return rv;
  }

  void
  CancelAllStreamCopying()
  {
    for (uint32_t i = 0; i < mList.Length(); ++i) {
      Entry& e = mList[i];
      if (e.mRequestStream && e.mRequestCopyContext) {
        CancelStreamCopy(e.mRequestStream, e.mRequestCopyContext,
                         e.mRequestBodyId);
        e.mRequestCopyContext = nullptr;
      }
      if (e.mResponseStream && e.mResponseCopyContext) {
        CancelStreamCopy(e.mResponseStream, e.mResponseCopyContext,
                         e.mResponseBodyId);
        e.mResponseCopyContext = nullptr;
      }
    }
  }

  void
  CancelStreamCopy(nsIInputStream* aSource, nsISupports* aCopyContext,
                   const nsID& aId)
  {
    if (!aSource || !aCopyContext) {
      return;
    }
    FileUtils::BodyCancelWrite(mDBDir, aId, aCopyContext);
  }

  static void
  AsyncCopyCompleteFunc(void* aClosure, nsresult aRv)
  {
    MOZ_ASSERT(aClosure);
    CachePutAllAction* action = static_cast<CachePutAllAction*>(aClosure);
    action->OnAsyncCopyComplete(aRv);
  }

  void
  DoResolve(nsresult aRv)
  {
    if (NS_FAILED(aRv)) {
      CancelAllStreamCopying();
    }

    mConn = nullptr;

    nsRefPtr<Resolver> resolver;
    mResolver.swap(resolver);

    if (resolver) {
      // This can trigger self destruction if a self-ref is not held by the
      // caller.
      resolver->Resolve(aRv);
    }
  }

  struct Entry
  {
    PCacheRequest mRequest;
    nsCOMPtr<nsIInputStream> mRequestStream;
    nsID mRequestBodyId;
    nsCOMPtr<nsISupports> mRequestCopyContext;

    PCacheResponse mResponse;
    nsCOMPtr<nsIInputStream> mResponseStream;
    nsID mResponseBodyId;
    nsCOMPtr<nsISupports> mResponseCopyContext;
  };

  nsRefPtr<Manager> mManager;
  const ListenerId mListenerId;
  const RequestId mRequestId;
  const CacheId mCacheId;
  nsTArray<Entry> mList;
  nsRefPtr<Resolver> mResolver;
  nsCOMPtr<nsIFile> mDBDir;
  nsCOMPtr<mozIStorageConnection> mConn;
  uint32_t mExpectedAsyncCopyCompletions;
  nsTArray<nsID> mDeletedBodyIdList;
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

    nsresult rv = DBSchema::CacheDelete(aConn, mCacheId, mRequest, mParams,
                                        mDeletedBodyIdList, &mSuccess);
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
    mManager->NoteOrphanedBodyIdList(mDeletedBodyIdList);
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
  nsTArray<nsID> mDeletedBodyIdList;
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
      rv = FileUtils::BodyOpen(mManager->mInitData, aDBDir,
                               mSavedRequests[i].mBodyId,
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
    rv = FileUtils::BodyOpen(mManager->mInitData, aDBDir,
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
      // If content is referencing this cache, mark it orphaned to be
      // deleted later.
      if (!mManager->SetCacheIdOrphanedIfRefed(mCacheId)) {

        // no outstanding references, delete immediately
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
  MOZ_ASSERT(aStreamControl);

  // For cases where multiple streams are serialized for a single list
  // then the control will get passed multiple times.  This ok, but
  // it should be the same control each time.
  if (mStreamControl) {
    MOZ_ASSERT(aStreamControl == mStreamControl);
    return;
  }

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

  for (uint32_t i = 0; i < mList.Length(); ++i) {
    mManager->AddRefBodyId(mList[i].mId);
  }
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
      mManager->ReleaseBodyId(aId);
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
    for (uint32_t i = 0; i < mList.Length(); ++i) {
      mManager->ReleaseBodyId(mList[i].mId);
    }
    mManager->ReleaseCacheId(mCacheId);
  }
}

// static
already_AddRefed<Manager>
Manager::ForOrigin(const CacheInitData& aInitData)
{
  mozilla::ipc::AssertIsOnBackgroundThread();
  return Factory::Instance().GetOrCreate(aInitData);
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
  entry->mOrphaned = false;
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
        bool orphaned = mCacheIdRefs[i].mOrphaned;
        mCacheIdRefs.RemoveElementAt(i);
        // TODO: note that we need to check this cache for staleness on startup
        if (orphaned && !mShuttingDown) {
          CurrentContext()->CancelForCacheId(aCacheId);
          nsRefPtr<Action> action = new DeleteOrphanedCacheAction(this,
                                                                  aCacheId);
          CurrentContext()->Dispatch(mIOThread, action);
        }
      }
      return;
    }
  }
  MOZ_ASSERT_UNREACHABLE("Attempt to release CacheId that is not referenced!");
}

bool
Manager::SetCacheIdOrphanedIfRefed(CacheId aCacheId)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  for (uint32_t i = 0; i < mCacheIdRefs.Length(); ++i) {
    if (mCacheIdRefs[i].mCacheId == aCacheId) {
      MOZ_ASSERT(mCacheIdRefs[i].mCount > 0);
      MOZ_ASSERT(!mCacheIdRefs[i].mOrphaned);
      mCacheIdRefs[i].mOrphaned = true;
      return true;
    }
  }
  return false;
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
      so->RemoveOrigin(mInitData.origin());
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
Manager::CachePutAll(Listener* aListener, RequestId aRequestId, CacheId aCacheId,
                     const nsTArray<CacheRequestResponse>& aPutList,
                     const nsTArray<nsCOMPtr<nsIInputStream>>& aRequestStreamList,
                     const nsTArray<nsCOMPtr<nsIInputStream>>& aResponseStreamList)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  MOZ_ASSERT(aListener);
  if (mShuttingDown) {
    aListener->OnCachePutAll(aRequestId, NS_ERROR_ILLEGAL_DURING_SHUTDOWN);
    return;
  }
  ListenerId listenerId = SaveListener(aListener);
  nsRefPtr<Action> action = new CachePutAllAction(this, listenerId, aRequestId,
                                                  aCacheId, aPutList,
                                                  aRequestStreamList,
                                                  aResponseStreamList);
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
      so->RemoveOrigin(mInitData.origin());
    }
  }
}

Manager::Manager(const CacheInitData& aInitData)
  : mInitData(aInitData)
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
    so->AddOrigin(mInitData.origin());
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
    nsRefPtr<Action> setupAction = new SetupAction(mInitData);
    mContext = new Context(this, mInitData, setupAction);
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

void
Manager::AddRefBodyId(const nsID& aBodyId)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  for (uint32_t i = 0; i < mBodyIdRefs.Length(); ++i) {
    if (mBodyIdRefs[i].mBodyId == aBodyId) {
      mBodyIdRefs[i].mCount += 1;
      return;
    }
  }
  BodyIdRefCounter* entry = mBodyIdRefs.AppendElement();
  entry->mBodyId = aBodyId;
  entry->mCount = 1;
  entry->mOrphaned = false;
}

void
Manager::ReleaseBodyId(const nsID& aBodyId)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  for (uint32_t i = 0; i < mBodyIdRefs.Length(); ++i) {
    if (mBodyIdRefs[i].mBodyId == aBodyId) {
      DebugOnly<uint32_t> oldRef = mBodyIdRefs[i].mCount;
      mBodyIdRefs[i].mCount -= 1;
      MOZ_ASSERT(mBodyIdRefs[i].mCount < oldRef);
      if (mBodyIdRefs[i].mCount < 1) {
        bool orphaned = mBodyIdRefs[i].mOrphaned;
        mBodyIdRefs.RemoveElementAt(i);
        // TODO: note that we need to check this body for staleness on startup
        if (orphaned && !mShuttingDown) {
          nsRefPtr<Action> action = new DeleteOrphanedBodyAction(aBodyId);
          CurrentContext()->Dispatch(mIOThread, action);
        }
      }
      return;
    }
  }
  MOZ_ASSERT_UNREACHABLE("Attempt to release BodyId that is not referenced!");
}

// TODO: provide way to set body non-orphaned if its added back to a cache
//       once same-origin de-duplication is implemented

bool
Manager::SetBodyIdOrphanedIfRefed(const nsID& aBodyId)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  for (uint32_t i = 0; i < mBodyIdRefs.Length(); ++i) {
    if (mBodyIdRefs[i].mBodyId == aBodyId) {
      MOZ_ASSERT(mBodyIdRefs[i].mCount > 0);
      MOZ_ASSERT(!mBodyIdRefs[i].mOrphaned);
      mBodyIdRefs[i].mOrphaned = true;
      return true;
    }
  }
  return false;
}

void
Manager::NoteOrphanedBodyIdList(const nsTArray<nsID>& aDeletedBodyIdList)
{
  NS_ASSERT_OWNINGTHREAD(Context::Listener);
  nsTArray<nsID> deleteNowList;
  for (uint32_t i = 0; i < aDeletedBodyIdList.Length(); ++i) {
    if (!SetBodyIdOrphanedIfRefed(aDeletedBodyIdList[i])) {
      deleteNowList.AppendElement(aDeletedBodyIdList[i]);
    }
  }

  if (!deleteNowList.IsEmpty()) {
    nsRefPtr<Action> action = new DeleteOrphanedBodyAction(deleteNowList);
    CurrentContext()->Dispatch(mIOThread, action);
  }
}

} // namespace cache
} // namespace dom
} // namespace mozilla
