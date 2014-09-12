/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MessagePortChild.h"
#include "MessagePort.h"
#include "mozilla/dom/MessageEvent.h"
#include "mozilla/ipc/PBackgroundChild.h"

namespace mozilla {
namespace dom {

MessagePortChild::MessagePortChild()
{
}

bool
MessagePortChild::RecvStopSendingDataConfirmed()
{
  MOZ_ASSERT(mPort);
  mPort->StopSendingDataConfirmed();
  return true;
}

bool
MessagePortChild::RecvEntangled(const nsTArray<MessagePortMessage>& aMessages)
{
  MOZ_ASSERT(mPort);
  mPort->Entangled(aMessages);
  return true;
}

bool
MessagePortChild::RecvReceiveData(const nsTArray<MessagePortMessage>& aMessages)
{
  MOZ_ASSERT(mPort);
  mPort->MessagesReceived(aMessages);
  return true;
}

bool
MessagePortChild::RecvClosed()
{
  if (mPort) {
    mPort->Closed();
  }

  return true;
}

} // dom namespace
} // mozilla namespace
