/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDBTransaction.h"

#include "ActorsChild.h"
#include "BackgroundChildImpl.h"
#include "FileInfo.h"
#include "IDBDatabase.h"
#include "IDBEvents.h"
#include "IDBObjectStore.h"
#include "IDBRequest.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/EventDispatcher.h"
#include "mozilla/dom/DOMError.h"
#include "mozilla/dom/DOMStringList.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "nsIAppShell.h"
#include "nsPIDOMWindow.h"
#include "nsWidgetsCID.h"
#include "ProfilerHelpers.h"
#include "ReportInternalError.h"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::indexedDB;

namespace {

NS_DEFINE_CID(kAppShellCID, NS_APPSHELL_CID);

} // anonymous namespace

IDBTransaction::IDBTransaction(IDBDatabase* aDatabase, Mode aMode)
  : IDBWrapperCache(aDatabase)
  , mDatabase(aDatabase)
  , mNextObjectStoreId(0)
  , mNextIndexId(0)
  , mAbortCode(NS_OK)
  , mPendingRequestCount(0)
  , mReadyState(IDBTransaction::INITIAL)
  , mMode(aMode)
  , mCreating(false)
#ifdef DEBUG
  , mFiredCompleteOrAbort(false)
#endif
{
  MOZ_ASSERT(aDatabase);
  aDatabase->AssertIsOnOwningThread();

  mBackgroundActor.mNormalBackgroundActor = nullptr;

#ifdef MOZ_ENABLE_PROFILER_SPS
  {
    using namespace mozilla::ipc;
    BackgroundChildImpl::ThreadLocal* threadLocal =
      BackgroundChildImpl::GetThreadLocalForCurrentThread();
    MOZ_ASSERT(threadLocal);

    mSerialNumber = threadLocal->mNextTransactionSerialNumber++;
  }
#endif
}

IDBTransaction::~IDBTransaction()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(!mPendingRequestCount);
  MOZ_ASSERT(!mCreating);
  MOZ_ASSERT(mFiredCompleteOrAbort);

  if (mBackgroundActor.mNormalBackgroundActor) {
    mBackgroundActor.mNormalBackgroundActor->SendDeleteMe();
    mBackgroundActor.mNormalBackgroundActor = nullptr;
  } else if (mBackgroundActor.mVersionChangeBackgroundActor) {
    mBackgroundActor.mVersionChangeBackgroundActor->SendDeleteMe();
    mBackgroundActor.mVersionChangeBackgroundActor = nullptr;
  }
}

// static
already_AddRefed<IDBTransaction>
IDBTransaction::CreateVersionChange(
                                IDBDatabase* aDatabase,
                                BackgroundVersionChangeTransactionChild* aActor,
                                int64_t aNextObjectStoreId,
                                int64_t aNextIndexId)
{
  MOZ_ASSERT(aDatabase);
  aDatabase->AssertIsOnOwningThread();
  MOZ_ASSERT(aActor);
  MOZ_ASSERT(aNextObjectStoreId > 0);
  MOZ_ASSERT(aNextIndexId > 0);

  nsRefPtr<IDBTransaction> transaction =
    new IDBTransaction(aDatabase, VERSION_CHANGE);

  transaction->SetScriptOwner(aDatabase->GetScriptOwner());
  transaction->mBackgroundActor.mVersionChangeBackgroundActor = aActor;
  transaction->mNextObjectStoreId = aNextObjectStoreId;
  transaction->mNextIndexId = aNextIndexId;

  // XXX Fix!
  MOZ_ASSERT(NS_IsMainThread(), "This won't work on non-main threads!");

  nsCOMPtr<nsIAppShell> appShell = do_GetService(kAppShellCID);
  if (NS_WARN_IF(!appShell) ||
      NS_WARN_IF(NS_FAILED(appShell->RunBeforeNextEvent(transaction)))) {
    return nullptr;
  }

  transaction->mCreating = true;

  aDatabase->RegisterTransaction(transaction);

  return transaction.forget();
}

// static
already_AddRefed<IDBTransaction>
IDBTransaction::Create(IDBDatabase* aDatabase,
                       const nsTArray<nsString>& aObjectStoreNames,
                       Mode aMode)
{
  MOZ_ASSERT(aDatabase);
  aDatabase->AssertIsOnOwningThread();
  MOZ_ASSERT(!aObjectStoreNames.IsEmpty());
  MOZ_ASSERT(aMode == READ_ONLY || aMode == READ_WRITE);

  nsRefPtr<IDBTransaction> transaction = new IDBTransaction(aDatabase, aMode);

  transaction->SetScriptOwner(aDatabase->GetScriptOwner());

  const uint32_t objectStoreCount = aObjectStoreNames.Length();

  transaction->mObjectStoreNames.SetCapacity(objectStoreCount);

  for (uint32_t index = 0; index < objectStoreCount; index++) {
    transaction->mObjectStoreNames.InsertElementSorted(
      aObjectStoreNames[index]);
  }

  // XXX Fix!
  MOZ_ASSERT(NS_IsMainThread(), "This won't work on non-main threads!");

  nsCOMPtr<nsIAppShell> appShell = do_GetService(kAppShellCID);
  if (NS_WARN_IF(!appShell) ||
      NS_WARN_IF(NS_FAILED(appShell->RunBeforeNextEvent(transaction)))) {
    return nullptr;
  }

  transaction->mCreating = true;

  aDatabase->RegisterTransaction(transaction);

  return transaction.forget();
}

// static
IDBTransaction*
IDBTransaction::GetCurrent()
{
  using namespace mozilla::ipc;

  MOZ_ASSERT(BackgroundChild::GetForCurrentThread());

  BackgroundChildImpl::ThreadLocal* threadLocal =
    BackgroundChildImpl::GetThreadLocalForCurrentThread();
  MOZ_ASSERT(threadLocal);

  return threadLocal->mCurrentTransaction;
}

#ifdef DEBUG

void
IDBTransaction::AssertIsOnOwningThread() const
{
  MOZ_ASSERT(mDatabase);
  mDatabase->AssertIsOnOwningThread();
}

#endif // DEBUG

void
IDBTransaction::SetBackgroundActor(BackgroundTransactionChild* aBackgroundActor)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aBackgroundActor);
  MOZ_ASSERT(!mBackgroundActor.mNormalBackgroundActor);
  MOZ_ASSERT(mMode != VERSION_CHANGE);

  mBackgroundActor.mNormalBackgroundActor = aBackgroundActor;
}

void
IDBTransaction::StartRequest(BackgroundRequestChild* aBackgroundActor,
                             const RequestParams& aParams)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aBackgroundActor);
  MOZ_ASSERT(aParams.type() != RequestParams::T__None);

  if (mMode == VERSION_CHANGE) {
    MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);

    mBackgroundActor.mVersionChangeBackgroundActor->
      SendPBackgroundIDBRequestConstructor(aBackgroundActor, aParams);
  } else {
    MOZ_ASSERT(mBackgroundActor.mNormalBackgroundActor);

    mBackgroundActor.mNormalBackgroundActor->
      SendPBackgroundIDBRequestConstructor(aBackgroundActor, aParams);
  }
}

void
IDBTransaction::RefreshSpec()
{
  AssertIsOnOwningThread();

  for (uint32_t count = mObjectStores.Length(), index = 0;
       index < count;
       index++) {
    mObjectStores[index]->RefreshSpec();
  }
}

void
IDBTransaction::OnNewRequest()
{
  AssertIsOnOwningThread();

  if (!mPendingRequestCount) {
    MOZ_ASSERT(INITIAL == mReadyState);
    mReadyState = LOADING;
  }

  ++mPendingRequestCount;
}

void
IDBTransaction::OnRequestFinished()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mPendingRequestCount);

  --mPendingRequestCount;

  if (!mPendingRequestCount) {
    MOZ_ASSERT(NS_FAILED(mAbortCode) || LOADING == mReadyState);
    mReadyState = COMMITTING;

    if (NS_SUCCEEDED(mAbortCode)) {
      SendCommit();
    } else {
      SendAbort(mAbortCode);
    }
  }
}

void
IDBTransaction::SendCommit()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(NS_SUCCEEDED(mAbortCode));

  if (mMode == VERSION_CHANGE) {
    MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);
    mBackgroundActor.mVersionChangeBackgroundActor->SendCommit();
  } else {
    MOZ_ASSERT(mBackgroundActor.mNormalBackgroundActor);
    mBackgroundActor.mNormalBackgroundActor->SendCommit();
  }
}

void
IDBTransaction::SendAbort(nsresult aResultCode)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(NS_FAILED(aResultCode));

  if (mMode == VERSION_CHANGE) {
    MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);
    mBackgroundActor.mVersionChangeBackgroundActor->SendAbort(aResultCode);
  } else {
    MOZ_ASSERT(mBackgroundActor.mNormalBackgroundActor);
    mBackgroundActor.mNormalBackgroundActor->SendAbort(aResultCode);
  }
}

bool
IDBTransaction::IsOpen() const
{
  AssertIsOnOwningThread();

  // If we haven't started anything then we're open.
  if (mReadyState == IDBTransaction::INITIAL) {
    return true;
  }

  // If we've already started then we need to check to see if we still have the
  // mCreating flag set. If we do (i.e. we haven't returned to the event loop
  // from the time we were created) then we are open. Otherwise check the
  // currently running transaction to see if it's the same. We only allow other
  // requests to be made if this transaction is currently running.
  if (mReadyState == IDBTransaction::LOADING &&
      (mCreating || GetCurrent() == this)) {
    return true;
  }

  return false;
}

already_AddRefed<IDBObjectStore>
IDBTransaction::CreateObjectStore(const ObjectStoreSpec& aSpec)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aSpec.metadata().id());
  MOZ_ASSERT(VERSION_CHANGE == mMode);
  MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);
  MOZ_ASSERT(IsOpen());

#ifdef DEBUG
  {
    const nsString& name = aSpec.metadata().name();

    for (uint32_t count = mObjectStores.Length(), index = 0;
         index < count;
         index++) {
      MOZ_ASSERT(mObjectStores[index]->Name() != name);
    }
  }
#endif

  MOZ_ALWAYS_TRUE(mBackgroundActor.mVersionChangeBackgroundActor->
                    SendCreateObjectStore(aSpec.metadata()));

  nsRefPtr<IDBObjectStore> objectStore = IDBObjectStore::Create(this, aSpec);
  MOZ_ASSERT(objectStore);

  mObjectStores.AppendElement(objectStore);

  return objectStore.forget();
}

void
IDBTransaction::DeleteObjectStore(int64_t aObjectStoreId)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aObjectStoreId);
  MOZ_ASSERT(VERSION_CHANGE == mMode);
  MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);
  MOZ_ASSERT(IsOpen());

  MOZ_ALWAYS_TRUE(mBackgroundActor.mVersionChangeBackgroundActor->
                    SendDeleteObjectStore(aObjectStoreId));

  for (uint32_t count = mObjectStores.Length(), index = 0;
       index < count;
       index++) {
    nsRefPtr<IDBObjectStore>& objectStore = mObjectStores[index];

    if (objectStore->Id() == aObjectStoreId) {
      objectStore->NoteDeletion();
      mObjectStores.RemoveElementAt(index);
      break;
    }
  }
}

void
IDBTransaction::CreateIndex(IDBObjectStore* aObjectStore,
                            const IndexMetadata& aMetadata)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aObjectStore);
  MOZ_ASSERT(aMetadata.id());
  MOZ_ASSERT(VERSION_CHANGE == mMode);
  MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);
  MOZ_ASSERT(IsOpen());

  MOZ_ALWAYS_TRUE(mBackgroundActor.mVersionChangeBackgroundActor->
                    SendCreateIndex(aObjectStore->Id(), aMetadata));
}

void
IDBTransaction::DeleteIndex(IDBObjectStore* aObjectStore,
                            int64_t aIndexId)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aObjectStore);
  MOZ_ASSERT(aIndexId);
  MOZ_ASSERT(VERSION_CHANGE == mMode);
  MOZ_ASSERT(mBackgroundActor.mVersionChangeBackgroundActor);
  MOZ_ASSERT(IsOpen());

  MOZ_ALWAYS_TRUE(mBackgroundActor.mVersionChangeBackgroundActor->
                    SendDeleteIndex(aObjectStore->Id(), aIndexId));
}

already_AddRefed<FileInfo>
IDBTransaction::GetFileInfo(nsIDOMBlob* aBlob)
{
  AssertIsOnOwningThread();

  nsRefPtr<FileInfo> fileInfo;
  mCreatedFileInfos.Get(aBlob, getter_AddRefs(fileInfo));

  return fileInfo.forget();
}

void
IDBTransaction::AddFileInfo(nsIDOMBlob* aBlob, FileInfo* aFileInfo)
{
  AssertIsOnOwningThread();

  mCreatedFileInfos.Put(aBlob, aFileInfo);
}

void
IDBTransaction::ClearCreatedFileInfos()
{
  AssertIsOnOwningThread();

  mCreatedFileInfos.Clear();
}

nsresult
IDBTransaction::AbortInternal(nsresult aAbortCode,
                              already_AddRefed<DOMError> aError)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(NS_FAILED(aAbortCode));

  nsRefPtr<DOMError> error = aError;

  if (IsFinished()) {
    return NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR;
  }

  bool isVersionChange = IDBTransaction::VERSION_CHANGE == mMode;
  bool needToAbort = IDBTransaction::INITIAL == mReadyState;

  mAbortCode = aAbortCode;
  mReadyState = IDBTransaction::DONE;
  mError = error.forget();

  if (isVersionChange) {
    // If a version change transaction is aborted, we must revert the world
    // back to its previous state.
    mDatabase->RevertToPreviousState();

    RefreshSpec();
  }

  // Fire the abort event if there are no outstanding requests. Otherwise the
  // abort event will be fired when all outstanding requests finish.
  if (needToAbort) {
    SendAbort(aAbortCode);
  }

  if (isVersionChange) {
    mDatabase->Close();
  }

  return NS_OK;
}

nsresult
IDBTransaction::Abort(IDBRequest* aRequest)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aRequest);

  ErrorResult rv;
  nsRefPtr<DOMError> error = aRequest->GetError(rv);

  return AbortInternal(aRequest->GetErrorCode(), error.forget());
}

nsresult
IDBTransaction::Abort(nsresult aErrorCode)
{
  AssertIsOnOwningThread();

  nsRefPtr<DOMError> error = new DOMError(GetOwner(), aErrorCode);
  return AbortInternal(aErrorCode, error.forget());
}

void
IDBTransaction::Abort(ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  aRv = AbortInternal(NS_ERROR_DOM_INDEXEDDB_ABORT_ERR, nullptr);
}

void
IDBTransaction::FireCompleteOrAbortEvents(nsresult aResult)
{
  AssertIsOnOwningThread();

  nsCOMPtr<nsIDOMEvent> event;
  if (NS_SUCCEEDED(aResult)) {
    event = CreateGenericEvent(this, NS_LITERAL_STRING(COMPLETE_EVT_STR),
                               eDoesNotBubble, eNotCancelable);
  } else {
    if (!mError) {
      mError = new DOMError(GetOwner(), aResult);
    }

    event = CreateGenericEvent(this, NS_LITERAL_STRING(ABORT_EVT_STR),
                               eDoesBubble, eNotCancelable);
  }

  if (NS_WARN_IF(!event)) {
    return;
  }

  IDB_PROFILER_MARK("IndexedDB Transaction %llu: Complete (rv = %lu)",
                    "IDBTransaction[%llu] MT Complete",
                    mTransaction->GetSerialNumber(), mAbortCode);

  bool dummy;
  NS_WARN_IF(NS_FAILED(DispatchEvent(event, &dummy)));

#ifdef DEBUG
  mFiredCompleteOrAbort = true;
#endif
}

int64_t
IDBTransaction::NextObjectStoreId()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(VERSION_CHANGE == mMode);

  return mNextObjectStoreId++;
}

int64_t
IDBTransaction::NextIndexId()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(VERSION_CHANGE == mMode);

  return mNextIndexId++;
}

nsPIDOMWindow*
IDBTransaction::GetParentObject() const
{
  AssertIsOnOwningThread();

  return mDatabase->GetParentObject();
}

IDBTransactionMode
IDBTransaction::GetMode(ErrorResult& aRv) const
{
  AssertIsOnOwningThread();

  switch (mMode) {
    case READ_ONLY:
      return IDBTransactionMode::Readonly;

    case READ_WRITE:
      return IDBTransactionMode::Readwrite;

    case VERSION_CHANGE:
      return IDBTransactionMode::Versionchange;

    case MODE_INVALID:
    default:
      MOZ_CRASH("Bad mode!");
  }
}

DOMError*
IDBTransaction::GetError(ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  return mError;
}

already_AddRefed<DOMStringList>
IDBTransaction::ObjectStoreNames()
{
  AssertIsOnOwningThread();

  if (mMode == IDBTransaction::VERSION_CHANGE) {
    return mDatabase->ObjectStoreNames();
  }

  nsRefPtr<DOMStringList> list = new DOMStringList();
  list->CopyList(mObjectStoreNames);
  return list.forget();
}

already_AddRefed<IDBObjectStore>
IDBTransaction::ObjectStore(const nsAString& aName, ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (IsFinished()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  const ObjectStoreSpec* spec = nullptr;

  if (IDBTransaction::VERSION_CHANGE == mMode ||
      mObjectStoreNames.Contains(aName)) {
    const nsTArray<ObjectStoreSpec>& objectStores =
      mDatabase->Spec()->objectStores();

    for (uint32_t count = objectStores.Length(), index = 0;
         index < count;
         index++) {
      const ObjectStoreSpec& objectStore = objectStores[index];
      if (objectStore.metadata().name() == aName) {
        spec = &objectStore;
        break;
      }
    }
  }

  if (!spec) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_FOUND_ERR);
    return nullptr;
  }

  const int64_t desiredId = spec->metadata().id();

  nsRefPtr<IDBObjectStore> objectStore;

  for (uint32_t count = mObjectStores.Length(), index = 0;
       index < count;
       index++) {
    nsRefPtr<IDBObjectStore>& existingObjectStore = mObjectStores[index];

    if (existingObjectStore->Id() == desiredId) {
      objectStore = existingObjectStore;
      break;
    }
  }

  if (!objectStore) {
    objectStore = IDBObjectStore::Create(this, *spec);
    MOZ_ASSERT(objectStore);

    mObjectStores.AppendElement(objectStore);
  }

  return objectStore.forget();
}

NS_IMPL_CYCLE_COLLECTION_CLASS(IDBTransaction)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(IDBTransaction,
                                                  IDBWrapperCache)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mDatabase)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mError)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mObjectStores)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(IDBTransaction, IDBWrapperCache)
  // Don't unlink mDatabase!
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mError)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mObjectStores)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(IDBTransaction)
  NS_INTERFACE_MAP_ENTRY(nsIRunnable)
NS_INTERFACE_MAP_END_INHERITING(IDBWrapperCache)

NS_IMPL_ADDREF_INHERITED(IDBTransaction, IDBWrapperCache)
NS_IMPL_RELEASE_INHERITED(IDBTransaction, IDBWrapperCache)

JSObject*
IDBTransaction::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aScope)
{
  AssertIsOnOwningThread();

  return IDBTransactionBinding::Wrap(aCx, aScope, this);
}

nsresult
IDBTransaction::PreHandleEvent(EventChainPreVisitor& aVisitor)
{
  AssertIsOnOwningThread();

  aVisitor.mCanHandle = true;
  aVisitor.mParentTarget = mDatabase;
  return NS_OK;
}

NS_IMETHODIMP
IDBTransaction::Run()
{
  AssertIsOnOwningThread();

  // We're back at the event loop, no longer newborn.
  mCreating = false;

  // Maybe set the readyState to DONE if there were no requests generated.
  if (mReadyState == IDBTransaction::INITIAL) {
    mReadyState = IDBTransaction::DONE;

    SendCommit();
  }

  return NS_OK;
}
