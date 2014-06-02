/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ActorsChild.h"

#include "BackgroundChildImpl.h"
#include "IDBDatabase.h"
#include "IDBEvents.h"
#include "IDBFactory.h"
#include "IDBIndex.h"
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
    nsISupports* mISupports;
    StructuredCloneReadInfo* mStructuredClone;
    const nsTArray<StructuredCloneReadInfo>* mStructuredCloneArray;
    const Key* mKey;
    const nsTArray<Key>* mKeyArray;
    const JS::Handle<JS::Value>* mJSVal;
  } mResult;

  enum
  {
    ResultTypeISupports,
    ResultTypeStructuredClone,
    ResultTypeStructuredCloneArray,
    ResultTypeKey,
    ResultTypeKeyArray,
    ResultTypeJSVal,
  } mResultType;

public:
  ResultHelper(IDBRequest* aRequest,
               IDBTransaction* aTransaction,
               nsISupports* aResult)
    : mRequest(aRequest)
    , mAutoTransaction(aTransaction)
    , mResultType(ResultTypeISupports)
  {
    MOZ_ASSERT(aRequest);
    MOZ_ASSERT(aResult);

    mResult.mISupports = aResult;
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
               const JS::Handle<JS::Value>* aResult)
    : mRequest(aRequest)
    , mAutoTransaction(aTransaction)
    , mResultType(ResultTypeJSVal)
  {
    MOZ_ASSERT(aRequest);

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
      case ResultTypeISupports:
        return helper->GetResult(aCx, helper->mResult.mISupports, aResult);

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
            nsISupports* aUserData,
            JS::MutableHandle<JS::Value> aResult)
  {
    if (!aUserData) {
      aResult.setNull();
      return NS_OK;
    }

    nsresult rv = nsContentUtils::WrapNative(aCx, aUserData, aResult);
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

  PROFILER_LABEL("IndexedDB",
                 "DispatchSuccessEvent",
                 js::ProfileEntry::Category::STORAGE);

  nsRefPtr<IDBRequest> request = aResultHelper->Request();
  MOZ_ASSERT(request);
  request->AssertIsOnOwningThread();

  nsRefPtr<IDBTransaction> transaction = aResultHelper->Transaction();

  nsCOMPtr<nsIDOMEvent> successEvent;
  if (!aEvent) {
    successEvent = CreateGenericEvent(request,
                                      nsDependentString(kSuccessEventType),
                                      eDoesNotBubble,
                                      eNotCancelable);
    if (NS_WARN_IF(!successEvent)) {
      return;
    }

    aEvent = successEvent;
  }

  request->SetResult(&ResultHelper::GetResult, aResultHelper);

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

  PROFILER_LABEL("IndexedDB",
                 "DispatchErrorEvent",
                 js::ProfileEntry::Category::STORAGE);

  nsRefPtr<IDBRequest> request = aRequest;
  nsRefPtr<IDBTransaction> transaction = aTransaction;

  request->SetError(aErrorCode);

  nsCOMPtr<nsIDOMEvent> errorEvent;
  if (!aEvent) {
    // Make an error event and fire it at the target.
    errorEvent = CreateGenericEvent(request,
                                    nsDependentString(kErrorEventType),
                                    eDoesBubble,
                                    eCancelable);
    if (NS_WARN_IF(!errorEvent)) {
      return;
    }

    aEvent = errorEvent;
  }

  Maybe<AutoSetCurrentTransaction> asct;
  if (aTransaction) {
    asct.construct(aTransaction);
  }

  bool doDefault;
  nsresult rv = request->DispatchEvent(aEvent, &doDefault);
  if (NS_SUCCEEDED(rv)) {
    MOZ_ASSERT(!transaction ||
               transaction->IsOpen() ||
               transaction->IsAborted());

    WidgetEvent* internalEvent = aEvent->GetInternalNSEvent();
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
  aRequest->AssertIsOnOwningThread();

  MOZ_COUNT_CTOR(indexedDB::BackgroundRequestChildBase);
}

BackgroundRequestChildBase::~BackgroundRequestChildBase()
{
  AssertIsOnOwningThread();

  MOZ_COUNT_DTOR(indexedDB::BackgroundRequestChildBase);
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

  MOZ_COUNT_CTOR(indexedDB::BackgroundFactoryChild);
}

BackgroundFactoryChild::~BackgroundFactoryChild()
{
  MOZ_COUNT_DTOR(indexedDB::BackgroundFactoryChild);
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
BackgroundFactoryChild::SendDeleteMeInternal()
{
  AssertIsOnOwningThread();

  if (mFactory) {
    mFactory->ClearBackgroundActor();
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
                                               bool aIsDeleteOp,
                                               uint64_t aRequestedVersion,
                                               PersistenceType aPersistenceType)
  : BackgroundRequestChildBase(aOpenRequest)
  , mFactory(aFactory)
  , mRequestedVersion(aRequestedVersion)
  , mPersistenceType(aPersistenceType)
  , mIsDeleteOp(aIsDeleteOp)
{
  // Can't assert owning thread here because IPDL has not yet set our manager!
  MOZ_ASSERT(aFactory);
  aFactory->AssertIsOnOwningThread();
  MOZ_ASSERT(aOpenRequest);

  MOZ_COUNT_CTOR(indexedDB::BackgroundFactoryRequestChild);
}

BackgroundFactoryRequestChild::~BackgroundFactoryRequestChild()
{
  MOZ_COUNT_DTOR(indexedDB::BackgroundFactoryRequestChild);
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

  ResultHelper helper(mRequest, nullptr,
                      static_cast<IDBWrapperCache*>(database));

  DispatchSuccessEvent(&helper);

  databaseActor->ReleaseDOMObject();

  return true;
}

bool
BackgroundFactoryRequestChild::HandleResponse(
                                 const DeleteDatabaseRequestResponse& aResponse)
{
  AssertIsOnOwningThread();

  ResultHelper helper(mRequest, nullptr, &JS::UndefinedHandleValue);

  nsCOMPtr<nsIDOMEvent> successEvent =
    IDBVersionChangeEvent::Create(mRequest,
                                  nsDependentString(kSuccessEventType),
                                  aResponse.previousVersion());
  if (NS_WARN_IF(!successEvent)) {
    return false;
  }

  DispatchSuccessEvent(&helper, successEvent);

  return true;
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

  const nsDependentString type(kBlockedEventType);

  nsCOMPtr<nsIDOMEvent> blockedEvent =
    mIsDeleteOp ?
    IDBVersionChangeEvent::Create(mRequest, type, aCurrentVersion) :
    IDBVersionChangeEvent::Create(mRequest,
                                  type,
                                  aCurrentVersion,
                                  mRequestedVersion);

  if (NS_WARN_IF(!blockedEvent)) {
    return false;
  }

  nsRefPtr<IDBRequest> kungFuDeathGrip = mRequest;

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
  , mPersistenceType(aOpenRequestActor->mPersistenceType)
{
  // Can't assert owning thread here because IPDL has not yet set our manager!
  MOZ_ASSERT(aOpenRequestActor);

  MOZ_COUNT_CTOR(indexedDB::BackgroundDatabaseChild);
}

BackgroundDatabaseChild::~BackgroundDatabaseChild()
{
  MOZ_COUNT_DTOR(indexedDB::BackgroundDatabaseChild);
}

void
BackgroundDatabaseChild::SendDeleteMeInternal()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(!mTemporaryStrongDatabase);
  MOZ_ASSERT(!mOpenRequestActor);

  if (mDatabase) {
    mDatabase->ClearBackgroundActor();
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

  mTemporaryStrongDatabase =
    IDBDatabase::Create(request, factory, this, mSpec, mPersistenceType);

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
    IDBVersionChangeEvent::Create(request,
                                  nsDependentString(kUpgradeNeededEventType),
                                  aCurrentVersion,
                                  aRequestedVersion);
  if (NS_WARN_IF(!upgradeNeededEvent)) {
    return false;
  }

  ResultHelper helper(request, transaction,
                      static_cast<IDBWrapperCache*>(mDatabase));

  DispatchSuccessEvent(&helper, upgradeNeededEvent);

  return true;
}

bool
BackgroundDatabaseChild::DeallocPBackgroundIDBVersionChangeTransactionChild(
                            PBackgroundIDBVersionChangeTransactionChild* aActor)
{
  MOZ_ASSERT(aActor);

  delete static_cast<BackgroundVersionChangeTransactionChild*>(aActor);
  return true;
}

bool
BackgroundDatabaseChild::RecvVersionChange(const uint64_t& aOldVersion,
                                           const NullableVersion& aNewVersion)
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
  const nsDependentString type(kVersionChangeEventType);

  nsCOMPtr<nsIDOMEvent> versionChangeEvent;

  switch (aNewVersion.type()) {
    case NullableVersion::Tnull_t:
      versionChangeEvent =
        IDBVersionChangeEvent::Create(mDatabase, type, aOldVersion);
      break;

    case NullableVersion::Tuint64_t:
      versionChangeEvent =
        IDBVersionChangeEvent::Create(mDatabase,
                                      type,
                                      aOldVersion,
                                      aNewVersion.get_uint64_t());
      break;

    default:
      MOZ_ASSUME_UNREACHABLE("Should never get here!");
  }

  if (NS_WARN_IF(!versionChangeEvent)) {
    return false;
  }

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
  MOZ_COUNT_CTOR(indexedDB::BackgroundTransactionBase);
}

BackgroundTransactionBase::BackgroundTransactionBase(
                                                   IDBTransaction* aTransaction)
  : mTemporaryStrongTransaction(aTransaction)
  , mTransaction(aTransaction)
{
  MOZ_ASSERT(aTransaction);
  aTransaction->AssertIsOnOwningThread();

  MOZ_COUNT_CTOR(indexedDB::BackgroundTransactionBase);
}

BackgroundTransactionBase::~BackgroundTransactionBase()
{
  MOZ_COUNT_DTOR(indexedDB::BackgroundTransactionBase);
}

#ifdef DEBUG

void
BackgroundTransactionBase::AssertIsOnOwningThread() const
{
  MOZ_ASSERT(mTransaction);
  mTransaction->AssertIsOnOwningThread();
}

#endif // DEBUG

void
BackgroundTransactionBase::NoteActorDestroyed()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT_IF(mTemporaryStrongTransaction, mTransaction);

  if (mTransaction) {
    mTransaction->ClearBackgroundActor();

    // Normally this would be DEBUG-only but NoteActorDestroyed is also called
    // from SendDeleteMeInternal. In that case we're going to receive an actual
    // ActorDestroy call later and we don't want to touch a dead object.
    mTemporaryStrongTransaction = nullptr;
    mTransaction = nullptr;
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
  MOZ_ASSERT(aTransaction);
  aTransaction->AssertIsOnOwningThread();

  MOZ_COUNT_CTOR(indexedDB::BackgroundTransactionChild);
}

BackgroundTransactionChild::~BackgroundTransactionChild()
{
  MOZ_COUNT_DTOR(indexedDB::BackgroundTransactionChild);
}

#ifdef DEBUG

void
BackgroundTransactionChild::AssertIsOnOwningThread() const
{
  static_cast<BackgroundDatabaseChild*>(Manager())->AssertIsOnOwningThread();
}

#endif // DEBUG

void
BackgroundTransactionChild::SendDeleteMeInternal()
{
  AssertIsOnOwningThread();

  if (mTransaction) {
    NoteActorDestroyed();

    MOZ_ALWAYS_TRUE(PBackgroundIDBTransactionChild::SendDeleteMe());
  }
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
  MOZ_ASSERT(aActor);

  delete static_cast<BackgroundRequestChild*>(aActor);
  return true;
}

PBackgroundIDBCursorChild*
BackgroundTransactionChild::AllocPBackgroundIDBCursorChild(
                                                const OpenCursorParams& aParams)
{
  AssertIsOnOwningThread();

  MOZ_CRASH("PBackgroundIDBCursorChild actors should be manually constructed!");
}

bool
BackgroundTransactionChild::DeallocPBackgroundIDBCursorChild(
                                              PBackgroundIDBCursorChild* aActor)
{
  MOZ_ASSERT(aActor);

  delete static_cast<BackgroundCursorChild*>(aActor);
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
  aOpenDBRequest->AssertIsOnOwningThread();

  MOZ_COUNT_CTOR(indexedDB::BackgroundVersionChangeTransactionChild);
}

BackgroundVersionChangeTransactionChild::
~BackgroundVersionChangeTransactionChild()
{
  AssertIsOnOwningThread();

  MOZ_COUNT_DTOR(indexedDB::BackgroundVersionChangeTransactionChild);
}

#ifdef DEBUG

void
BackgroundVersionChangeTransactionChild::AssertIsOnOwningThread() const
{
  static_cast<BackgroundDatabaseChild*>(Manager())->AssertIsOnOwningThread();
}

#endif // DEBUG

void
BackgroundVersionChangeTransactionChild::SendDeleteMeInternal()
{
  AssertIsOnOwningThread();

  if (mTransaction) {
    NoteActorDestroyed();

    MOZ_ALWAYS_TRUE(PBackgroundIDBVersionChangeTransactionChild::
                      SendDeleteMe());
  }
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
  MOZ_ASSERT(aActor);

  delete static_cast<BackgroundRequestChild*>(aActor);
  return true;
}

PBackgroundIDBCursorChild*
BackgroundVersionChangeTransactionChild::AllocPBackgroundIDBCursorChild(
                                                const OpenCursorParams& aParams)
{
  AssertIsOnOwningThread();

  MOZ_CRASH("PBackgroundIDBCursorChild actors should be manually constructed!");
}

bool
BackgroundVersionChangeTransactionChild::DeallocPBackgroundIDBCursorChild(
                                              PBackgroundIDBCursorChild* aActor)
{
  MOZ_ASSERT(aActor);

  delete static_cast<BackgroundCursorChild*>(aActor);
  return true;
}

/*******************************************************************************
 * BackgroundTransactionRequestChildBase
 ******************************************************************************/

BackgroundTransactionRequestChildBase::BackgroundTransactionRequestChildBase(
                                                           IDBRequest* aRequest)
  : BackgroundRequestChildBase(aRequest)
  , mTransaction(aRequest->GetTransaction())
{
  MOZ_ASSERT(mTransaction);
  mTransaction->AssertIsOnOwningThread();

  MOZ_COUNT_CTOR(indexedDB::BackgroundTransactionRequestChildBase);
}

BackgroundTransactionRequestChildBase::~BackgroundTransactionRequestChildBase()
{
  MOZ_COUNT_DTOR(indexedDB::BackgroundTransactionRequestChildBase);
}

/*******************************************************************************
 * BackgroundRequestChild
 ******************************************************************************/

BackgroundRequestChild::BackgroundRequestChild(IDBRequest* aRequest)
  : BackgroundTransactionRequestChildBase(aRequest)
{
  MOZ_ASSERT(mTransaction);

  MOZ_COUNT_CTOR(indexedDB::BackgroundRequestChild);

  mTransaction->OnNewRequest();
}

BackgroundRequestChild::~BackgroundRequestChild()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mTransaction);
  mTransaction->AssertIsOnOwningThread();

  MOZ_COUNT_DTOR(indexedDB::BackgroundRequestChild);

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
      return HandleResponse(JS::UndefinedHandleValue);

    case RequestResponse::TObjectStoreClearResponse:
      return HandleResponse(JS::UndefinedHandleValue);

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

/*******************************************************************************
 * BackgroundCursorChild
 ******************************************************************************/

BackgroundCursorChild::BackgroundCursorChild(IDBRequest* aRequest,
                                             IDBObjectStore* aObjectStore,
                                             Direction aDirection)
  : BackgroundTransactionRequestChildBase(aRequest)
  , mObjectStore(aObjectStore)
  , mIndex(nullptr)
  , mCursor(nullptr)
  , mDirection(aDirection)
{
  MOZ_ASSERT(aObjectStore);
  aObjectStore->AssertIsOnOwningThread();
  MOZ_ASSERT(mTransaction);

  MOZ_COUNT_CTOR(indexedDB::BackgroundCursorChild);
}

BackgroundCursorChild::BackgroundCursorChild(IDBRequest* aRequest,
                                             IDBIndex* aIndex,
                                             Direction aDirection)
  : BackgroundTransactionRequestChildBase(aRequest)
  , mObjectStore(nullptr)
  , mIndex(aIndex)
  , mCursor(nullptr)
  , mDirection(aDirection)
{
  MOZ_ASSERT(aIndex);
  aIndex->AssertIsOnOwningThread();
  MOZ_ASSERT(mTransaction);

  MOZ_COUNT_CTOR(indexedDB::BackgroundCursorChild);
}

BackgroundCursorChild::~BackgroundCursorChild()
{
  MOZ_COUNT_DTOR(indexedDB::BackgroundCursorChild);
}

void
BackgroundCursorChild::SendContinueInternal(const CursorRequestParams& aParams)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mRequest);
  MOZ_ASSERT(mTransaction);
  MOZ_ASSERT(mCursor);
  MOZ_ASSERT(!mStrongCursor);

  MOZ_ASSERT(mRequest->ReadyState() == IDBRequestReadyState::Done);
  mRequest->Reset();

  mTransaction->OnNewRequest();

  MOZ_ALWAYS_TRUE(PBackgroundIDBCursorChild::SendContinue(aParams));

  mStrongCursor = mCursor;
}

void
BackgroundCursorChild::SendDeleteMeInternal()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(!mStrongCursor);

  if (mCursor) {
    mCursor->ClearBackgroundActor();

    mObjectStore = nullptr;
    mIndex = nullptr;
    mCursor = nullptr;

    MOZ_ALWAYS_TRUE(PBackgroundIDBCursorChild::SendDeleteMe());
  }
}

void
BackgroundCursorChild::HandleResponse(
                                     const ObjectStoreCursorResponse& aResponse)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mObjectStore);

  // XXX Fix this somehow...
  auto& response = const_cast<ObjectStoreCursorResponse&>(aResponse);

  StructuredCloneReadInfo cloneReadInfo(Move(response.cloneInfo()));

  // XXX Fix blobs!
  /*
  IDBObjectStore::ConvertActorsToBlobs(aResponse.blobsChild(),
                                       cloneReadInfo.mFiles);
  */

  nsRefPtr<IDBCursor> cursor = mCursor;

  if (cursor) {
    cursor->Reset(Move(response.key()), Move(cloneReadInfo));
  } else {
    cursor = IDBCursor::Create(mRequest, mObjectStore, this, mDirection,
                               Move(response.key()), Move(cloneReadInfo));
    mCursor = cursor;
  }

  ResultHelper helper(mRequest, mTransaction, mCursor);

  DispatchSuccessEvent(&helper);
}

void
BackgroundCursorChild::HandleResponse(
                                  const ObjectStoreKeyCursorResponse& aResponse)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mObjectStore);

  // XXX Fix this somehow...
  auto& response = const_cast<ObjectStoreKeyCursorResponse&>(aResponse);

  nsRefPtr<IDBCursor> cursor = mCursor;

  if (cursor) {
    cursor->Reset(Move(response.key()));
  } else {
    cursor = IDBCursor::Create(mRequest, mObjectStore, this, mDirection,
                               Move(response.key()));
    mCursor = cursor;
  }

  ResultHelper helper(mRequest, mTransaction, mCursor);

  DispatchSuccessEvent(&helper);
}

void
BackgroundCursorChild::HandleResponse(const IndexCursorResponse& aResponse)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mIndex);

  // XXX Fix this somehow...
  auto& response = const_cast<IndexCursorResponse&>(aResponse);

  StructuredCloneReadInfo cloneReadInfo(Move(response.cloneInfo()));

  // XXX Fix blobs!
  /*
  IDBObjectStore::ConvertActorsToBlobs(aResponse.blobsChild(),
                                       cloneReadInfo.mFiles);
  */

  nsRefPtr<IDBCursor> cursor = mCursor;

  if (cursor) {
    cursor->Reset(Move(response.key()), Move(response.objectKey()),
                  Move(cloneReadInfo));
  } else {
    cursor = IDBCursor::Create(mRequest, mIndex, this, mDirection,
                               Move(response.key()), Move(response.objectKey()),
                               Move(cloneReadInfo));
    mCursor = cursor;
  }

  ResultHelper helper(mRequest, mTransaction, mCursor);

  DispatchSuccessEvent(&helper);
}

void
BackgroundCursorChild::HandleResponse(const IndexKeyCursorResponse& aResponse)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mIndex);

  // XXX Fix this somehow...
  auto& response = const_cast<IndexKeyCursorResponse&>(aResponse);

  nsRefPtr<IDBCursor> cursor = mCursor;

  if (cursor) {
    cursor->Reset(Move(response.key()), Move(response.objectKey()));
  } else {
    cursor = IDBCursor::Create(mRequest, mIndex, this, mDirection,
                               Move(response.key()),
                               Move(response.objectKey()));
    mCursor = cursor;
  }

  ResultHelper helper(mRequest, mTransaction, mCursor);

  DispatchSuccessEvent(&helper);
}

void
BackgroundCursorChild::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(!mStrongCursor);

  if (mCursor) {
    mCursor->ClearBackgroundActor();
#ifdef DEBUG
    mObjectStore = nullptr;
    mIndex = nullptr;
    mCursor = nullptr;
#endif
  }
}

bool
BackgroundCursorChild::RecvResponse(const CursorResponse& aResponse)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aResponse.type() != CursorResponse::T__None);
  MOZ_ASSERT(mRequest);
  MOZ_ASSERT(mTransaction);
  MOZ_ASSERT_IF(mCursor, mStrongCursor);

  nsRefPtr<IDBCursor> cursor;
  mStrongCursor.swap(cursor);

  switch (aResponse.type()) {
    case CursorResponse::Tnsresult: {
      nsresult response = aResponse.get_nsresult();
      MOZ_ASSERT(NS_FAILED(response));
      MOZ_ASSERT(NS_ERROR_GET_MODULE(response) ==
                 NS_ERROR_MODULE_DOM_INDEXEDDB);
      MOZ_ASSERT(mTransaction);

      DispatchErrorEvent(mRequest, response, mTransaction);
      break;
    }

    case CursorResponse::Tvoid_t: {
      if (mCursor) {
        mCursor->Reset();
      }

      ResultHelper helper(mRequest, mTransaction, &JS::NullHandleValue);

      DispatchSuccessEvent(&helper);
      break;
    }

    case CursorResponse::TObjectStoreCursorResponse:
      HandleResponse(aResponse.get_ObjectStoreCursorResponse());
      break;

    case CursorResponse::TObjectStoreKeyCursorResponse:
      HandleResponse(aResponse.get_ObjectStoreKeyCursorResponse());
      break;

    case CursorResponse::TIndexCursorResponse:
      HandleResponse(aResponse.get_IndexCursorResponse());
      break;

    case CursorResponse::TIndexKeyCursorResponse:
      HandleResponse(aResponse.get_IndexKeyCursorResponse());
      break;

    default:
      MOZ_ASSUME_UNREACHABLE("Should never get here!");
  }

  mTransaction->OnRequestFinished();

  return true;
}
