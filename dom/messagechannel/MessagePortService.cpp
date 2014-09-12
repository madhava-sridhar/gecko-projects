/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MessagePortService.h"
#include "MessagePortData.h"
#include "MessagePortParent.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/unused.h"
#include "nsDataHashtable.h"
#include "nsTArray.h"

namespace mozilla {
namespace dom {

static MessagePortService* sInstance = nullptr;

class MessagePortServiceData
{
public:
  MessagePortServiceData(const nsID& aDestinationUUID)
    : mDestinationUUID(aDestinationUUID)
    , mSequenceID(1)
    , mParent(nullptr)
  { }

  nsID mDestinationUUID;

  uint32_t mSequenceID;
  MessagePortParent* mParent;

  // MessagePortParent keeps the service alive, and we don't want a cycle.
  nsDataHashtable<nsUint32HashKey, MessagePortParent*> mNextParents;
  nsTArray<nsRefPtr<MessagePortData>> mMessages;
};

MessagePortService::MessagePortService()
{
  // sInstance is a raw MessagePortService*.
  MOZ_ASSERT(!sInstance);
  sInstance = this;
}

MessagePortService::~MessagePortService()
{
  MOZ_ASSERT(sInstance == this);
  sInstance = nullptr;
}

// static
already_AddRefed<MessagePortService>
MessagePortService::GetOrCreate()
{
  nsRefPtr<MessagePortService> instance = sInstance;
  if (!instance) {
    instance = new MessagePortService();
  }
  return instance.forget();
}

bool
MessagePortService::RequestEntangling(MessagePortParent* aParent,
                                      const nsID& aUUID,
                                      const nsID& aDestinationUUID,
                                      const uint32_t& aSequenceID)
{
  MessagePortServiceData* data;

  // If we already have a MessagePortServiceData, it means that we have 2
  // entangled ports.
  if (!mPorts.Get(aUUID, &data)) {
    // Create the MessagePortServiceData for the destination.
    if (mPorts.Get(aDestinationUUID, &data)) {
      MOZ_CRASH("The creation of the 2 ports should be in sync.");
      return false;
    }

    data = new MessagePortServiceData(aUUID);
    mPorts.Put(aDestinationUUID, data);

    data = new MessagePortServiceData(aDestinationUUID);
    mPorts.Put(aUUID, data);
  }

  // This is a security check.
  if (!data->mDestinationUUID.Equals(aDestinationUUID)) {
    MOZ_CRASH("DestinationUUIDs do not match!");
    return false;
  }

  if (aSequenceID < data->mSequenceID) {
    MOZ_CRASH("Invalid sequence ID!");
    return false;
  }

  if (aSequenceID == data->mSequenceID) {
    if (data->mParent) {
      MOZ_CRASH("Two ports cannot have the same sequenceID.");
      return false;
    }

    // We activate this port, sending all the messages.
    data->mParent = aParent;
    nsTArray<MessagePortMessage> array;
    MessagePortData::FromDataToMessages(data->mMessages, array);
    bool rv = aParent->Entangled(array);
    data->mMessages.Clear();

    return rv;
  }

  // This new parent will be the next one when a Disentangle request is
  // received from the current parent.
  data->mNextParents.Put(aSequenceID, aParent);
  return true;
}

bool
MessagePortService::DisentanglePort(
                                  MessagePortParent* aParent,
                                  const nsID& aUUID,
                                  const nsTArray<MessagePortMessage>& aMessages)
{
  MessagePortServiceData* data;
  if (!mPorts.Get(aUUID, &data)) {
    MOZ_CRASH("Unknown MessagePortParent should not happend.");
    return false;
  }

  if (data->mParent != aParent) {
    MOZ_CRASH("DisentanglePort() should be called just from the correct parent.");
    return false;
  }

  // Let's put the messages in the correct order. |aMessages| contains the
  // unsent messages so they have to go first.
  nsTArray<nsRefPtr<MessagePortData>> messages;
  MessagePortData::FromMessagesToData(aMessages, messages);
  messages.AppendElements(data->mMessages);
  data->mMessages.Clear();

  ++data->mSequenceID;

  // If we don't have a parent, we have to store the pending messages and wait.
  MessagePortParent* nextParent;
  if (!data->mNextParents.Get(data->mSequenceID, &nextParent)) {
    data->mMessages = messages;
    data->mParent = nullptr;
    return true;
  }

  // Let's activate the new part with the pending messages.
  data->mMessages.Clear();

  data->mParent = nextParent;
  data->mNextParents.Remove(data->mSequenceID);

  nsTArray<MessagePortMessage> array;
  MessagePortData::FromDataToMessages(messages, array);
  unused << data->mParent->Entangled(array);
  return true;
}

bool
MessagePortService::ClosePort(MessagePortParent* aParent,
                              const nsID& aUUID)
{
  MessagePortServiceData* data;
  if (!mPorts.Get(aUUID, &data)) {
    MOZ_CRASH("Unknown MessagePortParent should not happend.");
    return false;
  }

  if (data->mParent != aParent) {
    MOZ_CRASH("ClosePort() should be called just from the correct parent.");
    return false;
  }

  if (data->mNextParents.Count()) {
    MOZ_CRASH("ClosePort() should be called when there are not next parents.");
    return false;
  }

  // We don't want to send a message to this parent.
  data->mParent = nullptr;

  CloseAll(aUUID);
  return true;
}

namespace {

PLDHashOperator
ClosePortEnumerator(const uint32_t& aSequenceId, MessagePortParent* aParent,
                    void* aUnused)
{
  MOZ_ASSERT(aParent);
  aParent->Close();
  return PL_DHASH_NEXT;
}

} // anonymous namespace

void
MessagePortService::CloseAll(const nsID& aUUID)
{
  MessagePortServiceData* data;
  if (!mPorts.Get(aUUID, &data)) {
    return;
  }

  if (data->mParent) {
    data->mParent->Close();
  }

  data->mNextParents.EnumerateRead(ClosePortEnumerator, nullptr);

  nsID destinationUUID = data->mDestinationUUID;
  mPorts.Remove(aUUID);

  CloseAll(destinationUUID);
  return;
}

bool
MessagePortService::PostMessages(MessagePortParent* aParent,
                                 const nsID& aUUID,
                                 const nsTArray<MessagePortMessage>& aMessages)
{
  MessagePortServiceData* data;
  if (!mPorts.Get(aUUID, &data)) {
    MOZ_CRASH("Unknown MessagePortParent should not happend.");
    return false;
  }

  if (data->mParent != aParent) {
    MOZ_CRASH("PostMessages() should be called just from the correct parent.");
    return false;
  }

  // This cannot happen.
  if (!mPorts.Get(data->mDestinationUUID, &data)) {
    MOZ_CRASH("MessagePortServiceData for the destination port should exist.");
    return false;
  }

  // If the parent can send data to the child and we don't have any in queue,
  // don't serialize data, but let's send it immediatelly.
  if (data->mParent && data->mParent->CanSendData() &&
      data->mMessages.IsEmpty()) {
    unused << data->mParent->SendReceiveData(aMessages);
    return true;
  }

  nsTArray<nsRefPtr<MessagePortData>> array;
  MessagePortData::FromMessagesToData(aMessages, array);
  data->mMessages.AppendElements(array);

  // If the parent can send data to the child, let's proceed.
  if (data->mParent && data->mParent->CanSendData()) {
    nsTArray<MessagePortMessage> messages;
    MessagePortData::FromDataToMessages(data->mMessages, messages);
    unused << data->mParent->SendReceiveData(messages);
    data->mMessages.Clear();
  }

  return true;
}

namespace {

struct ParentDestroyData
{
  ParentDestroyData(MessagePortParent* aParent)
    : mParent(aParent)
    , mSequenceId(0)
  { }

  MessagePortParent* mParent;
  uint32_t mSequenceId;
};

PLDHashOperator
ParentDestroyEnumerator(const uint32_t& aSequenceId, MessagePortParent* aParent,
                        void* aData)
{
  MOZ_ASSERT(aParent);
  ParentDestroyData* data = static_cast<ParentDestroyData*>(aData);

  if (aParent == data->mParent) {
    data->mSequenceId = aSequenceId;
    return PL_DHASH_STOP;
  }

  return PL_DHASH_NEXT;
}

} // anonymous namespace

void
MessagePortService::ParentDestroy(MessagePortParent* aParent,
                                  const nsID& aUUID)
{
  // When the last parent is deleted, this service is freed but this cannot be
  // done when the hashtables are written by CloseAll.
  nsRefPtr<MessagePortService> kungFuDeathGrip = this;

  // This port has already been destroyed.
  MessagePortServiceData* data;
  if (!mPorts.Get(aUUID, &data)) {
    return;
  }

  // This port has to be closed.
  if (data->mParent == aParent) {
    CloseAll(aUUID);
    return;
  }

  ParentDestroyData d(aParent);
  data->mNextParents.EnumerateRead(ParentDestroyEnumerator, &d);
  MOZ_ASSERT(d.mSequenceId);

  // We don't want to send a message to this parent.
  data->mNextParents.Remove(d.mSequenceId);
  CloseAll(aUUID);
}

} // dom namespace
} // mozilla namespace
