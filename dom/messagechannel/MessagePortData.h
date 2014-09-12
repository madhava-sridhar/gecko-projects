/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_MessagePortData_h
#define mozilla_dom_MessagePortData_h

#include "mozilla/dom/StructuredCloneUtils.h"

namespace mozilla {
namespace dom {

class MessagePortMessage;

class MessagePortData MOZ_FINAL
{
public:
  NS_INLINE_DECL_REFCOUNTING(MessagePortData)

  JSAutoStructuredCloneBuffer mBuffer;
  StructuredCloneClosure mClosure;

  MessagePortData()
  { }

  static void
  FromDataToMessages(const nsTArray<nsRefPtr<MessagePortData>>& aData,
                     nsTArray<MessagePortMessage>& aArray);

  static void
  FromMessagesToData(const nsTArray<MessagePortMessage>& aArray,
                     nsTArray<nsRefPtr<MessagePortData>>& aData);

private:
  ~MessagePortData()
  { }

};

} // dom namespace
} // mozilla namespace

#endif // mozilla_dom_MessagePortData_h
