/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_MessagePortParent_h
#define mozilla_dom_MessagePortParent_h

#include "mozilla/dom/PMessagePortParent.h"

namespace mozilla {
namespace dom {

class MessagePortService;

class MessagePortParent MOZ_FINAL : public PMessagePortParent
{
public:
  MessagePortParent();
  ~MessagePortParent();

  virtual bool RecvEntangle(const nsID& aUUID,
                            const nsID& aDestinationUUID,
                            const uint32_t& aSequenceID) MOZ_OVERRIDE;

  virtual bool
  RecvPostMessages(const nsTArray<MessagePortMessage>& aMessages) MOZ_OVERRIDE;

  virtual bool
  RecvDisentangle(const nsTArray<MessagePortMessage>& aMessages) MOZ_OVERRIDE;

  virtual bool RecvStopSendingData() MOZ_OVERRIDE;

  virtual bool RecvClose() MOZ_OVERRIDE;

  virtual void ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;

  bool Entangled(const nsTArray<MessagePortMessage>& aMessages);

  void Close();

  bool CanSendData()
  {
    return mCanSendData;
  }

private:
  nsRefPtr<MessagePortService> mService;
  nsID mUUID;
  bool mEntangled;
  bool mCanSendData;
};

} // dom namespace
} // mozilla namespace

#endif // mozilla_dom_MessagePortParent_h
