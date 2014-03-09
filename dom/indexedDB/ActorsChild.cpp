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
#include "mozilla/BasicEvents.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsIDOMEvent.h"
#include "nsIEventTarget.h"
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
  const DebugOnly<IDBTransaction*> mDEBUGTransaction;

public:
  AutoSetCurrentTransaction(IDBTransaction* aTransaction)
    : mThreadLocal(nullptr)
    , mDEBUGTransaction(aTransaction)
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
    MOZ_ASSERT((mThreadLocal && mDEBUGTransaction) ||
               (!mThreadLocal && !mDEBUGTransaction));
    if (mThreadLocal) {
      MOZ_ASSERT(mDEBUGTransaction == mThreadLocal->mCurrentTransaction);
      mThreadLocal->mCurrentTransaction = nullptr;
    }
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
    const Key* mKey;
  } mResult;

  enum
  {
    ResultTypeISupports,
    ResultTypeStructuredClone,
    ResultTypeKey
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
               const Key* aResult)
    : mRequest(aRequest)
    , mAutoTransaction(aTransaction)
    , mResultType(ResultTypeKey)
  {
    MOZ_ASSERT(aRequest);
    MOZ_ASSERT(aResult);

    mResult.mKey = aResult;
  }

  IDBRequest*
  Request() const
  {
    return mRequest;
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
      case ResultTypeISupports: {
        if (nsISupports* result = helper->mResult.mISupports) {
          auto* wrapperCache = static_cast<IDBWrapperCache*>(helper->mRequest);

          JS::Rooted<JSObject*> global(aCx, wrapperCache->GetParentObject());
          MOZ_ASSERT(global);

          nsresult rv =
            nsContentUtils::WrapNative(aCx, global, result, aResult);
          if (NS_WARN_IF(NS_FAILED(rv))) {
            IDB_REPORT_INTERNAL_ERR();
            return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
          }
        } else {
          aResult.setNull();
        }
        break;
      }

      case ResultTypeStructuredClone: {
        StructuredCloneReadInfo* result = helper->mResult.mStructuredClone;
        MOZ_ASSERT(result);

        bool ok = IDBObjectStore::DeserializeValue(aCx, *result, aResult);

        result->mCloneBuffer.clear();

        if (NS_WARN_IF(!ok)) {
          return NS_ERROR_DOM_DATA_CLONE_ERR;
        }
        break;
      }

      case ResultTypeKey: {
        const Key* result = helper->mResult.mKey;
        MOZ_ASSERT(result);

        nsresult rv = result->ToJSVal(aCx, aResult);
        if (NS_WARN_IF(NS_FAILED(rv))) {
          return rv;
        }
        break;
      }

      default:
        MOZ_CRASH("Unknown result type!");
    }

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
                     IDBTransaction* aTransaction = nullptr,
                     nsIDOMEvent* aEvent = nullptr)
{
  MOZ_ASSERT(aResultHelper);

  PROFILER_LABEL("IndexedDB", "DispatchSuccessEvent");

  IDBRequest* request = aResultHelper->Request();
  MOZ_ASSERT(request);
  request->AssertIsOnOwningThread();

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
  MOZ_ASSERT_IF(aTransaction, aTransaction->IsOpen());

  bool dummy;
  nsresult rv = request->DispatchEvent(aEvent, &dummy);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return;
  }

  MOZ_ASSERT_IF(aTransaction,
                aTransaction->IsOpen() || aTransaction->IsAborted());

  WidgetEvent* internalEvent = aEvent->GetInternalNSEvent();
  MOZ_ASSERT(internalEvent);

  if (aTransaction &&
      aTransaction->IsOpen() &&
      internalEvent->mFlags.mExceptionHasBeenRisen) {
    MOZ_ALWAYS_TRUE(
      NS_SUCCEEDED(aTransaction->Abort(NS_ERROR_DOM_INDEXEDDB_ABORT_ERR)));
  }
}

void
DispatchErrorEvent(IDBRequest* aRequest,
                   nsresult aErrorCode,
                   IDBTransaction* aTransaction = nullptr,
                   nsIDOMEvent* aEvent = nullptr)
{
  MOZ_ASSERT(aRequest);
  MOZ_ASSERT(NS_FAILED(aErrorCode));
  MOZ_ASSERT(NS_ERROR_GET_MODULE(aErrorCode) == NS_ERROR_MODULE_DOM_INDEXEDDB);

  PROFILER_LABEL("IndexedDB", "DispatchErrorEvent");

  aRequest->SetError(aErrorCode);

  nsCOMPtr<nsIDOMEvent> event = aEvent;
  if (!event) {
    // Make an error event and fire it at the target.
    event = CreateGenericEvent(aRequest, NS_LITERAL_STRING(ERROR_EVT_STR),
                               eDoesBubble, eCancelable);
    MOZ_ASSERT(event);
  }

  Maybe<AutoSetCurrentTransaction> asct;
  if (aTransaction) {
    asct.construct(aTransaction);
  }

  bool doDefault;
  nsresult rv = aRequest->DispatchEvent(event, &doDefault);
  if (NS_SUCCEEDED(rv)) {
    MOZ_ASSERT(!aTransaction ||
               aTransaction->IsOpen() ||
               aTransaction->IsAborted());

    WidgetEvent* internalEvent = event->GetInternalNSEvent();
    MOZ_ASSERT(internalEvent);

    if (internalEvent->mFlags.mExceptionHasBeenRisen &&
        aTransaction &&
        aTransaction->IsOpen()) {
      NS_WARN_IF(NS_FAILED(
        aTransaction->Abort(NS_ERROR_DOM_INDEXEDDB_ABORT_ERR)));
    }

    if (doDefault &&
        aTransaction &&
        aTransaction->IsOpen()) {
      NS_WARN_IF(NS_FAILED(aTransaction->Abort(aRequest)));
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
                                                 IDBOpenDBRequest* aOpenRequest)
  : BackgroundRequestChildBase(aOpenRequest)
  , mFactory(aFactory)
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

  MOZ_CRASH("Implement me!");
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

  return new BackgroundVersionChangeTransactionChild();
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

  mDatabase->EnterSetVersionTransaction();

  nsRefPtr<IDBOpenDBRequest> request = mOpenRequestActor->GetOpenDBRequest();
  MOZ_ASSERT(request);

  request->SetTransaction(transaction);

  nsCOMPtr<nsIDOMEvent> upgradeNeededEvent =
    IDBVersionChangeEvent::CreateUpgradeNeeded(request, aCurrentVersion,
                                               aRequestedVersion);

  ResultHelper helper(request, transaction,
                      static_cast<IDBWrapperCache*>(mDatabase));

  DispatchSuccessEvent(&helper, transaction, upgradeNeededEvent);

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

  if (!mDatabase) {
    return true;
  }

  MOZ_CRASH("Implement me!");
}

bool
BackgroundDatabaseChild::RecvInvalidate()
{
  AssertIsOnOwningThread();

  if (!mDatabase) {
    return true;
  }

  MOZ_CRASH("Implement me!");
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
BackgroundVersionChangeTransactionChild()
{
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

  NoteActorDestroyed();
}

bool
BackgroundVersionChangeTransactionChild::RecvComplete(const nsresult& aResult)
{
  AssertIsOnOwningThread();

  if (!mTransaction) {
    return true;
  }

  IDBDatabase* database = mTransaction->Database();
  MOZ_ASSERT(database);

  database->ExitSetVersionTransaction();

  if (NS_FAILED(aResult)) {
    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(database->Close()));
  }

  mTransaction->FireCompleteOrAbortEvents(aResult);

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
BackgroundRequestChild::HandleResponse(const ObjectStoreGetResponse& aResponse)
{
  AssertIsOnOwningThread();

  const SerializedStructuredCloneReadInfo& cloneInfo = aResponse.cloneInfo();

  StructuredCloneReadInfo cloneReadInfo;
  if (!cloneReadInfo.SetFromSerialized(cloneInfo)) {
    IDB_WARNING("Failed to copy clone buffer!");
    return HandleResponse(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  }

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
BackgroundRequestChild::HandleResponse(const Key& aResponse)
{
  AssertIsOnOwningThread();

  ResultHelper helper(mRequest, mTransaction, &aResponse);

  DispatchSuccessEvent(&helper);
  return true;
}

bool
BackgroundRequestChild::Recv__delete__(const RequestResponse& aResponse)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mRequest);
  MOZ_ASSERT(mTransaction);

  switch (aResponse.type()) {
    case RequestResponse::Tnsresult:
      return HandleResponse(aResponse.get_nsresult());

    case RequestResponse::TObjectStoreGetResponse:
      return HandleResponse(aResponse.get_ObjectStoreGetResponse());

    case RequestResponse::TObjectStoreAddResponse:
      return HandleResponse(aResponse.get_ObjectStoreAddResponse().key());

    case RequestResponse::TObjectStorePutResponse:
      return HandleResponse(aResponse.get_ObjectStorePutResponse().key());

    default:
      MOZ_CRASH("Unknown response type!");
  }

  MOZ_ASSUME_UNREACHABLE("Should never get here!");
}
