/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Client.h"

#include "FileManager.h"
#include "IDBDatabase.h"
#include "IndexedDatabaseManager.h"
#include "mozilla/dom/quota/QuotaManager.h"
#include "mozilla/dom/quota/Utilities.h"
#include "nsIFile.h"
#include "nsISimpleEnumerator.h"
#include "nsString.h"
#include "TransactionThreadPool.h"

USING_INDEXEDDB_NAMESPACE
using mozilla::dom::quota::AssertIsOnIOThread;
using mozilla::dom::quota::QuotaManager;

namespace {


} // anonymous namespace

// This needs to be fully qualified to not confuse trace refcnt assertions.

