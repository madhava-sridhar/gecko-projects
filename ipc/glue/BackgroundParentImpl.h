/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_backgroundparentimpl_h__
#define mozilla_ipc_backgroundparentimpl_h__

#include "mozilla/Attributes.h"
#include "mozilla/ipc/PBackgroundParent.h"

namespace mozilla {
namespace dom {
  class PCacheStorageParent;
  class PCacheParent;
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

  virtual mozilla::dom::PCacheStorageParent*
  AllocPCacheStorageParent(const Namespace& aNamespace,
                           const nsCString& aOrigin,
                           const nsCString& aBaseDomain) MOZ_OVERRIDE;

  virtual bool
  DeallocPCacheStorageParent(mozilla::dom::PCacheStorageParent* aActor) MOZ_OVERRIDE;

  virtual mozilla::dom::PCacheParent*
  AllocPCacheParent(const nsCString& aOrigin,
                    const nsCString& aBaseDomain) MOZ_OVERRIDE;

  virtual bool
  DeallocPCacheParent(mozilla::dom::PCacheParent* aActor) MOZ_OVERRIDE;
};

} // namespace ipc
} // namespace mozilla

#endif // mozilla_ipc_backgroundparentimpl_h__
