/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BackgroundParentImpl.h"

#include "FileDescriptorSetParent.h"
#include "mozilla/Assertions.h"
#include "mozilla/dom/PBlobParent.h"
#include "mozilla/dom/indexedDB/ActorsParent.h"
#include "mozilla/dom/ipc/BlobParent.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozilla/ipc/PBackgroundTestParent.h"
#include "nsThreadUtils.h"
#include "nsTraceRefcnt.h"
#include "nsXULAppAPI.h"

using mozilla::ipc::AssertIsOnBackgroundThread;

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
    MOZ_COUNT_CTOR(mozilla::ipc::BackgroundTestParent);
  }

  ~TestParent()
  {
    MOZ_COUNT_DTOR(mozilla::ipc::BackgroundTestParent);
  }

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
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  return mozilla::dom::indexedDB::AllocPBackgroundIDBFactoryParent();
}

bool
BackgroundParentImpl::RecvPBackgroundIDBFactoryConstructor(
                                            PBackgroundIDBFactoryParent* aActor)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  return mozilla::dom::indexedDB::RecvPBackgroundIDBFactoryConstructor(aActor);
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
  MOZ_ASSERT(aParams.type() != BlobConstructorParams::T__None);

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

} // namespace ipc
} // namespace mozilla

void
TestParent::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();
}
