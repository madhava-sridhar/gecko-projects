/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/CacheStreamControlParent.h"

#include "mozilla/unused.h"
#include "mozilla/dom/cache/CacheStreamControlListener.h"

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::unused;

CacheStreamControlParent::CacheStreamControlParent()
{
}

CacheStreamControlParent::~CacheStreamControlParent()
{
  MOZ_ASSERT(!mStreamList);
}

void
CacheStreamControlParent::AddListener(CacheStreamControlListener* aListener)
{
  MOZ_ASSERT(aListener);
  mListeners.AppendElement(aListener);
}

void
CacheStreamControlParent::RemoveListener(CacheStreamControlListener* aListener)
{
  MOZ_ASSERT(aListener);
  mListeners.RemoveElement(aListener);
}

void
CacheStreamControlParent::ActorDestroy(ActorDestroyReason aReason)
{
  MOZ_ASSERT(mStreamList);
  mStreamList->RemoveStreamControl(this);
  mStreamList = nullptr;
}

bool
CacheStreamControlParent::RecvNoteClosed(const nsID& aId)
{
  MOZ_ASSERT(mStreamList);
  mStreamList->NoteClosed(aId);
  return true;
}

void
CacheStreamControlParent::SetStreamList(Manager::StreamList* aStreamList)
{
  MOZ_ASSERT(!mStreamList);
  mStreamList = aStreamList;
}

void
CacheStreamControlParent::Close(const nsID& aId)
{
  NotifyClose(aId);
  unused << SendClose(aId);
}

void
CacheStreamControlParent::CloseAll()
{
  NotifyCloseAll();
  unused << SendCloseAll();
}

void
CacheStreamControlParent::Shutdown()
{
  unused << Send__delete__(this);
}

void
CacheStreamControlParent::NotifyClose(const nsID& aId)
{
  // defensive copy of list since may be modified as we close streams
  nsTArray<CacheStreamControlListener*> listeners(mListeners);
  for (uint32_t i = 0; i < listeners.Length(); ++i) {
    // note, multiple streams may exist for same ID
    if (listeners[i]->MatchId(aId)) {
      listeners[i]->CloseStream();
    }
  }
}

void
CacheStreamControlParent::NotifyCloseAll()
{
  // defensive copy of list since may be modified as we close streams
  nsTArray<CacheStreamControlListener*> listeners(mListeners);
  for (uint32_t i = 0; i < listeners.Length(); ++i) {
    listeners[i]->CloseStream();
  }
}

} // namespace cache
} // namespace dom
} // namespace mozilla
