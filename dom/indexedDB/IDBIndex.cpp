/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDBIndex.h"

#include <algorithm>
#include "ActorsChild.h"
#include "AsyncConnectionHelper.h"
#include "DatabaseInfo.h"
#include "FileInfo.h"
#include "IDBCursor.h"
#include "IDBEvents.h"
#include "IDBKeyRange.h"
#include "IDBObjectStore.h"
#include "IDBTransaction.h"
#include "IndexedDatabase.h"
#include "IndexedDatabaseInlines.h"
#include "IndexedDatabaseManager.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBSharedTypes.h"
#include "mozilla/dom/ipc/Blob.h"
#include "mozilla/storage.h"
#include "nsThreadUtils.h"
#include "ProfilerHelpers.h"
#include "ReportInternalError.h"
#include "xpcpublic.h"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::indexedDB;
using namespace mozilla::dom::indexedDB::ipc;

namespace {

class IndexHelper : public AsyncConnectionHelper
{
protected:
  class IndexRequestParams;
  class IndexedDBIndexRequestChild;

public:
  IndexHelper(IDBTransaction* aTransaction,
              IDBRequest* aRequest,
              IDBIndex* aIndex)
  : AsyncConnectionHelper(aTransaction, aRequest), mIndex(aIndex),
    mActor(nullptr)
  {
    NS_ASSERTION(aTransaction, "Null transaction!");
    NS_ASSERTION(aRequest, "Null request!");
    NS_ASSERTION(aIndex, "Null index!");
  }

  virtual void ReleaseMainThreadObjects() MOZ_OVERRIDE;

  virtual nsresult Dispatch(nsIEventTarget* aDatabaseThread) MOZ_OVERRIDE;

  virtual nsresult
  PackArgumentsForParentProcess(IndexRequestParams& aParams) = 0;

  virtual nsresult
  UnpackResponseFromParentProcess(const ResponseValue& aResponseValue) = 0;

protected:
  nsRefPtr<IDBIndex> mIndex;

private:
  IndexedDBIndexRequestChild* mActor;
};

class OpenKeyCursorHelper : public IndexHelper
{
public:
  OpenKeyCursorHelper(IDBTransaction* aTransaction,
                      IDBRequest* aRequest,
                      IDBIndex* aIndex,
                      IDBKeyRange* aKeyRange,
                      IDBCursor::Direction aDirection)
  : IndexHelper(aTransaction, aRequest, aIndex), mKeyRange(aKeyRange),
    mDirection(aDirection)
  { }

  ~OpenKeyCursorHelper()
  {
    NS_ASSERTION(true, "bas");
  }

  virtual nsresult DoDatabaseWork(mozIStorageConnection* aConnection)
                                  MOZ_OVERRIDE;

  virtual nsresult GetSuccessResult(JSContext* aCx,
                                    JS::MutableHandle<JS::Value> aVal) MOZ_OVERRIDE;

  virtual void ReleaseMainThreadObjects() MOZ_OVERRIDE;

  virtual nsresult
  PackArgumentsForParentProcess(IndexRequestParams& aParams) MOZ_OVERRIDE;

  virtual ChildProcessSendResult
  SendResponseToChildProcess(nsresult aResultCode) MOZ_OVERRIDE;

  virtual nsresult
  UnpackResponseFromParentProcess(const ResponseValue& aResponseValue)
                                  MOZ_OVERRIDE;

protected:
  virtual nsresult EnsureCursor();

  // In-params.
  nsRefPtr<IDBKeyRange> mKeyRange;
  const IDBCursor::Direction mDirection;

  // Out-params.
  Key mKey;
  Key mObjectKey;
  nsCString mContinueQuery;
  nsCString mContinueToQuery;
  Key mRangeKey;

  // Only used in the parent process.
  nsRefPtr<IDBCursor> mCursor;
};

class OpenCursorHelper : public OpenKeyCursorHelper
{
public:
  OpenCursorHelper(IDBTransaction* aTransaction,
                   IDBRequest* aRequest,
                   IDBIndex* aIndex,
                   IDBKeyRange* aKeyRange,
                   IDBCursor::Direction aDirection)
  : OpenKeyCursorHelper(aTransaction, aRequest, aIndex, aKeyRange, aDirection)
  { }

  ~OpenCursorHelper()
  {
    IDBObjectStore::ClearCloneReadInfo(mCloneReadInfo);
  }

  virtual nsresult DoDatabaseWork(mozIStorageConnection* aConnection)
                                  MOZ_OVERRIDE;

  virtual void ReleaseMainThreadObjects() MOZ_OVERRIDE;

  virtual nsresult
  PackArgumentsForParentProcess(IndexRequestParams& aParams) MOZ_OVERRIDE;

  virtual ChildProcessSendResult
  SendResponseToChildProcess(nsresult aResultCode) MOZ_OVERRIDE;

private:
  virtual nsresult EnsureCursor();

  StructuredCloneReadInfo mCloneReadInfo;

  // Only used in the parent process.
  SerializedStructuredCloneReadInfo mSerializedCloneReadInfo;
};

inline
already_AddRefed<IDBRequest>
GenerateRequest(IDBIndex* aIndex)
{
  MOZ_ASSERT(aIndex);
  aIndex->AssertIsOnOwningThread();

  IDBTransaction* transaction = aIndex->ObjectStore()->Transaction();

  nsRefPtr<IDBRequest> request =
    IDBRequest::Create(aIndex, transaction->Database(), transaction);
  MOZ_ASSERT(request);

  return request.forget();
}

} // anonymous namespace

IDBIndex::IDBIndex(IDBObjectStore* aObjectStore, const IndexMetadata* aMetadata)
  : mObjectStore(aObjectStore)
  , mCachedKeyPath(JSVAL_VOID)
  , mMetadata(aMetadata)
  , mId(aMetadata->id())
  , mRooted(false)
{
  MOZ_ASSERT(aObjectStore);
  aObjectStore->AssertIsOnOwningThread();
  MOZ_ASSERT(aMetadata);

  SetIsDOMBinding();
}

IDBIndex::~IDBIndex()
{
  AssertIsOnOwningThread();

  if (mRooted) {
    mCachedKeyPath = JSVAL_VOID;
    mozilla::DropJSObjects(this);
  }
}

already_AddRefed<IDBIndex>
IDBIndex::Create(IDBObjectStore* aObjectStore,
                 const IndexMetadata& aMetadata)
{
  MOZ_ASSERT(aObjectStore);
  aObjectStore->AssertIsOnOwningThread();

  nsRefPtr<IDBIndex> index = new IDBIndex(aObjectStore, &aMetadata);

  return index.forget();
}

#ifdef DEBUG

void
IDBIndex::AssertIsOnOwningThread() const
{
  MOZ_ASSERT(mObjectStore);
  mObjectStore->AssertIsOnOwningThread();
}

#endif // DEBUG

void
IDBIndex::RefreshMetadata(bool aMayDelete)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT_IF(mDeletedMetadata, mMetadata == mDeletedMetadata);

  const nsTArray<IndexMetadata>& indexes = mObjectStore->Spec().indexes();

  bool found = false;

  for (uint32_t count = indexes.Length(), index = 0;
       index < count;
       index++) {
    const IndexMetadata& metadata = indexes[index];

    if (metadata.id() == Id()) {
      mMetadata = &metadata;

      found = true;
      break;
    }
  }

  MOZ_ASSERT_IF(!aMayDelete && !mDeletedMetadata, found);

  if (found) {
    MOZ_ASSERT(mMetadata != mDeletedMetadata);
    mDeletedMetadata = nullptr;
  } else {
    NoteDeletion();
  }
}

void
IDBIndex::NoteDeletion()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mMetadata);
  MOZ_ASSERT(Id() == mMetadata->id());

  if (mDeletedMetadata) {
    MOZ_ASSERT(mMetadata == mDeletedMetadata);
    return;
  }

  mDeletedMetadata = new IndexMetadata(*mMetadata);

  mMetadata = mDeletedMetadata;
}

const nsString&
IDBIndex::Name() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mMetadata);

  return mMetadata->name();
}

bool
IDBIndex::Unique() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mMetadata);

  return mMetadata->unique();
}

bool
IDBIndex::MultiEntry() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mMetadata);

  return mMetadata->multiEntry();
}

const KeyPath&
IDBIndex::GetKeyPath() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mMetadata);

  return mMetadata->keyPath();
}

// XXX Remove me!
/*
already_AddRefed<IDBRequest>
IDBIndex::OpenKeyCursorInternal(IDBKeyRange* aKeyRange, size_t aDirection,
                                ErrorResult& aRv)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  IDBTransaction* transaction = mObjectStore->Transaction();
  if (!transaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  IDBCursor::Direction direction =
    static_cast<IDBCursor::Direction>(aDirection);

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  if (!request) {
    IDB_WARNING("Failed to generate request!");
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  nsRefPtr<OpenKeyCursorHelper> helper =
    new OpenKeyCursorHelper(transaction, request, this, aKeyRange, direction);

  nsresult rv = helper->DispatchToTransactionPool();
  if (NS_FAILED(rv)) {
    IDB_WARNING("Failed to dispatch!");
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s).index(%s)."
                    "openKeyCursor(%s)",
                    "IDBRequest[%llu] MT IDBIndex.openKeyCursor()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(ObjectStore()->Transaction()->
                                        Database()),
                    IDB_PROFILER_STRING(ObjectStore()->Transaction()),
                    IDB_PROFILER_STRING(ObjectStore()),
                    IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(aKeyRange),
                    IDB_PROFILER_STRING(direction));

  return request.forget();
}

nsresult
IDBIndex::OpenCursorInternal(IDBKeyRange* aKeyRange,
                             size_t aDirection,
                             IDBRequest** _retval)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  IDBTransaction* transaction = mObjectStore->Transaction();
  if (!transaction->IsOpen()) {
    return NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR;
  }

  IDBCursor::Direction direction =
    static_cast<IDBCursor::Direction>(aDirection);

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  IDB_ENSURE_TRUE(request, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  nsRefPtr<OpenCursorHelper> helper =
    new OpenCursorHelper(transaction, request, this, aKeyRange, direction);

  nsresult rv = helper->DispatchToTransactionPool();
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s).index(%s)."
                    "openCursor(%s)",
                    "IDBRequest[%llu] MT IDBIndex.openCursor()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(ObjectStore()->Transaction()->
                                        Database()),
                    IDB_PROFILER_STRING(ObjectStore()->Transaction()),
                    IDB_PROFILER_STRING(ObjectStore()),
                    IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(aKeyRange),
                    IDB_PROFILER_STRING(direction));

  request.forget(_retval);
  return NS_OK;
}
*/

nsPIDOMWindow*
IDBIndex::GetParentObject() const
{
  AssertIsOnOwningThread();

  return mObjectStore->GetParentObject();
}

JS::Value
IDBIndex::GetKeyPath(JSContext* aCx, ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (!mCachedKeyPath.isUndefined()) {
    MOZ_ASSERT(mRooted);
    return mCachedKeyPath;
  }

  MOZ_ASSERT(!mRooted);

  aRv = GetKeyPath().ToJSVal(aCx, mCachedKeyPath);
  if (NS_WARN_IF(aRv.Failed())) {
    return JSVAL_VOID;
  }

  if (mCachedKeyPath.isGCThing()) {
    mozilla::HoldJSObjects(this);
    mRooted = true;
  }

  return mCachedKeyPath;
}

already_AddRefed<IDBRequest>
IDBIndex::GetInternal(bool aKeyOnly,
                      JSContext* aCx,
                      JS::Handle<JS::Value> aKey,
                      ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  IDBTransaction* transaction = mObjectStore->Transaction();
  if (!transaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aKey, getter_AddRefs(keyRange));
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  if (!keyRange) {
    // Must specify a key or keyRange for get() and getKey().
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_DATA_ERR);
    return nullptr;
  }

  const int64_t objectStoreId = mObjectStore->Id();
  const int64_t indexId = Id();

  OptionalKeyRange optionalKeyRange;
  if (keyRange) {
    SerializedKeyRange serializedKeyRange;
    keyRange->ToSerialized(serializedKeyRange);
    optionalKeyRange = serializedKeyRange;
  } else {
    optionalKeyRange = void_t();
  }

  RequestParams params;

  if (aKeyOnly) {
    params = IndexGetKeyParams(objectStoreId, indexId, optionalKeyRange);
  } else {
    params = IndexGetParams(objectStoreId, indexId, optionalKeyRange);
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  BackgroundRequestChild* actor = new BackgroundRequestChild(request);

  transaction->StartRequest(actor, params);

  if (aKeyOnly) {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s).index(%s)."
                      "getKey(%s)",
                      "IDBRequest[%llu] MT IDBIndex.getKey()",
                      request->GetSerialNumber(),
                      IDB_PROFILER_STRING(transaction->Database()),
                      IDB_PROFILER_STRING(transaction),
                      IDB_PROFILER_STRING(mObjectStore),
                      IDB_PROFILER_STRING(this),
                      IDB_PROFILER_STRING(aKey));
  } else {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s).index(%s)."
                      "get(%s)",
                      "IDBRequest[%llu] MT IDBIndex.get()",
                      request->GetSerialNumber(),
                      IDB_PROFILER_STRING(transaction->Database()),
                      IDB_PROFILER_STRING(transaction),
                      IDB_PROFILER_STRING(mObjectStore),
                      IDB_PROFILER_STRING(this),
                      IDB_PROFILER_STRING(aKey));
  }
  return request.forget();
}

already_AddRefed<IDBRequest>
IDBIndex::GetAllInternal(bool aKeysOnly,
                         JSContext* aCx,
                         JS::Handle<JS::Value> aKey,
                         const Optional<uint32_t>& aLimit,
                         ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  IDBTransaction* transaction = mObjectStore->Transaction();
  if (!transaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aKey, getter_AddRefs(keyRange));
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  const int64_t objectStoreId = mObjectStore->Id();
  const int64_t indexId = Id();

  OptionalKeyRange optionalKeyRange;
  if (keyRange) {
    SerializedKeyRange serializedKeyRange;
    keyRange->ToSerialized(serializedKeyRange);
    optionalKeyRange = serializedKeyRange;
  } else {
    optionalKeyRange = void_t();
  }

  const uint32_t limit = aLimit.WasPassed() ? aLimit.Value() : 0;

  RequestParams params;
  if (aKeysOnly) {
    params = IndexGetAllKeysParams(objectStoreId, indexId, optionalKeyRange,
                                   limit);
  } else {
    params = IndexGetAllParams(objectStoreId, indexId, optionalKeyRange, limit);
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  BackgroundRequestChild* actor = new BackgroundRequestChild(request);

  transaction->StartRequest(actor, params);

  if (aKeysOnly) {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s).index(%s)."
                      "getAllKeys(%s, %lu)",
                      "IDBRequest[%llu] MT IDBIndex.getAllKeys()",
                      request->GetSerialNumber(),
                      IDB_PROFILER_STRING(transaction->Database()),
                      IDB_PROFILER_STRING(transaction),
                      IDB_PROFILER_STRING(mObjectStore),
                      IDB_PROFILER_STRING(this),
                      IDB_PROFILER_STRING(aKeyRange),
                      aLimit);
  } else {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s).index(%s)."
                      "getAll(%s, %lu)",
                      "IDBRequest[%llu] MT IDBIndex.getAll()",
                      request->GetSerialNumber(),
                      IDB_PROFILER_STRING(transaction->Database()),
                      IDB_PROFILER_STRING(transaction),
                      IDB_PROFILER_STRING(mObjectStore),
                      IDB_PROFILER_STRING(this),
                      IDB_PROFILER_STRING(aKeyRange),
                      aLimit);
  }

  return request.forget();
}

already_AddRefed<IDBRequest>
IDBIndex::OpenCursor(JSContext* aCx,
                     JS::Handle<JS::Value> aRange,
                     IDBCursorDirection aDirection, ErrorResult& aRv)
{
  // XXX Fix!
  aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  return nullptr;

  /*
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  return nullptr;

  IDBTransaction* transaction = mObjectStore->Transaction();
  if (!transaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aRange, getter_AddRefs(keyRange));
  ENSURE_SUCCESS(aRv, nullptr);

  IDBCursor::Direction direction = IDBCursor::ConvertDirection(aDirection);

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  if (!request) {
    IDB_WARNING("Failed to generate request!");
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  nsRefPtr<OpenCursorHelper> helper =
    new OpenCursorHelper(transaction, request, this, keyRange, direction);

  nsresult rv = helper->DispatchToTransactionPool();
  if (NS_FAILED(rv)) {
    IDB_WARNING("Failed to dispatch!");
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  return request.forget();
  */
}

already_AddRefed<IDBRequest>
IDBIndex::OpenKeyCursor(JSContext* aCx,
                        JS::Handle<JS::Value> aRange,
                        IDBCursorDirection aDirection, ErrorResult& aRv)
{
  // XXX Fix!
  aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  return nullptr;

  /*
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  return nullptr;

  IDBTransaction* transaction = mObjectStore->Transaction();
  if (!transaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aRange, getter_AddRefs(keyRange));
  ENSURE_SUCCESS(aRv, nullptr);

  IDBCursor::Direction direction = IDBCursor::ConvertDirection(aDirection);

  return OpenKeyCursorInternal(keyRange, direction, aRv);
  */
}

already_AddRefed<IDBRequest>
IDBIndex::Count(JSContext* aCx,
                JS::Handle<JS::Value> aKey,
                ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  IDBTransaction* transaction = mObjectStore->Transaction();
  if (!transaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aKey, getter_AddRefs(keyRange));
  if (aRv.Failed()) {
    return nullptr;
  }

  IndexCountParams params;
  params.objectStoreId() = mObjectStore->Id();
  params.indexId() = Id();

  if (keyRange) {
    SerializedKeyRange serializedKeyRange;
    keyRange->ToSerialized(serializedKeyRange);
    params.optionalKeyRange() = serializedKeyRange;
  } else {
    params.optionalKeyRange() = void_t();
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  BackgroundRequestChild* actor = new BackgroundRequestChild(request);

  transaction->StartRequest(actor, params);

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s).index(%s)."
                    "count(%s)",
                    "IDBRequest[%llu] MT IDBObjectStore.count()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(transaction->Database()),
                    IDB_PROFILER_STRING(transaction),
                    IDB_PROFILER_STRING(mObjectStore),
                    IDB_PROFILER_STRING(this),
                    IDB_PROFILER_STRING(aKey));

  return request.forget();
}

NS_IMPL_CYCLE_COLLECTING_ADDREF(IDBIndex)
NS_IMPL_CYCLE_COLLECTING_RELEASE(IDBIndex)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(IDBIndex)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTION_CLASS(IDBIndex)

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN(IDBIndex)
  NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER
  NS_IMPL_CYCLE_COLLECTION_TRACE_JSVAL_MEMBER_CALLBACK(mCachedKeyPath)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(IDBIndex)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE_SCRIPT_OBJECTS
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mObjectStore)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(IDBIndex)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER

  // Don't unlink mObjectStore!

  tmp->mCachedKeyPath = JSVAL_VOID;

  if (tmp->mRooted) {
    mozilla::DropJSObjects(tmp);
    tmp->mRooted = false;
  }
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

JSObject*
IDBIndex::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aScope)
{
  return IDBIndexBinding::Wrap(aCx, aScope, this);
}

void
IndexHelper::ReleaseMainThreadObjects()
{
  mIndex = nullptr;
  AsyncConnectionHelper::ReleaseMainThreadObjects();
}

nsresult
IndexHelper::Dispatch(nsIEventTarget* aDatabaseThread)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB", "IndexHelper::Dispatch");

  if (IndexedDatabaseManager::IsMainProcess()) {
    return AsyncConnectionHelper::Dispatch(aDatabaseThread);
  }

  // If we've been invalidated then there's no point sending anything to the
  // parent process.
  if (mIndex->ObjectStore()->Transaction()->Database()->IsInvalidated()) {
    IDB_REPORT_INTERNAL_ERR();
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  MOZ_CRASH("Remove me!");
  /*
  IndexedDBIndexChild* indexActor = mIndex->GetActorChild();
  NS_ASSERTION(indexActor, "Must have an actor here!");

  IndexRequestParams params;
  nsresult rv = PackArgumentsForParentProcess(params);
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  NoDispatchEventTarget target;
  rv = AsyncConnectionHelper::Dispatch(&target);
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  mActor = new IndexedDBIndexRequestChild(this, mIndex, params.type());
  indexActor->SendPIndexedDBRequestConstructor(mActor, params);
  */
  return NS_OK;
}

nsresult
OpenKeyCursorHelper::DoDatabaseWork(mozIStorageConnection* aConnection)
{
  NS_ASSERTION(!NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(aConnection, "Passed a null connection!");

  PROFILER_LABEL("IndexedDB", "OpenKeyCursorHelper::DoDatabaseWork");

  nsCString table;
  if (mIndex->Unique()) {
    table.AssignLiteral("unique_index_data");
  }
  else {
    table.AssignLiteral("index_data");
  }

  NS_NAMED_LITERAL_CSTRING(value, "value");

  nsCString keyRangeClause;
  if (mKeyRange) {
    mKeyRange->GetBindingClause(value, keyRangeClause);
  }

  nsAutoCString directionClause(" ORDER BY value ");
  switch (mDirection) {
    case IDBCursor::NEXT:
    case IDBCursor::NEXT_UNIQUE:
      directionClause += NS_LITERAL_CSTRING("ASC, object_data_key ASC");
      break;

    case IDBCursor::PREV:
      directionClause += NS_LITERAL_CSTRING("DESC, object_data_key DESC");
      break;

    case IDBCursor::PREV_UNIQUE:
      directionClause += NS_LITERAL_CSTRING("DESC, object_data_key ASC");
      break;

    default:
      NS_NOTREACHED("Unknown direction!");
  }
  nsCString firstQuery = NS_LITERAL_CSTRING("SELECT value, object_data_key "
                                            "FROM ") + table +
                         NS_LITERAL_CSTRING(" WHERE index_id = :index_id") +
                         keyRangeClause + directionClause +
                         NS_LITERAL_CSTRING(" LIMIT 1");

  MOZ_CRASH("Remove me!");
  nsCOMPtr<mozIStorageStatement> stmt;/* =
    mTransaction->GetCachedStatement(firstQuery);*/
  IDB_ENSURE_TRUE(stmt, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  mozStorageStatementScoper scoper(stmt);

  nsresult rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("index_id"),
                                      mIndex->Id());
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  if (mKeyRange) {
    rv = mKeyRange->BindToStatement(stmt);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  bool hasResult;
  rv = stmt->ExecuteStep(&hasResult);
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  if (!hasResult) {
    mKey.Unset();
    return NS_OK;
  }

  rv = mKey.SetFromStatement(stmt, 0);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = mObjectKey.SetFromStatement(stmt, 1);
  NS_ENSURE_SUCCESS(rv, rv);

  // Now we need to make the query to get the next match.
  nsAutoCString queryStart = NS_LITERAL_CSTRING("SELECT value, object_data_key"
                                                " FROM ") + table +
                             NS_LITERAL_CSTRING(" WHERE index_id = :id");

  NS_NAMED_LITERAL_CSTRING(rangeKey, "range_key");

  switch (mDirection) {
    case IDBCursor::NEXT:
      if (mKeyRange && !mKeyRange->Upper().IsUnset()) {
        AppendConditionClause(value, rangeKey, true, !mKeyRange->UpperOpen(),
                              queryStart);
        mRangeKey = mKeyRange->Upper();
      }
      mContinueQuery =
        queryStart +
        NS_LITERAL_CSTRING(" AND value >= :current_key AND "
                           "( value > :current_key OR "
                           "  object_data_key > :object_key )") +
        directionClause +
        NS_LITERAL_CSTRING(" LIMIT ");
      mContinueToQuery =
        queryStart +
        NS_LITERAL_CSTRING(" AND value >= :current_key ") +
        directionClause +
        NS_LITERAL_CSTRING(" LIMIT ");
      break;

    case IDBCursor::NEXT_UNIQUE:
      if (mKeyRange && !mKeyRange->Upper().IsUnset()) {
        AppendConditionClause(value, rangeKey, true, !mKeyRange->UpperOpen(),
                              queryStart);
        mRangeKey = mKeyRange->Upper();
      }
      mContinueQuery =
        queryStart + NS_LITERAL_CSTRING(" AND value > :current_key") +
        directionClause +
        NS_LITERAL_CSTRING(" LIMIT ");
      mContinueToQuery =
        queryStart + NS_LITERAL_CSTRING(" AND value >= :current_key") +
        directionClause +
        NS_LITERAL_CSTRING(" LIMIT ");
      break;

    case IDBCursor::PREV:
      if (mKeyRange && !mKeyRange->Lower().IsUnset()) {
        AppendConditionClause(value, rangeKey, false, !mKeyRange->LowerOpen(),
                              queryStart);
        mRangeKey = mKeyRange->Lower();
      }

      mContinueQuery =
        queryStart +
        NS_LITERAL_CSTRING(" AND value <= :current_key AND "
                           "( value < :current_key OR "
                           "  object_data_key < :object_key )") +
        directionClause +
        NS_LITERAL_CSTRING(" LIMIT ");
      mContinueToQuery =
        queryStart +
        NS_LITERAL_CSTRING(" AND value <= :current_key ") +
        directionClause +
        NS_LITERAL_CSTRING(" LIMIT ");
      break;

    case IDBCursor::PREV_UNIQUE:
      if (mKeyRange && !mKeyRange->Lower().IsUnset()) {
        AppendConditionClause(value, rangeKey, false, !mKeyRange->LowerOpen(),
                              queryStart);
        mRangeKey = mKeyRange->Lower();
      }
      mContinueQuery =
        queryStart +
        NS_LITERAL_CSTRING(" AND value < :current_key") +
        directionClause +
        NS_LITERAL_CSTRING(" LIMIT ");
      mContinueToQuery =
        queryStart +
        NS_LITERAL_CSTRING(" AND value <= :current_key") +
        directionClause +
        NS_LITERAL_CSTRING(" LIMIT ");
      break;

    default:
      NS_NOTREACHED("Unknown direction type!");
  }

  return NS_OK;
}

nsresult
OpenKeyCursorHelper::EnsureCursor()
{
  if (mCursor || mKey.IsUnset()) {
    return NS_OK;
  }

  nsRefPtr<IDBCursor> cursor =
    IDBCursor::Create(mRequest, mTransaction, mIndex, mDirection, mRangeKey,
                      mContinueQuery, mContinueToQuery, mKey, mObjectKey);
  IDB_ENSURE_TRUE(cursor, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  mCursor.swap(cursor);
  return NS_OK;
}

nsresult
OpenKeyCursorHelper::GetSuccessResult(JSContext* aCx,
                                      JS::MutableHandle<JS::Value> aVal)
{
  nsresult rv = EnsureCursor();
  NS_ENSURE_SUCCESS(rv, rv);

  if (mCursor) {
    rv = WrapNative(aCx, mCursor, aVal);
    IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  }
  else {
    aVal.setUndefined();
  }

  return NS_OK;
}

void
OpenKeyCursorHelper::ReleaseMainThreadObjects()
{
  mKeyRange = nullptr;
  mCursor = nullptr;
  IndexHelper::ReleaseMainThreadObjects();
}

nsresult
OpenKeyCursorHelper::PackArgumentsForParentProcess(IndexRequestParams& aParams)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(!IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "OpenKeyCursorHelper::"
                             "PackArgumentsForParentProcess [IDBIndex.cpp]");

  MOZ_CRASH("Remove me!");
  /*
  OpenKeyCursorParams params;

  if (mKeyRange) {
    SerializedKeyRange keyRange;
    mKeyRange->ToSerialized(keyRange);
    params.optionalKeyRange() = keyRange;
  }
  else {
    params.optionalKeyRange() = mozilla::void_t();
  }

  params.direction() = mDirection;

  aParams = params;
  */
  return NS_OK;
}

AsyncConnectionHelper::ChildProcessSendResult
OpenKeyCursorHelper::SendResponseToChildProcess(nsresult aResultCode)
{
  MOZ_CRASH("Remove me!");
  /*
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(!mCursor, "Shouldn't have this yet!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "OpenKeyCursorHelper::SendResponseToChildProcess");

  IndexedDBRequestParentBase* actor = mRequest->GetActorParent();
  NS_ASSERTION(actor, "How did we get this far without an actor?");

  if (NS_SUCCEEDED(aResultCode)) {
    nsresult rv = EnsureCursor();
    if (NS_FAILED(rv)) {
      NS_WARNING("EnsureCursor failed!");
      aResultCode = rv;
    }
  }

  ResponseValue response;
  if (NS_FAILED(aResultCode)) {
    response = aResultCode;
  }
  else {
    OpenCursorResponse openCursorResponse;

    if (!mCursor) {
      openCursorResponse = mozilla::void_t();
    }
    else {
      IndexedDBIndexParent* indexActor = mIndex->GetActorParent();
      NS_ASSERTION(indexActor, "Must have an actor here!");

      IndexedDBRequestParentBase* requestActor = mRequest->GetActorParent();
      NS_ASSERTION(requestActor, "Must have an actor here!");

      IndexCursorConstructorParams params;
      params.requestParent() = requestActor;
      params.direction() = mDirection;
      params.key() = mKey;
      params.objectKey() = mObjectKey;
      params.optionalCloneInfo() = mozilla::void_t();

      if (!indexActor->OpenCursor(mCursor, params, openCursorResponse)) {
        return Error;
      }
    }

    response = openCursorResponse;
  }

  if (!actor->SendResponse(response)) {
    return Error;
  }
  */
  return Success_Sent;
}

nsresult
OpenKeyCursorHelper::UnpackResponseFromParentProcess(
                                            const ResponseValue& aResponseValue)
{
  MOZ_CRASH("Remove me!");
  /*
  NS_ASSERTION(aResponseValue.type() == ResponseValue::TOpenCursorResponse,
               "Bad response type!");
  NS_ASSERTION(aResponseValue.get_OpenCursorResponse().type() ==
               OpenCursorResponse::Tvoid_t ||
               aResponseValue.get_OpenCursorResponse().type() ==
               OpenCursorResponse::TPIndexedDBCursorChild,
               "Bad response union type!");
  NS_ASSERTION(!mCursor, "Shouldn't have this yet!");

  const OpenCursorResponse& response =
    aResponseValue.get_OpenCursorResponse();

  switch (response.type()) {
    case OpenCursorResponse::Tvoid_t:
      break;

    case OpenCursorResponse::TPIndexedDBCursorChild: {
      IndexedDBCursorChild* actor =
        static_cast<IndexedDBCursorChild*>(
          response.get_PIndexedDBCursorChild());

      mCursor = actor->ForgetStrongCursor();
      NS_ASSERTION(mCursor, "This should never be null!");

    } break;

    default:
      MOZ_CRASH();
  }
  */
  return NS_OK;
}

nsresult
OpenCursorHelper::DoDatabaseWork(mozIStorageConnection* aConnection)
{
  NS_ASSERTION(!NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(aConnection, "Passed a null connection!");

  PROFILER_LABEL("IndexedDB",
                 "OpenCursorHelper::DoDatabaseWork [IDBIndex.cpp]");

  nsCString indexTable;
  if (mIndex->Unique()) {
    indexTable.AssignLiteral("unique_index_data");
  }
  else {
    indexTable.AssignLiteral("index_data");
  }

  NS_NAMED_LITERAL_CSTRING(value, "index_table.value");

  nsCString keyRangeClause;
  if (mKeyRange) {
    mKeyRange->GetBindingClause(value, keyRangeClause);
  }

  nsAutoCString directionClause(" ORDER BY index_table.value ");
  switch (mDirection) {
    case IDBCursor::NEXT:
    case IDBCursor::NEXT_UNIQUE:
      directionClause +=
        NS_LITERAL_CSTRING("ASC, index_table.object_data_key ASC");
      break;

    case IDBCursor::PREV:
      directionClause +=
        NS_LITERAL_CSTRING("DESC, index_table.object_data_key DESC");
      break;

    case IDBCursor::PREV_UNIQUE:
      directionClause +=
        NS_LITERAL_CSTRING("DESC, index_table.object_data_key ASC");
      break;

    default:
      NS_NOTREACHED("Unknown direction!");
  }

  nsCString firstQuery =
    NS_LITERAL_CSTRING("SELECT index_table.value, "
                       "index_table.object_data_key, object_data.data, "
                       "object_data.file_ids FROM ") +
    indexTable +
    NS_LITERAL_CSTRING(" AS index_table INNER JOIN object_data ON "
                       "index_table.object_data_id = object_data.id "
                       "WHERE index_table.index_id = :id") +
    keyRangeClause + directionClause +
    NS_LITERAL_CSTRING(" LIMIT 1");

  MOZ_CRASH("Remove me!");
  nsCOMPtr<mozIStorageStatement> stmt;/* =
    mTransaction->GetCachedStatement(firstQuery);*/
  IDB_ENSURE_TRUE(stmt, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  mozStorageStatementScoper scoper(stmt);

  nsresult rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("id"), mIndex->Id());
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  if (mKeyRange) {
    rv = mKeyRange->BindToStatement(stmt);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  bool hasResult;
  rv = stmt->ExecuteStep(&hasResult);
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  if (!hasResult) {
    mKey.Unset();
    return NS_OK;
  }

  rv = mKey.SetFromStatement(stmt, 0);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = mObjectKey.SetFromStatement(stmt, 1);
  NS_ENSURE_SUCCESS(rv, rv);

  MOZ_CRASH("Fix!");/*
  rv = IDBObjectStore::GetStructuredCloneReadInfoFromStatement(stmt, 2, 3,
    mDatabase, mCloneReadInfo);*/
  NS_ENSURE_SUCCESS(rv, rv);

  // Now we need to make the query to get the next match.
  nsAutoCString queryStart =
    NS_LITERAL_CSTRING("SELECT index_table.value, "
                       "index_table.object_data_key, object_data.data, "
                       "object_data.file_ids FROM ") +
    indexTable +
    NS_LITERAL_CSTRING(" AS index_table INNER JOIN object_data ON "
                       "index_table.object_data_id = object_data.id "
                       "WHERE index_table.index_id = :id");

  NS_NAMED_LITERAL_CSTRING(rangeKey, "range_key");

  NS_NAMED_LITERAL_CSTRING(limit, " LIMIT ");

  switch (mDirection) {
    case IDBCursor::NEXT:
      if (mKeyRange && !mKeyRange->Upper().IsUnset()) {
        AppendConditionClause(value, rangeKey, true, !mKeyRange->UpperOpen(),
                              queryStart);
        mRangeKey = mKeyRange->Upper();
      }
      mContinueQuery =
        queryStart +
        NS_LITERAL_CSTRING(" AND index_table.value >= :current_key AND "
                           "( index_table.value > :current_key OR "
                           "  index_table.object_data_key > :object_key ) ") +
        directionClause + limit;
      mContinueToQuery =
        queryStart +
        NS_LITERAL_CSTRING(" AND index_table.value >= :current_key") +
        directionClause + limit;
      break;

    case IDBCursor::NEXT_UNIQUE:
      if (mKeyRange && !mKeyRange->Upper().IsUnset()) {
        AppendConditionClause(value, rangeKey, true, !mKeyRange->UpperOpen(),
                              queryStart);
        mRangeKey = mKeyRange->Upper();
      }
      mContinueQuery =
        queryStart +
        NS_LITERAL_CSTRING(" AND index_table.value > :current_key") +
        directionClause + limit;
      mContinueToQuery =
        queryStart +
        NS_LITERAL_CSTRING(" AND index_table.value >= :current_key") +
        directionClause + limit;
      break;

    case IDBCursor::PREV:
      if (mKeyRange && !mKeyRange->Lower().IsUnset()) {
        AppendConditionClause(value, rangeKey, false, !mKeyRange->LowerOpen(),
                              queryStart);
        mRangeKey = mKeyRange->Lower();
      }
      mContinueQuery =
        queryStart +
        NS_LITERAL_CSTRING(" AND index_table.value <= :current_key AND "
                           "( index_table.value < :current_key OR "
                           "  index_table.object_data_key < :object_key ) ") +
        directionClause + limit;
      mContinueToQuery =
        queryStart +
        NS_LITERAL_CSTRING(" AND index_table.value <= :current_key") +
        directionClause + limit;
      break;

    case IDBCursor::PREV_UNIQUE:
      if (mKeyRange && !mKeyRange->Lower().IsUnset()) {
        AppendConditionClause(value, rangeKey, false, !mKeyRange->LowerOpen(),
                              queryStart);
        mRangeKey = mKeyRange->Lower();
      }
      mContinueQuery =
        queryStart +
        NS_LITERAL_CSTRING(" AND index_table.value < :current_key") +
        directionClause + limit;
      mContinueToQuery =
        queryStart +
        NS_LITERAL_CSTRING(" AND index_table.value <= :current_key") +
        directionClause + limit;
      break;

    default:
      NS_NOTREACHED("Unknown direction type!");
  }

  return NS_OK;
}

nsresult
OpenCursorHelper::EnsureCursor()
{
  if (mCursor || mKey.IsUnset()) {
    return NS_OK;
  }

  MOZ_CRASH("Fix!");
  //mSerializedCloneReadInfo = mCloneReadInfo;

  NS_ASSERTION(!mSerializedCloneReadInfo.data().IsEmpty(),
               "Shouldn't be possible!");

  nsRefPtr<IDBCursor> cursor =
    IDBCursor::Create(mRequest, mTransaction, mIndex, mDirection, mRangeKey,
                      mContinueQuery, mContinueToQuery, mKey, mObjectKey,
                      Move(mCloneReadInfo));
  IDB_ENSURE_TRUE(cursor, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  NS_ASSERTION(!mCloneReadInfo.mCloneBuffer.data(), "Should have swapped!");

  mCursor.swap(cursor);
  return NS_OK;
}

void
OpenCursorHelper::ReleaseMainThreadObjects()
{
  IDBObjectStore::ClearCloneReadInfo(mCloneReadInfo);

  // These don't need to be released on the main thread but they're only valid
  // as long as mCursor is set.
  mSerializedCloneReadInfo.data().Clear();

  OpenKeyCursorHelper::ReleaseMainThreadObjects();
}

nsresult
OpenCursorHelper::PackArgumentsForParentProcess(IndexRequestParams& aParams)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(!IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "OpenCursorHelper::PackArgumentsForParentProcess "
                             "[IDBIndex.cpp]");

  MOZ_CRASH("Remove me!");
  /*
  OpenCursorParams params;

  if (mKeyRange) {
    SerializedKeyRange keyRange;
    mKeyRange->ToSerialized(keyRange);
    params.optionalKeyRange() = keyRange;
  }
  else {
    params.optionalKeyRange() = mozilla::void_t();
  }

  params.direction() = mDirection;

  aParams = params;
  */
  return NS_OK;
}

AsyncConnectionHelper::ChildProcessSendResult
OpenCursorHelper::SendResponseToChildProcess(nsresult aResultCode)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(!mCursor, "Shouldn't have this yet!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "OpenCursorHelper::SendResponseToChildProcess "
                             "[IDBIndex.cpp]");

  MOZ_CRASH("Remove me!");
  /*
  IndexedDBRequestParentBase* actor = mRequest->GetActorParent();
  NS_ASSERTION(actor, "How did we get this far without an actor?");

  InfallibleTArray<PBlobParent*> blobsParent;

  if (NS_SUCCEEDED(aResultCode)) {
    IDBDatabase* database = mIndex->ObjectStore()->Transaction()->Database();
    NS_ASSERTION(database, "This should never be null!");

    ContentParent* contentParent = database->GetContentParent();
    NS_ASSERTION(contentParent, "This should never be null!");

    FileManager* fileManager = database->Manager();
    NS_ASSERTION(fileManager, "This should never be null!");

    const nsTArray<StructuredCloneFile>& files = mCloneReadInfo.mFiles;

    aResultCode =
      IDBObjectStore::ConvertBlobsToActors(contentParent, fileManager, files,
                                           blobsParent);
    if (NS_FAILED(aResultCode)) {
      NS_WARNING("ConvertBlobsToActors failed!");
    }
  }

  if (NS_SUCCEEDED(aResultCode)) {
    nsresult rv = EnsureCursor();
    if (NS_FAILED(rv)) {
      NS_WARNING("EnsureCursor failed!");
      aResultCode = rv;
    }
  }

  ResponseValue response;
  if (NS_FAILED(aResultCode)) {
    response = aResultCode;
  }
  else {
    OpenCursorResponse openCursorResponse;

    if (!mCursor) {
      openCursorResponse = mozilla::void_t();
    }
    else {
      IndexedDBIndexParent* indexActor = mIndex->GetActorParent();
      NS_ASSERTION(indexActor, "Must have an actor here!");

      IndexedDBRequestParentBase* requestActor = mRequest->GetActorParent();
      NS_ASSERTION(requestActor, "Must have an actor here!");

      NS_ASSERTION(mSerializedCloneReadInfo.data &&
                   mSerializedCloneReadInfo.dataLength,
                   "Shouldn't be possible!");

      IndexCursorConstructorParams params;
      params.requestParent() = requestActor;
      params.direction() = mDirection;
      params.key() = mKey;
      params.objectKey() = mObjectKey;
      params.optionalCloneInfo() = mSerializedCloneReadInfo;
      params.blobsParent().SwapElements(blobsParent);

      if (!indexActor->OpenCursor(mCursor, params, openCursorResponse)) {
        return Error;
      }
    }

    response = openCursorResponse;
  }

  if (!actor->SendResponse(response)) {
    return Error;
  }
  */
  return Success_Sent;
}

