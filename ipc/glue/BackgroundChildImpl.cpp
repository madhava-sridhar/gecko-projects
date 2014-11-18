/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BackgroundChildImpl.h"

#include "FileDescriptorSetChild.h"
#include "mozilla/dom/PBlobChild.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBFactoryChild.h"
#include "mozilla/dom/ipc/BlobChild.h"
#include "mozilla/dom/MessagePortChild.h"
#include "mozilla/dom/cache/CacheChild.h"
#include "mozilla/dom/cache/PCacheStorageChild.h"
#include "mozilla/dom/cache/CacheStreamControlChild.h"
#include "mozilla/ipc/PBackgroundTestChild.h"
#include "nsTraceRefcnt.h"

using mozilla::dom::cache::PCacheStorageChild;
using mozilla::dom::cache::CacheChild;
using mozilla::dom::cache::PCacheChild;
using mozilla::dom::cache::PCacheStreamControlChild;
using mozilla::dom::cache::CacheStreamControlChild;

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
  : mCurrentTransaction(nullptr)
#ifdef MOZ_ENABLE_PROFILER_SPS
  , mNextTransactionSerialNumber(1)
  , mNextRequestSerialNumber(1)
#endif
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

BackgroundChildImpl::PBackgroundIDBFactoryChild*
BackgroundChildImpl::AllocPBackgroundIDBFactoryChild(
                                      const OptionalWindowId& aOptionalWindowId)
{
  MOZ_CRASH("PBackgroundIDBFactoryChild actors should be manually "
            "constructed!");
}

bool
BackgroundChildImpl::DeallocPBackgroundIDBFactoryChild(
                                             PBackgroundIDBFactoryChild* aActor)
{
  MOZ_ASSERT(aActor);

  delete aActor;
  return true;
}

auto
BackgroundChildImpl::AllocPBlobChild(const BlobConstructorParams& aParams)
  -> PBlobChild*
{
  MOZ_ASSERT(aParams.type() != BlobConstructorParams::T__None);

  return mozilla::dom::BlobChild::Create(this, aParams);
}

bool
BackgroundChildImpl::DeallocPBlobChild(PBlobChild* aActor)
{
  MOZ_ASSERT(aActor);

  mozilla::dom::BlobChild::Destroy(aActor);
  return true;
}

PFileDescriptorSetChild*
BackgroundChildImpl::AllocPFileDescriptorSetChild(
                                          const FileDescriptor& aFileDescriptor)
{
  return new FileDescriptorSetChild(aFileDescriptor);
}

bool
BackgroundChildImpl::DeallocPFileDescriptorSetChild(
                                                PFileDescriptorSetChild* aActor)
{
  MOZ_ASSERT(aActor);

  delete static_cast<FileDescriptorSetChild*>(aActor);
  return true;
}

PCacheStorageChild*
BackgroundChildImpl::AllocPCacheStorageChild(const CacheInitData& aInitData)
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
BackgroundChildImpl::AllocPCacheChild()
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

PCacheStreamControlChild*
BackgroundChildImpl::AllocPCacheStreamControlChild()
{
  return new CacheStreamControlChild();
}

bool
BackgroundChildImpl::DeallocPCacheStreamControlChild(PCacheStreamControlChild* aActor)
{
  delete aActor;
  return true;
}

// -----------------------------------------------------------------------------
// MessageChannel/MessagePort API
// -----------------------------------------------------------------------------

dom::PMessagePortChild*
BackgroundChildImpl::AllocPMessagePortChild()
{
  nsRefPtr<dom::MessagePortChild> agent = new dom::MessagePortChild();
  return agent.forget().take();
}

bool
BackgroundChildImpl::DeallocPMessagePortChild(PMessagePortChild* aActor)
{
  nsRefPtr<dom::MessagePortChild> child =
    dont_AddRef(static_cast<dom::MessagePortChild*>(aActor));
  MOZ_ASSERT(child);
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
