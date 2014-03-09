/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDBTransaction.h"

#include "ActorsChild.h"
#include "AsyncConnectionHelper.h"
#include "BackgroundChildImpl.h"
#include "DatabaseInfo.h"
#include "FileInfo.h"
#include "IDBCursor.h"
#include "IDBEvents.h"
#include "IDBFactory.h"
#include "IDBObjectStore.h"
#include "IndexedDatabaseManager.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/EventDispatcher.h"
#include "mozilla/dom/DOMError.h"
#include "mozilla/dom/DOMStringList.h"
#include "mozilla/dom/quota/QuotaManager.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "mozilla/storage.h"
#include "nsClassHashtable.h"
#include "nsDataHashtable.h"
#include "nsDOMClassInfoID.h"
#include "nsIAppShell.h"
#include "nsIScriptContext.h"
#include "nsPIDOMWindow.h"
#include "nsProxyRelease.h"
#include "nsThreadUtils.h"
#include "nsWidgetsCID.h"
#include "ProfilerHelpers.h"
#include "ReportInternalError.h"
#include "TransactionThreadPool.h"

#define SAVEPOINT_NAME "savepoint"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::indexedDB;
using namespace mozilla::dom::quota;
using namespace mozilla::ipc;

namespace {

NS_DEFINE_CID(kAppShellCID, NS_APPSHELL_CID);

PLDHashOperator
DoomCachedStatements(const nsACString& aQuery,
                     nsCOMPtr<mozIStorageStatement>& aStatement,
                     void* aUserArg)
{
  CommitHelper* helper = static_cast<CommitHelper*>(aUserArg);
  helper->AddDoomedObject(aStatement);
  return PL_DHASH_REMOVE;
}

// This runnable doesn't actually do anything beyond "prime the pump" and get
// transactions in the right order on the transaction thread pool.
class StartTransactionRunnable : public nsIRunnable
{
public:
  NS_DECL_ISUPPORTS

  NS_IMETHOD Run()
  {
    // NOP
    return NS_OK;
  }
};

// Could really use those NS_REFCOUNTING_HAHA_YEAH_RIGHT macros here.
NS_IMETHODIMP_(nsrefcnt) StartTransactionRunnable::AddRef()
{
  return 2;
}

NS_IMETHODIMP_(nsrefcnt) StartTransactionRunnable::Release()
{
  return 1;
}

NS_IMPL_QUERY_INTERFACE1(StartTransactionRunnable, nsIRunnable)

} // anonymous namespace

BEGIN_INDEXEDDB_NAMESPACE

class UpdateRefcountFunction MOZ_FINAL : public mozIStorageFunction
{
public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_MOZISTORAGEFUNCTION

  UpdateRefcountFunction(FileManager* aFileManager);
  ~UpdateRefcountFunction();

  void StartSavepoint()
  {
    MOZ_ASSERT(!mInSavepoint);
    MOZ_ASSERT(!mSavepointEntriesIndex.Count());

    mInSavepoint = true;
  }

  void ReleaseSavepoint()
  {
    MOZ_ASSERT(mInSavepoint);

    mSavepointEntriesIndex.Clear();

    mInSavepoint = false;
  }

  void RollbackSavepoint()
  {
    MOZ_ASSERT(mInSavepoint);

    mInSavepoint = false;

    mSavepointEntriesIndex.EnumerateRead(RollbackSavepointCallback, nullptr);

    mSavepointEntriesIndex.Clear();
  }

  void ClearFileInfoEntries()
  {
    mFileInfoEntries.Clear();
  }

  nsresult WillCommit(mozIStorageConnection* aConnection);
  void DidCommit();
  void DidAbort();

private:
  class FileInfoEntry
  {
  public:
    FileInfoEntry(FileInfo* aFileInfo);
    ~FileInfoEntry();

    nsRefPtr<FileInfo> mFileInfo;
    int32_t mDelta;
    int32_t mSavepointDelta;
  };

  enum UpdateType {
    eIncrement,
    eDecrement
  };

  class DatabaseUpdateFunction
  {
  public:
    DatabaseUpdateFunction(mozIStorageConnection* aConnection,
                           UpdateRefcountFunction* aFunction);
    ~DatabaseUpdateFunction();

    bool Update(int64_t aId, int32_t aDelta);
    nsresult ErrorCode()
    {
      return mErrorCode;
    }

  private:
    nsresult UpdateInternal(int64_t aId, int32_t aDelta);

    nsCOMPtr<mozIStorageConnection> mConnection;
    nsCOMPtr<mozIStorageStatement> mUpdateStatement;
    nsCOMPtr<mozIStorageStatement> mSelectStatement;
    nsCOMPtr<mozIStorageStatement> mInsertStatement;

    UpdateRefcountFunction* mFunction;

    nsresult mErrorCode;
  };

  nsresult ProcessValue(mozIStorageValueArray* aValues,
                        int32_t aIndex,
                        UpdateType aUpdateType);

  nsresult CreateJournals();

  nsresult RemoveJournals(const nsTArray<int64_t>& aJournals);

  static PLDHashOperator
  DatabaseUpdateCallback(const uint64_t& aKey,
                         FileInfoEntry* aValue,
                         void* aUserArg);

  static PLDHashOperator
  FileInfoUpdateCallback(const uint64_t& aKey,
                         FileInfoEntry* aValue,
                         void* aUserArg);

  static PLDHashOperator
  RollbackSavepointCallback(const uint64_t& aKey,
                            FileInfoEntry* aValue,
                            void* aUserArg);

  FileManager* mFileManager;
  nsClassHashtable<nsUint64HashKey, FileInfoEntry> mFileInfoEntries;
  nsDataHashtable<nsUint64HashKey, FileInfoEntry*> mSavepointEntriesIndex;

  nsTArray<int64_t> mJournalsToCreateBeforeCommit;
  nsTArray<int64_t> mJournalsToRemoveAfterCommit;
  nsTArray<int64_t> mJournalsToRemoveAfterAbort;

  bool mInSavepoint;
};

END_INDEXEDDB_NAMESPACE

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

  nsRefPtr<IDBTransaction> transaction = new IDBTransaction(aDatabase);

  transaction->SetScriptOwner(aDatabase->GetScriptOwner());
  transaction->mDatabase = aDatabase;
  transaction->mBackgroundActor.mVersionChangeBackgroundActor = aActor;
  transaction->mMode = VERSION_CHANGE;
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

  nsRefPtr<IDBTransaction> transaction = new IDBTransaction(aDatabase);

  transaction->SetScriptOwner(aDatabase->GetScriptOwner());
  transaction->mDatabase = aDatabase;
  transaction->mMode = aMode;

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

IDBTransaction::IDBTransaction(IDBDatabase* aDatabase)
  : IDBWrapperCache(aDatabase)
  , mReadyState(IDBTransaction::INITIAL)
  , mMode(IDBTransaction::READ_ONLY)
  , mPendingRequests(0)
  , mSavepointCount(0)
  , mAbortCode(NS_OK)
  , mNextObjectStoreId(-1)
  , mNextIndexId(-1)
  , mCreating(false)
#ifdef DEBUG
  , mFiredCompleteOrAbort(false)
#endif
{
  MOZ_ASSERT(aDatabase);
  aDatabase->AssertIsOnOwningThread();

  mBackgroundActor.mNormalBackgroundActor = nullptr;

#ifdef MOZ_ENABLE_PROFILER_SPS
  BackgroundChildImpl::ThreadLocal* threadLocal =
    BackgroundChildImpl::GetThreadLocalForCurrentThread();
  MOZ_ASSERT(threadLocal);

  mSerialNumber = threadLocal->mNextTransactionSerialNumber++;
#endif

}

IDBTransaction::~IDBTransaction()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(!mPendingRequests);
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

#ifdef DEBUG

void
IDBTransaction::AssertIsOnOwningThread() const
{
  MOZ_ASSERT(mDatabase);
  mDatabase->AssertIsOnOwningThread();
}

#endif // DEBUG

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

  if (!mPendingRequests) {
    MOZ_ASSERT(INITIAL == mReadyState);
    mReadyState = LOADING;
  }

  ++mPendingRequests;
}

void
IDBTransaction::OnRequestFinished()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mPendingRequests);

  --mPendingRequests;

  if (!mPendingRequests) {
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
IDBTransaction::OnRequestDisconnected()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mPendingRequests);

  --mPendingRequests;
}

void
IDBTransaction::SetTransactionListener(IDBTransactionListener* aListener)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(!mListener, "Shouldn't already have a listener!");
  mListener = aListener;
}

void
IDBTransaction::SendCommit()
{
  AssertIsOnOwningThread();

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
IDBTransaction::StartSavepoint()
{
  NS_PRECONDITION(!NS_IsMainThread(), "Wrong thread!");
  NS_PRECONDITION(mConnection, "No connection!");

  nsCOMPtr<mozIStorageStatement> stmt = GetCachedStatement(NS_LITERAL_CSTRING(
    "SAVEPOINT " SAVEPOINT_NAME
  ));
  NS_ENSURE_TRUE(stmt, false);

  mozStorageStatementScoper scoper(stmt);

  nsresult rv = stmt->Execute();
  NS_ENSURE_SUCCESS(rv, false);

  if (IsWriteAllowed()) {
    mUpdateFileRefcountFunction->StartSavepoint();
  }

  ++mSavepointCount;

  return true;
}

nsresult
IDBTransaction::ReleaseSavepoint()
{
  NS_PRECONDITION(!NS_IsMainThread(), "Wrong thread!");
  NS_PRECONDITION(mConnection, "No connection!");

  NS_ASSERTION(mSavepointCount, "Mismatch!");

  nsCOMPtr<mozIStorageStatement> stmt = GetCachedStatement(NS_LITERAL_CSTRING(
    "RELEASE SAVEPOINT " SAVEPOINT_NAME
  ));
  NS_ENSURE_TRUE(stmt, NS_OK);

  mozStorageStatementScoper scoper(stmt);

  nsresult rv = stmt->Execute();
  NS_ENSURE_SUCCESS(rv, NS_OK);

  if (IsWriteAllowed()) {
    mUpdateFileRefcountFunction->ReleaseSavepoint();
  }

  --mSavepointCount;

  return NS_OK;
}

void
IDBTransaction::RollbackSavepoint()
{
  NS_PRECONDITION(!NS_IsMainThread(), "Wrong thread!");
  NS_PRECONDITION(mConnection, "No connection!");

  NS_ASSERTION(mSavepointCount == 1, "Mismatch!");
  mSavepointCount = 0;

  nsCOMPtr<mozIStorageStatement> stmt = GetCachedStatement(NS_LITERAL_CSTRING(
    "ROLLBACK TO SAVEPOINT " SAVEPOINT_NAME
  ));
  NS_ENSURE_TRUE_VOID(stmt);

  mozStorageStatementScoper scoper(stmt);

  nsresult rv = stmt->Execute();
  NS_ENSURE_SUCCESS_VOID(rv);

  if (IsWriteAllowed()) {
    mUpdateFileRefcountFunction->RollbackSavepoint();
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
  nsRefPtr<FileInfo> fileInfo;
  mCreatedFileInfos.Get(aBlob, getter_AddRefs(fileInfo));
  return fileInfo.forget();
}

void
IDBTransaction::AddFileInfo(nsIDOMBlob* aBlob, FileInfo* aFileInfo)
{
  mCreatedFileInfos.Put(aBlob, aFileInfo);
}

void
IDBTransaction::ClearCreatedFileInfos()
{
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
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(aRequest, "This is undesirable.");

  ErrorResult rv;
  nsRefPtr<DOMError> error = aRequest->GetError(rv);

  return AbortInternal(aRequest->GetErrorCode(), error.forget());
}

nsresult
IDBTransaction::Abort(nsresult aErrorCode)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  nsRefPtr<DOMError> error = new DOMError(GetOwner(), aErrorCode);
  return AbortInternal(aErrorCode, error.forget());
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
  return IDBTransactionBinding::Wrap(aCx, aScope, this);
}

nsPIDOMWindow*
IDBTransaction::GetParentObject() const
{
  return mDatabase->GetParentObject();
}

mozilla::dom::IDBTransactionMode
IDBTransaction::GetMode(ErrorResult& aRv) const
{
  AssertIsOnOwningThread();

  switch (mMode) {
    case READ_ONLY:
      return mozilla::dom::IDBTransactionMode::Readonly;

    case READ_WRITE:
      return mozilla::dom::IDBTransactionMode::Readwrite;

    case VERSION_CHANGE:
      return mozilla::dom::IDBTransactionMode::Versionchange;

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
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

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

nsresult
IDBTransaction::PreHandleEvent(EventChainPreVisitor& aVisitor)
{
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

CommitHelper::CommitHelper(
              IDBTransaction* aTransaction,
              IDBTransactionListener* aListener,
              const nsTArray<nsRefPtr<IDBObjectStore> >& aUpdatedObjectStores)
: mTransaction(aTransaction),
  mListener(aListener),
  mAbortCode(aTransaction->mAbortCode)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  mConnection.swap(aTransaction->mConnection);
  mUpdateFileRefcountFunction.swap(aTransaction->mUpdateFileRefcountFunction);

  for (uint32_t i = 0; i < aUpdatedObjectStores.Length(); i++) {
    ObjectStoreInfo* info = aUpdatedObjectStores[i]->Info();
    if (info->comittedAutoIncrementId != info->nextAutoIncrementId) {
      mAutoIncrementObjectStores.AppendElement(aUpdatedObjectStores[i]);
    }
  }
}

CommitHelper::CommitHelper(IDBTransaction* aTransaction,
                           nsresult aAbortCode)
: mTransaction(aTransaction),
  mAbortCode(aAbortCode)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
}

CommitHelper::~CommitHelper()
{
}

NS_IMPL_ISUPPORTS1(CommitHelper, nsIRunnable)

NS_IMETHODIMP
CommitHelper::Run()
{
  if (NS_IsMainThread()) {
    PROFILER_MAIN_THREAD_LABEL("IndexedDB", "CommitHelper::Run");

    NS_ASSERTION(mDoomedObjects.IsEmpty(), "Didn't release doomed objects!");

    mTransaction->mReadyState = IDBTransaction::DONE;

    // Release file infos on the main thread, so they will eventually get
    // destroyed on correct thread.
    mTransaction->ClearCreatedFileInfos();
    if (mUpdateFileRefcountFunction) {
      mUpdateFileRefcountFunction->ClearFileInfoEntries();
      mUpdateFileRefcountFunction = nullptr;
    }

    nsCOMPtr<nsIDOMEvent> event;
    if (NS_FAILED(mAbortCode)) {
      if (mTransaction->GetMode() == IDBTransaction::VERSION_CHANGE) {
        // This will make the database take a snapshot of it's DatabaseInfo
        mTransaction->Database()->Close();
        // Then remove the info from the hash as it contains invalid data.
        DatabaseInfo::Remove(mTransaction->Database()->Id());
      }

      event = CreateGenericEvent(mTransaction,
                                 NS_LITERAL_STRING(ABORT_EVT_STR),
                                 eDoesBubble, eNotCancelable);

      // The transaction may already have an error object (e.g. if one of the
      // requests failed).  If it doesn't, and it wasn't aborted
      // programmatically, create one now.
      if (!mTransaction->mError &&
          mAbortCode != NS_ERROR_DOM_INDEXEDDB_ABORT_ERR) {
        mTransaction->mError = new DOMError(mTransaction->GetOwner(), mAbortCode);
      }
    }
    else {
      event = CreateGenericEvent(mTransaction,
                                 NS_LITERAL_STRING(COMPLETE_EVT_STR),
                                 eDoesNotBubble, eNotCancelable);
    }
    IDB_ENSURE_TRUE(event, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

    if (mListener) {
      mListener->NotifyTransactionPreComplete(mTransaction);
    }

    IDB_PROFILER_MARK("IndexedDB Transaction %llu: Complete (rv = %lu)",
                      "IDBTransaction[%llu] MT Complete",
                      mTransaction->GetSerialNumber(), mAbortCode);

    bool dummy;
    if (NS_FAILED(mTransaction->DispatchEvent(event, &dummy))) {
      NS_WARNING("Dispatch failed!");
    }

#ifdef DEBUG
    mTransaction->mFiredCompleteOrAbort = true;
#endif

    if (mListener) {
      mListener->NotifyTransactionPostComplete(mTransaction);
    }

    mTransaction->Database()->UnregisterTransaction(mTransaction);
    mTransaction = nullptr;

    return NS_OK;
  }

  PROFILER_LABEL("IndexedDB", "CommitHelper::Run");

  IDBDatabase* database = mTransaction->Database();
  if (database->IsInvalidated()) {
    IDB_REPORT_INTERNAL_ERR();
    mAbortCode = NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  if (mConnection) {
    QuotaManager::SetCurrentWindow(database->GetOwner());

    if (NS_SUCCEEDED(mAbortCode) && mUpdateFileRefcountFunction &&
        NS_FAILED(mUpdateFileRefcountFunction->WillCommit(mConnection))) {
      IDB_REPORT_INTERNAL_ERR();
      mAbortCode = NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
    }

    if (NS_SUCCEEDED(mAbortCode) && NS_FAILED(WriteAutoIncrementCounts())) {
      IDB_REPORT_INTERNAL_ERR();
      mAbortCode = NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
    }

    if (NS_SUCCEEDED(mAbortCode)) {
      NS_NAMED_LITERAL_CSTRING(release, "COMMIT TRANSACTION");
      nsresult rv = mConnection->ExecuteSimpleSQL(release);
      if (NS_SUCCEEDED(rv)) {
        if (mUpdateFileRefcountFunction) {
          mUpdateFileRefcountFunction->DidCommit();
        }
        CommitAutoIncrementCounts();
      }
      else if (rv == NS_ERROR_FILE_NO_DEVICE_SPACE) {
        // mozstorage translates SQLITE_FULL to NS_ERROR_FILE_NO_DEVICE_SPACE,
        // which we know better as NS_ERROR_DOM_INDEXEDDB_QUOTA_ERR.
        mAbortCode = NS_ERROR_DOM_INDEXEDDB_QUOTA_ERR;
      }
      else {
        IDB_REPORT_INTERNAL_ERR();
        mAbortCode = NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }
    }

    if (NS_FAILED(mAbortCode)) {
      if (mUpdateFileRefcountFunction) {
        mUpdateFileRefcountFunction->DidAbort();
      }
      RevertAutoIncrementCounts();
      NS_NAMED_LITERAL_CSTRING(rollback, "ROLLBACK TRANSACTION");
      if (NS_FAILED(mConnection->ExecuteSimpleSQL(rollback))) {
        NS_WARNING("Failed to rollback transaction!");
      }
    }
  }

  mDoomedObjects.Clear();

  if (mConnection) {
    if (mUpdateFileRefcountFunction) {
      nsresult rv = mConnection->RemoveFunction(
        NS_LITERAL_CSTRING("update_refcount"));
      if (NS_FAILED(rv)) {
        NS_WARNING("Failed to remove function!");
      }
    }

    mConnection->Close();
    mConnection = nullptr;

    QuotaManager::SetCurrentWindow(nullptr);
  }

  return NS_OK;
}

nsresult
CommitHelper::WriteAutoIncrementCounts()
{
  nsCOMPtr<mozIStorageStatement> stmt;
  nsresult rv;
  for (uint32_t i = 0; i < mAutoIncrementObjectStores.Length(); i++) {
    ObjectStoreInfo* info = mAutoIncrementObjectStores[i]->Info();
    if (!stmt) {
      rv = mConnection->CreateStatement(NS_LITERAL_CSTRING(
        "UPDATE object_store SET auto_increment = :ai "
        "WHERE id = :osid;"), getter_AddRefs(stmt));
      NS_ENSURE_SUCCESS(rv, rv);
    }
    else {
      stmt->Reset();
    }

    rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("osid"), info->id);
    NS_ENSURE_SUCCESS(rv, rv);
    
    rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("ai"),
                               info->nextAutoIncrementId);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = stmt->Execute();
    NS_ENSURE_SUCCESS(rv, rv);
  }
  
  return NS_OK;
}

void
CommitHelper::CommitAutoIncrementCounts()
{
  for (uint32_t i = 0; i < mAutoIncrementObjectStores.Length(); i++) {
    ObjectStoreInfo* info = mAutoIncrementObjectStores[i]->Info();
    info->comittedAutoIncrementId = info->nextAutoIncrementId;
  }
}

void
CommitHelper::RevertAutoIncrementCounts()
{
  for (uint32_t i = 0; i < mAutoIncrementObjectStores.Length(); i++) {
    ObjectStoreInfo* info = mAutoIncrementObjectStores[i]->Info();
    info->nextAutoIncrementId = info->comittedAutoIncrementId;
  }
}

UpdateRefcountFunction::
FileInfoEntry::FileInfoEntry(FileInfo* aFileInfo)
: mFileInfo(aFileInfo), mDelta(0), mSavepointDelta(0)
{ }

UpdateRefcountFunction::
FileInfoEntry::~FileInfoEntry()
{ }

UpdateRefcountFunction::
DatabaseUpdateFunction::DatabaseUpdateFunction(
                                             mozIStorageConnection* aConnection,
                                             UpdateRefcountFunction* aFunction)
: mConnection(aConnection), mFunction(aFunction), mErrorCode(NS_OK)
{ }

UpdateRefcountFunction::
DatabaseUpdateFunction::~DatabaseUpdateFunction()
{ }

UpdateRefcountFunction::UpdateRefcountFunction(FileManager* aFileManager)
: mFileManager(aFileManager), mInSavepoint(false)
{ }

UpdateRefcountFunction::~UpdateRefcountFunction()
{ }

NS_IMPL_ISUPPORTS1(UpdateRefcountFunction, mozIStorageFunction)

NS_IMETHODIMP
UpdateRefcountFunction::OnFunctionCall(mozIStorageValueArray* aValues,
                                       nsIVariant** _retval)
{
  *_retval = nullptr;

  uint32_t numEntries;
  nsresult rv = aValues->GetNumEntries(&numEntries);
  NS_ENSURE_SUCCESS(rv, rv);
  NS_ASSERTION(numEntries == 2, "unexpected number of arguments");

#ifdef DEBUG
  int32_t type1 = mozIStorageValueArray::VALUE_TYPE_NULL;
  aValues->GetTypeOfIndex(0, &type1);

  int32_t type2 = mozIStorageValueArray::VALUE_TYPE_NULL;
  aValues->GetTypeOfIndex(1, &type2);

  NS_ASSERTION(!(type1 == mozIStorageValueArray::VALUE_TYPE_NULL &&
                 type2 == mozIStorageValueArray::VALUE_TYPE_NULL),
               "Shouldn't be called!");
#endif

  rv = ProcessValue(aValues, 0, eDecrement);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = ProcessValue(aValues, 1, eIncrement);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

nsresult
UpdateRefcountFunction::WillCommit(mozIStorageConnection* aConnection)
{
  DatabaseUpdateFunction function(aConnection, this);

  mFileInfoEntries.EnumerateRead(DatabaseUpdateCallback, &function);

  nsresult rv = function.ErrorCode();
  NS_ENSURE_SUCCESS(rv, rv);

  rv = CreateJournals();
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

void
UpdateRefcountFunction::DidCommit()
{
  mFileInfoEntries.EnumerateRead(FileInfoUpdateCallback, nullptr);

  nsresult rv = RemoveJournals(mJournalsToRemoveAfterCommit);
  NS_ENSURE_SUCCESS_VOID(rv);
}

void
UpdateRefcountFunction::DidAbort()
{
  nsresult rv = RemoveJournals(mJournalsToRemoveAfterAbort);
  NS_ENSURE_SUCCESS_VOID(rv);
}

nsresult
UpdateRefcountFunction::ProcessValue(mozIStorageValueArray* aValues,
                                     int32_t aIndex,
                                     UpdateType aUpdateType)
{
  int32_t type;
  aValues->GetTypeOfIndex(aIndex, &type);
  if (type == mozIStorageValueArray::VALUE_TYPE_NULL) {
    return NS_OK;
  }

  nsString ids;
  aValues->GetString(aIndex, ids);

  nsTArray<int64_t> fileIds;
  nsresult rv = IDBObjectStore::ConvertFileIdsToArray(ids, fileIds);
  NS_ENSURE_SUCCESS(rv, rv);

  for (uint32_t i = 0; i < fileIds.Length(); i++) {
    int64_t id = fileIds.ElementAt(i);

    FileInfoEntry* entry;
    if (!mFileInfoEntries.Get(id, &entry)) {
      nsRefPtr<FileInfo> fileInfo = mFileManager->GetFileInfo(id);
      NS_ASSERTION(fileInfo, "Shouldn't be null!");

      nsAutoPtr<FileInfoEntry> newEntry(new FileInfoEntry(fileInfo));
      mFileInfoEntries.Put(id, newEntry);
      entry = newEntry.forget();
    }

    if (mInSavepoint) {
      mSavepointEntriesIndex.Put(id, entry);
    }

    switch (aUpdateType) {
      case eIncrement:
        entry->mDelta++;
        if (mInSavepoint) {
          entry->mSavepointDelta++;
        }
        break;
      case eDecrement:
        entry->mDelta--;
        if (mInSavepoint) {
          entry->mSavepointDelta--;
        }
        break;
      default:
        NS_NOTREACHED("Unknown update type!");
    }
  }

  return NS_OK;
}

nsresult
UpdateRefcountFunction::CreateJournals()
{
  nsCOMPtr<nsIFile> journalDirectory = mFileManager->GetJournalDirectory();
  NS_ENSURE_TRUE(journalDirectory, NS_ERROR_FAILURE);

  for (uint32_t i = 0; i < mJournalsToCreateBeforeCommit.Length(); i++) {
    int64_t id = mJournalsToCreateBeforeCommit[i];

    nsCOMPtr<nsIFile> file =
      mFileManager->GetFileForId(journalDirectory, id);
    NS_ENSURE_TRUE(file, NS_ERROR_FAILURE);

    nsresult rv = file->Create(nsIFile::NORMAL_FILE_TYPE, 0644);
    NS_ENSURE_SUCCESS(rv, rv);

    mJournalsToRemoveAfterAbort.AppendElement(id);
  }

  return NS_OK;
}

nsresult
UpdateRefcountFunction::RemoveJournals(const nsTArray<int64_t>& aJournals)
{
  nsCOMPtr<nsIFile> journalDirectory = mFileManager->GetJournalDirectory();
  NS_ENSURE_TRUE(journalDirectory, NS_ERROR_FAILURE);

  for (uint32_t index = 0; index < aJournals.Length(); index++) {
    nsCOMPtr<nsIFile> file =
      mFileManager->GetFileForId(journalDirectory, aJournals[index]);
    NS_ENSURE_TRUE(file, NS_ERROR_FAILURE);

    if (NS_FAILED(file->Remove(false))) {
      NS_WARNING("Failed to removed journal!");
    }
  }

  return NS_OK;
}

PLDHashOperator
UpdateRefcountFunction::DatabaseUpdateCallback(const uint64_t& aKey,
                                               FileInfoEntry* aValue,
                                               void* aUserArg)
{
  if (!aValue->mDelta) {
    return PL_DHASH_NEXT;
  }

  DatabaseUpdateFunction* function =
    static_cast<DatabaseUpdateFunction*>(aUserArg);

  if (!function->Update(aKey, aValue->mDelta)) {
    return PL_DHASH_STOP;
  }

  return PL_DHASH_NEXT;
}

PLDHashOperator
UpdateRefcountFunction::FileInfoUpdateCallback(const uint64_t& aKey,
                                               FileInfoEntry* aValue,
                                               void* aUserArg)
{
  if (aValue->mDelta) {
    aValue->mFileInfo->UpdateDBRefs(aValue->mDelta);
  }

  return PL_DHASH_NEXT;
}

PLDHashOperator
UpdateRefcountFunction::RollbackSavepointCallback(const uint64_t& aKey,
                                                  FileInfoEntry* aValue,
                                                  void* aUserArg)
{
  aValue->mDelta -= aValue->mSavepointDelta;

  return PL_DHASH_NEXT;
}

bool
UpdateRefcountFunction::DatabaseUpdateFunction::Update(int64_t aId,
                                                       int32_t aDelta)
{
  nsresult rv = UpdateInternal(aId, aDelta);
  if (NS_FAILED(rv)) {
    mErrorCode = rv;
    return false;
  }

  return true;
}

nsresult
UpdateRefcountFunction::DatabaseUpdateFunction::UpdateInternal(int64_t aId,
                                                               int32_t aDelta)
{
  nsresult rv;

  if (!mUpdateStatement) {
    rv = mConnection->CreateStatement(NS_LITERAL_CSTRING(
      "UPDATE file SET refcount = refcount + :delta WHERE id = :id"
    ), getter_AddRefs(mUpdateStatement));
    NS_ENSURE_SUCCESS(rv, rv);
  }

  mozStorageStatementScoper updateScoper(mUpdateStatement);

  rv = mUpdateStatement->BindInt32ByName(NS_LITERAL_CSTRING("delta"), aDelta);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = mUpdateStatement->BindInt64ByName(NS_LITERAL_CSTRING("id"), aId);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = mUpdateStatement->Execute();
  NS_ENSURE_SUCCESS(rv, rv);

  int32_t rows;
  rv = mConnection->GetAffectedRows(&rows);
  NS_ENSURE_SUCCESS(rv, rv);

  if (rows > 0) {
    if (!mSelectStatement) {
      rv = mConnection->CreateStatement(NS_LITERAL_CSTRING(
        "SELECT id FROM file where id = :id"
      ), getter_AddRefs(mSelectStatement));
      NS_ENSURE_SUCCESS(rv, rv);
    }

    mozStorageStatementScoper selectScoper(mSelectStatement);

    rv = mSelectStatement->BindInt64ByName(NS_LITERAL_CSTRING("id"), aId);
    NS_ENSURE_SUCCESS(rv, rv);

    bool hasResult;
    rv = mSelectStatement->ExecuteStep(&hasResult);
    NS_ENSURE_SUCCESS(rv, rv);

    if (!hasResult) {
      // Don't have to create the journal here, we can create all at once,
      // just before commit
      mFunction->mJournalsToCreateBeforeCommit.AppendElement(aId);
    }

    return NS_OK;
  }

  if (!mInsertStatement) {
    rv = mConnection->CreateStatement(NS_LITERAL_CSTRING(
      "INSERT INTO file (id, refcount) VALUES(:id, :delta)"
    ), getter_AddRefs(mInsertStatement));
    NS_ENSURE_SUCCESS(rv, rv);
  }

  mozStorageStatementScoper insertScoper(mInsertStatement);

  rv = mInsertStatement->BindInt64ByName(NS_LITERAL_CSTRING("id"), aId);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = mInsertStatement->BindInt32ByName(NS_LITERAL_CSTRING("delta"), aDelta);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = mInsertStatement->Execute();
  NS_ENSURE_SUCCESS(rv, rv);

  mFunction->mJournalsToRemoveAfterCommit.AppendElement(aId);

  return NS_OK;
}
