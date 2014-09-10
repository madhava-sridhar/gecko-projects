/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BackgroundParentImpl.h"

#include "mozilla/Assertions.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozilla/ipc/PBackgroundTestParent.h"
#include "mozilla/dom/CacheParent.h"
#include "mozilla/dom/CacheStorageParent.h"
#include "nsThreadUtils.h"
#include "nsTraceRefcnt.h"
#include "nsXULAppAPI.h"

using mozilla::ipc::AssertIsOnBackgroundThread;
using mozilla::dom::CacheStorageParent;
using mozilla::dom::PCacheStorageParent;
using mozilla::dom::CacheParent;
using mozilla::dom::PCacheParent;
using mozilla::dom::cache::Namespace;

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

PBackgroundTestParent*
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

PCacheStorageParent*
BackgroundParentImpl::AllocPCacheStorageParent(const Namespace& aNamespace,
                                               const nsCString& aOrigin,
                                               const nsCString& aBaseDomain)
{
  return new CacheStorageParent(aNamespace, aOrigin, aBaseDomain);
}

bool
BackgroundParentImpl::DeallocPCacheStorageParent(PCacheStorageParent* aActor)
{
  delete aActor;
  return true;
}

PCacheParent*
BackgroundParentImpl::AllocPCacheParent(const nsCString& aOrigin,
                                        const nsCString& aBaseDomain)
{
  return new CacheParent(aOrigin, aBaseDomain);
}

bool
BackgroundParentImpl::DeallocPCacheParent(PCacheParent* aActor)
{
  delete aActor;
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
