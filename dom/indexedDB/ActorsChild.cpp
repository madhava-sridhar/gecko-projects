/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ActorsChild.h"

#include "IDBFactory.h"
#include "IDBRequest.h"
#include "nsCOMPtr.h"
#include "nsIEventTarget.h"
#include "nsThreadUtils.h"
#include "nsTraceRefcnt.h"

USING_INDEXEDDB_NAMESPACE

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
BackgroundFactoryChild::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnOwningThread();

  if (mFactory) {
    mFactory->SetBackgroundActor(nullptr);
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
                                              const DatabaseMetadata& aMetadata)
{
  AssertIsOnOwningThread();

  return new BackgroundDatabaseChild(aMetadata);
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
                                                 const nsACString& aDatabaseId)
  : BackgroundRequestChildBase(aOpenRequest)
  , mFactory(aFactory)
  , mDatabaseId(aDatabaseId)
{
  // Can't assert owning thread here because IPDL has not yet set our manager!
  MOZ_ASSERT(aFactory);
  MOZ_ASSERT(aOpenRequest);
  MOZ_ASSERT(!aDatabaseId.IsEmpty());
  MOZ_COUNT_CTOR(mozilla::dom::indexedDB::BackgroundFactoryRequestChild);
}

BackgroundFactoryRequestChild::~BackgroundFactoryRequestChild()
{
  AssertIsOnOwningThread();
  MOZ_COUNT_DTOR(mozilla::dom::indexedDB::BackgroundFactoryRequestChild);
}

bool
BackgroundFactoryRequestChild::Recv__delete__(
                                        const FactoryRequestResponse& aResponse)
{
  AssertIsOnOwningThread();

  switch (aResponse.type()) {
    case FactoryRequestResponse::Tnsresult: {
      mRequest->DispatchError(aResponse.get_nsresult());
      break;
    }
    case FactoryRequestResponse::TOpenDatabaseRequestResponse: {
      MOZ_CRASH("Implement me!");
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

  MOZ_CRASH("Implement me!");
  return true;
}

/*******************************************************************************
 * BackgroundDatabaseChild
 ******************************************************************************/

BackgroundDatabaseChild::BackgroundDatabaseChild(
                                              const DatabaseMetadata& aMetadata)
  : mMetadata(aMetadata)
{
  // Can't assert owning thread here because IPDL has not yet set our manager!
  MOZ_COUNT_CTOR(mozilla::dom::indexedDB::BackgroundDatabaseChild);
}

BackgroundDatabaseChild::~BackgroundDatabaseChild()
{
  AssertIsOnOwningThread();
  MOZ_COUNT_DTOR(mozilla::dom::indexedDB::BackgroundDatabaseChild);
}

void
BackgroundDatabaseChild::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnOwningThread();
  MOZ_CRASH("Implement me!");
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
                                              const DatabaseMetadata& aMetadata)
{
  AssertIsOnOwningThread();

  return new BackgroundVersionChangeTransactionChild(aMetadata);
}

bool
BackgroundDatabaseChild::RecvPBackgroundIDBVersionChangeTransactionConstructor(
                            PBackgroundIDBVersionChangeTransactionChild* aActor,
                            const DatabaseMetadata& aMetadata)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aActor);

  MOZ_CRASH("Implement me!");
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
  MOZ_CRASH("Implement me!");
}

bool
BackgroundDatabaseChild::RecvInvalidate()
{
  AssertIsOnOwningThread();
  MOZ_CRASH("Implement me!");
}

/*******************************************************************************
 * BackgroundTransactionChild
 ******************************************************************************/

BackgroundTransactionChild::BackgroundTransactionChild()
{
  // Can't assert owning thread here because IPDL has not yet set our manager!
  MOZ_COUNT_CTOR(mozilla::dom::indexedDB::BackgroundTransactionChild);
}

BackgroundTransactionChild::~BackgroundTransactionChild()
{
  AssertIsOnOwningThread();
  MOZ_COUNT_DTOR(mozilla::dom::indexedDB::BackgroundTransactionChild);
}

void
BackgroundTransactionChild::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnOwningThread();
  MOZ_CRASH("Implement me!");
}

bool
BackgroundTransactionChild::RecvComplete(const nsresult& aResult)
{
  AssertIsOnOwningThread();
  MOZ_CRASH("Implement me!");
}

/*******************************************************************************
 * BackgroundVersionChangeTransactionChild
 ******************************************************************************/

BackgroundVersionChangeTransactionChild::
BackgroundVersionChangeTransactionChild(const DatabaseMetadata& aMetadata)
  : mMetadata(aMetadata)
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

void
BackgroundVersionChangeTransactionChild::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnOwningThread();
  MOZ_CRASH("Implement me!");
}

bool
BackgroundVersionChangeTransactionChild::RecvComplete(const nsresult& aResult)
{
  AssertIsOnOwningThread();
  MOZ_CRASH("Implement me!");
}
