/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ActorsChild.h"

#include "BackgroundChildImpl.h"
#include "IDBDatabase.h"
#include "IDBEvents.h"
#include "IDBFactory.h"
#include "IDBObjectStore.h"
#include "IDBRequest.h"
#include "IDBTransaction.h"
#include "IndexedDatabase.h"
#include "IndexedDatabaseInlines.h"
#include "mozilla/BasicEvents.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsIBFCacheEntry.h"
#include "nsIDocument.h"
#include "nsIDOMEvent.h"
#include "nsIEventTarget.h"
#include "nsPIDOMWindow.h"
#include "nsThreadUtils.h"
#include "nsTraceRefcnt.h"
#include "ProfilerHelpers.h"
#include "ReportInternalError.h"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::indexedDB;

/*******************************************************************************
 * Helper functions
 ******************************************************************************/

namespace {

class MOZ_STACK_CLASS AutoSetCurrentTransaction MOZ_FINAL
{
  typedef mozilla::ipc::BackgroundChildImpl BackgroundChildImpl;

  BackgroundChildImpl::ThreadLocal* mThreadLocal;
  IDBTransaction* mTransaction;

public:
  AutoSetCurrentTransaction(IDBTransaction* aTransaction)
    : mThreadLocal(nullptr)
    , mTransaction(aTransaction)
  {
    if (aTransaction) {
      mThreadLocal = BackgroundChildImpl::GetThreadLocalForCurrentThread();
      MOZ_ASSERT(mThreadLocal);

      MOZ_ASSERT(!mThreadLocal->mCurrentTransaction);
      mThreadLocal->mCurrentTransaction = aTransaction;
    }
  }

  ~AutoSetCurrentTransaction()
  {
    MOZ_ASSERT((mThreadLocal && mTransaction) ||
               (!mThreadLocal && !mTransaction));
    if (mThreadLocal) {
      MOZ_ASSERT(mTransaction == mThreadLocal->mCurrentTransaction);
      mThreadLocal->mCurrentTransaction = nullptr;
    }
  }

  IDBTransaction*
  Transaction() const
  {
    return mTransaction;
  }
};

class MOZ_STACK_CLASS ResultHelper MOZ_FINAL
{
  IDBRequest* mRequest;
  AutoSetCurrentTransaction mAutoTransaction;

  union
  {
    IDBDatabase* mDatabase;
    StructuredCloneReadInfo* mStructuredClone;
    const nsTArray<StructuredCloneReadInfo>* mStructuredCloneArray;
    const Key* mKey;
    const nsTArray<Key>* mKeyArray;
    JS::Handle<JS::Value>* mJSVal;
  } mResult;

  enum
  {
    ResultTypeDatabase,
    ResultTypeStructuredClone,
    ResultTypeStructuredCloneArray,
    ResultTypeKey,
    ResultTypeKeyArray,
    ResultTypeJSVal,
  } mResultType;

public:
  ResultHelper(IDBRequest* aRequest,
               IDBTransaction* aTransaction,
               IDBDatabase* aResult)
    : mRequest(aRequest)
    , mAutoTransaction(aTransaction)
    , mResultType(ResultTypeDatabase)
  {
    MOZ_ASSERT(aRequest);
    MOZ_ASSERT(aResult);

    mResult.mDatabase = aResult;
  }

  ResultHelper(IDBRequest* aRequest,
               IDBTransaction* aTransaction,
               StructuredCloneReadInfo* aResult)
    : mRequest(aRequest)
    , mAutoTransaction(aTransaction)
    , mResultType(ResultTypeStructuredClone)
  {
    MOZ_ASSERT(aRequest);
    MOZ_ASSERT(aResult);

    mResult.mStructuredClone = aResult;
  }

  ResultHelper(IDBRequest* aRequest,
               IDBTransaction* aTransaction,
               const nsTArray<StructuredCloneReadInfo>* aResult)
    : mRequest(aRequest)
    , mAutoTransaction(aTransaction)
    , mResultType(ResultTypeStructuredCloneArray)
  {
    MOZ_ASSERT(aRequest);
    MOZ_ASSERT(aResult);

    mResult.mStructuredCloneArray = aResult;
  }

  ResultHelper(IDBRequest* aRequest,
               IDBTransaction* aTransaction,
               const Key* aResult)
    : mRequest(aRequest)
    , mAutoTransaction(aTransaction)
    , mResultType(ResultTypeKey)
  {
    MOZ_ASSERT(aRequest);
    MOZ_ASSERT(aResult);

    mResult.mKey = aResult;
  }

  ResultHelper(IDBRequest* aRequest,
               IDBTransaction* aTransaction,
               const nsTArray<Key>* aResult)
    : mRequest(aRequest)
    , mAutoTransaction(aTransaction)
    , mResultType(ResultTypeKeyArray)
  {
    MOZ_ASSERT(aRequest);
    MOZ_ASSERT(aResult);

    mResult.mKeyArray = aResult;
  }

  ResultHelper(IDBRequest* aRequest,
               IDBTransaction* aTransaction,
               JS::Handle<JS::Value>* aResult)
    : mRequest(aRequest)
    , mAutoTransaction(aTransaction)
    , mResultType(ResultTypeJSVal)
  {
    MOZ_ASSERT(aRequest);
    MOZ_ASSERT(!aResult->isGCThing());

    mResult.mJSVal = aResult;
  }

  IDBRequest*
  Request() const
  {
    return mRequest;
  }

  IDBTransaction*
  Transaction() const
  {
    return mAutoTransaction.Transaction();
  }

  static nsresult
  GetResult(JSContext* aCx,
            void* aUserData,
            JS::MutableHandle<JS::Value> aResult)
  {
    MOZ_ASSERT(aCx);

    auto helper = static_cast<ResultHelper*>(aUserData);
    MOZ_ASSERT(helper);
    MOZ_ASSERT(helper->mRequest);

    switch (helper->mResultType) {
      case ResultTypeDatabase:
        return helper->GetResult(aCx, helper->mResult.mDatabase, aResult);

      case ResultTypeStructuredClone:
        return helper->GetResult(aCx, helper->mResult.mStructuredClone,
                                 aResult);

      case ResultTypeStructuredCloneArray:
        return helper->GetResult(aCx, helper->mResult.mStructuredCloneArray,
                                 aResult);

      case ResultTypeKey:
        return helper->GetResult(aCx, helper->mResult.mKey, aResult);

      case ResultTypeKeyArray:
        return helper->GetResult(aCx, helper->mResult.mKeyArray, aResult);

      case ResultTypeJSVal:
        aResult.set(*helper->mResult.mJSVal);
        return NS_OK;

      default:
        MOZ_CRASH("Unknown result type!");
    }

    MOZ_ASSUME_UNREACHABLE("Should never get here!");
  }

private:
  nsresult
  GetResult(JSContext* aCx,
            IDBDatabase* aUserData,
            JS::MutableHandle<JS::Value> aResult)
  {
    if (!aUserData) {
      aResult.setNull();
      return NS_OK;
    }

    auto* wrapperCache = static_cast<IDBWrapperCache*>(mRequest);

    JS::Rooted<JSObject*> global(aCx, wrapperCache->GetParentObject());
    MOZ_ASSERT(global);

    nsresult rv =
      nsContentUtils::WrapNative(aCx, global,
                                 static_cast<IDBWrapperCache*>(aUserData),
                                 aResult);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      IDB_REPORT_INTERNAL_ERR();
      return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
    }

    return NS_OK;
  }

  nsresult
  GetResult(JSContext* aCx,
            StructuredCloneReadInfo* aUserData,
            JS::MutableHandle<JS::Value> aResult)
  {
    bool ok = IDBObjectStore::DeserializeValue(aCx, *aUserData, aResult);

    aUserData->mCloneBuffer.clear();

    if (NS_WARN_IF(!ok)) {
      return NS_ERROR_DOM_DATA_CLONE_ERR;
    }

    return NS_OK;
  }

  nsresult
  GetResult(JSContext* aCx,
            const nsTArray<StructuredCloneReadInfo>* aUserData,
            JS::MutableHandle<JS::Value> aResult)
  {
    JS::Rooted<JSObject*> array(aCx, JS_NewArrayObject(aCx, 0));
    if (NS_WARN_IF(!array)) {
      IDB_REPORT_INTERNAL_ERR();
      return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
    }

    if (!aUserData->IsEmpty()) {
      const uint32_t count = aUserData->Length();

      if (NS_WARN_IF(!JS_SetArrayLength(aCx, array, count))) {
        IDB_REPORT_INTERNAL_ERR();
        return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }

      for (uint32_t index = 0; index < count; index++) {
        auto& cloneInfo =
          const_cast<StructuredCloneReadInfo&>(aUserData->ElementAt(index));

        JS::Rooted<JS::Value> value(aCx);
        bool ok = IDBObjectStore::DeserializeValue(aCx, cloneInfo, &value);

        cloneInfo.mData.Clear();

        if (NS_WARN_IF(!ok)) {
          return NS_ERROR_DOM_DATA_CLONE_ERR;
        }

        if (NS_WARN_IF(!JS_SetElement(aCx, array, index, value))) {
          IDB_REPORT_INTERNAL_ERR();
          return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
        }
      }
    }

    aResult.setObject(*array);
    return NS_OK;
  }

  nsresult
  GetResult(JSContext* aCx,
            const Key* aUserData,
            JS::MutableHandle<JS::Value> aResult)
  {
    nsresult rv = aUserData->ToJSVal(aCx, aResult);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
    return NS_OK;
  }

  nsresult
  GetResult(JSContext* aCx,
            const nsTArray<Key>* aUserData,
            JS::MutableHandle<JS::Value> aResult)
  {
    JS::Rooted<JSObject*> array(aCx, JS_NewArrayObject(aCx, 0));
    if (NS_WARN_IF(!array)) {
      IDB_REPORT_INTERNAL_ERR();
      return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
    }

    if (!aUserData->IsEmpty()) {
      const uint32_t count = aUserData->Length();

      if (NS_WARN_IF(!JS_SetArrayLength(aCx, array, count))) {
        IDB_REPORT_INTERNAL_ERR();
        return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }

      for (uint32_t index = 0; index < count; index++) {
        const Key& key = aUserData->ElementAt(index);
        MOZ_ASSERT(!key.IsUnset());

        JS::Rooted<JS::Value> value(aCx);
        nsresult rv = key.ToJSVal(aCx, &value);
        if (NS_WARN_IF(NS_FAILED(rv))) {
          return rv;
        }

        if (NS_WARN_IF(!JS_SetElement(aCx, array, index, value))) {
          IDB_REPORT_INTERNAL_ERR();
          return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
        }
      }
    }

    aResult.setObject(*array);
    return NS_OK;
  }
};

} // anonymous namespace

/*******************************************************************************
 * Helper functions
 ******************************************************************************/

namespace {

void
DispatchSuccessEvent(ResultHelper* aResultHelper,
                     nsIDOMEvent* aEvent = nullptr)
{
  MOZ_ASSERT(aResultHelper);

  PROFILER_LABEL("IndexedDB", "DispatchSuccessEvent");

  nsRefPtr<IDBRequest> request = aResultHelper->Request();
  MOZ_ASSERT(request);
  request->AssertIsOnOwningThread();

  nsRefPtr<IDBTransaction> transaction = aResultHelper->Transaction();

  nsCOMPtr<nsIDOMEvent> newEvent;
  if (!aEvent) {
    newEvent = CreateGenericEvent(request, NS_LITERAL_STRING(SUCCESS_EVT_STR),
                                  eDoesNotBubble, eNotCancelable);
    if (NS_WARN_IF(!newEvent)) {
      return;
    }

    aEvent = newEvent;
  }

  request->SetResultCallback(&ResultHelper::GetResult, aResultHelper);

  MOZ_ASSERT(aEvent);
  MOZ_ASSERT_IF(transaction, transaction->IsOpen());

  bool dummy;
  nsresult rv = request->DispatchEvent(aEvent, &dummy);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return;
  }

  MOZ_ASSERT_IF(transaction,
                transaction->IsOpen() || transaction->IsAborted());

  WidgetEvent* internalEvent = aEvent->GetInternalNSEvent();
  MOZ_ASSERT(internalEvent);

  if (transaction &&
      transaction->IsOpen() &&
      internalEvent->mFlags.mExceptionHasBeenRisen) {
    MOZ_ALWAYS_TRUE(
      NS_SUCCEEDED(transaction->Abort(NS_ERROR_DOM_INDEXEDDB_ABORT_ERR)));
  }
}

void
DispatchErrorEvent(IDBRequest* aRequest,
                   nsresult aErrorCode,
                   IDBTransaction* aTransaction = nullptr,
                   nsIDOMEvent* aEvent = nullptr)
{
  MOZ_ASSERT(aRequest);
  aRequest->AssertIsOnOwningThread();
  MOZ_ASSERT(NS_FAILED(aErrorCode));
  MOZ_ASSERT(NS_ERROR_GET_MODULE(aErrorCode) == NS_ERROR_MODULE_DOM_INDEXEDDB);

  PROFILER_LABEL("IndexedDB", "DispatchErrorEvent");

  nsRefPtr<IDBRequest> request = aRequest;
  nsRefPtr<IDBTransaction> transaction = aTransaction;

  request->SetError(aErrorCode);

  nsCOMPtr<nsIDOMEvent> event = aEvent;
  if (!event) {
    // Make an error event and fire it at the target.
    event = CreateGenericEvent(request, NS_LITERAL_STRING(ERROR_EVT_STR),
                               eDoesBubble, eCancelable);
    MOZ_ASSERT(event);
  }

  Maybe<AutoSetCurrentTransaction> asct;
  if (aTransaction) {
    asct.construct(aTransaction);
  }

  bool doDefault;
  nsresult rv = request->DispatchEvent(event, &doDefault);
  if (NS_SUCCEEDED(rv)) {
    MOZ_ASSERT(!transaction ||
               transaction->IsOpen() ||
               transaction->IsAborted());

    WidgetEvent* internalEvent = event->GetInternalNSEvent();
    MOZ_ASSERT(internalEvent);

    if (internalEvent->mFlags.mExceptionHasBeenRisen &&
        transaction &&
        transaction->IsOpen()) {
      NS_WARN_IF(NS_FAILED(
        transaction->Abort(NS_ERROR_DOM_INDEXEDDB_ABORT_ERR)));
    }

    if (doDefault &&
        transaction &&
        transaction->IsOpen()) {
      NS_WARN_IF(NS_FAILED(transaction->Abort(request)));
    }
  } else {
    NS_WARNING("DispatchEvent failed!");
  }
}

} // anonymous namespace

/*******************************************************************************
 * BackgroundRequestChildBase
 ******************************************************************************/
BackgroundRequestChildBase::BackgroundRequestChildBase(IDBRequest* aRequest)
  : mRequest(aRequest)
{
  MOZ_ASSERT(aRequest);
  MOZ_COUNT_CTOR(mozilla::dom::indexedDB::BackgroundRequestChildBase);
}

BackgroundRequestChildBase::~BackgroundRequestChildBase()
{
  MOZ_COUNT_DTOR(mozilla::dom::indexedDB::BackgroundRequestChildBase);
}

#ifdef DEBUG

void
BackgroundRequestChildBase::AssertIsOnOwningThread() const
{
  MOZ_ASSERT(mRequest);
  mRequest->AssertIsOnOwningThread();
}

#endif // DEBUG

/*******************************************************************************
 * BackgroundFactoryChild
 ******************************************************************************/

BackgroundFactoryChild::BackgroundFactoryChild(IDBFactory* aFactory)
  : mFactory(aFactory)
#ifdef DEBUG
  , mOwningThread(NS_GetCurrentThread())
#endif
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aFactory);
  aFactory->AssertIsOnOwningThread();

  MOZ_COUNT_CTOR(mozilla::dom::indexedDB::BackgroundFactoryChild);
}

BackgroundFactoryChild::~BackgroundFactoryChild()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(!mFactory);

  MOZ_COUNT_DTOR(mozilla::dom::indexedDB::BackgroundFactoryChild);
}

#ifdef DEBUG

void
BackgroundFactoryChild::AssertIsOnOwningThread() const
{
  MOZ_ASSERT(mOwningThread);

  bool current;
  MOZ_ASSERT(NS_SUCCEEDED(mOwningThread->IsOnCurrentThread(&current)));
  MOZ_ASSERT(current);
}

#endif // DEBUG

void
BackgroundFactoryChild::SendDeleteMe()
{
  AssertIsOnOwningThread();

  if (mFactory) {
    mFactory = nullptr;

    MOZ_ALWAYS_TRUE(PBackgroundIDBFactoryChild::SendDeleteMe());
  }
}

void
BackgroundFactoryChild::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnOwningThread();

  if (mFactory) {
    mFactory->ClearBackgroundActor();
#ifdef DEBUG
    mFactory = nullptr;
#endif
  }
}

PBackgroundIDBFactoryRequestChild*
BackgroundFactoryChild::AllocPBackgroundIDBFactoryRequestChild(
                                            const FactoryRequestParams& aParams)
{
  MOZ_CRASH("PBackgroundIDBFactoryRequestChild actors should be manually "
            "constructed!");
}

bool
BackgroundFactoryChild::DeallocPBackgroundIDBFactoryRequestChild(
                                      PBackgroundIDBFactoryRequestChild* aActor)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aActor);

  delete static_cast<BackgroundFactoryRequestChild*>(aActor);
  return true;
}

PBackgroundIDBDatabaseChild*
BackgroundFactoryChild::AllocPBackgroundIDBDatabaseChild(
                                    const DatabaseSpec& aSpec,
                                    PBackgroundIDBFactoryRequestChild* aRequest)
{
  AssertIsOnOwningThread();

  auto request = static_cast<BackgroundFactoryRequestChild*>(aRequest);
  MOZ_ASSERT(request);

  return new BackgroundDatabaseChild(aSpec, request);
}

bool
BackgroundFactoryChild::DeallocPBackgroundIDBDatabaseChild(
                                            PBackgroundIDBDatabaseChild* aActor)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aActor);

  delete static_cast<BackgroundDatabaseChild*>(aActor);
  return true;
}

/*******************************************************************************
 * BackgroundFactoryRequestChild
 ******************************************************************************/

BackgroundFactoryRequestChild::BackgroundFactoryRequestChild(
                                                 IDBFactory* aFactory,
                                                 IDBOpenDBRequest* aOpenRequest,
                                                 uint64_t aRequestedVersion)
  : BackgroundRequestChildBase(aOpenRequest)
  , mFactory(aFactory)
  , mRequestedVersion(aRequestedVersion)
{
  // Can't assert owning thread here because IPDL has not yet set our manager!
  MOZ_ASSERT(aFactory);
  aFactory->AssertIsOnOwningThread();
  MOZ_ASSERT(aOpenRequest);

  MOZ_COUNT_CTOR(mozilla::dom::indexedDB::BackgroundFactoryRequestChild);
}

BackgroundFactoryRequestChild::~BackgroundFactoryRequestChild()
{
  AssertIsOnOwningThread();

  MOZ_COUNT_DTOR(mozilla::dom::indexedDB::BackgroundFactoryRequestChild);
}

IDBOpenDBRequest*
BackgroundFactoryRequestChild::GetOpenDBRequest() const
{
  AssertIsOnOwningThread();

  IDBRequest* baseRequest = BackgroundRequestChildBase::GetDOMObject();
  return static_cast<IDBOpenDBRequest*>(baseRequest);
}

bool
BackgroundFactoryRequestChild::HandleResponse(nsresult aResponse)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(NS_FAILED(aResponse));
  MOZ_ASSERT(NS_ERROR_GET_MODULE(aResponse) == NS_ERROR_MODULE_DOM_INDEXEDDB);

  mRequest->Reset();

  DispatchErrorEvent(mRequest, aResponse);

  return true;
}

bool
BackgroundFactoryRequestChild::HandleResponse(
                                   const OpenDatabaseRequestResponse& aResponse)
{
  AssertIsOnOwningThread();

  mRequest->Reset();

  auto databaseActor =
    static_cast<BackgroundDatabaseChild*>(aResponse.databaseChild());
  MOZ_ASSERT(databaseActor);

  if (!databaseActor->EnsureDOMObject()) {
    DispatchErrorEvent(mRequest, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return false;
  }

  IDBDatabase* database = databaseActor->GetDOMObject();
  MOZ_ASSERT(database);

  ResultHelper helper(mRequest, nullptr, database);

  DispatchSuccessEvent(&helper);

  databaseActor->ReleaseDOMObject();

  return true;
}

bool
BackgroundFactoryRequestChild::HandleResponse(
                                 const DeleteDatabaseRequestResponse& aResponse)
{
  AssertIsOnOwningThread();

  MOZ_CRASH("Implement me!");
}

bool
BackgroundFactoryRequestChild::Recv__delete__(
                                        const FactoryRequestResponse& aResponse)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mRequest);

  switch (aResponse.type()) {
    case FactoryRequestResponse::Tnsresult:
      return HandleResponse(aResponse.get_nsresult());

    case FactoryRequestResponse::TOpenDatabaseRequestResponse:
      return HandleResponse(aResponse.get_OpenDatabaseRequestResponse());

    case FactoryRequestResponse::TDeleteDatabaseRequestResponse:
      return HandleResponse(aResponse.get_DeleteDatabaseRequestResponse());

    default:
      MOZ_CRASH("Unknown response type!");
  }

  MOZ_ASSUME_UNREACHABLE("Should never get here!");
}

bool
BackgroundFactoryRequestChild::RecvBlocked(const uint64_t& aCurrentVersion)
{
  AssertIsOnOwningThread();

  if (!mRequest) {
    return true;
  }

  nsRefPtr<IDBRequest> kungFuDeathGrip = mRequest;

  nsCOMPtr<nsIDOMEvent> blockedEvent =
    IDBVersionChangeEvent::CreateBlocked(mRequest, aCurrentVersion,
                                         mRequestedVersion);
  MOZ_ASSERT(blockedEvent);

  bool dummy;
  NS_WARN_IF(NS_FAILED(mRequest->DispatchEvent(blockedEvent, &dummy)));

  return true;
}

/*******************************************************************************
 * BackgroundDatabaseChild
 ******************************************************************************/

BackgroundDatabaseChild::BackgroundDatabaseChild(
                               const DatabaseSpec& aSpec,
                               BackgroundFactoryRequestChild* aOpenRequestActor)
  : mSpec(new DatabaseSpec(aSpec))
  , mOpenRequestActor(aOpenRequestActor)
  , mDatabase(nullptr)
{
  // Can't assert owning thread here because IPDL has not yet set our manager!
  MOZ_ASSERT(aOpenRequestActor);

  MOZ_COUNT_CTOR(mozilla::dom::indexedDB::BackgroundDatabaseChild);
}

BackgroundDatabaseChild::~BackgroundDatabaseChild()
{
  AssertIsOnOwningThread();

  MOZ_COUNT_DTOR(mozilla::dom::indexedDB::BackgroundDatabaseChild);
}

void
BackgroundDatabaseChild::SendDeleteMe()
{
  AssertIsOnOwningThread();

  if (mDatabase) {
    mDatabase = nullptr;

    MOZ_ALWAYS_TRUE(PBackgroundIDBDatabaseChild::SendDeleteMe());
  }
}

bool
BackgroundDatabaseChild::EnsureDOMObject()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mOpenRequestActor);

  if (mTemporaryStrongDatabase) {
    MOZ_ASSERT(!mSpec);
    return true;
  }

  MOZ_ASSERT(mSpec);

  auto request = mOpenRequestActor->GetDOMObject();
  MOZ_ASSERT(request);

  auto factory =
    static_cast<BackgroundFactoryChild*>(Manager())->GetDOMObject();
  MOZ_ASSERT(factory);

  mTemporaryStrongDatabase = IDBDatabase::Create(request, factory, this, mSpec);

  MOZ_ASSERT(mTemporaryStrongDatabase);
  mTemporaryStrongDatabase->AssertIsOnOwningThread();

  mDatabase = mTemporaryStrongDatabase;
  mSpec.forget();

  return true;
}

void
BackgroundDatabaseChild::ReleaseDOMObject()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mTemporaryStrongDatabase);
  mTemporaryStrongDatabase->AssertIsOnOwningThread();
  MOZ_ASSERT(mOpenRequestActor);
  MOZ_ASSERT(mDatabase == mTemporaryStrongDatabase);

  mTemporaryStrongDatabase = nullptr;
  mOpenRequestActor = nullptr;
}

void
BackgroundDatabaseChild::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnOwningThread();

  if (mDatabase) {
    mDatabase->ClearBackgroundActor();
#ifdef DEBUG
    mDatabase = nullptr;
#endif
  }
}

PBackgroundIDBTransactionChild*
BackgroundDatabaseChild::AllocPBackgroundIDBTransactionChild(
                                    const nsTArray<nsString>& aObjectStoreNames,
                                    const Mode& aMode)
{
  MOZ_CRASH("PBackgroundIDBTransactionChild actors should be manually "
            "constructed!");
}

bool
BackgroundDatabaseChild::DeallocPBackgroundIDBTransactionChild(
                                         PBackgroundIDBTransactionChild* aActor)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aActor);

  delete static_cast<BackgroundTransactionChild*>(aActor);
  return true;
}

PBackgroundIDBVersionChangeTransactionChild*
BackgroundDatabaseChild::AllocPBackgroundIDBVersionChangeTransactionChild(
                                              const uint64_t& aCurrentVersion,
                                              const uint64_t& aRequestedVersion,
                                              const int64_t& aNextObjectStoreId,
                                              const int64_t& aNextIndexId)
{
  AssertIsOnOwningThread();

  IDBOpenDBRequest* request = mOpenRequestActor->GetOpenDBRequest();
  MOZ_ASSERT(request);

  return new BackgroundVersionChangeTransactionChild(request);
}

bool
BackgroundDatabaseChild::RecvPBackgroundIDBVersionChangeTransactionConstructor(
                            PBackgroundIDBVersionChangeTransactionChild* aActor,
                            const uint64_t& aCurrentVersion,
                            const uint64_t& aRequestedVersion,
                            const int64_t& aNextObjectStoreId,
                            const int64_t& aNextIndexId)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aActor);
  MOZ_ASSERT(mOpenRequestActor);

  if (!EnsureDOMObject()) {
    return false;
  }

  auto actor = static_cast<BackgroundVersionChangeTransactionChild*>(aActor);

  nsRefPtr<IDBTransaction> transaction =
    IDBTransaction::CreateVersionChange(mDatabase, actor, aNextObjectStoreId,
                                        aNextIndexId);
  if (NS_WARN_IF(!transaction)) {
    return false;
  }

  transaction->AssertIsOnOwningThread();

  actor->SetDOMTransaction(transaction);

  mDatabase->EnterSetVersionTransaction(aRequestedVersion);

  nsRefPtr<IDBOpenDBRequest> request = mOpenRequestActor->GetOpenDBRequest();
  MOZ_ASSERT(request);

  request->SetTransaction(transaction);

  nsCOMPtr<nsIDOMEvent> upgradeNeededEvent =
    IDBVersionChangeEvent::CreateUpgradeNeeded(request, aCurrentVersion,
                                               aRequestedVersion);

  ResultHelper helper(request, transaction, mDatabase);

  DispatchSuccessEvent(&helper, upgradeNeededEvent);

  return true;
}

bool
BackgroundDatabaseChild::DeallocPBackgroundIDBVersionChangeTransactionChild(
                            PBackgroundIDBVersionChangeTransactionChild* aActor)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aActor);

  delete static_cast<BackgroundVersionChangeTransactionChild*>(aActor);
  return true;
}

bool
BackgroundDatabaseChild::RecvVersionChange(const uint64_t& aOldVersion,
                                           const uint64_t& aNewVersion)
{
  AssertIsOnOwningThread();

  if (!mDatabase || mDatabase->IsClosed()) {
    return true;
  }

  nsRefPtr<IDBDatabase> kungFuDeathGrip = mDatabase;

  // First check if the document the IDBDatabase is part of is bfcached.
  nsCOMPtr<nsIDocument> ownerDoc = mDatabase->GetOwnerDocument();
  nsIBFCacheEntry* bfCacheEntry;
  if (ownerDoc && (bfCacheEntry = ownerDoc->GetBFCacheEntry())) {
    bfCacheEntry->RemoveFromBFCacheSync();
    MOZ_ASSERT(mDatabase->IsClosed(),
               "Kicking doc out of bfcache should have closed database");
    return true;
  }

  // Next check if it's in the process of being bfcached.
  nsPIDOMWindow* owner = mDatabase->GetOwner();
  if (owner && owner->IsFrozen()) {
    // We can't kick the document out of the bfcache because it's not yet fully
    // in the bfcache. Instead we'll abort everything for the window and mark it
    // as not-bfcacheable.

    // XXX Fix me!
    /*
    QuotaManager* quotaManager = QuotaManager::Get();
    NS_ASSERTION(quotaManager, "Huh?");
    quotaManager->AbortCloseStoragesForWindow(owner);
    */

    MOZ_ASSERT(mDatabase->IsClosed(),
               "AbortCloseStoragesForWindow should have closed database");

    ownerDoc->DisallowBFCaching();
    return true;
  }

  // Otherwise fire a versionchange event.
  nsCOMPtr<nsIDOMEvent> versionChangeEvent =
    IDBVersionChangeEvent::Create(mDatabase, aOldVersion, aNewVersion);
  MOZ_ASSERT(versionChangeEvent);

  bool dummy;
  NS_WARN_IF(NS_FAILED(mDatabase->DispatchEvent(versionChangeEvent, &dummy)));

  if (!mDatabase->IsClosed()) {
    SendBlocked();
  }

  return true;
}

bool
BackgroundDatabaseChild::RecvInvalidate()
{
  AssertIsOnOwningThread();

  if (mDatabase) {
    mDatabase->Invalidate();
  }

  return true;
}

/*******************************************************************************
 * BackgroundTransactionBase
 ******************************************************************************/

BackgroundTransactionBase::BackgroundTransactionBase()
: mTransaction(nullptr)
{
}

BackgroundTransactionBase::BackgroundTransactionBase(
                                                   IDBTransaction* aTransaction)
  : mTemporaryStrongTransaction(aTransaction)
  , mTransaction(aTransaction)
{
  MOZ_ASSERT(aTransaction);
  aTransaction->AssertIsOnOwningThread();
}

BackgroundTransactionBase::~BackgroundTransactionBase()
{
}

void
BackgroundTransactionBase::NoteActorDestroyed()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT_IF(mTemporaryStrongTransaction, mTransaction);

  if (mTransaction) {
    mTransaction->ClearBackgroundActor();
    mTemporaryStrongTransaction = nullptr;
#ifdef DEBUG
    mTransaction = nullptr;
#endif
  }
}

void
BackgroundTransactionBase::SetDOMTransaction(IDBTransaction* aTransaction)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aTransaction);
  aTransaction->AssertIsOnOwningThread();
  MOZ_ASSERT(!mTemporaryStrongTransaction);
  MOZ_ASSERT(!mTransaction);

  mTemporaryStrongTransaction = aTransaction;
  mTransaction = aTransaction;
}

void
BackgroundTransactionBase::NoteComplete()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT_IF(mTransaction, mTemporaryStrongTransaction);

  mTemporaryStrongTransaction = nullptr;
}

/*******************************************************************************
 * BackgroundTransactionChild
 ******************************************************************************/

BackgroundTransactionChild::BackgroundTransactionChild(
                                                   IDBTransaction* aTransaction)
  : BackgroundTransactionBase(aTransaction)
{
  // Can't assert owning thread here because IPDL has not yet set our manager!
  MOZ_ASSERT(aTransaction);
  aTransaction->AssertIsOnOwningThread();

  MOZ_COUNT_CTOR(mozilla::dom::indexedDB::BackgroundTransactionChild);
}

BackgroundTransactionChild::~BackgroundTransactionChild()
{
  AssertIsOnOwningThread();

  MOZ_COUNT_DTOR(mozilla::dom::indexedDB::BackgroundTransactionChild);
}

#ifdef DEBUG

void
BackgroundTransactionChild::AssertIsOnOwningThread() const
{
  static_cast<BackgroundDatabaseChild*>(Manager())->AssertIsOnOwningThread();
}

#endif // DEBUG

void
BackgroundTransactionChild::SendDeleteMe()
{
  AssertIsOnOwningThread();

  NoteActorDestroyed();
}

void
BackgroundTransactionChild::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnOwningThread();

  NoteActorDestroyed();
}

bool
BackgroundTransactionChild::RecvComplete(const nsresult& aResult)
{
  AssertIsOnOwningThread();

  if (!mTransaction) {
    return true;
  }

  mTransaction->FireCompleteOrAbortEvents(aResult);

  NoteComplete();
  return true;
}

PBackgroundIDBRequestChild*
BackgroundTransactionChild::AllocPBackgroundIDBRequestChild(
                                                   const RequestParams& aParams)
{
  MOZ_CRASH("PBackgroundIDBRequestChild actors should be manually "
            "constructed!");
}

bool
BackgroundTransactionChild::DeallocPBackgroundIDBRequestChild(
                                             PBackgroundIDBRequestChild* aActor)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aActor);

  delete static_cast<BackgroundRequestChild*>(aActor);
  return true;
}

/*******************************************************************************
 * BackgroundVersionChangeTransactionChild
 ******************************************************************************/

BackgroundVersionChangeTransactionChild::
BackgroundVersionChangeTransactionChild(IDBOpenDBRequest* aOpenDBRequest)
  : mOpenDBRequest(aOpenDBRequest)
{
  MOZ_ASSERT(aOpenDBRequest);
  // Can't assert owning thread here because IPDL has not yet set our manager!
  MOZ_COUNT_CTOR(mozilla::dom::indexedDB::BackgroundVersionChangeTransactionChild);
}

BackgroundVersionChangeTransactionChild::
~BackgroundVersionChangeTransactionChild()
{
  AssertIsOnOwningThread();
  MOZ_COUNT_DTOR(mozilla::dom::indexedDB::BackgroundVersionChangeTransactionChild);
}

#ifdef DEBUG

void
BackgroundVersionChangeTransactionChild::AssertIsOnOwningThread() const
{
  static_cast<BackgroundDatabaseChild*>(Manager())->AssertIsOnOwningThread();
}

#endif // DEBUG

void
BackgroundVersionChangeTransactionChild::SendDeleteMe()
{
  AssertIsOnOwningThread();

  NoteActorDestroyed();
}

void
BackgroundVersionChangeTransactionChild::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnOwningThread();

  mOpenDBRequest = nullptr;

  NoteActorDestroyed();
}

bool
BackgroundVersionChangeTransactionChild::RecvComplete(const nsresult& aResult)
{
  AssertIsOnOwningThread();

  if (!mTransaction) {
    return true;
  }

  MOZ_ASSERT(mOpenDBRequest);

  IDBDatabase* database = mTransaction->Database();
  MOZ_ASSERT(database);

  database->ExitSetVersionTransaction();

  if (NS_FAILED(aResult)) {
    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(database->Close()));
  }

  mTransaction->FireCompleteOrAbortEvents(aResult);

  mOpenDBRequest->SetTransaction(nullptr);
  mOpenDBRequest = nullptr;

  NoteComplete();
  return true;
}

PBackgroundIDBRequestChild*
BackgroundVersionChangeTransactionChild::AllocPBackgroundIDBRequestChild(
                                                   const RequestParams& aParams)
{
  MOZ_CRASH("PBackgroundIDBRequestChild actors should be manually "
            "constructed!");
}

bool
BackgroundVersionChangeTransactionChild::DeallocPBackgroundIDBRequestChild(
                                             PBackgroundIDBRequestChild* aActor)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aActor);

  delete static_cast<BackgroundRequestChild*>(aActor);
  return true;
}

/*******************************************************************************
 * BackgroundRequestChild
 ******************************************************************************/

BackgroundRequestChild::BackgroundRequestChild(IDBRequest* aRequest)
  : BackgroundRequestChildBase(aRequest)
  , mTransaction(aRequest->GetTransaction())
{
  MOZ_ASSERT(mTransaction);
  mTransaction->AssertIsOnOwningThread();

  mTransaction->OnNewRequest();
}

BackgroundRequestChild::~BackgroundRequestChild()
{
  MOZ_ASSERT(mTransaction);
  mTransaction->AssertIsOnOwningThread();

  mTransaction->OnRequestFinished();
}

bool
BackgroundRequestChild::HandleResponse(nsresult aResponse)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(NS_FAILED(aResponse));
  MOZ_ASSERT(NS_ERROR_GET_MODULE(aResponse) == NS_ERROR_MODULE_DOM_INDEXEDDB);
  MOZ_ASSERT(mTransaction);

  DispatchErrorEvent(mRequest, aResponse, mTransaction);
  return true;
}

bool
BackgroundRequestChild::HandleResponse(const Key& aResponse)
{
  AssertIsOnOwningThread();

  ResultHelper helper(mRequest, mTransaction, &aResponse);

  DispatchSuccessEvent(&helper);
  return true;
}

bool
BackgroundRequestChild::HandleResponse(const nsTArray<Key>& aResponse)
{
  AssertIsOnOwningThread();

  ResultHelper helper(mRequest, mTransaction, &aResponse);

  DispatchSuccessEvent(&helper);
  return true;
}

bool
BackgroundRequestChild::HandleResponse(
                             const SerializedStructuredCloneReadInfo& aResponse)
{
  AssertIsOnOwningThread();

  // XXX Fix this somehow...
  auto& cloneInfo = const_cast<SerializedStructuredCloneReadInfo&>(aResponse);

  StructuredCloneReadInfo cloneReadInfo(Move(cloneInfo));

  // XXX Fix blobs!
  /*
  IDBObjectStore::ConvertActorsToBlobs(aResponse.blobsChild(),
                                       cloneReadInfo.mFiles);
  */

  ResultHelper helper(mRequest, mTransaction, &cloneReadInfo);

  DispatchSuccessEvent(&helper);
  return true;
}

bool
BackgroundRequestChild::HandleResponse(
                   const nsTArray<SerializedStructuredCloneReadInfo>& aResponse)
{
  AssertIsOnOwningThread();

  nsTArray<StructuredCloneReadInfo> cloneInfos;

  if (!aResponse.IsEmpty()) {
    const uint32_t count = aResponse.Length();

    cloneInfos.SetCapacity(count);

    for (uint32_t index = 0; index < count; index++) {
      // XXX Fix this somehow...
      auto& serializedCloneInfo =
        const_cast<SerializedStructuredCloneReadInfo&>(aResponse[index]);

      // XXX This should work but doesn't yet.
      // cloneInfos.AppendElement(Move(serializedCloneInfo));

      // XXX Fix blobs!
      StructuredCloneReadInfo* cloneInfo = cloneInfos.AppendElement();
      cloneInfo->mData.SwapElements(serializedCloneInfo.data());
    }
  }

  ResultHelper helper(mRequest, mTransaction, &cloneInfos);

  DispatchSuccessEvent(&helper);
  return true;
}

bool
BackgroundRequestChild::HandleResponse(JS::Handle<JS::Value> aResponse)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(!aResponse.isGCThing());

  ResultHelper helper(mRequest, mTransaction, &aResponse);

  DispatchSuccessEvent(&helper);
  return true;
}

bool
BackgroundRequestChild::HandleResponse(uint64_t aResponse)
{
  AssertIsOnOwningThread();

  nsRefPtr<IDBRequest> domRequest = GetDOMObject();
  MOZ_ASSERT(domRequest);

  JSContext* cx = domRequest->GetJSContext();
  MOZ_ASSERT(cx);

  JSAutoRequest ar(cx);

  JS::Rooted<JS::Value> value(cx);
  value.setNumber(static_cast<double>(aResponse));

  return HandleResponse(value);
}

bool
BackgroundRequestChild::HandleUndefinedResponse()
{
  AssertIsOnOwningThread();

  nsRefPtr<IDBRequest> domRequest = GetDOMObject();
  MOZ_ASSERT(domRequest);

  JSContext* cx = domRequest->GetJSContext();
  MOZ_ASSERT(cx);

  JSAutoRequest ar(cx);

  JS::Rooted<JS::Value> value(cx);
  value.setUndefined();

  return HandleResponse(value);
}

bool
BackgroundRequestChild::Recv__delete__(const RequestResponse& aResponse)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mRequest);
  MOZ_ASSERT(mTransaction);

  // Always fire an "error" event with ABORT_ERR if the transaction was aborted,
  // even if the request succeeded or failed with another error.
  if (mTransaction->IsAborted()) {
    return HandleResponse(NS_ERROR_DOM_INDEXEDDB_ABORT_ERR);
  }

  switch (aResponse.type()) {
    case RequestResponse::Tnsresult:
      return HandleResponse(aResponse.get_nsresult());

    case RequestResponse::TObjectStoreAddResponse:
      return HandleResponse(aResponse.get_ObjectStoreAddResponse().key());

    case RequestResponse::TObjectStorePutResponse:
      return HandleResponse(aResponse.get_ObjectStorePutResponse().key());

    case RequestResponse::TObjectStoreGetResponse:
      return HandleResponse(aResponse.get_ObjectStoreGetResponse().cloneInfo());

    case RequestResponse::TObjectStoreGetAllResponse:
      return HandleResponse(aResponse.get_ObjectStoreGetAllResponse()
                                     .cloneInfos());

    case RequestResponse::TObjectStoreGetAllKeysResponse:
      return HandleResponse(aResponse.get_ObjectStoreGetAllKeysResponse()
                                     .keys());

    case RequestResponse::TObjectStoreDeleteResponse:
      return HandleUndefinedResponse();

    case RequestResponse::TObjectStoreClearResponse:
      return HandleUndefinedResponse();

    case RequestResponse::TObjectStoreCountResponse:
      return HandleResponse(aResponse.get_ObjectStoreCountResponse().count());

    case RequestResponse::TIndexGetResponse:
      return HandleResponse(aResponse.get_IndexGetResponse().cloneInfo());

    case RequestResponse::TIndexGetKeyResponse:
      return HandleResponse(aResponse.get_IndexGetKeyResponse().key());

    case RequestResponse::TIndexGetAllResponse:
      return HandleResponse(aResponse.get_IndexGetAllResponse().cloneInfos());

    case RequestResponse::TIndexGetAllKeysResponse:
      return HandleResponse(aResponse.get_IndexGetAllKeysResponse().keys());

    case RequestResponse::TIndexCountResponse:
      return HandleResponse(aResponse.get_IndexCountResponse().count());

    default:
      MOZ_CRASH("Unknown response type!");
  }

  MOZ_ASSUME_UNREACHABLE("Should never get here!");
}
