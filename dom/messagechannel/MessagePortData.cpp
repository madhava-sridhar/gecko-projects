/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MessagePortData.h"
#include "mozilla/dom/PMessagePort.h"

namespace mozilla {
namespace dom {

namespace {

void
FromIdentifierToTransferredPort(const MessagePortIdentifier& aIdentifier,
                                TransferredMessagePortIdentifier* aPort)
{
  aPort->uuid() = aIdentifier.mUUID;
  aPort->destinationUuid() = aIdentifier.mDestinationUUID;
  aPort->sequenceId() = aIdentifier.mSequenceID;
  aPort->neutered() = aIdentifier.mNeutered;
}

void
FromTransferredPortToIdentifier(const TransferredMessagePortIdentifier& aPort,
                                MessagePortIdentifier* aIdentifier)
{
  aIdentifier->mUUID = aPort.uuid();
  aIdentifier->mDestinationUUID = aPort.destinationUuid();
  aIdentifier->mSequenceID = aPort.sequenceId();
  aIdentifier->mNeutered = aPort.neutered();
}

} // anonymous namespace

void
MessagePortData::FromDataToMessages(
                               const nsTArray<nsRefPtr<MessagePortData>>& aData,
                               nsTArray<MessagePortMessage>& aArray)
{
  aArray.Clear();

  for (uint32_t i = 0, len = aData.Length(); i < len; ++i) {
    MessagePortMessage* message = aArray.AppendElement();

    SerializedStructuredCloneBuffer& buffer = message->data();
    aData[i]->mBuffer.steal(&buffer.data, &buffer.dataLength);

#ifdef DEBUG
    {
      const nsTArray<nsCOMPtr<nsIDOMBlob>>& blobs = aData[i]->mClosure.mBlobs;
      MOZ_ASSERT(blobs.IsEmpty());
    }
#endif

    {
      const nsTArray<MessagePortIdentifier>& identifiers =
        aData[i]->mClosure.mMessagePortIdentifiers;
      nsTArray<TransferredMessagePortIdentifier>& transferredPorts =
        message->transferredPorts();

      for (uint32_t t = 0; t < identifiers.Length(); ++t) {
        TransferredMessagePortIdentifier* data =
          transferredPorts.AppendElement();
        FromIdentifierToTransferredPort(identifiers[t], data);
      }
    }
  }
}

void
MessagePortData::FromMessagesToData(const nsTArray<MessagePortMessage>& aArray,
                                    nsTArray<nsRefPtr<MessagePortData>>& aData)
{
  aData.Clear();

  for (uint32_t i = 0, len = aArray.Length(); i < len; ++i) {
    nsRefPtr<MessagePortData> data = new MessagePortData();

    const SerializedStructuredCloneBuffer& buffer = aArray[i].data();
    data->mBuffer.copy(buffer.data, buffer.dataLength);

    {
      const nsTArray<TransferredMessagePortIdentifier>& transferredPorts =
        aArray[i].transferredPorts();
      nsTArray<MessagePortIdentifier>& identifiers =
        data->mClosure.mMessagePortIdentifiers;

      for (uint32_t t = 0; t < transferredPorts.Length(); ++t) {
        MessagePortIdentifier* identifier = identifiers.AppendElement();
        FromTransferredPortToIdentifier(transferredPorts[t], identifier);
      }
    }

    aData.AppendElement(data);
  }
}

} // dom namespace
} // mozilla namespace
