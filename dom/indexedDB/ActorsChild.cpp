/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ActorsChild.h"

#include "BackgroundChildImpl.h"
#include "IDBDatabase.h"
#include "IDBEvents.h"
#include "IDBFactory.h"
#include "IDBRequest.h"
#include "IDBTransaction.h"
#include "mozilla/BasicEvents.h"
#include "mozilla/ipc/BackgroundChild.h"
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

class MOZ_STACK_CLASS ResultHelper MOZ_FINAL
{
  typedef mozilla::ipc::BackgroundChildImpl BackgroundChildImpl;

  IDBWrapperCache* mWrapper;
  nsISupports* mResult;
  BackgroundChildImpl::ThreadLocal* mThreadLocal;

public:
  ResultHelper(IDBWrapperCache* aWrapper, nsISupports* aResult,
               IDBTransaction* aTransaction = nullptr)
    : mWrapper(aWrapper)
    , mResult(aResult)
    , mThreadLocal(nullptr)
  {
    MOZ_ASSERT(aWrapper);

    if (aTransaction) {
      mThreadLocal = BackgroundChildImpl::GetThreadLocalForCurrentThread();
      MOZ_ASSERT(mThreadLocal);

      MOZ_ASSERT(!mThreadLocal->mCurrentTransaction);
      mThreadLocal->mCurrentTransaction = aTransaction;
    }
  }

  ~ResultHelper()
  {
    if (mThreadLocal) {
      MOZ_ASSERT(mThreadLocal->mCurrentTransaction);
      mThreadLocal->mCurrentTransaction = nullptr;
    }
  }

  static nsresult
  GetResult(JSContext* aCx,
            void* aUserData,
            JS::MutableHandle<JS::Value> aResult)
  {
    MOZ_ASSERT(aCx);

    auto helper = static_cast<ResultHelper*>(aUserData);
    MOZ_ASSERT(helper);
    MOZ_ASSERT(helper->mWrapper);

    if (helper->mResult) {
      JS::Rooted<JSObject*> global(aCx, helper->mWrapper->GetParentObject());
      MOZ_ASSERT(global);

      nsresult rv =
        nsContentUtils::WrapNative(aCx, global, helper->mResult, aResult);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        IDB_REPORT_INTERNAL_ERR();
        return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }
    } else {
      aResult.setNull();
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
DispatchSuccessEvent(IDBRequest* aRequest,
                     ResultHelper* aResultHelper,
                     IDBTransaction* aTransaction = nullptr,
                     nsIDOMEvent* aEvent = nullptr)
{
  MOZ_ASSERT(aRequest);
  MOZ_ASSERT(aResultHelper);

  PROFILER_LABEL("IndexedDB", "DispatchSuccessEvent");

  nsCOMPtr<nsIDOMEvent> newEvent;
  if (!aEvent) {
    newEvent = CreateGenericEvent(aRequest, NS_LITERAL_STRING(SUCCESS_EVT_STR),
                                  eDoesNotBubble, eNotCancelable);
    if (NS_WARN_IF(!newEvent)) {
      return;
    }

    aEvent = newEvent;
  }

  aRequest->SetResultCallback(&ResultHelper::GetResult, aResultHelper);

  MOZ_ASSERT(aEvent);
  MOZ_ASSERT_IF(aTransaction, aTransaction->IsOpen());

  bool dummy;
  nsresult rv = aRequest->DispatchEvent(aEvent, &dummy);
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
BackgroundFactoryRequestChild::GetDOMObject() const
{
  AssertIsOnOwningThread();

  IDBRequest* baseRequest = BackgroundRequestChildBase::GetDOMObject();
  return static_cast<IDBOpenDBRequest*>(baseRequest);
}

bool
BackgroundFactoryRequestChild::Recv__delete__(
                                        const FactoryRequestResponse& aResponse)
{
  AssertIsOnOwningThread();

  switch (aResponse.type()) {
    case FactoryRequestResponse::Tnsresult: {
      MOZ_ASSERT(NS_FAILED(aResponse.get_nsresult()));
      mRequest->DispatchError(aResponse.get_nsresult());
      break;
    }

    case FactoryRequestResponse::TOpenDatabaseRequestResponse: {
      const OpenDatabaseRequestResponse& response =
        aResponse.get_OpenDatabaseRequestResponse();
      auto databaseActor =
        static_cast<BackgroundDatabaseChild*>(response.databaseChild());
      MOZ_ASSERT(databaseActor);

      if (!databaseActor->EnsureDOMObject()) {
        return false;
      }

      mRequest->Reset();

      IDBDatabase* database = databaseActor->GetDOMObject();
      MOZ_ASSERT(database);

      ResultHelper helper(mRequest, static_cast<IDBWrapperCache*>(database));

      DispatchSuccessEvent(mRequest, &helper);

      databaseActor->ReleaseDOMObject();

      break;
    }

    case FactoryRequestResponse::TDeleteDatabaseRequestResponse: {
      MOZ_CRASH("Implement me!");
      break;
    }

    default:
      MOZ_CRASH("Unknown response type!");
  }

  return true;
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
  if (!transaction) {
    return false;
  }

  transaction->AssertIsOnOwningThread();

  actor->SetDOMObject(transaction);

  mDatabase->EnterSetVersionTransaction();

  nsRefPtr<IDBOpenDBRequest> request = mOpenRequestActor->GetDOMObject();
  MOZ_ASSERT(request);

  request->SetTransaction(transaction);

  nsCOMPtr<nsIDOMEvent> upgradeNeededEvent =
    IDBVersionChangeEvent::CreateUpgradeNeeded(request, aCurrentVersion,
                                               aRequestedVersion);

  ResultHelper helper(request, static_cast<IDBWrapperCache*>(mDatabase),
                      transaction);

  DispatchSuccessEvent(request, &helper, transaction, upgradeNeededEvent);

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
{
}

BackgroundTransactionBase::BackgroundTransactionBase(
                                                   IDBTransaction* aTransaction)
  : mTransaction(aTransaction)
{
  MOZ_ASSERT(aTransaction);
}

BackgroundTransactionBase::~BackgroundTransactionBase()
{
}

void
BackgroundTransactionBase::NoteActorDestroyed()
{
  AssertIsOnOwningThread();

  if (mTransaction) {
    mTransaction->ClearBackgroundActor();
#ifdef DEBUG
    mTransaction = nullptr;
#endif
  }
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
BackgroundVersionChangeTransactionChild::SetDOMObject(
                                                   IDBTransaction* aTransaction)
{
  MOZ_ASSERT(aTransaction);
  MOZ_ASSERT(!mTransaction);

  mTransaction = aTransaction;
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

  return true;
}
