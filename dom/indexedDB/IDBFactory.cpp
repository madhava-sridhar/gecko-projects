/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDBFactory.h"

#include "ActorsChild.h"
#include "IDBRequest.h"
#include "IndexedDatabaseManager.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/dom/BindingDeclarations.h"
#include "mozilla/dom/IDBFactoryBinding.h"
#include "mozilla/dom/TabChild.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "mozilla/ipc/PBackground.h"
#include "mozilla/ipc/PBackgroundChild.h"
#include "nsGlobalWindow.h"
#include "nsIIPCBackgroundChildCreateCallback.h"
#include "nsIPrincipal.h"
#include "ProfilerHelpers.h"
#include "ReportInternalError.h"

#ifdef DEBUG
#include "nsContentUtils.h" // For IsCallerChrome assertions.
#endif

#define PREF_INDEXEDDB_ENABLED "dom.indexedDB.enabled"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::indexedDB;
using namespace mozilla::dom::quota;
using namespace mozilla::ipc;

namespace {

nsresult
GetPrincipalInfo(nsIPrincipal* aPrincipal,
                 PrincipalInfo* aPrincipalInfo)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aPrincipal);
  MOZ_ASSERT(aPrincipalInfo);

  bool isNullPrincipal;
  nsresult rv = aPrincipal->GetIsNullPrincipal(&isNullPrincipal);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (isNullPrincipal) {
    NS_WARNING("IndexedDB not supported from this principal!");
    IDB_REPORT_INTERNAL_ERR();
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  rv = PrincipalToPrincipalInfo(aPrincipal, aPrincipalInfo);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

} // anonymous namespace

class IDBFactory::BackgroundCreateCallback MOZ_FINAL
  : public nsIIPCBackgroundChildCreateCallback
{
  nsRefPtr<IDBFactory> mFactory;

public:
  BackgroundCreateCallback(IDBFactory* aFactory)
  : mFactory(aFactory)
  {
    MOZ_ASSERT(aFactory);
  }

  NS_DECL_ISUPPORTS

private:
  ~BackgroundCreateCallback()
  { }

  NS_DECL_NSIIPCBACKGROUNDCHILDCREATECALLBACK
};

struct IDBFactory::PendingRequestInfo
{
  nsRefPtr<IDBOpenDBRequest> mRequest;
  FactoryRequestParams mParams;

  PendingRequestInfo(IDBOpenDBRequest* aRequest,
                     const FactoryRequestParams& aParams)
  : mRequest(aRequest), mParams(aParams)
  {
    MOZ_ASSERT(aRequest);
    MOZ_ASSERT(aParams.type() != FactoryRequestParams::T__None);
  }
};

IDBFactory::IDBFactory()
  : mOwningObject(nullptr)
  , mBackgroundActor(nullptr)
  , mRootedOwningObject(false)
  , mBackgroundActorFailed(false)
{
#ifdef DEBUG
  mOwningThread = PR_GetCurrentThread();
#endif
  AssertIsOnOwningThread();

  SetIsDOMBinding();
}

IDBFactory::~IDBFactory()
{
  MOZ_ASSERT_IF(mBackgroundActorFailed, !mBackgroundActor);

  if (mRootedOwningObject) {
    mOwningObject = nullptr;
    mozilla::DropJSObjects(this);
  }

  if (mBackgroundActor) {
    mBackgroundActor->SendDeleteMeInternal();
    MOZ_ASSERT(!mBackgroundActor, "SendDeleteMeInternal should have cleared!");
  }
}

// static
nsresult
IDBFactory::Create(nsPIDOMWindow* aWindow,
                   IDBFactory** aFactory)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aWindow);

  if (aWindow->IsOuterWindow()) {
    aWindow = aWindow->GetCurrentInnerWindow();
    IDB_ENSURE_TRUE(aWindow, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  }

  // Make sure that the manager is up before we do anything here since lots of
  // decisions depend on which process we're running in.
  IndexedDatabaseManager* mgr = IndexedDatabaseManager::GetOrCreate();
  IDB_ENSURE_TRUE(mgr, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  nsCOMPtr<nsIScriptObjectPrincipal> sop = do_QueryInterface(aWindow);
  IDB_ENSURE_TRUE(sop, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  nsCOMPtr<nsIPrincipal> principal = sop->GetPrincipal();
  IDB_ENSURE_TRUE(principal, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  nsAutoPtr<PrincipalInfo> principalInfo(new PrincipalInfo());

  if (NS_WARN_IF(NS_FAILED(GetPrincipalInfo(principal, principalInfo)))) {
    // Not allowed.
    *aFactory = nullptr;
    return NS_OK;
  }

  nsRefPtr<IDBFactory> factory = new IDBFactory();
  factory->mPrincipalInfo = Move(principalInfo);
  factory->mWindow = aWindow;
  factory->mTabChild = TabChild::GetFrom(aWindow);

  factory.forget(aFactory);
  return NS_OK;
}

// static
nsresult
IDBFactory::Create(JSContext* aCx,
                   JS::Handle<JSObject*> aOwningObject,
                   IDBFactory** aFactory)
{
  MOZ_ASSERT_IF(NS_IsMainThread(), nsContentUtils::IsCallerChrome());
  MOZ_ASSERT(aCx);
  MOZ_ASSERT(aOwningObject);
  MOZ_ASSERT(JS_GetGlobalForObject(aCx, aOwningObject) == aOwningObject,
             "Not a global object!");

  if (!NS_IsMainThread()) {
    MOZ_CRASH("Implement me for workers!");
  }

  // Make sure that the manager is up before we do anything here since lots of
  // decisions depend on which process we're running in.
  IndexedDatabaseManager* mgr = IndexedDatabaseManager::GetOrCreate();
  IDB_ENSURE_TRUE(mgr, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  nsRefPtr<IDBFactory> factory = new IDBFactory();
  factory->mPrincipalInfo = new PrincipalInfo(SystemPrincipalInfo());
  factory->mOwningObject = aOwningObject;

  mozilla::HoldJSObjects(factory.get());
  factory->mRootedOwningObject = true;

  factory.forget(aFactory);
  return NS_OK;
}

#ifdef DEBUG

void
IDBFactory::AssertIsOnOwningThread() const
{
  MOZ_ASSERT(mOwningThread);
  MOZ_ASSERT(PR_GetCurrentThread() == mOwningThread);
}

#endif // DEBUG

void
IDBFactory::SetBackgroundActor(BackgroundFactoryChild* aBackgroundActor)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aBackgroundActor);
  MOZ_ASSERT(!mBackgroundActor);

  mBackgroundActor = aBackgroundActor;
}

already_AddRefed<IDBOpenDBRequest>
IDBFactory::Open(const nsAString& aName,
                 uint64_t aVersion,
                 ErrorResult& aRv)
{
  return OpenInternal(nullptr, aName, Optional<uint64_t>(aVersion),
                      Optional<StorageType>(), false, aRv);
}

already_AddRefed<IDBOpenDBRequest>
IDBFactory::Open(const nsAString& aName,
                 const IDBOpenDBOptions& aOptions,
                 ErrorResult& aRv)
{
  return OpenInternal(nullptr, aName, aOptions.mVersion, aOptions.mStorage,
                      false, aRv);
}

already_AddRefed<IDBOpenDBRequest>
IDBFactory::DeleteDatabase(const nsAString& aName,
                           const IDBOpenDBOptions& aOptions,
                           ErrorResult& aRv)
{
  return OpenInternal (nullptr, aName, Optional<uint64_t>(), aOptions.mStorage,
                       true, aRv);
}

int16_t
IDBFactory::Cmp(JSContext* aCx, JS::Handle<JS::Value> aFirst,
                JS::Handle<JS::Value> aSecond, ErrorResult& aRv)
{
  Key first, second;
  nsresult rv = first.SetFromJSVal(aCx, aFirst);
  if (NS_FAILED(rv)) {
    aRv.Throw(rv);
    return 0;
  }

  rv = second.SetFromJSVal(aCx, aSecond);
  if (NS_FAILED(rv)) {
    aRv.Throw(rv);
    return 0;
  }

  if (first.IsUnset() || second.IsUnset()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_DATA_ERR);
    return 0;
  }

  return Key::CompareKeys(first, second);
}

already_AddRefed<IDBOpenDBRequest>
IDBFactory::OpenForPrincipal(nsIPrincipal* aPrincipal, const nsAString& aName,
                             uint64_t aVersion, ErrorResult& aRv)
{
  MOZ_ASSERT(aPrincipal);
  if (!NS_IsMainThread()) {
    MOZ_CRASH("Figure out security checks for workers!");
  }
  MOZ_ASSERT(nsContentUtils::IsCallerChrome());

  return OpenInternal(aPrincipal, aName, Optional<uint64_t>(aVersion),
                      Optional<StorageType>(), false, aRv);
}

already_AddRefed<IDBOpenDBRequest>
IDBFactory::OpenForPrincipal(nsIPrincipal* aPrincipal, const nsAString& aName,
                             const IDBOpenDBOptions& aOptions, ErrorResult& aRv)
{
  MOZ_ASSERT(aPrincipal);
  if (!NS_IsMainThread()) {
    MOZ_CRASH("Figure out security checks for workers!");
  }
  MOZ_ASSERT(nsContentUtils::IsCallerChrome());

  return OpenInternal(aPrincipal, aName, aOptions.mVersion, aOptions.mStorage,
                      false, aRv);
}

already_AddRefed<IDBOpenDBRequest>
IDBFactory::DeleteForPrincipal(nsIPrincipal* aPrincipal, const nsAString& aName,
                               const IDBOpenDBOptions& aOptions,
                               ErrorResult& aRv)
{
  MOZ_ASSERT(aPrincipal);
  if (!NS_IsMainThread()) {
    MOZ_CRASH("Figure out security checks for workers!");
  }
  MOZ_ASSERT(nsContentUtils::IsCallerChrome());

  return OpenInternal(aPrincipal, aName, Optional<uint64_t>(),
                      aOptions.mStorage, true, aRv);
}

already_AddRefed<IDBOpenDBRequest>
IDBFactory::OpenInternal(nsIPrincipal* aPrincipal,
                         const nsAString& aName,
                         const Optional<uint64_t>& aVersion,
                         const Optional<StorageType>& aStorageType,
                         bool aDeleting,
                         ErrorResult& aRv)
{
  MOZ_ASSERT(mWindow || mOwningObject);

  PrincipalInfo principalInfo;

  if (aPrincipal) {
    if (!NS_IsMainThread()) {
      MOZ_CRASH("Figure out security checks for workers!");
    }
    MOZ_ASSERT(nsContentUtils::IsCallerChrome());

    nsresult rv = GetPrincipalInfo(aPrincipal, &principalInfo);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      IDB_REPORT_INTERNAL_ERR();
      aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
      return nullptr;
    }
  } else {
    principalInfo = *mPrincipalInfo;
  }

  uint64_t version = 0;
  if (!aDeleting && aVersion.WasPassed()) {
    if (aVersion.Value() < 1) {
      aRv.ThrowTypeError(MSG_INVALID_VERSION);
      return nullptr;
    }
    version = aVersion.Value();
  }

  // Nothing can be done here if we have previously failed to create a
  // background actor.
  if (mBackgroundActorFailed) {
    IDB_REPORT_INTERNAL_ERR();
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  // XXX We need a bug to switch to temporary storage by default.

  // Chrome privilege always gets persistent storage.
  PersistenceType persistenceType =
    principalInfo.type() == PrincipalInfo::TSystemPrincipalInfo ?
    PERSISTENCE_TYPE_PERSISTENT :
    PersistenceTypeFromStorage(aStorageType, PERSISTENCE_TYPE_PERSISTENT);

  DatabaseMetadata metadata;
  metadata.name() = aName;
  metadata.persistenceType() = persistenceType;

  FactoryRequestParams params;
  if (aDeleting) {
    metadata.version() = 0;
    params = DeleteDatabaseRequestParams(metadata, principalInfo);
  } else {
    metadata.version() = version;
    params = OpenDatabaseRequestParams(metadata, principalInfo);
  }

  if (!mBackgroundActor) {
    // If another consumer has already created a background actor for this
    // thread then we can start this request immediately.
    if (PBackgroundChild* bgActor = BackgroundChild::GetForCurrentThread()) {
      nsresult rv = BackgroundActorCreated(bgActor);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        IDB_REPORT_INTERNAL_ERR();
        aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
        return nullptr;
      }
      MOZ_ASSERT(mBackgroundActor);
    }
  }

  AutoJSContext cx;
  nsPIDOMWindow* window;
  JS::Rooted<JSObject*> scriptOwner(cx);

  if (mWindow) {
    window = mWindow;
    scriptOwner = static_cast<nsGlobalWindow*>(window)->FastGetGlobalJSObject();
  } else {
    window = nullptr;
    scriptOwner = mOwningObject;
  }

  nsRefPtr<IDBOpenDBRequest> request =
    IDBOpenDBRequest::Create(this, window, scriptOwner);
  MOZ_ASSERT(request);

  // If we already have a background actor then we can start this request now.
  if (mBackgroundActor) {
    nsresult rv = InitiateRequest(request, params);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      IDB_REPORT_INTERNAL_ERR();
      aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
      return nullptr;
    }
  } else {
    mPendingRequests.AppendElement(new PendingRequestInfo(request, params));

    if (mPendingRequests.Length() == 1) {
      // We need to start the sequence to create a background actor for this
      // thread.
      nsRefPtr<BackgroundCreateCallback> cb =
        new BackgroundCreateCallback(this);
      if (NS_WARN_IF(!BackgroundChild::GetOrCreateForCurrentThread(cb))) {
        IDB_REPORT_INTERNAL_ERR();
        aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
        return nullptr;
      }
    }
  }

#ifdef IDB_PROFILER_USE_MARKS
  {
    NS_ConvertUTF16toUTF8 profilerName(aName);
    if (aDeleting) {
      IDB_PROFILER_MARK("IndexedDB Request %llu: deleteDatabase(\"%s\")",
                        "MT IDBFactory.deleteDatabase()",
                        request->GetSerialNumber(), profilerName.get());
    } else {
      IDB_PROFILER_MARK("IndexedDB Request %llu: open(\"%s\", %lld)",
                        "MT IDBFactory.open()",
                        request->GetSerialNumber(), profilerName.get(),
                        aVersion);
    }
  }
#endif

  return request.forget();
}

nsresult
IDBFactory::BackgroundActorCreated(PBackgroundChild* aBackgroundActor)
{
  MOZ_ASSERT(aBackgroundActor);
  MOZ_ASSERT(!mBackgroundActor);
  MOZ_ASSERT(!mBackgroundActorFailed);

  {
    BackgroundFactoryChild* actor = new BackgroundFactoryChild(this);

    mBackgroundActor =
      static_cast<BackgroundFactoryChild*>(
        aBackgroundActor->SendPBackgroundIDBFactoryConstructor(actor));
  }

  if (NS_WARN_IF(!mBackgroundActor)) {
    BackgroundActorFailed();
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  nsresult rv = NS_OK;

  for (uint32_t index = 0, count = mPendingRequests.Length();
       index < count;
       index++) {
    nsAutoPtr<PendingRequestInfo> info(mPendingRequests[index].forget());

    nsresult rv2 = InitiateRequest(info->mRequest, info->mParams);

    // Warn for every failure, but just return the first failure if there are
    // multiple failures.
    if (NS_WARN_IF(NS_FAILED(rv2)) && NS_SUCCEEDED(rv)) {
      rv = rv2;
    }
  }

  mPendingRequests.Clear();

  return rv;
}

void
IDBFactory::BackgroundActorFailed()
{
  MOZ_ASSERT(!mPendingRequests.IsEmpty());
  MOZ_ASSERT(!mBackgroundActor);
  MOZ_ASSERT(!mBackgroundActorFailed);

  mBackgroundActorFailed = true;

  for (uint32_t index = 0, count = mPendingRequests.Length();
       index < count;
       index++) {
    nsAutoPtr<PendingRequestInfo> info(mPendingRequests[index].forget());
    info->mRequest->
      DispatchNonTransactionError(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  }

  mPendingRequests.Clear();
}

nsresult
IDBFactory::InitiateRequest(IDBOpenDBRequest* aRequest,
                            const FactoryRequestParams& aParams)
{
  MOZ_ASSERT(aRequest);
  MOZ_ASSERT(mBackgroundActor);
  MOZ_ASSERT(!mBackgroundActorFailed);

  bool deleting;
  uint64_t requestedVersion;
  PersistenceType persistenceType;

  switch (aParams.type()) {
    case FactoryRequestParams::TDeleteDatabaseRequestParams: {
      const DatabaseMetadata& metadata =
        aParams.get_DeleteDatabaseRequestParams().metadata();
      deleting = true;
      requestedVersion = metadata.version();
      persistenceType = metadata.persistenceType();
      break;
    }

    case FactoryRequestParams::TOpenDatabaseRequestParams: {
      const DatabaseMetadata& metadata =
        aParams.get_OpenDatabaseRequestParams().metadata();
      deleting = false;
      requestedVersion = metadata.version();
      persistenceType = metadata.persistenceType();
      break;
    }

    default:
      MOZ_CRASH("Should never get here!");
  }

  auto actor =
    new BackgroundFactoryRequestChild(this, aRequest, deleting,
                                      requestedVersion, persistenceType);

  if (!mBackgroundActor->SendPBackgroundIDBFactoryRequestConstructor(actor,
                                                                     aParams)) {
    aRequest->DispatchNonTransactionError(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  return NS_OK;
}

NS_IMPL_CYCLE_COLLECTING_ADDREF(IDBFactory)
NS_IMPL_CYCLE_COLLECTING_RELEASE(IDBFactory)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(IDBFactory)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTION_CLASS(IDBFactory)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(IDBFactory)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE_SCRIPT_OBJECTS
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mWindow)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(IDBFactory)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER
  if (tmp->mOwningObject) {
    tmp->mOwningObject = nullptr;
  }
  if (tmp->mRootedOwningObject) {
    mozilla::DropJSObjects(tmp);
    tmp->mRootedOwningObject = false;
  }
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mWindow)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN(IDBFactory)
  NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER
  NS_IMPL_CYCLE_COLLECTION_TRACE_JS_MEMBER_CALLBACK(mOwningObject)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

JSObject*
IDBFactory::WrapObject(JSContext* aCx)
{
  return IDBFactoryBinding::Wrap(aCx, this);
}

NS_IMPL_ISUPPORTS(IDBFactory::BackgroundCreateCallback,
                  nsIIPCBackgroundChildCreateCallback)

void
IDBFactory::BackgroundCreateCallback::ActorCreated(PBackgroundChild* aActor)
{
  MOZ_ASSERT(aActor);
  MOZ_ASSERT(mFactory);

  nsRefPtr<IDBFactory> factory;
  mFactory.swap(factory);

  factory->BackgroundActorCreated(aActor);
}

void
IDBFactory::BackgroundCreateCallback::ActorFailed()
{
  MOZ_ASSERT(mFactory);

  nsRefPtr<IDBFactory> factory;
  mFactory.swap(factory);

  factory->BackgroundActorFailed();
}
