ok(!!caches, 'caches object should be available on global');
caches.create('snafu').then(function(createCache) {
  ok(!!createCache, 'cache object should be resolved from caches.create');
  return caches.get('snafu');
}).then(function(getCache) {
  ok(!!getCache, 'cache object should be resolved from caches.get');
  return caches.has('snafu');
}).then(function(hasResult) {
  ok(hasResult, 'caches.has() should resolve true');
  return caches.keys();
}).then(function(keys) {
  ok(!!keys, 'caches.keys() should resolve to a truthy value');
  is(1, keys.length, 'caches.keys() should resolve to an array of length 1');
  is(0, keys.indexOf('snafu'), 'caches.keys() should resolve to an array containing key');
  return caches.delete('snafu');
}).then(function(deleteResult) {
  ok(deleteResult, 'caches.delete() should resolve true');
  return caches.get('snafu');
}).then(function(getMissingCache) {
  is(undefined, getMissingCache, 'missing key should resolve to undefined cache');
}).then(function() {
  return caches.create('snafu');
}).then(function(snafu) {
  return snafu.keys();
}).then(function(empty) {
  is(0, empty.length, 'cache.keys() should resolve to an array of length 0');
}).then(function() {
  return caches.get('snafu');
}).then(function(snafu) {
  var req = './cachekey';
  var res = new Response("Hello world");
  return snafu.put(req, res).then(function(v) {
    return snafu;
  });
}).then(function(snafu) {
  return Promise.all([snafu, snafu.keys()]);
}).then(function(args) {
  var snafu = args[0];
  var keys = args[1];
  is(1, keys.length, 'cache.keys() should resolve to an array of length 1');
  ok(keys[0] instanceof Request, 'key should be a Request');
  ok(keys[0].url.match(/cachekey$/), 'Request URL should match original');
  return snafu.match(keys[0]);
}).then(function(response) {
  ok(response instanceof Response, 'value should be a Response');
  is(response.status, 200, 'Response status should be 200');
  return response.text().then(function(v) {
    is(v, "Hello world", "Response body should match original");
  });
}).then(function() {
  workerTestDone();
})
