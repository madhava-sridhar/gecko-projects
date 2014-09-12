/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MessagePortData.h"
#include "mozilla/dom/PMessagePort.h"

namespace mozilla {
namespace dom {

void
MessagePortData::FromDataToMessages(
                               const nsTArray<nsRefPtr<MessagePortData>>& aData,
                               nsTArray<MessagePortMessage>& aArray)
{
  aArray.Clear();

  for (uint32_t i = 0, len = aData.Length(); i < len; ++i) {
    MessagePortMessage message;

    SerializedStructuredCloneBuffer& buffer = message.data();
    buffer.data = aData[i]->mBuffer.data();
    buffer.dataLength = aData[i]->mBuffer.nbytes();

#ifdef DEBUG
    {
      const nsTArray<nsCOMPtr<nsIDOMBlob>>& blobs = aData[i]->mClosure.mBlobs;
      MOZ_ASSERT(blobs.IsEmpty());
    }
#endif

    aArray.AppendElement(message);
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

    aData.AppendElement(data);
  }
}

} // dom namespace
} // mozilla namespace
