/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_backgroundparentimpl_h__
#define mozilla_ipc_backgroundparentimpl_h__

#include "mozilla/Attributes.h"
#include "mozilla/ipc/PBackgroundParent.h"

namespace mozilla {
namespace dom {
namespace cache {
  class PCacheParent;
  class PCacheStorageParent;
  class PCacheStreamControlParent;
}
}
namespace ipc {

// Instances of this class should never be created directly. This class is meant
// to be inherited in BackgroundImpl.
class BackgroundParentImpl : public PBackgroundParent
{
protected:
  BackgroundParentImpl();
  virtual ~BackgroundParentImpl();

  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;

  virtual PBackgroundTestParent*
  AllocPBackgroundTestParent(const nsCString& aTestArg) MOZ_OVERRIDE;

  virtual bool
  RecvPBackgroundTestConstructor(PBackgroundTestParent* aActor,
                                 const nsCString& aTestArg) MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundTestParent(PBackgroundTestParent* aActor) MOZ_OVERRIDE;

  virtual PBackgroundIDBFactoryParent*
  AllocPBackgroundIDBFactoryParent(const OptionalWindowId& aOptionalWindowId)
                                   MOZ_OVERRIDE;

  virtual bool
  RecvPBackgroundIDBFactoryConstructor(
                                      PBackgroundIDBFactoryParent* aActor,
                                      const OptionalWindowId& aOptionalWindowId)
                                      MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundIDBFactoryParent(PBackgroundIDBFactoryParent* aActor)
                                     MOZ_OVERRIDE;

  virtual PBlobParent*
  AllocPBlobParent(const BlobConstructorParams& aParams) MOZ_OVERRIDE;

  virtual bool
  DeallocPBlobParent(PBlobParent* aActor) MOZ_OVERRIDE;

  virtual PFileDescriptorSetParent*
  AllocPFileDescriptorSetParent(const FileDescriptor& aFileDescriptor)
                                MOZ_OVERRIDE;

  virtual bool
  DeallocPFileDescriptorSetParent(PFileDescriptorSetParent* aActor)
                                  MOZ_OVERRIDE;

  virtual mozilla::dom::cache::PCacheStorageParent*
  AllocPCacheStorageParent(const CacheInitData& aInitData) MOZ_OVERRIDE;

  virtual bool
  DeallocPCacheStorageParent(mozilla::dom::cache::PCacheStorageParent* aActor) MOZ_OVERRIDE;

  virtual mozilla::dom::cache::PCacheParent* AllocPCacheParent() MOZ_OVERRIDE;

  virtual bool
  DeallocPCacheParent(mozilla::dom::cache::PCacheParent* aActor) MOZ_OVERRIDE;

  virtual mozilla::dom::cache::PCacheStreamControlParent*
  AllocPCacheStreamControlParent();

  virtual bool
  DeallocPCacheStreamControlParent(mozilla::dom::cache::PCacheStreamControlParent* aActor);

  virtual PMessagePortParent*
  AllocPMessagePortParent() MOZ_OVERRIDE;

  virtual bool
  DeallocPMessagePortParent(PMessagePortParent* aActor) MOZ_OVERRIDE;
};

} // namespace ipc
} // namespace mozilla

#endif // mozilla_ipc_backgroundparentimpl_h__
