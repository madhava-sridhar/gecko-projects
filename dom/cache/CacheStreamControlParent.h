/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_CacheStreamControlParent_h
#define mozilla_dom_cache_CacheStreamControlParent_h

#include "mozilla/dom/cache/Manager.h"

namespace mozilla {
namespace dom {
namespace cache {

class CacheStreamControlListener;

class CacheStreamControlParent : public Manager::StreamControl
{
public:
  CacheStreamControlParent();
  ~CacheStreamControlParent();

  void AddListener(CacheStreamControlListener* aListener);
  void RemoveListener(CacheStreamControlListener* aListener);

  // PCacheStreamControlParent methods
  virtual void ActorDestroy(ActorDestroyReason aReason) MOZ_OVERRIDE;
  virtual bool RecvNoteClosed(const nsID& aId) MOZ_OVERRIDE;

  // Manager::StreamControl methods
  virtual void SetStreamList(Manager::StreamList* aStreamList) MOZ_OVERRIDE;
  virtual void Close(const nsID& aId) MOZ_OVERRIDE;
  virtual void CloseAll() MOZ_OVERRIDE;
  virtual void Shutdown() MOZ_OVERRIDE;

private:
  void NotifyClose(const nsID& aId);
  void NotifyCloseAll();

  nsRefPtr<Manager::StreamList> mStreamList;
  nsTArray<CacheStreamControlListener*> mListeners;
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_CacheStreamControlParent_h
