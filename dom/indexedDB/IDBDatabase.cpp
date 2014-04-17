/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDBDatabase.h"

#include "ActorsChild.h"
#include "AsyncConnectionHelper.h"
#include "DatabaseInfo.h"
#include "FileManager.h"
#include "IDBEvents.h"
#include "IDBFactory.h"
#include "IDBFileHandle.h"
#include "IDBIndex.h"
#include "IDBObjectStore.h"
#include "IDBTransaction.h"
#include "IDBFactory.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/EventDispatcher.h"
#include "MainThreadUtils.h"
#include "mozilla/storage.h"
#include "mozilla/dom/BindingDeclarations.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/DOMStringList.h"
#include "mozilla/dom/DOMStringListBinding.h"
#include "mozilla/dom/IDBDatabaseBinding.h"
#include "mozilla/dom/IDBObjectStoreBinding.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBSharedTypes.h"
#include "mozilla/dom/quota/Client.h"
#include "mozilla/dom/quota/QuotaManager.h"
#include "nsCOMPtr.h"
#include "nsIDocument.h"
#include "ProfilerHelpers.h"
#include "ReportInternalError.h"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::indexedDB;
using namespace mozilla::dom::quota;

namespace {

class NoRequestDatabaseHelper : public AsyncConnectionHelper
{
public:
  NoRequestDatabaseHelper(IDBTransaction* aTransaction)
  : AsyncConnectionHelper(aTransaction, nullptr)
  {
    NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
    NS_ASSERTION(aTransaction, "Null transaction!");
  }

  virtual ChildProcessSendResult
  SendResponseToChildProcess(nsresult aResultCode) MOZ_OVERRIDE;

  virtual nsresult
  UnpackResponseFromParentProcess(const ResponseValue& aResponseValue)
                                  MOZ_OVERRIDE;

  virtual nsresult OnSuccess() MOZ_OVERRIDE;

  virtual void OnError() MOZ_OVERRIDE;
};

class CreateFileHelper : public AsyncConnectionHelper
{
public:
  CreateFileHelper(IDBDatabase* aDatabase,
                   IDBRequest* aRequest,
                   const nsAString& aName,
                   const nsAString& aType)
  : AsyncConnectionHelper(aDatabase, aRequest),
    mName(aName), mType(aType)
  { }

  ~CreateFileHelper()
  { }

  nsresult DoDatabaseWork(mozIStorageConnection* aConnection);
  nsresult GetSuccessResult(JSContext* aCx,
                            JS::MutableHandle<JS::Value> aVal);
  void ReleaseMainThreadObjects()
  {
    mFileInfo = nullptr;
    AsyncConnectionHelper::ReleaseMainThreadObjects();
  }

  virtual ChildProcessSendResult SendResponseToChildProcess(
                                                           nsresult aResultCode)
                                                           MOZ_OVERRIDE
  {
    return Success_NotSent;
  }

  virtual nsresult UnpackResponseFromParentProcess(
                                            const ResponseValue& aResponseValue)
                                            MOZ_OVERRIDE
  {
    MOZ_CRASH("Should never get here!");
  }

private:
  // In-params.
  nsString mName;
  nsString mType;

  // Out-params.
  nsRefPtr<FileInfo> mFileInfo;
};

} // anonymous namespace

IDBDatabase::IDBDatabase(IDBWrapperCache* aOwnerCache,
                         IDBFactory* aFactory,
                         BackgroundDatabaseChild* aActor,
                         DatabaseSpec* aSpec,
                         PersistenceType aPersistenceType)
  : IDBWrapperCache(aOwnerCache)
  , mFactory(aFactory)
  , mSpec(aSpec)
  , mBackgroundActor(aActor)
  , mClosed(false)
  , mInvalidated(false)
{
  MOZ_ASSERT(aOwnerCache);
  MOZ_ASSERT(aFactory);
  aFactory->AssertIsOnOwningThread();
  MOZ_ASSERT(aActor);
  MOZ_ASSERT(aSpec);

  // nsIOfflineStorage
  mPersistenceType = aPersistenceType;

  SetIsDOMBinding();
}

IDBDatabase::~IDBDatabase()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(!mBackgroundActor);
}

// static
already_AddRefed<IDBDatabase>
IDBDatabase::Create(IDBWrapperCache* aOwnerCache,
                    IDBFactory* aFactory,
                    BackgroundDatabaseChild* aActor,
                    DatabaseSpec* aSpec,
                    PersistenceType aPersistenceType)
{
  MOZ_ASSERT(aOwnerCache);
  MOZ_ASSERT(aFactory);
  aFactory->AssertIsOnOwningThread();
  MOZ_ASSERT(aActor);
  MOZ_ASSERT(aSpec);

  nsRefPtr<IDBDatabase> db =
    new IDBDatabase(aOwnerCache, aFactory, aActor, aSpec, aPersistenceType);

  db->SetScriptOwner(aOwnerCache->GetScriptOwner());

  return db.forget();
}

// static
IDBDatabase*
IDBDatabase::FromStorage(nsIOfflineStorage* aStorage)
{
  return aStorage->GetClient()->GetType() == Client::IDB ?
         static_cast<IDBDatabase*>(aStorage) : nullptr;
}

#ifdef DEBUG

void
IDBDatabase::AssertIsOnOwningThread() const
{
  MOZ_ASSERT(mFactory);
  mFactory->AssertIsOnOwningThread();
}

#endif // DEBUG

void
IDBDatabase::CloseInternal()
{
  AssertIsOnOwningThread();

  if (!mClosed) {
    mClosed = true;

    if (mBackgroundActor && !mInvalidated) {
      mBackgroundActor->SendClose();
    }
  }
}

NS_IMETHODIMP_(bool)
IDBDatabase::IsClosed()
{
  AssertIsOnOwningThread();
  return mClosed;
}

void
IDBDatabase::EnterSetVersionTransaction(uint64_t aNewVersion)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aNewVersion);
  MOZ_ASSERT(!RunningVersionChangeTransaction());
  MOZ_ASSERT(mSpec);
  MOZ_ASSERT(!mPreviousSpec);

  mPreviousSpec = new DatabaseSpec(*mSpec);

  mSpec->metadata().version() = aNewVersion;
}

void
IDBDatabase::ExitSetVersionTransaction()
{
  AssertIsOnOwningThread();

  if (mPreviousSpec) {
    mPreviousSpec = nullptr;
  }
}

void
IDBDatabase::RevertToPreviousState()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(RunningVersionChangeTransaction());
  MOZ_ASSERT(mPreviousSpec);

  // Hold the current spec alive until RefreshTransactionsSpecEnumerator has
  // finished!
  nsAutoPtr<DatabaseSpec> currentSpec(mSpec.forget());

  mSpec = mPreviousSpec.forget();

  RefreshSpec(/* aMayDelete */ true);
}

void
IDBDatabase::RefreshSpec(bool aMayDelete)
{
  AssertIsOnOwningThread();

  class MOZ_STACK_CLASS Helper MOZ_FINAL
  {
  public:
    static PLDHashOperator
    RefreshTransactionsSpec(nsPtrHashKey<IDBTransaction>* aTransaction,
                            void* aClosure)
    {
      MOZ_ASSERT(aTransaction);
      aTransaction->GetKey()->AssertIsOnOwningThread();
      MOZ_ASSERT(aClosure);

      bool mayDelete = *static_cast<bool*>(aClosure);

      nsRefPtr<IDBTransaction> transaction = aTransaction->GetKey();
      transaction->RefreshSpec(mayDelete);

      return PL_DHASH_NEXT;
    }
  };

  mTransactions.EnumerateEntries(Helper::RefreshTransactionsSpec, &aMayDelete);
}

nsPIDOMWindow*
IDBDatabase::GetParentObject() const
{
  return mFactory->GetParentObject();
}

const nsString&
IDBDatabase::Name() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return mSpec->metadata().name();
}

uint64_t
IDBDatabase::Version() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return mSpec->metadata().version();
}

already_AddRefed<DOMStringList>
IDBDatabase::ObjectStoreNames() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  const nsTArray<ObjectStoreSpec>& objectStores = mSpec->objectStores();

  nsRefPtr<DOMStringList> list = new DOMStringList();

  if (!objectStores.IsEmpty()) {
    nsTArray<nsString>& listNames = list->StringArray();
    listNames.SetCapacity(objectStores.Length());

    for (uint32_t index = 0; index < objectStores.Length(); index++) {
      listNames.InsertElementSorted(objectStores[index].metadata().name());
    }
  }

  return list.forget();
}

already_AddRefed<nsIDocument>
IDBDatabase::GetOwnerDocument() const
{
  if (nsPIDOMWindow* window = GetOwner()) {
    nsCOMPtr<nsIDocument> doc = window->GetExtantDoc();
    return doc.forget();
  }
  return nullptr;
}

already_AddRefed<IDBObjectStore>
IDBDatabase::CreateObjectStore(
                            JSContext* aCx,
                            const nsAString& aName,
                            const IDBObjectStoreParameters& aOptionalParameters,
                            ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  IDBTransaction* transaction = IDBTransaction::GetCurrent();

  if (!transaction ||
      transaction->GetMode() != IDBTransaction::VERSION_CHANGE) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  MOZ_ASSERT(transaction->IsOpen());

  KeyPath keyPath(0);
  if (NS_FAILED(KeyPath::Parse(aCx, aOptionalParameters.mKeyPath, &keyPath))) {
    aRv.Throw(NS_ERROR_DOM_SYNTAX_ERR);
    return nullptr;
  }

  nsTArray<ObjectStoreSpec>& objectStores = mSpec->objectStores();
  for (uint32_t count = objectStores.Length(), index = 0;
       index < count;
       index++) {
    if (aName == objectStores[index].metadata().name()) {
      aRv.Throw(NS_ERROR_DOM_INDEXEDDB_CONSTRAINT_ERR);
      return nullptr;
    }
  }

  if (!keyPath.IsAllowedForObjectStore(aOptionalParameters.mAutoIncrement)) {
    aRv.Throw(NS_ERROR_DOM_INVALID_ACCESS_ERR);
    return nullptr;
  }

  const ObjectStoreSpec* oldSpecElements =
    objectStores.IsEmpty() ? nullptr : objectStores.Elements();

  ObjectStoreSpec* newSpec = objectStores.AppendElement();
  newSpec->metadata() =
    ObjectStoreMetadata(transaction->NextObjectStoreId(), nsString(aName),
                        keyPath, aOptionalParameters.mAutoIncrement);

  if (oldSpecElements &&
      oldSpecElements != objectStores.Elements()) {
    MOZ_ASSERT(objectStores.Length() > 1);

    // Array got moved, update the spec pointers for all live objectStores and
    // indexes.
    RefreshSpec(/* aMayDelete */ false);
  }

  nsRefPtr<IDBObjectStore> objectStore =
    transaction->CreateObjectStore(*newSpec);
  MOZ_ASSERT(objectStore);

  IDB_PROFILER_MARK("IndexedDB Pseudo-request: "
                    "database(%s).transaction(%s).createObjectStore(%s)",
                    "MT IDBDatabase.createObjectStore()",
                    IDB_PROFILER_STRING(this),
                    IDB_PROFILER_STRING(aTransaction),
                    IDB_PROFILER_STRING(objectStore));

  return objectStore.forget();
}

void
IDBDatabase::DeleteObjectStore(const nsAString& aName, ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  IDBTransaction* transaction = IDBTransaction::GetCurrent();

  if (!transaction ||
      transaction->GetMode() != IDBTransaction::VERSION_CHANGE) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return;
  }

  MOZ_ASSERT(transaction->IsOpen());

  nsTArray<ObjectStoreSpec>& specArray = mSpec->objectStores();

  int64_t objectStoreId = 0;

  for (uint32_t specCount = specArray.Length(), specIndex = 0;
       specIndex < specCount;
       specIndex++) {
    const ObjectStoreMetadata& metadata = specArray[specIndex].metadata();
    MOZ_ASSERT(metadata.id());

    if (aName == metadata.name()) {
      objectStoreId = metadata.id();

      // Must do this before altering the metadata array!
      transaction->DeleteObjectStore(objectStoreId);

      specArray.RemoveElementAt(specIndex);

      RefreshSpec(/* aMayDelete */ false);
      break;
    }
  }

  if (!objectStoreId) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_FOUND_ERR);
    return;
  }

  IDB_PROFILER_MARK("IndexedDB Pseudo-request: "
                    "database(%s).transaction(%s).deleteObjectStore(\"%s\")",
                    "MT IDBDatabase.deleteObjectStore()",
                    IDB_PROFILER_STRING(this),
                    IDB_PROFILER_STRING(transaction),
                    NS_ConvertUTF16toUTF8(aName).get());
}

already_AddRefed<IDBTransaction>
IDBDatabase::Transaction(const nsAString& aStoreName,
                         IDBTransactionMode aMode,
                         ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  Sequence<nsString> storeNames;
  if (!storeNames.AppendElement(aStoreName)) {
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
    return nullptr;
  }

  return Transaction(storeNames, aMode, aRv);
}

already_AddRefed<indexedDB::IDBTransaction>
IDBDatabase::Transaction(const Sequence<nsString>& aStoreNames,
                         IDBTransactionMode aMode,
                         ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (QuotaManager::IsShuttingDown()) {
    IDB_REPORT_INTERNAL_ERR();
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  if (mClosed) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  if (RunningVersionChangeTransaction()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  if (aStoreNames.IsEmpty()) {
    aRv.Throw(NS_ERROR_DOM_INVALID_ACCESS_ERR);
    return nullptr;
  }

  IDBTransaction::Mode mode;
  switch (aMode) {
    case IDBTransactionMode::Readonly:
      mode = IDBTransaction::READ_ONLY;
      break;
    case IDBTransactionMode::Readwrite:
      mode = IDBTransaction::READ_WRITE;
      break;
    case IDBTransactionMode::Versionchange:
      aRv.Throw(NS_ERROR_DOM_INVALID_ACCESS_ERR);
      return nullptr;
    default:
      MOZ_CRASH("Unknown mode!");
  }

  // Now check to make sure the object store names we collected actually exist.
  for (uint32_t nameCount = aStoreNames.Length(), nameIndex = 0;
       nameIndex < nameCount;
       nameIndex++) {
    const nsString& name = aStoreNames[nameIndex];
    const nsTArray<ObjectStoreSpec>& objectStores = mSpec->objectStores();

    bool found = false;

    for (uint32_t objCount = objectStores.Length(), objIndex = 0;
         objIndex < objCount;
         objIndex++) {
      if (objectStores[objIndex].metadata().name() == name) {
        found = true;
        break;
      }
    }

    if (!found) {
      aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_FOUND_ERR);
      return nullptr;
    }
  }

  nsRefPtr<IDBTransaction> transaction =
    IDBTransaction::Create(this, aStoreNames, mode);
  MOZ_ASSERT(transaction);

  BackgroundTransactionChild* actor =
    new BackgroundTransactionChild(transaction);

  MOZ_ALWAYS_TRUE(
    mBackgroundActor->SendPBackgroundIDBTransactionConstructor(actor,
                                                               aStoreNames,
                                                               mode));

  transaction->SetBackgroundActor(actor);

  IDB_PROFILER_MARK("IndexedDB Transaction %llu: database(%s).transaction(%s)",
                    "IDBTransaction[%llu] MT Started",
                    transaction->GetSerialNumber(), IDB_PROFILER_STRING(this),
                    IDB_PROFILER_STRING(transaction));

  return transaction.forget();
}

already_AddRefed<IDBRequest>
IDBDatabase::MozCreateFileHandle(const nsAString& aName,
                                 const Optional<nsAString>& aType,
                                 ErrorResult& aRv)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  // XXX Fix!
  aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  return nullptr;

  if (!IndexedDatabaseManager::IsMainProcess()) {
    IDB_WARNING("Not supported yet!");
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  if (QuotaManager::IsShuttingDown()) {
    IDB_REPORT_INTERNAL_ERR();
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  if (mClosed) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  nsRefPtr<IDBRequest> request = IDBRequest::Create(this, nullptr);

  nsRefPtr<CreateFileHelper> helper =
    new CreateFileHelper(this, request, aName,
                         aType.WasPassed() ? aType.Value() : EmptyString());

  QuotaManager* quotaManager = QuotaManager::Get();
  NS_ASSERTION(quotaManager, "We should definitely have a manager here");

  nsresult rv = helper->Dispatch(quotaManager->IOThread());
  if (NS_FAILED(rv)) {
    IDB_WARNING("Failed to dispatch!");
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  return request.forget();
}

void
IDBDatabase::RegisterTransaction(IDBTransaction* aTransaction)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aTransaction);
  aTransaction->AssertIsOnOwningThread();
  MOZ_ASSERT(!mTransactions.Contains(aTransaction));

  mTransactions.PutEntry(aTransaction);
}

void
IDBDatabase::UnregisterTransaction(IDBTransaction* aTransaction)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aTransaction);
  aTransaction->AssertIsOnOwningThread();
  MOZ_ASSERT(mTransactions.Contains(aTransaction));

  mTransactions.RemoveEntry(aTransaction);
}

void
IDBDatabase::AbortTransactions()
{
  AssertIsOnOwningThread();

  class MOZ_STACK_CLASS Helper MOZ_FINAL
  {
  public:
    static void
    AbortTransactions(nsTHashtable<nsPtrHashKey<IDBTransaction>>& aTable)
    {
      const uint32_t count = aTable.Count();
      if (!count) {
        return;
      }

      nsTArray<nsRefPtr<IDBTransaction>> transactions;
      transactions.SetCapacity(count);

      aTable.EnumerateEntries(Collect, &transactions);

      MOZ_ASSERT(transactions.Length() == count);

      for (uint32_t index = 0; index < count; index++) {
        nsRefPtr<IDBTransaction> transaction = transactions[index].forget();
        MOZ_ASSERT(transaction);

        // This can fail, for example if the transaction is in the process of
        // being committed. That is expected and fine, so we ignore any returned
        // errors.
        ErrorResult rv;
        transaction->Abort(rv);
      }
    }

  private:
    static PLDHashOperator
    Collect(nsPtrHashKey<IDBTransaction>* aTransaction, void* aClosure)
    {
      MOZ_ASSERT(aTransaction);
      aTransaction->GetKey()->AssertIsOnOwningThread();
      MOZ_ASSERT(aClosure);

      auto* array = static_cast<nsTArray<nsRefPtr<IDBTransaction>>*>(aClosure);
      array->AppendElement(aTransaction->GetKey());

      return PL_DHASH_NEXT;
    }
  };

  Helper::AbortTransactions(mTransactions);
}

NS_IMPL_ADDREF_INHERITED(IDBDatabase, IDBWrapperCache)
NS_IMPL_RELEASE_INHERITED(IDBDatabase, IDBWrapperCache)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(IDBDatabase)
  NS_INTERFACE_MAP_ENTRY(nsIOfflineStorage)
NS_INTERFACE_MAP_END_INHERITING(IDBWrapperCache)

NS_IMPL_CYCLE_COLLECTION_CLASS(IDBDatabase)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(IDBDatabase, IDBWrapperCache)
  tmp->AssertIsOnOwningThread();
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mFactory)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(IDBDatabase, IDBWrapperCache)
  tmp->AssertIsOnOwningThread();

  // Don't unlink mFactory!

  // We've been unlinked, at the very least we should be able to prevent further
  // transactions from starting and unblock any other SetVersion callers.
  tmp->CloseInternal();
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

void
IDBDatabase::LastRelease()
{
  AssertIsOnOwningThread();

  CloseInternal();

  if (mBackgroundActor) {
    mBackgroundActor->SendDeleteMeInternal();
    MOZ_ASSERT(!mBackgroundActor, "SendDeleteMeInternal should have cleared!");
  }
}

NS_IMETHODIMP_(void)
IDBDatabase::Invalidate()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(!mInvalidated);

  mInvalidated = true;

  AbortTransactions();

  CloseInternal();
}

NS_IMETHODIMP
IDBDatabase::Close()
{
  AssertIsOnOwningThread();

  CloseInternal();
  return NS_OK;
}

NS_IMETHODIMP_(const nsACString&)
IDBDatabase::Id()
{
  MOZ_CRASH("Remove me!");
}

NS_IMETHODIMP_(mozilla::dom::quota::Client*)
IDBDatabase::GetClient()
{
  return mQuotaClient;
}

NS_IMETHODIMP_(bool)
IDBDatabase::IsOwned(nsPIDOMWindow* aOwner)
{
  return GetOwner() == aOwner;
}

NS_IMETHODIMP_(const nsACString&)
IDBDatabase::Origin()
{
  MOZ_CRASH("Remove me!");
}

nsresult
IDBDatabase::PostHandleEvent(EventChainPostVisitor& aVisitor)
{
  return IndexedDatabaseManager::FireWindowOnError(GetOwner(), aVisitor);
}

JSObject*
IDBDatabase::WrapObject(JSContext* aCx)
{
  return IDBDatabaseBinding::Wrap(aCx, this);
}

AsyncConnectionHelper::ChildProcessSendResult
NoRequestDatabaseHelper::SendResponseToChildProcess(nsresult aResultCode)
{
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  return Success_NotSent;
}

nsresult
NoRequestDatabaseHelper::UnpackResponseFromParentProcess(
                                            const ResponseValue& aResponseValue)
{
  MOZ_CRASH("Should never get here!");
}

nsresult
NoRequestDatabaseHelper::OnSuccess()
{
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  return NS_OK;
}

void
NoRequestDatabaseHelper::OnError()
{
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  mTransaction->Abort(GetResultCode());
}

nsresult
CreateFileHelper::DoDatabaseWork(mozIStorageConnection* aConnection)
{
  AssertIsOnIOThread();
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_LABEL("CreateFileHelper", "DoDatabaseWork",
    js::ProfileEntry::Category::STORAGE);

  if (IndexedDatabaseManager::InLowDiskSpaceMode()) {
    NS_WARNING("Refusing to create file because disk space is low!");
    return NS_ERROR_DOM_INDEXEDDB_QUOTA_ERR;
  }

  MOZ_CRASH("Move me!");/*
  FileManager* fileManager = mDatabase->Manager();

  mFileInfo = fileManager->GetNewFileInfo();
  IDB_ENSURE_TRUE(mFileInfo, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  const int64_t& fileId = mFileInfo->Id();

  nsCOMPtr<nsIFile> directory = fileManager->EnsureJournalDirectory();
  NS_ENSURE_TRUE(directory, NS_ERROR_FAILURE);

  nsCOMPtr<nsIFile> file = fileManager->GetFileForId(directory, fileId);
  NS_ENSURE_TRUE(file, NS_ERROR_FAILURE);

  nsresult rv = file->Create(nsIFile::NORMAL_FILE_TYPE, 0644);
  NS_ENSURE_SUCCESS(rv, rv);

  directory = fileManager->GetDirectory();
  IDB_ENSURE_TRUE(directory, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  file = fileManager->GetFileForId(directory, fileId);
  IDB_ENSURE_TRUE(file, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  rv = file->Create(nsIFile::NORMAL_FILE_TYPE, 0644);
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  */
  return NS_OK;
}

nsresult
CreateFileHelper::GetSuccessResult(JSContext* aCx,
                                   JS::MutableHandle<JS::Value> aVal)
{
  nsRefPtr<IDBFileHandle> fileHandle =
    IDBFileHandle::Create(mName, mType, mDatabase, mFileInfo.forget());
  IDB_ENSURE_TRUE(fileHandle, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  return WrapNative(aCx, NS_ISUPPORTS_CAST(EventTarget*, fileHandle), aVal);
}
