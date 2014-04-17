/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDBFactory.h"

#include "ActorsChild.h"
#include <algorithm>
#include "AsyncConnectionHelper.h"
#include "CheckPermissionsHelper.h"
#include "DatabaseInfo.h"
#include "FileManager.h"
#include "IDBDatabase.h"
#include "IDBEvents.h"
#include "IDBKeyRange.h"
#include "IndexedDatabaseManager.h"
#include "Key.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/Preferences.h"
#include "mozilla/storage.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/IDBFactoryBinding.h"
#include "mozilla/dom/PBrowserChild.h"
#include "mozilla/dom/quota/OriginOrPatternString.h"
#include "mozilla/dom/quota/QuotaManager.h"
#include "mozilla/dom/TabChild.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "mozilla/ipc/PBackgroundChild.h"
#include "nsAutoPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsCharSeparatedTokenizer.h"
#include "nsContentUtils.h"
#include "nsCxPusher.h"
#include "nsDOMClassInfoID.h"
#include "nsGlobalWindow.h"
#include "nsHashKeys.h"
#include "nsIFile.h"
#include "nsIIPCBackgroundChildCreateCallback.h"
#include "nsIPrincipal.h"
#include "nsIScriptContext.h"
#include "nsIScriptSecurityManager.h"
#include "nsIXPConnect.h"
#include "nsIXPCScriptable.h"
#include "nsNetUtil.h"
#include "nsPIDOMWindow.h"
#include "nsServiceManagerUtils.h"
#include "nsThreadUtils.h"
#include "nsXPCOMCID.h"
#include "ProfilerHelpers.h"
#include "ReportInternalError.h"

#define PREF_INDEXEDDB_ENABLED "dom.indexedDB.enabled"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::indexedDB;
using namespace mozilla::dom::quota;
using namespace mozilla::ipc;

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
  : mPrivilege(Content)
  , mDefaultPersistenceType(PERSISTENCE_TYPE_TEMPORARY)
  , mOwningObject(nullptr)
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
                   const nsACString& aGroup,
                   const nsACString& aASCIIOrigin,
                   ContentParent* aContentParent,
                   IDBFactory** aFactory)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(aASCIIOrigin.IsEmpty() || nsContentUtils::IsCallerChrome(),
               "Non-chrome may not supply their own origin!");

  IDB_ENSURE_TRUE(aWindow, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  if (aWindow->IsOuterWindow()) {
    aWindow = aWindow->GetCurrentInnerWindow();
    IDB_ENSURE_TRUE(aWindow, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  }

  // Make sure that the manager is up before we do anything here since lots of
  // decisions depend on which process we're running in.
  indexedDB::IndexedDatabaseManager* mgr =
    indexedDB::IndexedDatabaseManager::GetOrCreate();
  IDB_ENSURE_TRUE(mgr, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  nsresult rv;

  nsCString group(aGroup);
  nsCString origin(aASCIIOrigin);
  StoragePrivilege privilege;
  PersistenceType defaultPersistenceType;
  if (origin.IsEmpty()) {
    NS_ASSERTION(aGroup.IsEmpty(), "Should be empty too!");

    rv = QuotaManager::GetInfoFromWindow(aWindow, &group, &origin, &privilege,
                                         &defaultPersistenceType);
  }
  else {
    rv = QuotaManager::GetInfoFromWindow(aWindow, nullptr, nullptr, &privilege,
                                         &defaultPersistenceType);
  }
  if (NS_FAILED(rv)) {
    // Not allowed.
    *aFactory = nullptr;
    return NS_OK;
  }

  nsRefPtr<IDBFactory> factory = new IDBFactory();
  factory->mGroup = group;
  factory->mASCIIOrigin = origin;
  factory->mPrivilege = privilege;
  factory->mDefaultPersistenceType = defaultPersistenceType;
  factory->mWindow = aWindow;

  factory.forget(aFactory);
  return NS_OK;
}

// static
nsresult
IDBFactory::Create(JSContext* aCx,
                   JS::Handle<JSObject*> aOwningObject,
                   ContentParent* aContentParent,
                   IDBFactory** aFactory)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(aCx, "Null context!");
  NS_ASSERTION(aOwningObject, "Null object!");
  NS_ASSERTION(JS_GetGlobalForObject(aCx, aOwningObject) == aOwningObject,
               "Not a global object!");
  NS_ASSERTION(nsContentUtils::IsCallerChrome(), "Only for chrome!");

  // Make sure that the manager is up before we do anything here since lots of
  // decisions depend on which process we're running in.
  IndexedDatabaseManager* mgr = IndexedDatabaseManager::GetOrCreate();
  IDB_ENSURE_TRUE(mgr, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  nsCString group;
  nsCString origin;
  StoragePrivilege privilege;
  PersistenceType defaultPersistenceType;
  QuotaManager::GetInfoForChrome(&group, &origin, &privilege,
                                 &defaultPersistenceType);

  nsRefPtr<IDBFactory> factory = new IDBFactory();
  factory->mGroup = group;
  factory->mASCIIOrigin = origin;
  factory->mPrivilege = privilege;
  factory->mDefaultPersistenceType = defaultPersistenceType;
  factory->mOwningObject = aOwningObject;

  mozilla::HoldJSObjects(factory.get());
  factory->mRootedOwningObject = true;

  factory.forget(aFactory);
  return NS_OK;
}

// static
nsresult
IDBFactory::Create(ContentParent* aContentParent,
                   IDBFactory** aFactory)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(nsContentUtils::IsCallerChrome(), "Only for chrome!");
  NS_ASSERTION(aContentParent, "Null ContentParent!");

  NS_ASSERTION(!nsContentUtils::GetCurrentJSContext(), "Should be called from C++");

  // We need to get this information before we push a null principal to avoid
  // IsCallerChrome() assertion in quota manager.
  nsCString group;
  nsCString origin;
  StoragePrivilege privilege;
  PersistenceType defaultPersistenceType;
  QuotaManager::GetInfoForChrome(&group, &origin, &privilege,
                                 &defaultPersistenceType);

  nsCOMPtr<nsIPrincipal> principal =
    do_CreateInstance("@mozilla.org/nullprincipal;1");
  NS_ENSURE_TRUE(principal, NS_ERROR_FAILURE);

  AutoSafeJSContext cx;

  nsIXPConnect* xpc = nsContentUtils::XPConnect();
  NS_ASSERTION(xpc, "This should never be null!");

  nsCOMPtr<nsIXPConnectJSObjectHolder> globalHolder;
  nsresult rv = xpc->CreateSandbox(cx, principal, getter_AddRefs(globalHolder));
  NS_ENSURE_SUCCESS(rv, rv);

  JS::Rooted<JSObject*> global(cx, globalHolder->GetJSObject());
  NS_ENSURE_STATE(global);

  // The CreateSandbox call returns a proxy to the actual sandbox object. We
  // don't need a proxy here.
  global = js::UncheckedUnwrap(global);

  JSAutoCompartment ac(cx, global);

  nsRefPtr<IDBFactory> factory = new IDBFactory();
  factory->mGroup = group;
  factory->mASCIIOrigin = origin;
  factory->mPrivilege = privilege;
  factory->mDefaultPersistenceType = defaultPersistenceType;
  factory->mOwningObject = global;

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

nsresult
IDBFactory::OpenInternal(const nsAString& aName,
                         int64_t aVersion,
                         PersistenceType aPersistenceType,
                         const nsACString& aGroup,
                         const nsACString& aASCIIOrigin,
                         StoragePrivilege aPrivilege,
                         bool aDeleting,
                         IDBOpenDBRequest** _retval)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(mWindow || mOwningObject, "Must have one of these!");

  // Nothing can be done here if we have previously failed to create a
  // background actor.
  if (mBackgroundActorFailed) {
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  AutoJSContext cx;
  nsCOMPtr<nsPIDOMWindow> window;
  JS::Rooted<JSObject*> scriptOwner(cx);

  if (mWindow) {
    window = mWindow;
    scriptOwner =
      static_cast<nsGlobalWindow*>(window.get())->FastGetGlobalJSObject();
  }
  else {
    scriptOwner = mOwningObject;
  }

  if (aPrivilege == Chrome) {
    // Chrome privilege, ignore the persistence type parameter.
    aPersistenceType = PERSISTENCE_TYPE_PERSISTENT;
  }

  nsRefPtr<IDBOpenDBRequest> request =
    IDBOpenDBRequest::Create(this, window, scriptOwner);
  MOZ_ASSERT(request);

  DatabaseMetadata metadata;
  metadata.name() = aName;
  metadata.persistenceType() = aPersistenceType;

  FactoryRequestParams params;
  if (aDeleting) {
    metadata.version() = 0;
    params = DeleteDatabaseRequestParams(metadata);
  } else {
    metadata.version() = aVersion;
    params = OpenDatabaseRequestParams(metadata);
  }

  if (!mBackgroundActor) {
    // If another consumer has already created a background actor for this
    // thread then we can start this request immediately.
    if (PBackgroundChild* bgActor = BackgroundChild::GetForCurrentThread()) {
      nsresult rv = BackgroundActorCreated(bgActor);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
      MOZ_ASSERT(mBackgroundActor);
    }
  }

  // If we already have a background actor then we can start this request now.
  if (mBackgroundActor) {
    nsresult rv = InitiateRequest(request, params);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  }
  else {
    mPendingRequests.AppendElement(new PendingRequestInfo(request, params));

    if (mPendingRequests.Length() == 1) {
      // We need to start the sequence to create a background actor for this
      // thread.
      nsRefPtr<BackgroundCreateCallback> cb =
        new BackgroundCreateCallback(this);
      if (NS_WARN_IF(!BackgroundChild::GetOrCreateForCurrentThread(cb))) {
        return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }
    }
  }

#if 0 // XXX Remove me!
  if (IndexedDatabaseManager::IsMainProcess()) {
    nsRefPtr<OpenDatabaseHelper> openHelper =
      new OpenDatabaseHelper(request, aName, aGroup, aASCIIOrigin, aVersion,
                             aPersistenceType, aDeleting, mContentParent,
                             aPrivilege);

    rv = openHelper->Init();
    IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

    if (!Preferences::GetBool(PREF_INDEXEDDB_ENABLED)) {
      openHelper->SetError(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
      rv = openHelper->WaitForOpenAllowed();
    }
    else {
      if (mPrivilege != Chrome &&
          aPersistenceType == PERSISTENCE_TYPE_PERSISTENT) {
        nsRefPtr<CheckPermissionsHelper> permissionHelper =
          new CheckPermissionsHelper(openHelper, window);

        QuotaManager* quotaManager = QuotaManager::Get();
        NS_ASSERTION(quotaManager, "This should never be null!");

        rv = quotaManager->
          WaitForOpenAllowed(OriginOrPatternString::FromOrigin(aASCIIOrigin),
                             Nullable<PersistenceType>(aPersistenceType),
                             openHelper->Id(), permissionHelper);
      }
      else {
        // Chrome and temporary storage doesn't need to check the permission.
        rv = openHelper->WaitForOpenAllowed();
      }
    }
    IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  }
#endif

#ifdef IDB_PROFILER_USE_MARKS
  {
    NS_ConvertUTF16toUTF8 profilerName(aName);
    if (aDeleting) {
      IDB_PROFILER_MARK("IndexedDB Request %llu: deleteDatabase(\"%s\")",
                        "MT IDBFactory.deleteDatabase()",
                        request->GetSerialNumber(), profilerName.get());
    }
    else {
      IDB_PROFILER_MARK("IndexedDB Request %llu: open(\"%s\", %lld)",
                        "MT IDBFactory.open()",
                        request->GetSerialNumber(), profilerName.get(),
                        aVersion);
    }
  }
#endif

  request.forget(_retval);
  return NS_OK;
}

void
IDBFactory::SetBackgroundActor(BackgroundFactoryChild* aBackgroundActor)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aBackgroundActor);
  MOZ_ASSERT(!mBackgroundActor);

  mBackgroundActor = aBackgroundActor;
}

already_AddRefed<IDBOpenDBRequest>
IDBFactory::Open(const nsAString& aName, const IDBOpenDBOptions& aOptions,
                 ErrorResult& aRv)
{
  return Open(nullptr, aName, aOptions.mVersion, aOptions.mStorage, false, aRv);
}

already_AddRefed<IDBOpenDBRequest>
IDBFactory::DeleteDatabase(const nsAString& aName,
                           const IDBOpenDBOptions& aOptions,
                           ErrorResult& aRv)
{
  return Open(nullptr, aName, Optional<uint64_t>(), aOptions.mStorage, true,
              aRv);
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
  // Just to be on the extra-safe side
  if (!nsContentUtils::IsCallerChrome()) {
    MOZ_CRASH();
  }

  return Open(aPrincipal, aName, Optional<uint64_t>(aVersion),
              Optional<mozilla::dom::StorageType>(), false, aRv);
}

already_AddRefed<IDBOpenDBRequest>
IDBFactory::OpenForPrincipal(nsIPrincipal* aPrincipal, const nsAString& aName,
                             const IDBOpenDBOptions& aOptions, ErrorResult& aRv)
{
  // Just to be on the extra-safe side
  if (!nsContentUtils::IsCallerChrome()) {
    MOZ_CRASH();
  }

  return Open(aPrincipal, aName, aOptions.mVersion, aOptions.mStorage, false,
              aRv);
}

already_AddRefed<IDBOpenDBRequest>
IDBFactory::DeleteForPrincipal(nsIPrincipal* aPrincipal, const nsAString& aName,
                               const IDBOpenDBOptions& aOptions,
                               ErrorResult& aRv)
{
  // Just to be on the extra-safe side
  if (!nsContentUtils::IsCallerChrome()) {
    MOZ_CRASH();
  }

  return Open(aPrincipal, aName, Optional<uint64_t>(), aOptions.mStorage, true,
              aRv);
}

already_AddRefed<IDBOpenDBRequest>
IDBFactory::Open(nsIPrincipal* aPrincipal, const nsAString& aName,
                 const Optional<uint64_t>& aVersion,
                 const Optional<mozilla::dom::StorageType>& aStorageType,
                 bool aDelete, ErrorResult& aRv)
{
  nsresult rv;

  nsCString group;
  nsCString origin;
  StoragePrivilege privilege;
  PersistenceType defaultPersistenceType;
  if (aPrincipal) {
    rv = QuotaManager::GetInfoFromPrincipal(aPrincipal, &group, &origin,
                                            &privilege,
                                            &defaultPersistenceType);
    if (NS_FAILED(rv)) {
      IDB_REPORT_INTERNAL_ERR();
      aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
      return nullptr;
    }
  }
  else {
    group = mGroup;
    origin = mASCIIOrigin;
    privilege = mPrivilege;
    defaultPersistenceType = mDefaultPersistenceType;
  }

  uint64_t version = 0;
  if (!aDelete && aVersion.WasPassed()) {
    if (aVersion.Value() < 1) {
      aRv.ThrowTypeError(MSG_INVALID_VERSION);
      return nullptr;
    }
    version = aVersion.Value();
  }

  PersistenceType persistenceType =
    PersistenceTypeFromStorage(aStorageType, defaultPersistenceType);

  nsRefPtr<IDBOpenDBRequest> request;
  rv = OpenInternal(aName, version, persistenceType, group, origin, privilege,
                    aDelete, getter_AddRefs(request));
  if (NS_FAILED(rv)) {
    aRv.Throw(rv);
    return nullptr;
  }

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
        aBackgroundActor->SendPBackgroundIDBFactoryConstructor(actor, mGroup,
                                                               mASCIIOrigin,
                                                               mPrivilege));
  }

  if (NS_WARN_IF(!mBackgroundActor)) {
    BackgroundActorFailed();
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  nsresult rv = NS_OK;

  for (uint32_t index = 0; index < mPendingRequests.Length(); index++) {
    nsAutoPtr<PendingRequestInfo> info = mPendingRequests[index].forget();

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

  for (uint32_t index = 0; index < mPendingRequests.Length(); index++) {
    nsAutoPtr<PendingRequestInfo> info = mPendingRequests[index].forget();
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
      MOZ_ASSUME_UNREACHABLE("Should never get here!");
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

NS_IMPL_ISUPPORTS1(IDBFactory::BackgroundCreateCallback,
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
