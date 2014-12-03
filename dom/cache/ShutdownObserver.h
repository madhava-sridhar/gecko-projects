/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_ShutdownObserver_h
#define mozilla_dom_cache_ShutdownObserver_h

#include "mozilla/Attributes.h"
#include "nsCOMPtr.h"
#include "nsIObserver.h"
#include "nsIThread.h"
#include "nsString.h"
#include "nsTArray.h"

namespace mozilla {
namespace dom {
namespace cache {

class ManagerId;

class ShutdownObserver MOZ_FINAL : public nsIObserver
{
public:
  static already_AddRefed<ShutdownObserver> Instance();

  nsresult AddManagerId(ManagerId* aManagerId);
  void RemoveManagerId(ManagerId* aManagerId);

private:
  ShutdownObserver();
  virtual ~ShutdownObserver();

  void InitOnMainThread();
  void AddManagerIdOnMainThread(ManagerId* aManagerId);
  void RemoveManagerIdOnMainThread(ManagerId* aManagerId);

  void StartShutdownOnBgThread();
  void FinishShutdownOnBgThread();

  void DoShutdown();

  nsCOMPtr<nsIThread> mBackgroundThread;

  // main thread only
  nsTArray<nsRefPtr<ManagerId>> mManagerIds;

  // set on main thread once and read on bg thread
  nsTArray<nsRefPtr<ManagerId>> mManagerIdsInProcess;

  // bg thread only
  bool mShuttingDown;

public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIOBSERVER
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_ShutdownObserver_h
