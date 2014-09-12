/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MessagePortParent.h"
#include "MessagePortService.h"
#include "mozilla/unused.h"

namespace mozilla {
namespace dom {

MessagePortParent::MessagePortParent()
  : mService(MessagePortService::GetOrCreate())
  , mEntangled(false)
  , mCanSendData(true)
{
}

MessagePortParent::~MessagePortParent()
{
  MOZ_ASSERT(!mService);
  MOZ_ASSERT(!mEntangled);
}

bool
MessagePortParent::RecvEntangle(const nsID& aUUID,
                                const nsID& aDestinationUUID,
                                const uint32_t& aSequenceID)
{
  MOZ_ASSERT(mService, "Entangle is called after a shutdown!");
  MOZ_ASSERT(!mEntangled);

  mUUID = aUUID;
  return mService->RequestEntangling(this, aUUID, aDestinationUUID,
                                     aSequenceID);
}

bool
MessagePortParent::RecvPostMessages(
                                  const nsTArray<MessagePortMessage>& aMessages)
{
  if (!mEntangled) {
    return true;
  }

  MOZ_ASSERT(mService, "PostMessage is called after a shutdown!");
  return mService->PostMessages(this, mUUID, aMessages);
}

bool
MessagePortParent::RecvDisentangle(
                                  const nsTArray<MessagePortMessage>& aMessages)
{
  if (!mEntangled) {
    return true;
  }

  MOZ_ASSERT(mService, "Disentangle is called after a shutdown!");
  if (!mService->DisentanglePort(this, mUUID, aMessages)) {
    return false;
  }

  mService = nullptr;
  mEntangled = false;

  unused << Send__delete__(this);
  return true;
}

bool
MessagePortParent::RecvStopSendingData()
{
  if (!mEntangled) {
    return true;
  }

  mCanSendData = false;
  unused << SendStopSendingDataConfirmed();
  return true;
}

bool
MessagePortParent::RecvClose()
{
  if (mService) {
    MOZ_ASSERT(mService);
    MOZ_ASSERT(mEntangled);

    if (!mService->ClosePort(this, mUUID)) {
      return false;
    }

    mService = nullptr;
    mEntangled = false;
  }

  MOZ_ASSERT(!mService);
  MOZ_ASSERT(!mEntangled);

  unused << Send__delete__(this);
  return true;
}

void
MessagePortParent::ActorDestroy(ActorDestroyReason aWhy)
{
  if (mService && mEntangled) {
    mService->ParentDestroy(this, mUUID);
  }
}

bool
MessagePortParent::Entangled(const nsTArray<MessagePortMessage>& aMessages)
{
  MOZ_ASSERT(!mEntangled);
  mEntangled = true;
  return SendEntangled(aMessages);
}

void
MessagePortParent::Close()
{
  mService = nullptr;
  mEntangled = false;

  unused << SendClosed();
}

} // dom namespace
} // mozilla namespace
