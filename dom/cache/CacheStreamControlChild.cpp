/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/CacheStreamControlChild.h"

#include "mozilla/unused.h"

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::unused;

void
CacheStreamControlChild::AddListener(Listener* aListener)
{
  MOZ_ASSERT(aListener);
  mListeners.AppendElement(aListener);
}

void
CacheStreamControlChild::RemoveListener(Listener* aListener)
{
  MOZ_ASSERT(aListener);
  mListeners.RemoveElement(aListener);
}

void
CacheStreamControlChild::NoteClosed(const nsID& aId)
{
  unused << SendNoteClosed(aId);
}

void
CacheStreamControlChild::ActorDestroy(ActorDestroyReason aReason)
{
  RecvCloseAll();
}

bool
CacheStreamControlChild::RecvClose(const nsID& aId)
{
  // defensive copy of list since may be modified as we close streams
  nsTArray<Listener*> listeners(mListeners);
  for (uint32_t i = 0; i < listeners.Length(); ++i) {
    // note, multiple streams may exist for same ID
    if (listeners[i]->MatchId(aId)) {
      listeners[i]->CloseStream();
    }
  }
  return true;
}

bool
CacheStreamControlChild::RecvCloseAll()
{
  // defensive copy of list since may be modified as we close streams
  nsTArray<Listener*> listeners(mListeners);
  for (uint32_t i = 0; i < listeners.Length(); ++i) {
    listeners[i]->CloseStream();
  }
  return true;
}

} // namespace cache
} // namespace dom
} // namespace mozilla
