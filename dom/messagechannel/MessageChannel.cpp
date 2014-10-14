/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MessageChannel.h"

#include "mozilla/Preferences.h"
#include "mozilla/dom/MessageChannelBinding.h"
#include "mozilla/dom/MessagePort.h"
#include "mozilla/dom/Navigator.h"
#include "mozilla/dom/WorkerPrivate.h"
#include "mozilla/dom/WorkerRunnable.h"
#include "nsContentUtils.h"
#include "nsPIDOMWindow.h"
#include "nsServiceManagerUtils.h"
#include "nsIDocument.h"
#include "nsIPrincipal.h"

using namespace mozilla::dom::workers;

namespace mozilla {
namespace dom {

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(MessageChannel, mWindow, mPort1, mPort2)
NS_IMPL_CYCLE_COLLECTING_ADDREF(MessageChannel)
NS_IMPL_CYCLE_COLLECTING_RELEASE(MessageChannel)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(MessageChannel)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

namespace {
bool gPrefInitialized = false;
bool gPrefEnabled = false;

bool
CheckPermission(nsIPrincipal* aPrincipal, bool aCallerChrome)
{
  if (!gPrefInitialized) {
    Preferences::AddBoolVarCache(&gPrefEnabled, "dom.messageChannel.enabled");
    gPrefInitialized = true;
  }

  // Enabled by pref
  if (gPrefEnabled) {
    return true;
  }

  // Chrome callers are allowed.
  if (aCallerChrome) {
    return true;
  }

  nsCOMPtr<nsIURI> uri;
  if (NS_FAILED(aPrincipal->GetURI(getter_AddRefs(uri))) || !uri) {
    return false;
  }

  bool isResource = false;
  if (NS_FAILED(uri->SchemeIs("resource", &isResource))) {
    return false;
  }

  return isResource;
}

nsIPrincipal*
GetPrincipalFromWorkerPrivate(WorkerPrivate* aWorkerPrivate)
{
  nsIPrincipal* principal = aWorkerPrivate->GetPrincipal();
  if (principal) {
    return principal;
  }

  // Walk up to our containing page
  WorkerPrivate* wp = aWorkerPrivate;
  while (wp->GetParent()) {
    wp = wp->GetParent();
  }

  nsPIDOMWindow* window = wp->GetWindow();
  if (!window) {
    return nullptr;
  }

  nsIDocument* doc = window->GetExtantDoc();
  if (!doc) {
    return nullptr;
  }

  return doc->NodePrincipal();
}

// A WorkerMainThreadRunnable to synchronously dispatch the call of
// CheckPermission() from the worker thread to the main thread.
class CheckPermissionRunnable MOZ_FINAL : public WorkerMainThreadRunnable
{
public:
  bool mResult;
  bool mCallerChrome;

  CheckPermissionRunnable(WorkerPrivate* aWorkerPrivate)
    : WorkerMainThreadRunnable(aWorkerPrivate)
    , mResult(false)
    , mCallerChrome(nsContentUtils::ThreadsafeIsCallerChrome())
  {
    MOZ_ASSERT(aWorkerPrivate);
    aWorkerPrivate->AssertIsOnWorkerThread();
  }

protected:
  virtual bool
  MainThreadRun() MOZ_OVERRIDE
  {
    AssertIsOnMainThread();

    nsIPrincipal* principal = GetPrincipalFromWorkerPrivate(mWorkerPrivate);
    if (!principal) {
      return true;
    }

    mResult = CheckPermission(principal, mCallerChrome);
    return true;
  }
};

} // anonymous namespace

/* static */ bool
MessageChannel::Enabled(JSContext* aCx, JSObject* aGlobal)
{
  if (NS_IsMainThread()) {
    JS::Rooted<JSObject*> global(aCx, aGlobal);

    nsCOMPtr<nsPIDOMWindow> win = Navigator::GetWindowFromGlobal(global);
    if (!win) {
      return false;
    }

    nsIDocument* doc = win->GetExtantDoc();
    if (!doc) {
      return false;
    }

    return CheckPermission(doc->NodePrincipal(),
                           nsContentUtils::ThreadsafeIsCallerChrome());
  }

  WorkerPrivate* workerPrivate = GetWorkerPrivateFromContext(aCx);
  workerPrivate->AssertIsOnWorkerThread();

  nsRefPtr<CheckPermissionRunnable> runnable =
    new CheckPermissionRunnable(workerPrivate);
  runnable->Dispatch(aCx);

  return runnable->mResult;
}

MessageChannel::MessageChannel(nsPIDOMWindow* aWindow,
                               const nsID& aPortUUID1,
                               const nsID& aPortUUID2)
  : mWindow(aWindow)
{
  MOZ_COUNT_CTOR(MessageChannel);

  mPort1 = new MessagePort(mWindow, aPortUUID1, aPortUUID2);
  mPort2 = new MessagePort(mWindow, aPortUUID2, aPortUUID1);

  mPort1->UnshippedEntangle(mPort2);
  mPort2->UnshippedEntangle(mPort1);
}

MessageChannel::~MessageChannel()
{
  MOZ_COUNT_DTOR(MessageChannel);
}

JSObject*
MessageChannel::WrapObject(JSContext* aCx)
{
  return MessageChannelBinding::Wrap(aCx, this);
}

/* static */ already_AddRefed<MessageChannel>
MessageChannel::Constructor(const GlobalObject& aGlobal, ErrorResult& aRv)
{
  nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(aGlobal.GetAsSupports());
  // window can be null in workers.

  nsID portUUID1;
  aRv = nsContentUtils::GenerateUUID(portUUID1);
  if (aRv.Failed()) {
    return nullptr;
  }

  nsID portUUID2;
  aRv = nsContentUtils::GenerateUUID(portUUID2);
  if (aRv.Failed()) {
    return nullptr;
  }

  nsRefPtr<MessageChannel> channel =
    new MessageChannel(window, portUUID1, portUUID2);

  return channel.forget();
}

} // namespace dom
} // namespace mozilla
