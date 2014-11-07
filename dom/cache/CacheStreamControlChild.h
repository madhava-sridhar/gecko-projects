/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_CacheStreamControlChild_h
#define mozilla_dom_cache_CacheStreamControlChild_h

#include "mozilla/dom/cache/PCacheStreamControlChild.h"
#include "nsTArray.h"

namespace mozilla {
namespace dom {
namespace cache {

class CacheStreamControlListener;

class CacheStreamControlChild : public PCacheStreamControlChild
{
public:
  CacheStreamControlChild();
  virtual ~CacheStreamControlChild();

  void AddListener(CacheStreamControlListener* aListener);
  void RemoveListener(CacheStreamControlListener* aListener);

  void NoteClosed(const nsID& aId);

  // PCacheStreamControlChild methods
  virtual void ActorDestroy(ActorDestroyReason aReason) MOZ_OVERRIDE;
  virtual bool RecvClose(const nsID& aId) MOZ_OVERRIDE;
  virtual bool RecvCloseAll() MOZ_OVERRIDE;

private:
  nsTArray<CacheStreamControlListener*> mListeners;
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_CacheStreamControlChild_h
