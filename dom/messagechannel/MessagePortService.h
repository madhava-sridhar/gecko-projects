/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_MessagePortService_h
#define mozilla_dom_MessagePortService_h

#include "nsISupportsImpl.h"
#include "nsHashKeys.h"
#include "nsClassHashtable.h"

namespace mozilla {
namespace dom {

class MessagePortMessage;
class MessagePortParent;
class MessagePortServiceData;

class MessagePortService MOZ_FINAL
{
public:
  NS_INLINE_DECL_REFCOUNTING(MessagePortService)

  static already_AddRefed<MessagePortService> GetOrCreate();

  bool RequestEntangling(MessagePortParent* aParent,
                         const nsID& aUUID,
                         const nsID& aDestinationUUID,
                         const uint32_t& aSequenceID);

  bool DisentanglePort(MessagePortParent* aParent,
                       const nsID& aUUID,
                       const nsTArray<MessagePortMessage>& aMessages);

  bool ClosePort(MessagePortParent* aParent,
                 const nsID& aUUID);

  bool PostMessages(MessagePortParent* aParent,
                    const nsID& aUUID,
                    const nsTArray<MessagePortMessage>& aMessages);

  bool RequestData(MessagePortParent* aParent,
                   const nsID& aUUID);

  void ParentDestroy(MessagePortParent* aParent,
                     const nsID& aUUID);

private:
  MessagePortService();
  ~MessagePortService();

  void CloseAll(const nsID& aUUID);

  nsClassHashtable<nsIDHashKey, MessagePortServiceData> mPorts;
};

} // dom namespace
} // mozilla namespace

#endif // mozilla_dom_MessagePortService_h
