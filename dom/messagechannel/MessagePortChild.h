/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_MessagePortChild_h
#define mozilla_dom_MessagePortChild_h

#include "mozilla/dom/PMessagePortChild.h"

namespace mozilla {
namespace dom {

class MessagePort;
class PostMessageRunnable;

class MessagePortChild MOZ_FINAL : public PMessagePortChild
{
public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(MessagePortChild)

  MessagePortChild();

  void SetPort(MessagePort* aPort)
  {
    mPort = aPort;
  }

  virtual bool
  RecvEntangled(const nsTArray<MessagePortMessage>& aMessages) MOZ_OVERRIDE;

  virtual bool
  RecvReceiveData(const nsTArray<MessagePortMessage>& aMessages) MOZ_OVERRIDE;

  virtual bool RecvStopSendingDataConfirmed() MOZ_OVERRIDE;

  virtual bool RecvClosed() MOZ_OVERRIDE;

private:
  ~MessagePortChild() {
    MOZ_ASSERT(!mPort);
  }

  // This raw pointer is the parent and it's kept alive until this object is
  // not fully deleted.
  MessagePort* mPort;
};

} // dom namespace
} // mozilla namespace

#endif // mozilla_dom_MessagePortChild_h
