/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * For more information on this interface, please see
 * http://slightlyoff.github.io/ServiceWorker/spec/service_worker/index.html
 */

[Constructor(DOMString type, optional FetchEventInit eventInitDict),
 Func="mozilla::dom::workers::ServiceWorkerEventsVisible",
 Exposed=(ServiceWorker)]
interface FetchEvent : Event {
  readonly attribute Request request;
  readonly attribute ServiceWorkerClient client; // The window issuing the request.
  readonly attribute Context context;
  readonly attribute boolean isReload;

  [Throws] void respondWith(Promise<Response> r);
  Promise<Response> forwardTo(ScalarValueString url);
  Promise<Response> default();
};

dictionary FetchEventInit : EventInit {
  Request request;
  ServiceWorkerClient client;
  Context context;
  boolean isReload;
};

enum Context {
  "connect",
  "font",
  "img",
  "object",
  "script",
  "style",
  "worker",
  "popup",
  "child",
  "navigate"
};
