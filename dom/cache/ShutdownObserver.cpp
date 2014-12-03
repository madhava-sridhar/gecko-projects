/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/ShutdownObserver.h"

#include "mozilla/dom/cache/Manager.h"
#include "mozilla/dom/cache/ManagerId.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozilla/Services.h"
#include "nsIObserverService.h"
#include "nsThreadUtils.h"

namespace {

static bool sInstanceInit = false;
static nsRefPtr<mozilla::dom::cache::ShutdownObserver> sInstance = nullptr;

} // anonymous namespace

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::services::GetObserverService;

NS_IMPL_ISUPPORTS(mozilla::dom::cache::ShutdownObserver, nsIObserver);

// static
already_AddRefed<ShutdownObserver>
ShutdownObserver::Instance()
{
  mozilla::ipc::AssertIsOnBackgroundThread();

  if (!sInstanceInit) {
    sInstanceInit = true;
    sInstance = new ShutdownObserver();
  }

  nsRefPtr<ShutdownObserver> ref = sInstance;
  return ref.forget();
}

nsresult
ShutdownObserver::AddManagerId(ManagerId* aManagerId)
{
  mozilla::ipc::AssertIsOnBackgroundThread();

  if (mShuttingDown) {
    return NS_ERROR_ILLEGAL_DURING_SHUTDOWN;
  }

  nsCOMPtr<nsIRunnable> runnable =
    NS_NewRunnableMethodWithArg<nsRefPtr<ManagerId>>(
      this, &ShutdownObserver::AddManagerIdOnMainThread, aManagerId);

  DebugOnly<nsresult> rv =
    NS_DispatchToMainThread(runnable, nsIThread::DISPATCH_NORMAL);

  MOZ_ASSERT(NS_SUCCEEDED(rv));

  return NS_OK;
}

void
ShutdownObserver::RemoveManagerId(ManagerId* aManagerId)
{
  mozilla::ipc::AssertIsOnBackgroundThread();

  nsCOMPtr<nsIRunnable> runnable =
    NS_NewRunnableMethodWithArg<nsRefPtr<ManagerId>>(
      this, &ShutdownObserver::RemoveManagerIdOnMainThread, aManagerId);

  DebugOnly<nsresult> rv =
    NS_DispatchToMainThread(runnable, nsIThread::DISPATCH_NORMAL);

  MOZ_ASSERT(NS_SUCCEEDED(rv));
}

ShutdownObserver::ShutdownObserver()
  : mBackgroundThread(NS_GetCurrentThread())
  , mShuttingDown(false)
{
  mozilla::ipc::AssertIsOnBackgroundThread();

  nsCOMPtr<nsIRunnable> runnable =
    NS_NewRunnableMethod(this, &ShutdownObserver::InitOnMainThread);
  DebugOnly<nsresult> rv =
    NS_DispatchToMainThread(runnable, nsIThread::DISPATCH_NORMAL);
  MOZ_ASSERT(NS_SUCCEEDED(rv));
}

ShutdownObserver::~ShutdownObserver()
{
  // This can happen on either main thread or background thread.
}

void
ShutdownObserver::InitOnMainThread()
{
  MOZ_ASSERT(NS_IsMainThread());

  nsCOMPtr<nsIObserverService> os = GetObserverService();

  // If there is no observer service then we are already shutting down,
  // but content just tried to use the Cache API for the first time.
  // Trigger an immediate Cache shutdown.
  if (!os) {
    DoShutdown();
    return;
  }

  os->AddObserver(this, "profile-before-change", false /* weak ref */);
}

void
ShutdownObserver::AddManagerIdOnMainThread(ManagerId* aManagerId)
{
  MOZ_ASSERT(NS_IsMainThread());

  for (uint32_t i = 0; i < mManagerIds.Length(); ++i) {
    if (*mManagerIds[i] == *aManagerId) {
      return;
    }
  }
  mManagerIds.AppendElement(aManagerId);
}

void
ShutdownObserver::RemoveManagerIdOnMainThread(ManagerId* aManagerId)
{
  MOZ_ASSERT(NS_IsMainThread());

  for (uint32_t i = 0; i < mManagerIds.Length(); ++i) {
    if (*mManagerIds[i] == *aManagerId) {
      mManagerIds.RemoveElementAt(i);
      return;
    }
  }
}

void
ShutdownObserver::StartShutdownOnBgThread()
{
  mozilla::ipc::AssertIsOnBackgroundThread();

  mShuttingDown = true;

  for (uint32_t i = 0; i < mManagerIdsInProcess.Length(); ++i) {
    nsRefPtr<Manager> manager = Manager::Get(mManagerIdsInProcess[i]);
    if (manager) {
      manager->Shutdown();
    }
  }
}

void
ShutdownObserver::FinishShutdownOnBgThread()
{
  mozilla::ipc::AssertIsOnBackgroundThread();
  MOZ_ASSERT(mShuttingDown);

  sInstance = nullptr;
}

void
ShutdownObserver::DoShutdown()
{
  MOZ_ASSERT(NS_IsMainThread());

  nsCOMPtr<nsIObserverService> os = mozilla::services::GetObserverService();
  if (os) {
    os->RemoveObserver(this, "profile-before-change");
  }

  // Copy origins to separate array to process to avoid races
  mManagerIdsInProcess = mManagerIds;

  // Send shutdown notification to origin managers
  nsCOMPtr<nsIRunnable> runnable =
    NS_NewRunnableMethod(this, &ShutdownObserver::StartShutdownOnBgThread);
  DebugOnly<nsresult> rv =
    mBackgroundThread->Dispatch(runnable, nsIThread::DISPATCH_NORMAL);
  MOZ_ASSERT(NS_SUCCEEDED(rv));

  runnable = nullptr;

  // Wait for managers to shutdown
  while (!mManagerIds.IsEmpty()) {
    if (!NS_ProcessNextEvent()) {
      NS_WARNING("Something bad happened!");
      break;
    }
  }

  // schedule runnable to clear singleton ref on background thread
  runnable =
    NS_NewRunnableMethod(this, &ShutdownObserver::FinishShutdownOnBgThread);
  rv = mBackgroundThread->Dispatch(runnable, nsIThread::DISPATCH_NORMAL);
  MOZ_ASSERT(NS_SUCCEEDED(rv));
}

NS_IMETHODIMP
ShutdownObserver::Observe(nsISupports* aSubject, const char* aTopic,
                          const char16_t* aData)
{
  MOZ_ASSERT(NS_IsMainThread());

  if (!strcmp(aTopic, "profile-before-change")) {
    DoShutdown();
  }

  return NS_OK;
}

} // namespace cache
} // namespace dom
} // namespace mozilla
