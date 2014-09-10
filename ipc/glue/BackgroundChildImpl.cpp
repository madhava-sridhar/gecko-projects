/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BackgroundChildImpl.h"

#include "mozilla/dom/CacheChild.h"
#include "mozilla/dom/PCacheStorageChild.h"
#include "mozilla/ipc/PBackgroundTestChild.h"
#include "nsTraceRefcnt.h"

using mozilla::dom::PCacheStorageChild;
using mozilla::dom::CacheChild;
using mozilla::dom::PCacheChild;

namespace {

class TestChild MOZ_FINAL : public mozilla::ipc::PBackgroundTestChild
{
  friend class mozilla::ipc::BackgroundChildImpl;

  nsCString mTestArg;

  explicit TestChild(const nsCString& aTestArg)
  : mTestArg(aTestArg)
  {
    MOZ_COUNT_CTOR(TestChild);
  }

protected:
  ~TestChild()
  {
    MOZ_COUNT_DTOR(TestChild);
  }

public:
  virtual bool
  Recv__delete__(const nsCString& aTestArg) MOZ_OVERRIDE;
};

} // anonymous namespace

namespace mozilla {
namespace ipc {

// -----------------------------------------------------------------------------
// BackgroundChildImpl::ThreadLocal
// -----------------------------------------------------------------------------

BackgroundChildImpl::
ThreadLocal::ThreadLocal()
{
  // May happen on any thread!
  MOZ_COUNT_CTOR(mozilla::ipc::BackgroundChildImpl::ThreadLocal);
}

BackgroundChildImpl::
ThreadLocal::~ThreadLocal()
{
  // May happen on any thread!
  MOZ_COUNT_DTOR(mozilla::ipc::BackgroundChildImpl::ThreadLocal);
}

// -----------------------------------------------------------------------------
// BackgroundChildImpl
// -----------------------------------------------------------------------------

BackgroundChildImpl::BackgroundChildImpl()
{
  // May happen on any thread!
  MOZ_COUNT_CTOR(mozilla::ipc::BackgroundChildImpl);
}

BackgroundChildImpl::~BackgroundChildImpl()
{
  // May happen on any thread!
  MOZ_COUNT_DTOR(mozilla::ipc::BackgroundChildImpl);
}

void
BackgroundChildImpl::ActorDestroy(ActorDestroyReason aWhy)
{
  // May happen on any thread!
}

PBackgroundTestChild*
BackgroundChildImpl::AllocPBackgroundTestChild(const nsCString& aTestArg)
{
  return new TestChild(aTestArg);
}

bool
BackgroundChildImpl::DeallocPBackgroundTestChild(PBackgroundTestChild* aActor)
{
  MOZ_ASSERT(aActor);

  delete static_cast<TestChild*>(aActor);
  return true;
}

PCacheStorageChild*
BackgroundChildImpl::AllocPCacheStorageChild(const Namespace& aNamespace,
                                             const nsCString& aOrigin,
                                             const nsCString& aBaseDomain)
{
  MOZ_CRASH("CacheStorageChild actor must be provided to PBackground manager");
  return nullptr;
}

bool
BackgroundChildImpl::DeallocPCacheStorageChild(PCacheStorageChild* aActor)
{
  // The CacheStorageChild actor is provided to the PBackground manager, but
  // we own the object and must delete it.
  delete aActor;
  return true;
}

PCacheChild*
BackgroundChildImpl::AllocPCacheChild(const nsCString& aOrigin,
                                      const nsCString& aBaseDomain)
{
  return new CacheChild();
}

bool
BackgroundChildImpl::DeallocPCacheChild(PCacheChild* aActor)
{
  // The CacheChild actor is provided to the PBackground manager, but
  // we own the object and must delete it.
  delete aActor;
  return true;
}

} // namespace ipc
} // namespace mozilla

bool
TestChild::Recv__delete__(const nsCString& aTestArg)
{
  MOZ_RELEASE_ASSERT(aTestArg == mTestArg,
                     "BackgroundTest message was corrupted!");

  return true;
}
