/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BackgroundParentImpl.h"

#include "FileDescriptorSetParent.h"
#include "mozilla/AppProcessChecker.h"
#include "mozilla/Assertions.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/PBlobParent.h"
#include "mozilla/dom/indexedDB/ActorsParent.h"
#include "mozilla/dom/ipc/BlobParent.h"
#include "mozilla/dom/MessagePortParent.h"
#include "mozilla/dom/cache/CacheStorageParent.h"
#include "mozilla/dom/cache/PCacheParent.h"
#include "mozilla/dom/cache/PCacheStreamControlParent.h"
#include "mozilla/dom/ServiceWorkerRegistrar.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "mozilla/ipc/PBackgroundSharedTypes.h"
#include "mozilla/ipc/PBackgroundTestParent.h"
#include "nsThreadUtils.h"
#include "nsTraceRefcnt.h"
#include "nsXULAppAPI.h"

#ifdef DISABLE_ASSERTS_FOR_FUZZING
#define ASSERT_UNLESS_FUZZING(...) do { } while (0)
#else
#define ASSERT_UNLESS_FUZZING(...) MOZ_ASSERT(false)
#endif

using mozilla::ipc::AssertIsOnBackgroundThread;

using namespace mozilla::dom;
using mozilla::dom::cache::PCacheParent;
using mozilla::dom::cache::CacheStorageParent;
using mozilla::dom::cache::PCacheStorageParent;
using mozilla::dom::cache::PCacheStreamControlParent;
using mozilla::dom::ContentParent;
using mozilla::dom::ServiceWorkerRegistrationData;

namespace {

void
AssertIsInMainProcess()
{
  MOZ_ASSERT(XRE_GetProcessType() == GeckoProcessType_Default);
}

void
AssertIsOnMainThread()
{
  MOZ_ASSERT(NS_IsMainThread());
}

class TestParent MOZ_FINAL : public mozilla::ipc::PBackgroundTestParent
{
  friend class mozilla::ipc::BackgroundParentImpl;

  TestParent()
  {
    MOZ_COUNT_CTOR(TestParent);
  }

protected:
  ~TestParent()
  {
    MOZ_COUNT_DTOR(TestParent);
  }

public:
  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;
};

} // anonymous namespace

namespace mozilla {
namespace ipc {

BackgroundParentImpl::BackgroundParentImpl()
{
  AssertIsInMainProcess();
  AssertIsOnMainThread();

  MOZ_COUNT_CTOR(mozilla::ipc::BackgroundParentImpl);
}

BackgroundParentImpl::~BackgroundParentImpl()
{
  AssertIsInMainProcess();
  AssertIsOnMainThread();

  MOZ_COUNT_DTOR(mozilla::ipc::BackgroundParentImpl);
}

void
BackgroundParentImpl::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
}

BackgroundParentImpl::PBackgroundTestParent*
BackgroundParentImpl::AllocPBackgroundTestParent(const nsCString& aTestArg)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return new TestParent();
}

bool
BackgroundParentImpl::RecvPBackgroundTestConstructor(
                                                  PBackgroundTestParent* aActor,
                                                  const nsCString& aTestArg)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return PBackgroundTestParent::Send__delete__(aActor, aTestArg);
}

bool
BackgroundParentImpl::DeallocPBackgroundTestParent(
                                                  PBackgroundTestParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  delete static_cast<TestParent*>(aActor);
  return true;
}

auto
BackgroundParentImpl::AllocPBackgroundIDBFactoryParent()
  -> PBackgroundIDBFactoryParent*
{
  using mozilla::dom::indexedDB::AllocPBackgroundIDBFactoryParent;

  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return AllocPBackgroundIDBFactoryParent();
}

bool
BackgroundParentImpl::RecvPBackgroundIDBFactoryConstructor(
                                            PBackgroundIDBFactoryParent* aActor)
{
  using mozilla::dom::indexedDB::RecvPBackgroundIDBFactoryConstructor;

  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return RecvPBackgroundIDBFactoryConstructor(aActor);
}

bool
BackgroundParentImpl::DeallocPBackgroundIDBFactoryParent(
                                            PBackgroundIDBFactoryParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return mozilla::dom::indexedDB::DeallocPBackgroundIDBFactoryParent(aActor);
}

auto
BackgroundParentImpl::AllocPBlobParent(const BlobConstructorParams& aParams)
  -> PBlobParent*
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  if (NS_WARN_IF(aParams.type() !=
                   BlobConstructorParams::TParentBlobConstructorParams)) {
    ASSERT_UNLESS_FUZZING();
    return nullptr;
  }

  return mozilla::dom::BlobParent::Create(this, aParams);
}

bool
BackgroundParentImpl::DeallocPBlobParent(PBlobParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  mozilla::dom::BlobParent::Destroy(aActor);
  return true;
}

PFileDescriptorSetParent*
BackgroundParentImpl::AllocPFileDescriptorSetParent(
                                          const FileDescriptor& aFileDescriptor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return new FileDescriptorSetParent(aFileDescriptor);
}

bool
BackgroundParentImpl::DeallocPFileDescriptorSetParent(
                                               PFileDescriptorSetParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  delete static_cast<FileDescriptorSetParent*>(aActor);
  return true;
}

PCacheStorageParent*
BackgroundParentImpl::AllocPCacheStorageParent(const Namespace& aNamespace,
                                               const PrincipalInfo& aPrincipalInfo)
{
  return new CacheStorageParent(this, aNamespace, aPrincipalInfo);
}

bool
BackgroundParentImpl::DeallocPCacheStorageParent(PCacheStorageParent* aActor)
{
  delete aActor;
  return true;
}

PCacheParent*
BackgroundParentImpl::AllocPCacheParent()
{
  MOZ_CRASH("CacheParent actor must be provided to PBackground manager");
  return nullptr;
}

bool
BackgroundParentImpl::DeallocPCacheParent(PCacheParent* aActor)
{
  // The CacheParent actor is provided to the PBackground manager, but
  // we own the object and must delete it.
  delete aActor;
  return true;
}

PCacheStreamControlParent*
BackgroundParentImpl::AllocPCacheStreamControlParent()
{
  MOZ_CRASH("CacheStreamControlParent actor must be provided to PBackground manager");
  return nullptr;
}

bool
BackgroundParentImpl::DeallocPCacheStreamControlParent(PCacheStreamControlParent* aActor)
{
  // The CacheStreamControlParent actor is provided to the PBackground manager, but
  // we own the object and must delete it.
  delete aActor;
  return true;
}

PMessagePortParent*
BackgroundParentImpl::AllocPMessagePortParent()
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return new MessagePortParent();
}

bool
BackgroundParentImpl::DeallocPMessagePortParent(PMessagePortParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  delete static_cast<MessagePortParent*>(aActor);
  return true;
}

namespace {

class RegisterServiceWorkerCallback MOZ_FINAL : public nsRunnable
{
public:
  RegisterServiceWorkerCallback(const ServiceWorkerRegistrationData& aData)
    : mData(aData)
  {
    AssertIsInMainProcess();
    AssertIsOnBackgroundThread();
  }

  NS_IMETHODIMP
  Run()
  {
    AssertIsInMainProcess();
    AssertIsOnBackgroundThread();

    nsRefPtr<dom::ServiceWorkerRegistrar> service =
      dom::ServiceWorkerRegistrar::Get();
    MOZ_ASSERT(service);

    service->RegisterServiceWorker(mData);
    return NS_OK;
  }

private:
  ServiceWorkerRegistrationData mData;
};

class UnregisterServiceWorkerCallback MOZ_FINAL : public nsRunnable
{
public:
  UnregisterServiceWorkerCallback(const nsString& aScope)
    : mScope(aScope)
  {
    AssertIsInMainProcess();
    AssertIsOnBackgroundThread();
  }

  NS_IMETHODIMP
  Run()
  {
    AssertIsInMainProcess();
    AssertIsOnBackgroundThread();

    nsRefPtr<dom::ServiceWorkerRegistrar> service =
      dom::ServiceWorkerRegistrar::Get();
    MOZ_ASSERT(service);

    service->UnregisterServiceWorker(NS_ConvertUTF16toUTF8(mScope));
    return NS_OK;
  }

private:
  nsString mScope;
};

class CheckPrincipalRunnable : MOZ_FINAL public nsRunnable
{
public:
  CheckPrincipalRunnable(already_AddRefed<ContentParent> aParent,
                         const PrincipalInfo& aPrincipalInfo,
                         nsRunnable* aCallback)
    : mContentParent(aParent)
    , mPrincipalInfo(aPrincipalInfo)
    , mCallback(aCallback)
    , mBackgroundThread(NS_GetCurrentThread())
  {
    AssertIsInMainProcess();
    AssertIsOnBackgroundThread();

    MOZ_ASSERT(mContentParent);
    MOZ_ASSERT(mCallback);
    MOZ_ASSERT(mBackgroundThread);
  }

  NS_IMETHODIMP Run()
  {
    if (NS_IsMainThread()) {
      nsCOMPtr<nsIPrincipal> principal = PrincipalInfoToPrincipal(mPrincipalInfo);
      AssertAppPrincipal(mContentParent, principal);
      mContentParent = nullptr;

      mBackgroundThread->Dispatch(this, NS_DISPATCH_NORMAL);
      return NS_OK;
    }

    AssertIsOnBackgroundThread();
    mCallback->Run();
    mCallback = nullptr;

    return NS_OK;
  }

private:
  nsRefPtr<ContentParent> mContentParent;
  PrincipalInfo mPrincipalInfo;
  nsRefPtr<nsRunnable> mCallback;
  nsCOMPtr<nsIThread> mBackgroundThread;
};

} // anonymous namespace

bool
BackgroundParentImpl::RecvRegisterServiceWorker(
                                     const ServiceWorkerRegistrationData& aData)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  // Basic validation.
  if (aData.scope().IsEmpty() ||
      aData.scriptSpec().IsEmpty() ||
      aData.currentWorkerURL().IsEmpty() ||
      aData.principal().type() == PrincipalInfo::TNullPrincipalInfo) {
    return false;
  }

  nsRefPtr<RegisterServiceWorkerCallback> callback =
    new RegisterServiceWorkerCallback(aData);

  nsRefPtr<ContentParent> parent = BackgroundParent::GetContentParent(this);

  // If the ContentParent is null we are dealing with a same-process actor.
  if (!parent) {
    callback->Run();
    return true;
  }

  nsRefPtr<CheckPrincipalRunnable> runnable =
    new CheckPrincipalRunnable(parent.forget(), aData.principal(), callback);
  nsresult rv = NS_DispatchToMainThread(runnable);
  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(rv));

  return true;
}

bool
BackgroundParentImpl::RecvUnregisterServiceWorker(
                                            const PrincipalInfo& aPrincipalInfo,
                                            const nsString& aScope)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  // Basic validation.
  if (aScope.IsEmpty() ||
      aPrincipalInfo.type() == PrincipalInfo::TNullPrincipalInfo) {
    return false;
  }

  nsRefPtr<UnregisterServiceWorkerCallback> callback =
    new UnregisterServiceWorkerCallback(aScope);

  nsRefPtr<ContentParent> parent = BackgroundParent::GetContentParent(this);

  // If the ContentParent is null we are dealing with a same-process actor.
  if (!parent) {
    callback->Run();
    return true;
  }

  nsRefPtr<CheckPrincipalRunnable> runnable =
    new CheckPrincipalRunnable(parent.forget(), aPrincipalInfo, callback);
  nsresult rv = NS_DispatchToMainThread(runnable);
  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(rv));

  return true;
}

bool
BackgroundParentImpl::RecvShutdownServiceWorkerRegistrar()
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  if (BackgroundParent::IsOtherProcessActor(this)) {
    return false;
  }

  nsRefPtr<dom::ServiceWorkerRegistrar> service =
    dom::ServiceWorkerRegistrar::Get();
  MOZ_ASSERT(service);

  service->Shutdown();
  return true;
}

} // namespace ipc
} // namespace mozilla

void
TestParent::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
}
