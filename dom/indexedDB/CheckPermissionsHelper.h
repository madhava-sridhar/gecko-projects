/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_checkpermissionshelper_h__
#define mozilla_dom_indexeddb_checkpermissionshelper_h__

#include "nsAutoPtr.h"
#include "nsCOMPtr.h"
#include "nsIInterfaceRequestor.h"
#include "nsIObserver.h"
#include "nsIRunnable.h"

class nsIDOMWindow;

namespace mozilla {
namespace dom {
namespace indexedDB {

class OpenDatabaseHelper;

class CheckPermissionsHelper MOZ_FINAL
  : public nsIRunnable
  , public nsIInterfaceRequestor
  , public nsIObserver
{
  nsRefPtr<OpenDatabaseHelper> mHelper;
  nsCOMPtr<nsIDOMWindow> mWindow;
  uint32_t mPromptResult;
  bool mPromptAllowed;
  bool mHasPrompted;

public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIRUNNABLE
  NS_DECL_NSIINTERFACEREQUESTOR
  NS_DECL_NSIOBSERVER

  CheckPermissionsHelper(OpenDatabaseHelper* aHelper,
                         nsIDOMWindow* aWindow);

private:
  ~CheckPermissionsHelper();
};

} // namespace indexedDB
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_indexeddb_checkpermissionshelper_h__
