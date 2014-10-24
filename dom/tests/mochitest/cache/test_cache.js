ok(!!caches, 'caches object should be available on global');
caches.open('snafu').then(function(openCache) {
  ok(!!openCache, 'cache object should be resolved from caches.open');
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
  return caches.has('snafu');
}).then(function(hasMissingCache) {
  ok(!hasMissingCache, 'missing key should return false from has');
  workerTestDone();
});
