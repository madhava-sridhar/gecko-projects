/* -*- Mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; tab-width: 40 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_workers_WorkerStructuredClone_h
#define mozilla_dom_workers_WorkerStructuredClone_h

#include "mozilla/dom/MessagePort.h"
#include "mozilla/dom/MessagePortBinding.h"

BEGIN_WORKERS_NAMESPACE

struct
WorkerStructuredCloneClosure
{
  // This is used for creating the new MessagePort it can be null if in workers.
  nsCOMPtr<nsPIDOMWindow> mParent;

  nsTArray<nsCOMPtr<nsISupports>> mClonedObjects;

  // The transferred port created on the 'other side' of the Structured Clone
  // Algorithm.
  nsTArray<nsRefPtr<MessagePortBase>> mMessagePorts;

  // Information for the transferring.
  nsTArray<MessagePortIdentifier> mMessagePortIdentifiers;

  // To avoid duplicates in the transferred ports.
  nsTHashtable<nsRefPtrHashKey<MessagePortBase>> mTransferredPorts;
};

END_WORKERS_NAMESPACE

#endif // mozilla_dom_workers_WorkerStructuredClone_h
